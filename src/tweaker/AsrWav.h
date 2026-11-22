// SPDX-License-Identifier: GPL-3.0-only
/*
 * Copyright (C) 2022-present, PenUniverse.
 * This file is part of the PenMods open source project.
 */

#pragma once

#include <QByteArray>
#include <QtGlobal>

/// WAV 读写的小工具（纯函数，不碰 Qt 音频/网络）。
///
/// DeepSeek 语音识别只接受 16-bit PCM WAV 或 Ogg Opus，所以录音和转码两条路都必须能
/// 自己拼出合规的 RIFF 头，并且能按"数据块边界"把长音频切开 —— 单次请求上限是
/// 120 秒 / 8 MiB，切片只能落在采样帧边界上，切半个采样会让整段音频变成噪音。
namespace mod::asr {

/// 从 RIFF 头里解析出来的格式信息。
struct WavFormat {
    int    sampleRate    = 0;
    int    channels      = 0;
    int    bitsPerSample = 0;
    qint64 dataOffset    = 0; ///< 音频数据在文件中的起始偏移
    qint64 dataBytes     = 0; ///< 头部声明的音频数据长度（可能不可信，调用方按文件大小修正）

    [[nodiscard]] bool valid() const { return sampleRate > 0 && channels > 0 && bitsPerSample > 0 && dataBytes != 0; }

    /// 每秒 PCM 字节数：把"秒"换算成"字节"的唯一依据。
    [[nodiscard]] qint64 bytesPerSecond() const {
        return static_cast<qint64>(sampleRate) * channels * (bitsPerSample / 8);
    }

    /// 一个采样帧的字节数（切片必须对齐到它）。
    [[nodiscard]] qint64 frameBytes() const { return static_cast<qint64>(channels) * (bitsPerSample / 8); }
};

/// 解析 WAV 头（head 至少包含到 data 块头，64 KiB 足够）。
///
/// 只有整数 PCM（fmt tag 1，以及 WAVE_FORMAT_EXTENSIBLE 里 PCM 子格式）才算可用；
/// 浮点 / 压缩 WAV 直接返回 false，让调用方交给 mpv 转码 —— 把 float32 或 24-bit
/// 的数据当 16-bit 上传，接口只会回一个语无伦次的识别结果。
bool parseWavHeader(const QByteArray& head, WavFormat& out);

/// 从交错 PCM 里抽出一个声道。ASR 只识别左声道，双声道源固定取 channel = 0。
QByteArray extractChannel(const QByteArray& interleaved, int channels, int channel);

/// 给裸 PCM 套一个 44 字节 RIFF 头。
QByteArray buildWav(const QByteArray& pcm, int sampleRate, int channels, int bitsPerSample = 16);

} // namespace mod::asr
