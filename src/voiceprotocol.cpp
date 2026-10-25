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

#include "voiceprotocol.h"

#include <QFile>
#include <QXmlStreamReader>

namespace VoiceProtocol {

static const int IndexTable[16] = {
    -1, -1, -1, -1, 2, 4, 6, 8,
    -1, -1, -1, -1, 2, 4, 6, 8
};

static const int StepTable[89] = {
    7, 8, 9, 10, 11, 12, 13, 14, 16, 17, 19, 21, 23, 25, 28, 31, 34, 37, 41, 45,
    50, 55, 60, 66, 73, 80, 88, 97, 107, 118, 130, 143, 157, 173, 190, 209, 230,
    253, 279, 307, 337, 371, 408, 449, 494, 544, 598, 658, 724, 796, 876, 963,
    1060, 1166, 1282, 1411, 1552, 1707, 1878, 2066, 2272, 2499, 2749, 3024, 3327,
    3660, 4026, 4428, 4871, 5358, 5894, 6484, 7132, 7845, 8630, 9493, 10442,
    11487, 12635, 13899, 15289, 16818, 18500, 20350, 22385, 24623, 27086, 29794,
    32767
};

static quint8 encodeSample(int &predictor, int &index, int sample)
{
    int step = StepTable[index];
    int diff = sample - predictor;
    quint8 nibble = 0;
    if (diff < 0) {
        nibble = 8;
        diff = -diff;
    }
    // The same sum the decoder forms, so both sides predict alike
    int vpdiff = step >> 3;
    if (diff >= step) { nibble |= 4; diff -= step; vpdiff += step; }
    step >>= 1;
    if (diff >= step) { nibble |= 2; diff -= step; vpdiff += step; }
    step >>= 1;
    if (diff >= step) { nibble |= 1; vpdiff += step; }
    predictor += (nibble & 8) ? -vpdiff : vpdiff;
    predictor = qBound(-32768, predictor, 32767);
    index = qBound(0, index + IndexTable[nibble], 88);
    return nibble;
}

static qint16 decodeSample(int &predictor, int &index, quint8 nibble)
{
    const int step = StepTable[index];
    int vpdiff = step >> 3;
    if (nibble & 4) vpdiff += step;
    if (nibble & 2) vpdiff += step >> 1;
    if (nibble & 1) vpdiff += step >> 2;
    predictor += (nibble & 8) ? -vpdiff : vpdiff;
    predictor = qBound(-32768, predictor, 32767);
    index = qBound(0, index + IndexTable[nibble & 15], 88);
    return qint16(predictor);
}

void adpcmEncode(AdpcmState &state, const qint16 *pcm, int n, quint8 *out)
{
    int predictor = state.predictor, index = qBound(0, int(state.index), 88);
    for (int i = 0; i + 1 < n; i += 2) {
        const quint8 lo = encodeSample(predictor, index, pcm[i]);
        const quint8 hi = encodeSample(predictor, index, pcm[i + 1]);
        out[i / 2] = quint8(lo | (hi << 4));
    }
    state.predictor = qint16(predictor);
    state.index = quint8(index);
}

void adpcmDecode(AdpcmState &state, const quint8 *in, int n, qint16 *pcm)
{
    int predictor = state.predictor, index = qBound(0, int(state.index), 88);
    for (int i = 0; i + 1 < n; i += 2) {
        pcm[i] = decodeSample(predictor, index, in[i / 2] & 15);
        pcm[i + 1] = decodeSample(predictor, index, in[i / 2] >> 4);
    }
    state.predictor = qint16(predictor);
    state.index = quint8(index);
}

QByteArray adpcmFrame(quint8 sid, quint16 seq, AdpcmState &state, const qint16 *pcm)
{
    QByteArray f(FrameBytes, '\0');
    quint8 *p = reinterpret_cast<quint8 *>(f.data());
    p[0] = FrameTypeAdpcm;
    p[1] = sid;
    p[2] = quint8(seq & 0xff);
    p[3] = quint8(seq >> 8);
    p[4] = quint8(quint16(state.predictor) & 0xff);
    p[5] = quint8(quint16(state.predictor) >> 8);
    p[6] = state.index;
    p[7] = 0;
    adpcmEncode(state, pcm, FrameSamples, p + FrameHeaderBytes);
    return f;
}

QString escape(const QString &text)
{
    QString out;
    out.reserve(text.size());
    for (const QChar c : text) {
        switch (c.unicode()) {
        case '&': out += QStringLiteral("&amp;"); break;
        case '<': out += QStringLiteral("&lt;"); break;
        case '>': out += QStringLiteral("&gt;"); break;
        case '"': out += QStringLiteral("&quot;"); break;
        case '\'': out += QStringLiteral("&apos;"); break;
        default:
            if (c.unicode() >= 0x20 || c == QLatin1Char('\n') || c == QLatin1Char('\t'))
                out += c;
        }
    }
    return out;
}

QByteArray startXml(quint8 sid, int phoneId, const QString &lang)
{
    QString x = QStringLiteral("<voice><st>start</st><sid>%1</sid><id>%2</id><co>adpcm</co><ra>%3</ra>")
                    .arg(sid).arg(phoneId).arg(SampleRate);
    if (!lang.isEmpty())
        x += QStringLiteral("<la>") + escape(lang) + QStringLiteral("</la>");
    x += QStringLiteral("</voice>");
    return x.toUtf8();
}

QByteArray endXml(quint8 sid, int frames)
{
    return QStringLiteral("<voice><st>end</st><sid>%1</sid><n>%2</n></voice>").arg(sid).arg(frames).toUtf8();
}

QByteArray cancelXml(quint8 sid)
{
    return QStringLiteral("<voice><st>cancel</st><sid>%1</sid></voice>").arg(sid).toUtf8();
}

Result parseResult(const QByteArray &xml)
{
    Result r;
    QXmlStreamReader reader(xml);
    if (!reader.readNextStartElement() || reader.name() != QLatin1String("vr"))
        return r;
    bool ok = true;
    while (ok && reader.readNextStartElement()) {
        const auto name = reader.name();
        if (name == QLatin1String("sid")) {
            r.sid = reader.readElementText().trimmed().toInt(&ok);
        } else if (name == QLatin1String("st")) {
            r.state = reader.readElementText().trimmed();
        } else if (name == QLatin1String("la")) {
            r.lang = reader.readElementText().trimmed();
        } else if (name == QLatin1String("pt")) {
            r.partial = reader.readElementText();
            r.hasPartial = true;
        } else if (name == QLatin1String("ft")) {
            r.final = reader.readElementText();
            r.hasFinal = true;
        } else if (name == QLatin1String("er")) {
            r.error = reader.readElementText().trimmed();
            if (r.error.isEmpty())
                r.error = QStringLiteral("engine");
        } else {
            reader.skipCurrentElement();
        }
    }
    r.valid = ok && !reader.hasError();
    return r;
}

QByteArray capabilitiesXml()
{
    return QByteArrayLiteral("<voice><version>1</version><codecs>adpcm</codecs>"
                             "<ra>16000</ra><fr>20</fr></voice>");
}

static quint32 le32(const QByteArray &b, int pos)
{
    const uchar *p = reinterpret_cast<const uchar *>(b.constData()) + pos;
    return p[0] | (p[1] << 8) | (p[2] << 16) | (quint32(p[3]) << 24);
}

static quint16 le16(const QByteArray &b, int pos)
{
    const uchar *p = reinterpret_cast<const uchar *>(b.constData()) + pos;
    return quint16(p[0] | (p[1] << 8));
}

bool readWav(const QString &path, QByteArray &pcm, QString &error)
{
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) {
        error = QStringLiteral("cannot open ") + path;
        return false;
    }
    if (f.size() > 16 * 1024 * 1024) {
        error = QStringLiteral("too big");
        return false;
    }
    const QByteArray all = f.readAll();
    if (all.size() < 12 || !all.startsWith("RIFF") || all.mid(8, 4) != "WAVE") {
        error = QStringLiteral("not a WAV file");
        return false;
    }
    int pos = 12, format = -1, channels = 0, rate = 0, bits = 0;
    while (pos + 8 <= all.size()) {
        const QByteArray id = all.mid(pos, 4);
        const quint32 len = le32(all, pos + 4);
        const int body = pos + 8;
        if (id == "fmt " && body + 16 <= all.size()) {
            format = le16(all, body);
            channels = le16(all, body + 2);
            rate = int(le32(all, body + 4));
            bits = le16(all, body + 14);
        } else if (id == "data") {
            if ((format != 1 && format != 0xfffe) || bits != 16 || channels != 1 || rate != SampleRate) {
                error = QStringLiteral("need 16 kHz mono 16-bit PCM (format %1, %2 Hz, %3 ch, %4 bit)")
                            .arg(format).arg(rate).arg(channels).arg(bits);
                return false;
            }
            const int n = int(qMin<qint64>(len, all.size() - body)) & ~1;
            pcm = all.mid(body, n);
            return true;
        }
        if (len > quint32(all.size()))
            break;
        pos = body + int(len) + int(len & 1);
    }
    error = QStringLiteral("no data chunk");
    return false;
}

}
