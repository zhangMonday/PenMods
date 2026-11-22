// SPDX-License-Identifier: GPL-3.0-only
/*
 * Copyright (C) 2022-present, PenUniverse.
 * This file is part of the PenMods open source project.
 */

#include "tweaker/AsrWav.h"

#include <QtEndian>

namespace mod::asr {

namespace {

constexpr int kChunkHeaderBytes = 8;

quint16 readU16(const QByteArray& data, int offset) {
    return qFromLittleEndian<quint16>(reinterpret_cast<const uchar*>(data.constData()) + offset);
}

quint32 readU32(const QByteArray& data, int offset) {
    return qFromLittleEndian<quint32>(reinterpret_cast<const uchar*>(data.constData()) + offset);
}

void appendU16(QByteArray& out, quint16 value) {
    uchar buffer[2];
    qToLittleEndian<quint16>(value, buffer);
    out.append(reinterpret_cast<const char*>(buffer), 2);
}

void appendU32(QByteArray& out, quint32 value) {
    uchar buffer[4];
    qToLittleEndian<quint32>(value, buffer);
    out.append(reinterpret_cast<const char*>(buffer), 4);
}

} // namespace

bool parseWavHeader(const QByteArray& head, WavFormat& out) {
    out = WavFormat{};
    if (head.size() < 12) return false;
    if (head.mid(0, 4) != QByteArrayLiteral("RIFF") || head.mid(8, 4) != QByteArrayLiteral("WAVE")) return false;

    bool sawFormat = false;
    int  pos       = 12;
    while (pos + kChunkHeaderBytes <= head.size()) {
        const QByteArray id   = head.mid(pos, 4);
        const quint32    size = readU32(head, pos + 4);
        const int        body = pos + kChunkHeaderBytes;
        // 长度字段是 u32；大于 1 GiB 的单个块只可能是坏数据（传入的 head 也不会那么长）
        if (size > 0x40000000u) return false;

        if (id == QByteArrayLiteral("fmt ")) {
            if (body + 16 > head.size()) return false;
            quint16 audioFormat = readU16(head, body);
            out.channels        = readU16(head, body + 2);
            out.sampleRate      = static_cast<int>(readU32(head, body + 4));
            out.bitsPerSample   = readU16(head, body + 14);
            if (audioFormat == 0xFFFE) {
                // WAVE_FORMAT_EXTENSIBLE：真正的小格式 tag 在 SubFormat GUID 的头两个字节
                if (body + 26 > head.size()) return false;
                audioFormat = readU16(head, body + 24);
            }
            if (audioFormat != 1) return false; // 浮点 / 压缩，交给 mpv
            if (out.channels <= 0 || out.channels > 8) return false;
            if (out.bitsPerSample <= 0 || out.bitsPerSample % 8 != 0) return false;
            if (out.sampleRate <= 0 || out.sampleRate > 192000) return false;
            sawFormat = true;
        } else if (id == QByteArrayLiteral("data")) {
            if (!sawFormat) return false;
            out.dataOffset = body;
            out.dataBytes  = static_cast<qint64>(size);
            // 流式写入的 WAV 常把 data 长度留成 0 / 0xFFFFFFFF；这里标记为"未知"，由调用方按文件大小修正
            if (out.dataBytes <= 0) out.dataBytes = -1;
            return true;
        }

        pos = body + static_cast<int>(size) + static_cast<int>(size & 1u); // 块按偶数字节对齐
    }
    return false;
}

QByteArray extractChannel(const QByteArray& interleaved, int channels, int channel) {
    if (channels <= 1 || channel < 0 || channel >= channels) {
        QByteArray mono = interleaved;
        if (mono.size() & 1) mono.chop(1); // 丢掉可能被截断的半个采样
        return mono;
    }

    const int  frameBytes = channels * 2;
    const int  frames     = static_cast<int>(interleaved.size() / frameBytes);
    QByteArray mono;
    mono.resize(frames * 2);
    const char* source = interleaved.constData();
    char*       target = mono.data();
    for (int frame = 0; frame < frames; ++frame) {
        const int offset      = frame * frameBytes + channel * 2;
        target[frame * 2]     = source[offset];
        target[frame * 2 + 1] = source[offset + 1];
    }
    return mono;
}

QByteArray buildWav(const QByteArray& pcm, int sampleRate, int channels, int bitsPerSample) {
    const int  blockAlign = channels * (bitsPerSample / 8);
    QByteArray out;
    out.reserve(44 + pcm.size());
    out.append(QByteArrayLiteral("RIFF"));
    appendU32(out, static_cast<quint32>(36 + pcm.size()));
    out.append(QByteArrayLiteral("WAVE"));
    out.append(QByteArrayLiteral("fmt "));
    appendU32(out, 16);                                            // fmt 块长度
    appendU16(out, 1);                                             // PCM
    appendU16(out, static_cast<quint16>(channels));                //
    appendU32(out, static_cast<quint32>(sampleRate));              //
    appendU32(out, static_cast<quint32>(sampleRate * blockAlign)); // 字节率
    appendU16(out, static_cast<quint16>(blockAlign));              //
    appendU16(out, static_cast<quint16>(bitsPerSample));           //
    out.append(QByteArrayLiteral("data"));
    appendU32(out, static_cast<quint32>(pcm.size()));
    out.append(pcm);
    return out;
}

} // namespace mod::asr
