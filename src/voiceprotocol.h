/*
 * Copyright (C) 2026 Reuben Horner <reubarb.scons@gmail.com>
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program. If not, see <http://www.gnu.org/licenses/>.
 */

#ifndef VOICEPROTOCOL_H
#define VOICEPROTOCOL_H

#include <QByteArray>
#include <QString>
#include <QtGlobal>

/*
 * Voice replies: the watch streams microphone audio to the phone, the phone
 * recognises it and sends the text back (PROTOCOL-voice.md). Everything here
 * is Qt Core only, so the tests run without D-Bus.
 */
namespace VoiceProtocol {

// 16 kHz mono, 20 ms frames of 320 samples
const int SampleRate = 16000;
const int FrameSamples = 320;
const int FramePcmBytes = FrameSamples * 2;
// type, sid, seq (LE u16), predictor (LE s16), step index, reserved, 160 bytes
const int FrameHeaderBytes = 8;
const int FrameBytes = FrameHeaderBytes + FrameSamples / 2;
const quint8 FrameTypeAdpcm = 1;

// IMA ADPCM state: the predicted sample and the step table index
struct AdpcmState {
    qint16 predictor = 0;
    quint8 index = 0;
};

// n samples (even) -> n / 2 bytes, first sample in the low nibble
void adpcmEncode(AdpcmState &state, const qint16 *pcm, int n, quint8 *out);
void adpcmDecode(AdpcmState &state, const quint8 *in, int n, qint16 *pcm);

// One audio frame: the header carries the encoder state before the frame,
// so a receiver can decode any frame on its own (a lost frame costs 20 ms)
QByteArray adpcmFrame(quint8 sid, quint16 seq, AdpcmState &state, const qint16 *pcm);

QString escape(const QString &text);

// Watch -> phone events
QByteArray startXml(quint8 sid, int phoneId, const QString &lang);
QByteArray endXml(quint8 sid, int frames);
QByteArray cancelXml(quint8 sid);

// Phone -> watch, joined until </vr>:
//   <vr><st>ready</st><la>en-US</la></vr>       voice replies available
//   <vr><st>unavailable</st><er>CODE</er></vr>  not available, and why
//   <vr><sid>N</sid><pt>TEXT</pt></vr>           partial text
//   <vr><sid>N</sid><ft>TEXT</ft></vr>           final text
//   <vr><sid>N</sid><er>CODE</er></vr>           the session failed
struct Result {
    bool valid = false;
    int sid = -1;
    QString state;      // ready | unavailable (no session)
    QString lang;
    QString partial;
    QString final;
    QString error;
    bool hasPartial = false;
    bool hasFinal = false;
};
Result parseResult(const QByteArray &xml);

// Phone -> watch capabilities read: what this watch sends
QByteArray capabilitiesXml();

// PCM16 mono WAV -> raw samples (little endian bytes); false with a reason
bool readWav(const QString &path, QByteArray &pcm, QString &error);

}

#endif // VOICEPROTOCOL_H
