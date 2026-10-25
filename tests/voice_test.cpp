/*
 * Tests of the voice protocol without D-Bus (cmake -DBTSYNCD_TESTS=ON):
 * IMA ADPCM round trip, frame layout, result parsing, WAV reading.
 *   voice_test [WAV...]      run the tests (WAVs: round trip SNR, >= 20 dB)
 *   voice_test --vector      print a test vector for Gadgetbridge's decoder test
 *   voice_test --encode WAV  print the frames the watch would send, one hex line each
 *   voice_test --check-stream HEXFILE WAV
 *                            check audio notifications captured from btsyncd (one hex
 *                            line each) against WAV: whole frames, one session, seq
 *                            0, 1, 2..., and bit-exact with encoding WAV here
 */

#include "voiceprotocol.h"

#include <QCoreApplication>
#include <QFile>
#include <QStringList>
#include <QtMath>
#include <cstdio>

using namespace VoiceProtocol;

static int failures = 0;

#define CHECK(cond) do { if (!(cond)) { fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); failures++; } } while (0)

static double snrDb(const qint16 *a, const qint16 *b, int n)
{
    double sig = 0, err = 0;
    for (int i = 0; i < n; i++) {
        sig += double(a[i]) * a[i];
        err += double(a[i] - b[i]) * (a[i] - b[i]);
    }
    if (err == 0)
        return 200;
    return 10 * std::log10(sig / err);
}

// Encodes in frames as the watch does, decodes each frame from its own header
static double roundTrip(const QByteArray &pcmBytes, int *framesOut = nullptr)
{
    const int frames = pcmBytes.size() / FramePcmBytes;
    const qint16 *pcm = reinterpret_cast<const qint16 *>(pcmBytes.constData());
    QVector<qint16> out(frames * FrameSamples);
    AdpcmState enc;
    for (int f = 0; f < frames; f++) {
        const QByteArray fr = adpcmFrame(7, quint16(f), enc, pcm + f * FrameSamples);
        CHECK(fr.size() == FrameBytes);
        const quint8 *p = reinterpret_cast<const quint8 *>(fr.constData());
        CHECK(p[0] == FrameTypeAdpcm);
        CHECK(p[1] == 7);
        CHECK((p[2] | (p[3] << 8)) == (f & 0xffff));
        AdpcmState dec;
        dec.predictor = qint16(p[4] | (p[5] << 8));
        dec.index = p[6];
        adpcmDecode(dec, p + FrameHeaderBytes, FrameSamples, out.data() + f * FrameSamples);
    }
    if (framesOut)
        *framesOut = frames;
    return snrDb(pcm, out.constData(), frames * FrameSamples);
}

static void testSine()
{
    QByteArray pcm(FramePcmBytes * 50, '\0');
    qint16 *s = reinterpret_cast<qint16 *>(pcm.data());
    for (int i = 0; i < FrameSamples * 50; i++)
        s[i] = qint16(8000 * std::sin(2 * M_PI * 440 * i / SampleRate) + 3000 * std::sin(2 * M_PI * 1700 * i / SampleRate));
    const double snr = roundTrip(pcm);
    printf("sine 440+1700 Hz: SNR %.1f dB\n", snr);
    CHECK(snr > 20);

    // clipping and silence stay in range
    QByteArray loud(FramePcmBytes * 4, '\0');
    qint16 *l = reinterpret_cast<qint16 *>(loud.data());
    for (int i = 0; i < FrameSamples * 4; i++)
        l[i] = (i / 7) % 2 ? 32767 : -32768;
    roundTrip(loud);
    QByteArray quiet(FramePcmBytes * 4, '\0');
    CHECK(roundTrip(quiet) > 100);
}

static void testXml()
{
    CHECK(startXml(3, 42, QString()) == "<voice><st>start</st><sid>3</sid><id>42</id><co>adpcm</co><ra>16000</ra></voice>");
    CHECK(startXml(3, 0, "en-NZ").contains("<la>en-NZ</la>"));
    CHECK(endXml(9, 150) == "<voice><st>end</st><sid>9</sid><n>150</n></voice>");
    CHECK(cancelXml(1) == "<voice><st>cancel</st><sid>1</sid></voice>");
    CHECK(escape("a<b>&\"c'") == "a&lt;b&gt;&amp;&quot;c&apos;");

    Result r = parseResult("<vr><st>ready</st><la>en-US</la></vr>");
    CHECK(r.valid && r.state == "ready" && r.lang == "en-US" && r.sid == -1);
    r = parseResult("<vr><st>unavailable</st><er>no-recognizer</er></vr>");
    CHECK(r.valid && r.state == "unavailable" && r.error == "no-recognizer");
    r = parseResult("<vr><sid>5</sid><pt>I&apos;m on</pt></vr>");
    CHECK(r.valid && r.sid == 5 && r.hasPartial && r.partial == "I'm on" && !r.hasFinal);
    r = parseResult(QString::fromUtf8("<vr><sid>5</sid><ft>Meet me at 7:30 &amp; café \U0001F600</ft></vr>").toUtf8());
    CHECK(r.valid && r.hasFinal && r.final == QString::fromUtf8("Meet me at 7:30 & café \U0001F600"));
    r = parseResult("<vr><sid>5</sid><ft></ft></vr>");
    CHECK(r.valid && r.hasFinal && r.final.isEmpty());
    r = parseResult("<vr><sid>5</sid><er>language</er></vr>");
    CHECK(r.valid && r.error == "language");
    r = parseResult("<vr><sid>x</sid></vr>");
    CHECK(!r.valid);
    r = parseResult("<other/>");
    CHECK(!r.valid);
    CHECK(capabilitiesXml().startsWith("<voice><version>1</version>"));
}

static void testWav(const QString &path)
{
    QByteArray pcm;
    QString err;
    if (!readWav(path, pcm, err)) {
        fprintf(stderr, "FAIL %s: %s\n", qPrintable(path), qPrintable(err));
        failures++;
        return;
    }
    int frames = 0;
    const double snr = roundTrip(pcm, &frames);
    printf("%s: %d frames, SNR %.1f dB\n", qPrintable(path.section('/', -1)), frames, snr);
    CHECK(snr > 20);
}

static void testBadWav()
{
    const QString p = QStringLiteral("/tmp/voice_test_bad.wav");
    QFile f(p);
    if (f.open(QIODevice::WriteOnly)) {
        f.write("RIFF\x24\0\0\0WAVEfmt \x10\0\0\0\x01\0\x02\0\x44\xac\0\0\x10\xb1\x02\0\x04\0\x10\0data\0\0\0\0", 44);
        f.close();
    }
    QByteArray pcm;
    QString err;
    CHECK(!readWav(p, pcm, err) && err.contains("16 kHz mono"));
    CHECK(!readWav("/nonexistent.wav", pcm, err));
    QFile::remove(p);
}

// 640 samples (two frames) of a chirp: the encoded frames and the PCM a
// decoder must produce from each frame's own header
static void printVector()
{
    QVector<qint16> pcm(2 * FrameSamples);
    for (int i = 0; i < pcm.size(); i++)
        pcm[i] = qint16(12000 * std::sin(2 * M_PI * (200 + 3 * i) * i / SampleRate));
    AdpcmState enc;
    for (int f = 0; f < 2; f++) {
        const QByteArray fr = adpcmFrame(1, quint16(f), enc, pcm.constData() + f * FrameSamples);
        printf("frame%d %s\n", f, fr.toHex().constData());
        const quint8 *p = reinterpret_cast<const quint8 *>(fr.constData());
        AdpcmState dec;
        dec.predictor = qint16(p[4] | (p[5] << 8));
        dec.index = p[6];
        QVector<qint16> out(FrameSamples);
        adpcmDecode(dec, p + FrameHeaderBytes, FrameSamples, out.data());
        printf("pcm%d", f);
        for (qint16 v : out)
            printf(" %d", v);
        printf("\n");
    }
}

static int encodeWav(const QString &path)
{
    QByteArray pcm;
    QString err;
    if (!readWav(path, pcm, err)) {
        fprintf(stderr, "%s\n", qPrintable(err));
        return 1;
    }
    AdpcmState enc;
    const int frames = pcm.size() / FramePcmBytes;
    for (int f = 0; f < frames; f++)
        printf("%s\n", adpcmFrame(1, quint16(f), enc,
                                  reinterpret_cast<const qint16 *>(pcm.constData()) + f * FrameSamples).toHex().constData());
    return 0;
}

static int checkStream(const QString &hexPath, const QString &wavPath)
{
    QFile f(hexPath);
    if (!f.open(QIODevice::ReadOnly)) {
        fprintf(stderr, "cannot open %s\n", qPrintable(hexPath));
        return 1;
    }
    QByteArray pcm;
    QString err;
    if (!readWav(wavPath, pcm, err)) {
        fprintf(stderr, "%s\n", qPrintable(err));
        return 1;
    }
    const int rest = pcm.size() % FramePcmBytes;
    if (rest)
        pcm.append(QByteArray(FramePcmBytes - rest, '\0'));
    // what the watch should have sent
    AdpcmState enc;
    QList<QByteArray> want;
    for (int i = 0; i < pcm.size() / FramePcmBytes; i++)
        want << adpcmFrame(0, quint16(i), enc, reinterpret_cast<const qint16 *>(pcm.constData()) + i * FramePcmBytes / 2);

    int notifications = 0, frames = 0, bad = 0, sid = -1, maxPer = 0;
    while (!f.atEnd()) {
        const QByteArray line = f.readLine().trimmed();
        if (line.isEmpty())
            continue;
        const QByteArray v = QByteArray::fromHex(line);
        notifications++;
        if (v.size() % FrameBytes) {
            fprintf(stderr, "notification %d: %d bytes, not whole frames\n", notifications, int(v.size()));
            bad++;
            continue;
        }
        maxPer = qMax(maxPer, int(v.size() / FrameBytes));
        for (int off = 0; off < v.size(); off += FrameBytes, frames++) {
            QByteArray fr = v.mid(off, FrameBytes);
            const quint8 *p = reinterpret_cast<const quint8 *>(fr.constData());
            if (sid < 0)
                sid = p[1];
            const int seq = p[2] | (p[3] << 8);
            if (p[0] != FrameTypeAdpcm || p[1] != sid || seq != (frames & 0xffff)) {
                if (bad++ < 5)
                    fprintf(stderr, "frame %d: type %d sid %d seq %d\n", frames, p[0], p[1], seq);
                continue;
            }
            fr[1] = 0;
            if (frames >= want.size() || fr != want[frames]) {
                if (bad++ < 5)
                    fprintf(stderr, "frame %d differs from the local encoding\n", frames);
            }
        }
    }
    printf("stream: %d notifications, %d frames (%d per notification at most), session %d; WAV %d frames; %d problems\n",
           notifications, frames, maxPer, sid, int(want.size()), bad);
    if (frames != want.size()) {
        printf("FAIL frame count\n");
        return 1;
    }
    printf(bad ? "FAIL\n" : "PASS bit-exact\n");
    return bad ? 1 : 0;
}

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    const QStringList args = app.arguments().mid(1);
    if (args.value(0) == "--vector") {
        printVector();
        return 0;
    }
    if (args.value(0) == "--encode")
        return encodeWav(args.value(1));
    if (args.value(0) == "--check-stream")
        return checkStream(args.value(1), args.value(2));
    testSine();
    testXml();
    testBadWav();
    for (const QString &a : args)
        testWav(a);
    printf(failures ? "%d FAILED\n" : "all passed\n", failures);
    return failures ? 1 : 0;
}
