// SPDX-License-Identifier: GPL-3.0-only
/*
 * Copyright (C) 2022-present, PenUniverse.
 * This file is part of the PenMods open source project.
 */

#pragma once

#include "common/service/Logger.h"
#include "mod/Config.h"

#include <QByteArray>
#include <QElapsedTimer>
#include <QObject>
#include <QPointer>
#include <QString>
#include <QStringList>
#include <QTimer>
#include <QVector>

class QQmlContext;
class QProcess;

namespace mod {

class AsrCapture;
class DeepSeekAsr;

/// 需求 10（DeepSeek 语音识别接管）+ 需求 11（音频转文字）。
///
/// 设计约束（都是硬性的）：
///   • **默认关闭**。关闭时不安装任何 hook、不碰麦克风、不建网络管理器 —— 行为与没有这个类时
///     完全一致；`ai.asr.enabled` 是唯一的开关。
///   • 打开后只做"启动/结果级"接管：厂商的语音识别协议是未知的，我们不解析它的音频，也不重写
///     它的状态机，只在自己录音 → DeepSeek 识别 → 通过 `YSpeechManager::setAsrResult` 注入文本。
///   • 任何一步失败都必须能回到用户熟悉的行为。启动阶段（还没开始录音）失败 → 直接调用
///     `origin(...)` 让厂商自己识别；录制之后失败（超时/HTTP/解析/空音频）没法重放用户的这句话，
///     所以只弹一次气泡说明原因并复位 UI，让用户重来 —— 绝不把界面留在"聆听中"。
///   • 网络全程异步（QNetworkAccessManager + 超时定时器），UI 线程不和网络同步。
///   • 日志不写音频、不写 API Key。
///
/// 接管范围：键盘语音键（KeyBoard 直接分流）、以及"语音助手"开启时的语音助手页 / 长按语音
/// （YRecordCenter::startAsr 与 YSpeechManager::startAsrRecord 两条入口）。跟读评分
/// （startFollowSpeech）完全不动。
class AsrService : public QObject, public Singleton<AsrService>, private Logger {
    Q_OBJECT

    // ───────────── 需求 10：语言识别服务设定 ─────────────
    Q_PROPERTY(bool enabled READ enabled WRITE setEnabled NOTIFY settingsChanged)
    /// 当前选择的语音服务："system"（默认有道语音）/ "deepseek"（映射到 enabled）
    Q_PROPERTY(QString provider READ provider NOTIFY settingsChanged)
    Q_PROPERTY(bool hasApiKey READ hasApiKey NOTIFY settingsChanged)
    Q_PROPERTY(QString apiKeyHint READ apiKeyHint NOTIFY settingsChanged)
    Q_PROPERTY(QString apiKeySource READ apiKeySource NOTIFY settingsChanged)
    Q_PROPERTY(int timeoutMs READ timeoutMs WRITE setTimeoutMs NOTIFY settingsChanged)
    /// 我们自己的麦克风采集/识别是否在进行（键盘语音键、语音助手页的会话状态）。
    /// QML 用它和厂商的 `speechManager.recognizing` 一起判断"是否正在聆听"，因为接管期间厂商的
    /// 状态机根本不会前进（我们从来没有调用它的 startAsrRecord）。
    Q_PROPERTY(bool voiceActive READ voiceActive NOTIFY voiceChanged)
    Q_PROPERTY(QString voiceStatus READ voiceStatus NOTIFY voiceChanged)

    // ───────────── 需求 11：音频转文字 ─────────────
    Q_PROPERTY(bool transcribeBusy READ transcribeBusy NOTIFY transcribeChanged)
    Q_PROPERTY(QString transcribeStatus READ transcribeStatus NOTIFY transcribeChanged)
    Q_PROPERTY(QString transcribeProgress READ transcribeProgress NOTIFY transcribeChanged)
    Q_PROPERTY(QString transcribePreview READ transcribePreview NOTIFY transcribeChanged)
    Q_PROPERTY(bool transcribeReady READ transcribeReady NOTIFY transcribeChanged)
    Q_PROPERTY(QString transcribeSource READ transcribeSource NOTIFY transcribeChanged)

public:
    // ───────────── 需求 10：设置页 ─────────────
    [[nodiscard]] bool    enabled() const { return mEnabled; }
    void                  setEnabled(bool value);
    [[nodiscard]] QString provider() const { return mEnabled ? QStringLiteral("deepseek") : QStringLiteral("system"); }
    [[nodiscard]] bool    hasApiKey() const { return !_apiKey().isEmpty(); }
    [[nodiscard]] QString apiKeyHint() const;
    [[nodiscard]] QString apiKeySource() const;
    [[nodiscard]] int     timeoutMs() const { return mTimeoutMs; }
    void                  setTimeoutMs(int value);
    [[nodiscard]] bool    voiceActive() const { return mVoiceState != VoiceState::Idle; }
    [[nodiscard]] QString voiceStatus() const { return mVoiceStatus; }

    Q_INVOKABLE void selectProvider(const QString& provider);
    Q_INVOKABLE void setApiKey(const QString& key);
    Q_INVOKABLE void clearApiKey();

    /// 键盘语音键（`mod::KeyBoard::startVoiceInput` / `stopVoiceInput`）的接管入口。
    /// 返回 true 表示 AsrService 已经接管这次语音输入，调用方不要再进厂商路径。
    bool beginVoiceCapture(QObject* speechManager);
    bool endVoiceCapture(QObject* speechManager);
    /// 放弃当前会话（离开输入页/语音页时用）：不录音、不上传、直接把麦克风还给系统。
    Q_INVOKABLE void cancelVoiceCapture();

    /// 语音助手页 / 长按语音的接管入口。
    ///
    /// 由 `src/recorder/AudioRecorder.cpp` 里**已经存在**的 `YRecordCenter::startAsr` PEN_HOOK 调用
    /// （那个地址不能再被 DobbyHook 第二次，否则会把录音期间"挡住厂商录音"的保护顶掉）。
    /// 返回 true = 已接管，调用方应直接返回一个自增的录音序号，不要再进厂商录音路径。
    bool onRecordCenterStartAsr(void* recordCenter);

    // ───────────── 需求 11：音频转文字 ─────────────
    [[nodiscard]] bool    transcribeBusy() const { return mTranscribeBusy; }
    [[nodiscard]] QString transcribeStatus() const { return mTranscribeStatus; }
    [[nodiscard]] QString transcribeProgress() const { return mTranscribeProgress; }
    [[nodiscard]] QString transcribePreview() const { return mTranscribePreview; }
    [[nodiscard]] bool    transcribeReady() const { return !mTranscribePreview.isEmpty(); }
    [[nodiscard]] QString transcribeSource() const { return mTranscribeSource; }

    /// 不可用时返回一句可以直接弹气泡的原因；可用返回空串。
    Q_INVOKABLE QString transcribeUnavailableReason() const;
    Q_INVOKABLE void    startTranscribe(const QString& path);
    /// 当前预览结果是否属于这首音频（按绝对路径比较，避免把 A 的文本保存到 B 上）。
    Q_INVOKABLE bool transcribeResultFor(const QString& path) const;
    Q_INVOKABLE void cancelTranscribe();
    /// 保存为音频同目录同名 `.txt`（不覆盖已有文件，QSaveFile 原子写）。
    Q_INVOKABLE bool saveTranscript();

signals:
    void settingsChanged();
    void voiceChanged();
    void transcribeChanged();

private:
    friend Singleton<AsrService>;
    explicit AsrService();

    /// 我们的语音会话状态。Listening = 正在录音；Recognizing = 已停止采集、等 DeepSeek 回结果。
    /// 两段都算 voiceActive，因为 QML 要在这两段里都把"正在聆听/识别中"显示出来，而且要挡掉
    /// 第二次"开始"（第二次点击的含义是停止 / 取消）。
    enum class VoiceState { Idle, Listening, Recognizing };

    // ── 配置（ai.asr 段；Config 只支持顶层键，读写都在 ai 对象里做） ──
    json _readSection() const;
    void _saveSection();
    void _load();

    // ── API Key 解析链 ──
    QString        _apiKey() const;
    QString        _apiKeyFrom(const QString& source) const;
    static QString _trimmed(const json& object, const char* key);

    // ── 接管（需求 10） ──
    bool     _mayTakeOver(QString& reason) const;
    bool     _startSession(QObject* speechManager, bool vendorOwned, QString& reason);
    void     _finishSession();
    void     _abortSession(const QString& reason);
    void     _setVoiceState(VoiceState state, const QString& status);
    QObject* _contextSpeechManager() const;
    /// 语音助手（`ai.speech_assistant`）是否开启。语音助手页的文本只有在这个开关打开时才会被
    /// 送到聊天页；关闭时那一页走的是原厂语音指令解析，我们复现不了，所以那种情况下不接管。
    bool _speechAssistantEnabled() const;
    void _injectResult(QObject* manager, const QString& text);
    void _scheduleStateRecovery(QObject* manager);
    int  _readRecognizing(void* manager);
    bool _pushRecognizing(void* manager, int state);
    void _toast(const QString& text);
    void _installHooks();
    void _removeHooks();

    // YSpeechManager 两个入口的接管回调（返回 true 表示已接管，不要调用 origin）。
    // YRecordCenter::startAsr 走 AudioRecorder.cpp 里已有的 hook，见 onRecordCenterStartAsr()。
    bool _onVendorStartAsrRecord(void* manager);
    bool _onVendorStopAsrRecord(void* manager);

    // ── 音频转文字（需求 11） ──
    void                     _transcribeBegin(const QString& path);
    void                     _transcribeFail(const QString& reason);
    void                     _transcribeCleanup();
    void                     _transcribeDirect(const QString& path);
    void                     _transcribeTranscode(const QString& path);
    void                     _transcribeOnTranscodeFinished();
    void                     _transcribePlanWav(const QString& wavPath);
    void                     _transcribePlanRaw(const QString& pcmPath);
    void                     _transcribePlanRange(qint64 dataOffset, qint64 dataBytes, int rate, int channels);
    void                     _transcribeFinish();
    void                     _transcribeRelease();
    [[nodiscard]] QByteArray _segmentPayload() const;
    void                     _uploadNextSegment();
    void                     _segmentSucceeded(const QString& text);
    void                     _segmentFailed(const QString& reason);
    [[nodiscard]] bool       _transcribeTargetStillCurrent() const;
    void                     _transcribeSetStatus(const QString& status, const QString& progress = QString());

    // ── 状态 ──
    json mCfg;
    bool mEnabled{false};
    int  mTimeoutMs{20000};
    int  mMaxRecordMs{45000};
    bool mHooksInstalled{false};

    QPointer<QQmlContext> mRootContext;
    AsrCapture*           mCapture{nullptr};
    DeepSeekAsr*          mVoiceClient{nullptr};
    DeepSeekAsr*          mFileClient{nullptr};

    // 语音会话
    QPointer<QObject> mSessionManager; ///< 本次会话要注入文本的 YSpeechManager
    QPointer<QObject> mPendingManager; ///< 已停止采集、等识别结果的注入目标
    QTimer            mSessionDeadline;
    QElapsedTimer     mSessionClock;
    VoiceState        mVoiceState{VoiceState::Idle};
    bool              mSessionVendorOwned{false};
    bool              mRecognizingProbed{false};
    void*             mRecognizingGetter{nullptr};
    bool              mSetterProbed{false};
    void*             mRecognizingSetter{nullptr};
    QString           mVoiceStatus;
    QString           mLastToastText; ///< 同一条气泡在短时间内只弹一次（多条入口可能连续失败）
    QElapsedTimer     mLastToastClock;

    // 音频转文字
    QStringList        mSegmentTexts;   ///< 每段识别出来的文本（按顺序拼接）
    QVector<qint64>    mSegmentOffsets; ///< 分段在 WAV 数据块里的偏移
    QVector<qint64>    mSegmentBytes;   ///< 每段的 PCM 字节数（已对齐到采样帧）
    QString            mSegmentSource;  ///< 分段的来源文件（转码产物或原 WAV）
    QString            mSegmentSuffix{QStringLiteral("wav")};
    bool               mSegmentDirectFile{false}; ///< true = 整文件直接当一段上传（Ogg）
    int                mSegmentRate{16000};
    int                mSegmentChannels{1};
    QPointer<QProcess> mTranscode;
    QTimer             mTranscodeDeadline;
    QString            mTranscodeTemp; ///< mpv 转码输出（临时文件）
    QString            mTranscribeTarget;
    QString            mTranscribeSource;
    QString            mTranscribeStatus;
    QString            mTranscribeProgress;
    QString            mTranscribePreview;
    int                mSegmentIndex{0};
    int                mSegmentTotal{0};
    bool               mTranscribeBusy{false};
};

} // namespace mod
