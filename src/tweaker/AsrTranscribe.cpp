// SPDX-License-Identifier: GPL-3.0-only
/*
 * Copyright (C) 2022-present, PenUniverse.
 * This file is part of the PenMods open source project.
 */

// 需求 11：音频播放页「音频转文字」。
//
// 流程：当前曲目本地路径 → （必要时）mpv 转成 16 kHz 单声道 16-bit WAV → 按数据块边界切片
// → 串行上传 DeepSeek ASR → 按顺序拼接文本 → 预览 → 保存成同目录同名 .txt。
//
// 关键约束（接口事实）：单次请求 ≤120 秒 / ≤8 MiB，所以切片上限取 110 秒 / 6 MiB，两边都留余量。
// 全程异步：转码用 QProcess，上传用 QNetworkAccessManager；切换曲目时按"目标是否仍是当前曲目"
// 放弃后续请求（和 `MusicPlayerLyrics.cpp` 的做法一致）。

#include "filemanager/player/MusicPlayer.h"

#include "tweaker/AsrService.h"
#include "tweaker/AsrWav.h"
#include "tweaker/DeepSeekAsr.h"

#include "common/Utils.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QProcess>
#include <QSaveFile>

namespace mod {

namespace {

/// 单段上限：接口是 120 秒 / 8 MiB，两个都留余量（wav 头 + 可能的声道转换会带来一点膨胀）。
constexpr int    kSegmentSeconds = 110;
constexpr qint64 kSegmentBytes   = 6 * 1024 * 1024;
/// 直传（不转码）的文件上限：接口硬上限 8 MiB。
constexpr qint64 kDirectBytes = 8 * 1024 * 1024;
/// 转码产物的上限。16 kHz 单声道 16-bit = 32 KiB/s，64 MiB 约等于 34 分钟；
/// 设备 /tmp 是 tmpfs（内存盘），再大就不该往里面写了。
constexpr qint64 kTranscodeBytes   = 64 * 1024 * 1024;
constexpr int    kTranscodeTimeout = 120000;
/// 设备上的 mpv（外部视频播放器用的那个）。没有 ffmpeg CLI，转码只能借它。
constexpr auto kMpvPath = "/userdisk/mpv/bin/mpv";

/// 粗略估计 Ogg 时长：最后一个 "OggS" 页的 granulepos 就是 PCM 采样数（Opus 固定按 48 kHz 计）。
/// 只用来判断"这个 Ogg 能不能整段直接上传"；估不出来返回 -1，调用方转码后按 WAV 正常切片。
double oggDurationSeconds(const QString& path) {
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) return -1;
    const qint64 tailSize = qMin<qint64>(file.size(), 64 * 1024);
    if (tailSize < 64) return -1;
    if (!file.seek(file.size() - tailSize)) return -1;
    const QByteArray tail = file.read(tailSize);
    const int        last = tail.lastIndexOf("OggS");
    if (last < 0 || last + 14 > tail.size()) return -1;
    const auto* data    = reinterpret_cast<const uchar*>(tail.constData()) + last;
    qint64      granule = 0;
    for (int i = 0; i < 8; ++i) granule |= static_cast<qint64>(data[6 + i]) << (8 * i);
    if (granule <= 0) return -1;
    return static_cast<double>(granule) / 48000.0;
}

} // namespace

QString AsrService::transcribeUnavailableReason() const {
    if (!mEnabled) return QStringLiteral("请先在「设置 → 显示与输入 → 语言识别」里开启 DeepSeek 语音识别");
    if (!hasApiKey()) return QStringLiteral("未配置 DeepSeek API Key");
    if (mTranscribeBusy) return QStringLiteral("正在转写，请稍候");
    return {};
}

void AsrService::startTranscribe(const QString& path) {
    const auto reason = transcribeUnavailableReason();
    if (!reason.isEmpty()) {
        _toast(reason);
        return;
    }
    const QFileInfo info(path);
    if (path.isEmpty() || !info.exists() || !info.isFile()) {
        _toast(QStringLiteral("找不到当前音频文件"));
        return;
    }
    _transcribeBegin(info.absoluteFilePath());
}

bool AsrService::transcribeResultFor(const QString& path) const {
    if (mTranscribeSource.isEmpty() || path.isEmpty()) return false;
    return QFileInfo(path).absoluteFilePath() == mTranscribeSource;
}

void AsrService::cancelTranscribe() {
    if (!mTranscribeBusy) return;
    info("音频转文字：用户取消");
    _transcribeCleanup();
    mTranscribeStatus = QStringLiteral("已取消转写");
    mTranscribeProgress.clear();
    emit transcribeChanged();
}

// ───────────────────────── 生命周期 ─────────────────────────

void AsrService::_transcribeBegin(const QString& path) {
    _transcribeCleanup();
    mTranscribeTarget = path;
    mTranscribeSource = path;
    mTranscribePreview.clear();
    mTranscribeBusy = true;
    _transcribeSetStatus(QStringLiteral("正在准备音频…"));
    emit transcribeChanged();

    connect(mFileClient, &DeepSeekAsr::succeeded, this, [this](const QString& text, int) { _segmentSucceeded(text); });
    connect(mFileClient, &DeepSeekAsr::failed, this, [this](const QString& reason) { _segmentFailed(reason); });

    _transcribeDirect(path);
}

void AsrService::_transcribeRelease() {
    mTranscodeDeadline.stop();
    if (auto* process = mTranscode.data()) {
        mTranscode.clear();
        process->disconnect(this);
        process->kill();
        process->deleteLater();
    }
    if (mFileClient) disconnect(mFileClient, nullptr, this, nullptr);
    mFileClient->cancel();
    if (!mTranscodeTemp.isEmpty()) {
        QFile::remove(mTranscodeTemp);
        mTranscodeTemp.clear();
    }
}

void AsrService::_transcribeCleanup() {
    _transcribeRelease();
    mSegmentTexts.clear();
    mSegmentOffsets.clear();
    mSegmentBytes.clear();
    mSegmentSource.clear();
    mSegmentDirectFile = false;
    mSegmentIndex      = 0;
    mSegmentTotal      = 0;
    mTranscribeProgress.clear();
    mTranscribeBusy = false;
}

void AsrService::_transcribeFail(const QString& reason) {
    warn("音频转文字失败：{}", reason.toStdString());
    _transcribeCleanup();
    mTranscribePreview = QString();
    mTranscribeStatus  = reason;
    emit transcribeChanged();
    _toast(QStringLiteral("音频转文字失败：") + reason);
}

void AsrService::_transcribeSetStatus(const QString& status, const QString& progress) {
    mTranscribeStatus = status;
    if (!progress.isNull()) mTranscribeProgress = progress;
    emit transcribeChanged();
}

// ───────────────────────── 输入处理 ─────────────────────────

void AsrService::_transcribeDirect(const QString& path) {
    // 注意：局部变量不能叫 info —— 本类继承 Logger，Logger::info() 是写日志用的，
    // 同名局部量会让后面 info("...") 这类日志调用解析到 QFileInfo 上。
    const QFileInfo fileInfo(path);
    const auto      suffix = fileInfo.suffix().toLower();

    if (suffix == QLatin1String("wav")) {
        QFile file(path);
        if (!file.open(QIODevice::ReadOnly)) {
            _transcribeFail(QStringLiteral("音频文件无法读取"));
            return;
        }
        const auto head = file.read(64 * 1024);
        file.close();
        asr::WavFormat format;
        if (!asr::parseWavHeader(head, format)) {
            // 浮点 / 压缩 WAV（或其实是别的容器）：交给 mpv 变成标准 PCM
            info("音频不是可直传的整数 PCM WAV，改用 mpv 转码");
            _transcribeTranscode(path);
            return;
        }
        _transcribePlanWav(path);
        return;
    }

    if (suffix == QLatin1String("ogg") || suffix == QLatin1String("opus")) {
        const double duration = oggDurationSeconds(path);
        if (fileInfo.size() <= kDirectBytes && duration > 0 && duration <= kSegmentSeconds) {
            // 整段可直接用（接口原生支持 Ogg Opus）
            mSegmentSource     = path;
            mSegmentSuffix     = QStringLiteral("ogg");
            mSegmentDirectFile = true;
            mSegmentOffsets.clear();
            mSegmentBytes.clear();
            mSegmentOffsets << 0;
            mSegmentBytes << fileInfo.size();
            mSegmentTotal = 1;
            mSegmentIndex = 0;
            _uploadNextSegment();
            return;
        }
        info("Ogg 超过单次请求限制（{} 字节 / {} 秒），转码后分段", fileInfo.size(), duration);
        _transcribeTranscode(path);
        return;
    }

    _transcribeTranscode(path);
}

void AsrService::_transcribeTranscode(const QString& path) {
    if (!QFileInfo::exists(QString::fromLatin1(kMpvPath))) {
        _transcribeFail(QStringLiteral("设备上没有 mpv，无法把这个音频转成 WAV"));
        return;
    }

    mTranscodeTemp = QDir::tempPath() + QStringLiteral("/penmods-asr-%1.wav").arg(generateUUID().left(8));
    QFile::remove(mTranscodeTemp);

    auto* process = new QProcess(this);
    mTranscode    = process;
    process->setProcessChannelMode(QProcess::MergedChannels);
    connect(process, QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished), this, [this, process](int code, QProcess::ExitStatus status) {
        if (mTranscode != process) return;
        mTranscode.clear();
        mTranscodeDeadline.stop();
        process->disconnect(this);
        process->deleteLater();
        if (status != QProcess::NormalExit || code != 0) {
            _transcribeFail(QStringLiteral("音频转码失败（mpv 退出码 %1）").arg(code));
            return;
        }
        _transcribeOnTranscodeFinished();
    });
    // 进程起不来时 Qt 不会发 finished（只有 errorOccurred），不能干等转码超时
    connect(process, &QProcess::errorOccurred, this, [this, process](QProcess::ProcessError error) {
        if (mTranscode != process) return;
        if (error == QProcess::FailedToStart)
            _transcribeFail(QStringLiteral("无法启动 mpv，请检查 /userdisk/mpv/bin/mpv"));
    });
    mTranscodeDeadline.setSingleShot(true);
    disconnect(&mTranscodeDeadline, nullptr, this, nullptr);
    connect(&mTranscodeDeadline, &QTimer::timeout, this, [this]() {
        warn("音频转码超时");
        _transcribeFail(QStringLiteral("音频转码超时"));
    });

    // 待真机验证：以下 mpv 参数（0.36.0）在设备上的实际行为。
    //   --no-video            只要音频，避免打开任何视频输出
    //   --audio-samplerate=16 16 kHz（接口支持的采样率之一）
    //   --audio-channels=1    单声道（双声道文件接口只识别左声道，直接混成单声道更稳）
    //   --audio-format=s16    16-bit 有符号 PCM
    //   --ao=pcm              用 pcm 音频输出落盘，默认带 WAV 头
    //   --ao-pcm-file=<out>   落盘路径
    //   --no-config           不加载 /userdisk/mpv/config（那里配的是 wlshm 视频输出，与这里无关）
    const QStringList arguments{
        QStringLiteral("--no-config"),
        QStringLiteral("--really-quiet"),
        QStringLiteral("--no-video"),
        QStringLiteral("--audio-samplerate=16000"),
        QStringLiteral("--audio-channels=1"),
        QStringLiteral("--audio-format=s16"),
        QStringLiteral("--ao=pcm"),
        QStringLiteral("--ao-pcm-file=") + mTranscodeTemp,
        path,
    };
    _transcribeSetStatus(QStringLiteral("正在转码…"));
    mTranscodeDeadline.start(kTranscodeTimeout);
    info("音频转文字：开始 mpv 转码");
    process->start(QString::fromLatin1(kMpvPath), arguments);
}

void AsrService::_transcribeOnTranscodeFinished() {
    const QFileInfo output(mTranscodeTemp);
    if (!output.exists() || output.size() <= 44) {
        _transcribeFail(QStringLiteral("转码没有产生音频数据"));
        return;
    }
    if (output.size() > kTranscodeBytes) {
        _transcribeFail(QStringLiteral("音频太长（转码结果超过 64 MiB），暂不支持"));
        return;
    }
    QFile probe(mTranscodeTemp);
    if (probe.open(QIODevice::ReadOnly)) {
        const auto     head = probe.read(64 * 1024);
        asr::WavFormat format;
        const bool     valid = asr::parseWavHeader(head, format);
        probe.close();
        if (!valid) {
            _transcribePlanRaw(mTranscodeTemp);
            return;
        }
    }
    _transcribePlanWav(mTranscodeTemp);
}

void AsrService::_transcribePlanWav(const QString& wavPath) {
    QFile file(wavPath);
    if (!file.open(QIODevice::ReadOnly)) {
        _transcribeFail(QStringLiteral("音频文件无法读取"));
        return;
    }
    const auto     head = file.read(64 * 1024);
    asr::WavFormat format;
    if (!asr::parseWavHeader(head, format)) {
        _transcribeFail(QStringLiteral("音频不是可识别的 16-bit PCM WAV"));
        return;
    }
    // 头里声明的 data 长度不可信（流式写入时是 0 / 0xFFFFFFFF），以文件实际剩余长度为准
    qint64       dataBytes = format.dataBytes;
    const qint64 available = file.size() - format.dataOffset;
    if (available <= 0) {
        _transcribeFail(QStringLiteral("音频里没有可识别的内容"));
        return;
    }
    if (dataBytes <= 0 || dataBytes > available) dataBytes = available;
    file.close();

    mSegmentSource     = wavPath;
    mSegmentSuffix     = QStringLiteral("wav");
    mSegmentDirectFile = false;
    _transcribePlanRange(format.dataOffset, dataBytes, format.sampleRate, format.channels);
}

/// mpv 的 `--ao=pcm` 在个别构建里会关掉 waveheader，落盘的是裸 PCM。参数是我们自己指定的
/// （16000 Hz / 单声道 / s16），所以这里按这个格式把整文件当数据块处理，切片时再套 WAV 头 ——
/// 比让整条链路因为少 44 字节而失败要好得多。
void AsrService::_transcribePlanRaw(const QString& pcmPath) {
    // 同 _transcribeDirect：局部量不能与 Logger::info 同名。
    const QFileInfo fileInfo(pcmPath);
    if (!fileInfo.exists() || fileInfo.size() <= 0) {
        _transcribeFail(QStringLiteral("转码没有产生音频数据"));
        return;
    }
    mSegmentSource     = pcmPath;
    mSegmentSuffix     = QStringLiteral("wav");
    mSegmentDirectFile = false;
    info("音频转文字：转码输出没有 WAV 头，按 16 kHz 单声道裸 PCM 处理");
    _transcribePlanRange(0, fileInfo.size(), 16000, 1);
}

/// 按给定的数据范围切片：每片同时满足"≤110 秒"和"≤6 MiB"，并严格对齐到采样帧边界
/// （切半个采样会让每段结尾都变成噪音）。
void AsrService::_transcribePlanRange(qint64 dataOffset, qint64 dataBytes, int rate, int channels) {
    const qint64 frame          = qMax<qint64>(1, static_cast<qint64>(channels) * 2); // 16-bit
    const qint64 bytesPerSecond = static_cast<qint64>(rate) * frame;
    if (bytesPerSecond <= 0 || dataBytes <= 0) {
        _transcribeFail(QStringLiteral("音频里没有可识别的内容"));
        return;
    }

    qint64 maxBytes = qMin(kSegmentBytes, bytesPerSecond * kSegmentSeconds);
    maxBytes        = (maxBytes / frame) * frame;
    if (maxBytes <= 0) {
        _transcribeFail(QStringLiteral("音频格式不支持分段"));
        return;
    }

    mSegmentRate     = rate;
    mSegmentChannels = channels;
    mSegmentTexts.clear();
    mSegmentOffsets.clear();
    mSegmentBytes.clear();
    qint64 offset    = dataOffset;
    qint64 remaining = dataBytes;
    while (remaining > 0) {
        qint64 take = qMin(remaining, maxBytes);
        take        = (take / frame) * frame;
        if (take <= 0) break;
        mSegmentOffsets << offset;
        mSegmentBytes << take;
        offset    += take;
        remaining -= take;
    }
    if (mSegmentOffsets.isEmpty()) {
        _transcribeFail(QStringLiteral("音频里没有可识别的内容"));
        return;
    }

    mSegmentTotal = mSegmentOffsets.size();
    mSegmentIndex = 0;
    info("音频转文字：{} 段（{} 秒上限）", mSegmentTotal, kSegmentSeconds);
    _uploadNextSegment();
}

// ───────────────────────── 上传与结果 ─────────────────────────

QByteArray AsrService::_segmentPayload() const {
    QFile file(mSegmentSource);
    if (!file.open(QIODevice::ReadOnly)) return {};

    if (mSegmentDirectFile) {
        if (file.size() > kDirectBytes) return {};
        return file.readAll();
    }

    if (mSegmentIndex < 0 || mSegmentIndex >= mSegmentOffsets.size()) return {};
    if (!file.seek(mSegmentOffsets.at(mSegmentIndex))) return {};
    const QByteArray raw = file.read(mSegmentBytes.at(mSegmentIndex));
    if (raw.isEmpty()) return {};
    // 双声道只识别左声道：抽出来重写单声道 WAV，顺带把体积减半
    const QByteArray pcm = mSegmentChannels > 1 ? asr::extractChannel(raw, mSegmentChannels, 0) : raw;
    return asr::buildWav(pcm, mSegmentRate, 1);
}

void AsrService::_uploadNextSegment() {
    if (!mTranscribeBusy) return;
    if (mSegmentIndex >= mSegmentTotal) {
        _transcribeFinish();
        return;
    }
    if (!_transcribeTargetStillCurrent()) {
        _transcribeFail(QStringLiteral("曲目已切换，已取消转写"));
        return;
    }

    const auto payload = _segmentPayload();
    if (payload.isEmpty()) {
        _transcribeFail(QStringLiteral("读取音频分段失败"));
        return;
    }

    _transcribeSetStatus(
        QStringLiteral("正在识别第 %1/%2 段…").arg(mSegmentIndex + 1).arg(mSegmentTotal),
        QStringLiteral("%1/%2").arg(mSegmentIndex + 1).arg(mSegmentTotal)
    );
    info("音频转文字：上传第 {}/{} 段，{} 字节", mSegmentIndex + 1, mSegmentTotal, payload.size());
    // 失败会同步走到 _segmentFailed → _transcribeFail
    mFileClient->upload(payload, mSegmentSuffix, _apiKey(), mTimeoutMs);
}

void AsrService::_segmentSucceeded(const QString& text) {
    if (!mTranscribeBusy) return;
    const auto trimmed = text.trimmed();
    if (!trimmed.isEmpty()) mSegmentTexts << trimmed;
    ++mSegmentIndex;
    _uploadNextSegment();
}

void AsrService::_segmentFailed(const QString& reason) {
    if (!mTranscribeBusy) return;
    _transcribeFail(reason);
}

void AsrService::_transcribeFinish() {
    // 段间用换行连接：语音识别本身没有标点，换行至少让长音频的结果可读
    const QString text  = mSegmentTexts.join(QLatin1Char('\n')).trimmed();
    const int     total = mSegmentTotal;
    _transcribeRelease();
    mTranscribePreview = text;
    mSegmentTexts.clear();
    mSegmentOffsets.clear();
    mSegmentBytes.clear();
    mSegmentDirectFile = false;
    mSegmentIndex      = 0;
    mSegmentTotal      = 0;
    mTranscribeProgress.clear();
    mTranscribeBusy   = false;
    mTranscribeStatus = text.isEmpty() ? QStringLiteral("没有识别出文字")
                                       : QStringLiteral("转写完成，共 %1 段，点「保存文本」写入音频目录").arg(total);
    emit transcribeChanged();
}

bool AsrService::_transcribeTargetStillCurrent() const {
    if (mTranscribeTarget.isEmpty()) return false;
    // 切歌之后继续上传会把两首歌的文本拼在一起，所以每个分段开始前都确认一次。
    // 播放器被清空（没有当前曲目）时不算切歌，允许继续把已经开始的转写做完。
    const auto current = filemanager::MusicPlayer::getInstance().currentAudioLocalPath();
    return current.isEmpty() || current == mTranscribeTarget;
}

// ───────────────────────── 保存 ─────────────────────────

bool AsrService::saveTranscript() {
    if (mTranscribePreview.trimmed().isEmpty()) {
        _toast(QStringLiteral("还没有可保存的转写结果"));
        return false;
    }
    if (mTranscribeSource.isEmpty()) {
        _toast(QStringLiteral("找不到对应的音频文件"));
        return false;
    }

    const QFileInfo audio(mTranscribeSource);
    const auto      path = audio.absoluteDir().filePath(audio.completeBaseName() + QStringLiteral(".txt"));
    // 和歌词保存一致：绝不覆盖用户已有的同名 sidecar
    if (QFileInfo::exists(path)) {
        _toast(QStringLiteral("同名 txt 已存在，未覆盖原文件"));
        return false;
    }

    QSaveFile  file(path);
    const auto bytes = mTranscribePreview.toUtf8();
    if (!file.open(QIODevice::WriteOnly) || file.write(bytes) != bytes.size() || !file.commit()) {
        _toast(QStringLiteral("保存失败，请检查音乐目录是否可写"));
        return false;
    }

    info("音频转文字：已保存 {}", path.toStdString());
    _toast(QStringLiteral("转写文本已保存到音频目录"));
    _transcribeSetStatus(QStringLiteral("已保存：") + QFileInfo(path).fileName());
    return true;
}

} // namespace mod
