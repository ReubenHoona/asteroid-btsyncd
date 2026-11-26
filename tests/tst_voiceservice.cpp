/*
 * Voice replies (PROTOCOL-voice.md) end to end without a phone: the voice
 * service is driven through its characteristics as BlueZ would (result
 * writes, notify subscriptions) with a WAV file as the microphone, and the
 * events / audio frames it notifies are decoded and checked:
 *   - availability from the phone's <st>ready|unavailable</st>, the MTU check
 *   - a session: start event, ADPCM frames (header, seq, whole frames per
 *     notification), end event, partial / final text, sending the reply
 *     through the notification service (feedback to the phone)
 *   - stop / cancel / phone errors / timeouts / unsubscribe, session ids
 *   - result write framing (split, trailing bytes, junk, overflow)
 *   - VoiceProtocol edge cases beyond voice_test (WAV chunks, ADPCM limits)
 */
#include "voiceservice.h"
#include "voiceprotocol.h"
#include "voicesource.h"
#include "notificationservice.h"
#include "mdconfitem.h"
#include "testmain.h"

#include <QDataStream>
#include <QFile>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTest>
#include <QtMath>

using namespace VoiceProtocol;

static QByteArray le16(quint16 v) { return QByteArray(1, char(v & 0xff)) + char(v >> 8); }
static QByteArray le32(quint32 v) { return le16(quint16(v & 0xffff)) + le16(quint16(v >> 16)); }

// A WAV file: "fmt " (format, channels, rate, bits), optional chunks
// before it, and the data
static QByteArray wav(const QByteArray &pcm, quint16 format = 1, quint16 channels = 1, quint32 rate = 16000,
                      quint16 bits = 16, const QByteArray &before = QByteArray(), qint64 dataLen = -1)
{
    QByteArray fmt = "fmt " + le32(16) + le16(format) + le16(channels) + le32(rate)
                     + le32(rate * channels * bits / 8) + le16(quint16(channels * bits / 8)) + le16(bits);
    QByteArray data = "data" + le32(quint32(dataLen < 0 ? pcm.size() : dataLen)) + pcm;
    QByteArray body = "WAVE" + before + fmt + data;
    return "RIFF" + le32(quint32(body.size())) + body;
}

static QByteArray sine(int samples, double freq = 440, double amp = 8000)
{
    QByteArray pcm(samples * 2, '\0');
    qint16 *p = reinterpret_cast<qint16 *>(pcm.data());
    for (int i = 0; i < samples; i++)
        p[i] = qint16(amp * qSin(2 * M_PI * freq * i / SampleRate));
    return pcm;
}

class TestVoiceService : public QObject
{
    Q_OBJECT

private:
    QTemporaryDir mDir;
    FakePeers *mPeers = nullptr;
    NotificationService *mNotifications = nullptr;
    VoiceService *mVoice = nullptr;
    VoiceNotifyChrc *mEvent = nullptr;
    VoiceNotifyChrc *mAudio = nullptr;
    Characteristic *mResult = nullptr;
    Characteristic *mCaps = nullptr;
    QList<QByteArray> mEvents;
    QList<QByteArray> mAudioChunks;
    QList<QByteArray> mFeedback;
    QString mShortWav, mLongWav;

    void result(const QByteArray &xml, int mtu = 0)
    {
        QVariantMap options;
        if (mtu > 0)
            options.insert("mtu", mtu);
        mResult->WriteValue(xml, options);
    }

    QByteArray events() const
    {
        QByteArray all;
        for (const QByteArray &e : mEvents)
            all += e;
        return all;
    }

    void ready(int mtu = 247) { result("<vr><st>ready</st><la>en-US</la></vr>", mtu); }

    QString file(const QString &name, const QByteArray &content)
    {
        const QString path = mDir.filePath(name);
        QFile f(path);
        if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate))
            return QString();
        f.write(content);
        return path;
    }

private slots:
    void initTestCase()
    {
        QVERIFY(mDir.isValid());
        if (haveTestBus()) {
            mPeers = new FakePeers;
            QVERIFY(mPeers->startLipstick());
            mNotifications = new NotificationService(0, QDBusConnection::systemBus(), this);
            NotificationsFeedbackChrc *fb = qobject_cast<NotificationsFeedbackChrc *>(mNotifications->getCharacteristics()[1]);
            connect(fb, &NotificationsFeedbackChrc::valueChanged, this, [this, fb] {
                mFeedback << fb->property("Value").toByteArray();
            });
        }
        mVoice = new VoiceService(7, QDBusConnection::systemBus(), mNotifications, this);
        const QList<Characteristic *> chrcs = mVoice->getCharacteristics();
        QCOMPARE(chrcs.size(), 4);
        mEvent = qobject_cast<VoiceNotifyChrc *>(chrcs[0]);
        mAudio = qobject_cast<VoiceNotifyChrc *>(chrcs[1]);
        mResult = chrcs[2];
        mCaps = chrcs[3];
        QVERIFY(mEvent && mAudio);
        QCOMPARE(mEvent->getUuid(), QStringLiteral(VOICE_EVENT_UUID));
        QCOMPARE(mAudio->getUuid(), QStringLiteral(VOICE_AUDIO_UUID));
        QCOMPARE(mResult->getUuid(), QStringLiteral(VOICE_RESULT_UUID));
        QCOMPARE(mCaps->getUuid(), QStringLiteral(VOICE_CAP_UUID));
        connect(mEvent, &VoiceNotifyChrc::valueChanged, this, [this] { mEvents << mEvent->property("Value").toByteArray(); });
        connect(mAudio, &VoiceNotifyChrc::valueChanged, this, [this] { mAudioChunks << mAudio->property("Value").toByteArray(); });

        mShortWav = file("short.wav", wav(sine(10 * FrameSamples)));          // 200 ms, 10 frames
        mLongWav = file("long.wav", wav(sine(250 * FrameSamples)));           // 5 s
        QVERIFY(!mShortWav.isEmpty() && !mLongWav.isEmpty());
    }

    void init()
    {
        mVoice->cancel();
        mVoice->cancel();   // twice: from Done / Recognising to Idle
        // the phone says nothing yet
        mVoice->notifyChanged(mAudio, false);
        mEvents.clear();
        mAudioChunks.clear();
        mFeedback.clear();
        MDConfItem::reset();
        qunsetenv("BTSYNCD_VOICE_SOURCE");
    }

    void cleanupTestCase()
    {
        delete mVoice;
        delete mNotifications;
        delete mPeers;
    }

    void capabilities()
    {
        const QByteArray caps = capabilitiesXml();
        QCOMPARE(mCaps->ReadValue({}), caps);
        QCOMPARE(mCaps->ReadValue({{"offset", 7}}), caps.mid(7));
        QCOMPARE(caps, QByteArray("<voice><version>1</version><codecs>adpcm</codecs><ra>16000</ra><fr>20</fr></voice>"));
    }

    void noPhoneNoSession()
    {
        QSignalSpy failed(mVoice, &VoiceService::failed);
        QVERIFY(!mVoice->available());
        QCOMPARE(mVoice->start(1, mShortWav), 0u);
        QCOMPARE(mVoice->state(), QStringLiteral("idle"));
        // after Start()'s reply
        QCOMPARE(failed.size(), 0);
        QTRY_COMPARE(failed.size(), 1);
        QCOMPARE(failed[0][0].toString(), QStringLiteral("no-phone"));
        QVERIFY(mEvents.isEmpty());
    }

    void phoneSaysUnavailable()
    {
        QSignalSpy failed(mVoice, &VoiceService::failed);
        ready();
        QVERIFY(mVoice->available());
        result("<vr><st>unavailable</st><er>no-recognizer</er></vr>");
        QVERIFY(!mVoice->available());
        QCOMPARE(mVoice->start(1, mShortWav), 0u);
        QTRY_COMPARE(failed.size(), 1);
        QCOMPARE(failed[0][0].toString(), QStringLiteral("no-recognizer"));
        // without a reason
        result("<vr><st>unavailable</st></vr>");
        mVoice->start(1, mShortWav);
        QTRY_COMPARE(failed.size(), 2);
        QCOMPARE(failed[1][0].toString(), QStringLiteral("unavailable"));
        // ready again
        ready();
        QVERIFY(mVoice->available());
        // an unknown state changes nothing
        result("<vr><st>sleepy</st></vr>");
        QVERIFY(mVoice->available());
    }

    void subscribingMakesItAvailable()
    {
        // An older phone that never says ready, but subscribes to the audio
        QVERIFY(!mVoice->available());
        mAudio->StartNotify();
        QVERIFY(mAudio->notifying());
        QVERIFY(mVoice->available());
        mAudio->StopNotify();
        QVERIFY(!mAudio->notifying());
        QVERIFY(!mVoice->available());
        mEvent->StartNotify();
        QVERIFY(!mVoice->available());      // the audio is what counts
        mEvent->StopNotify();
    }

    void mtuTooSmallForAFrame()
    {
        QSignalSpy failed(mVoice, &VoiceService::failed);
        ready(100);     // 97 bytes < one 168-byte frame
        QCOMPARE(mVoice->start(1, mShortWav), 0u);
        QTRY_COMPARE(failed.size(), 1);
        QCOMPARE(failed[0][0].toString(), QStringLiteral("mtu"));
    }

    void badSources_data()
    {
        QTest::addColumn<QString>("source");
        QTest::newRow("missing file") << QStringLiteral("/nonexistent/voice.wav");
        QTest::newRow("microphone without PulseAudio") << QStringLiteral("mic");
    }

    void badSources()
    {
        QFETCH(QString, source);
        QSignalSpy failed(mVoice, &VoiceService::failed);
        ready();
        QCOMPARE(mVoice->start(1, source), 0u);
        QTRY_COMPARE(failed.size(), 1);
        QCOMPARE(failed[0][0].toString(), QStringLiteral("source"));
        QCOMPARE(mVoice->state(), QStringLiteral("idle"));
        QVERIFY(mEvents.isEmpty());
    }

    void configuredSource()
    {
        ready();
        // dconf, then the environment, then the microphone
        MDConfItem("/org/asteroidos/btsyncd/voice-source").set(mShortWav);
        QVERIFY(mVoice->start(1, QString()) != 0);
        mVoice->cancel();
        MDConfItem::reset();
        qputenv("BTSYNCD_VOICE_SOURCE", mShortWav.toUtf8());
        QVERIFY(mVoice->start(1, QString()) != 0);
        mVoice->cancel();
        qunsetenv("BTSYNCD_VOICE_SOURCE");
        QSignalSpy failed(mVoice, &VoiceService::failed);
        QCOMPARE(mVoice->start(1, QString()), 0u);      // "mic", built without PulseAudio
        QTRY_COMPARE(failed.size(), 1);
    }

    void wholeSession()
    {
        QSignalSpy partial(mVoice, &VoiceService::partialText);
        QSignalSpy final(mVoice, &VoiceService::finalText);
        MDConfItem("/org/asteroidos/btsyncd/voice-language").set("de-DE");
        uint lipId = 0;
        if (mNotifications) {
            // the notification the user replies to, from the phone (id 41)
            mNotifications->getCharacteristics()[0]->WriteValue(
                "<insert><id>41</id><an>Chat</an><su>Sam</su><bo>Coming?</bo><ac>reply</ac></insert>", {{"mtu", 247}});
            lipId = mNotifications->mKnownAndroidNotifs.value(41);
            QVERIFY(lipId);
        }
        ready(400);     // 397 bytes: 2 frames per notification
        const uint sid = mVoice->start(lipId, mShortWav);
        QVERIFY(sid > 0 && sid < 256);
        QCOMPARE(mVoice->state(), QStringLiteral("listening"));
        QCOMPARE(events(), startXml(quint8(sid), mNotifications ? 41 : 0, "de-DE"));

        // The WAV runs out after 200 ms: the session ends by itself
        QTRY_COMPARE_WITH_TIMEOUT(mVoice->state(), QStringLiteral("recognising"), 3000);
        QCOMPARE(events(), startXml(quint8(sid), mNotifications ? 41 : 0, "de-DE") + endXml(quint8(sid), 10));

        // Audio: whole frames, 2 a notification, seq 0..9, decodable alone
        QCOMPARE(mAudioChunks.size(), 5);
        QByteArray pcm(10 * FramePcmBytes, '\0');
        int seq = 0;
        for (const QByteArray &chunk : std::as_const(mAudioChunks)) {
            QCOMPARE(chunk.size(), 2 * FrameBytes);
            for (int f = 0; f < 2; f++, seq++) {
                const quint8 *p = reinterpret_cast<const quint8 *>(chunk.constData()) + f * FrameBytes;
                QCOMPARE(p[0], FrameTypeAdpcm);
                QCOMPARE(uint(p[1]), sid);
                QCOMPARE(p[2] | (p[3] << 8), seq);
                QCOMPARE(p[7], quint8(0));
                AdpcmState dec;
                dec.predictor = qint16(p[4] | (p[5] << 8));
                dec.index = p[6];
                adpcmDecode(dec, p + FrameHeaderBytes, FrameSamples,
                            reinterpret_cast<qint16 *>(pcm.data()) + seq * FrameSamples);
            }
        }
        // the decoded audio is the sine (ADPCM: > 20 dB)
        const QByteArray ref = sine(10 * FrameSamples);
        const qint16 *a = reinterpret_cast<const qint16 *>(ref.constData());
        const qint16 *b = reinterpret_cast<const qint16 *>(pcm.constData());
        double sig = 0, err = 0;
        for (int i = 0; i < 10 * FrameSamples; i++) {
            sig += double(a[i]) * a[i];
            err += double(a[i] - b[i]) * (a[i] - b[i]);
        }
        QVERIFY2(10 * std::log10(sig / err) > 20, qPrintable(QString::number(10 * std::log10(sig / err))));

        // Text from the phone, split over writes as the phone may
        const QByteArray s = QByteArray::number(sid);
        result("<vr><sid>" + s + "</sid><pt>see you</pt></vr>");
        result("<vr><sid>" + s + "</sid><pt>see you</pt></vr>");     // the same again: one signal
        result("<vr><sid>" + s + "</sid><pt></pt></vr>");            // empty: none
        QCOMPARE(partial.size(), 1);
        QCOMPARE(partial[0][0].toString(), QStringLiteral("see you"));
        result("<vr><sid>" + s + "</sid><ft> see you ");
        result("soon &amp; bye </ft></vr>");
        QCOMPARE(final.size(), 1);
        QCOMPARE(final[0][0].toString(), QStringLiteral("see you soon & bye"));
        QCOMPARE(mVoice->state(), QStringLiteral("done"));

        // Late results are for a finished session
        result("<vr><sid>" + s + "</sid><pt>late</pt></vr>");
        QCOMPARE(partial.size(), 1);

        if (!mNotifications) {
            QVERIFY(!mVoice->send());
            return;
        }
        VoiceDBusApi *api = mVoice->findChild<VoiceDBusApi *>();
        QVERIFY(api);
        QCOMPARE(api->State(), QStringLiteral("done"));
        QVERIFY(api->Send());
        QCOMPARE(api->State(), QStringLiteral("idle"));
        QByteArray fb;
        (void)QTest::qWaitFor([&] { fb.clear(); for (const QByteArray &c : std::as_const(mFeedback)) fb += c; return fb.endsWith("</action>"); }, 3000);
        QCOMPARE(fb, QByteArray("<action><id>41</id><ty>reply</ty><tx>see you soon &amp; bye</tx></action>"));
        QVERIFY(!api->Send());      // once
    }

    void stopEarlyAndTimeout()
    {
        QSignalSpy final(mVoice, &VoiceService::finalText);
        QSignalSpy failed(mVoice, &VoiceService::failed);
        ready(247);     // 244 bytes: 1 frame per notification
        VoiceDBusApi *api = mVoice->findChild<VoiceDBusApi *>();
        QVERIFY(api->Available());
        const uint sid = api->Start(0, mLongWav);
        QVERIFY(sid);
        QTest::qWait(150);
        QVERIFY(api->Stop());
        QVERIFY(!api->Stop());      // only while listening
        QCOMPARE(api->State(), QStringLiteral("recognising"));
        const int frames = mAudioChunks.size();
        QVERIFY(frames >= 3 && frames < 250);
        for (const QByteArray &c : std::as_const(mAudioChunks))
            QCOMPARE(c.size(), FrameBytes);
        QVERIFY(events().endsWith(endXml(quint8(sid), frames)));
        QTest::qWait(100);
        QCOMPARE(mAudioChunks.size(), frames);      // the source stopped

        // No final text: the last partial is used
        result("<vr><sid>" + QByteArray::number(sid) + "</sid><pt>almost there</pt></vr>");
        QVERIFY(QMetaObject::invokeMethod(mVoice, "onResultTimeout"));
        QCOMPARE(final.size(), 1);
        QCOMPARE(final[0][0].toString(), QStringLiteral("almost there"));
        QCOMPARE(mVoice->state(), QStringLiteral("done"));

        // and without any text: a timeout
        mVoice->cancel();
        const uint sid2 = mVoice->start(0, mLongWav);
        QCOMPARE(sid2, sid % 255 + 1);
        QVERIFY(mVoice->stop());
        QVERIFY(QMetaObject::invokeMethod(mVoice, "onResultTimeout"));
        QCOMPARE(failed.size(), 1);
        QCOMPARE(failed[0][0].toString(), QStringLiteral("timeout"));
        QCOMPARE(mVoice->state(), QStringLiteral("idle"));
        // a stray timeout later does nothing
        QVERIFY(QMetaObject::invokeMethod(mVoice, "onResultTimeout"));
        QCOMPARE(failed.size(), 1);
    }

    void cancelSession()
    {
        ready();
        VoiceDBusApi *api = mVoice->findChild<VoiceDBusApi *>();
        QVERIFY(!api->Cancel());    // nothing to cancel
        const uint sid = api->Start(0, mLongWav);
        QVERIFY(sid);
        QTest::qWait(60);
        mEvents.clear();
        QVERIFY(api->Cancel());
        QCOMPARE(events(), cancelXml(quint8(sid)));
        QCOMPARE(api->State(), QStringLiteral("idle"));
        const int frames = mAudioChunks.size();
        QTest::qWait(100);
        QCOMPARE(mAudioChunks.size(), frames);
        // A new start while one listens cancels it first
        const uint a = mVoice->start(0, mLongWav);
        mEvents.clear();
        const uint b = mVoice->start(0, mLongWav);
        QCOMPARE(b, a % 255 + 1);
        QVERIFY(events().startsWith(cancelXml(quint8(a))));
        QVERIFY(events().contains("<st>start</st><sid>" + QByteArray::number(b) + "</sid>"));
    }

    void phoneErrors_data()
    {
        QTest::addColumn<QByteArray>("inner");
        QTest::addColumn<QString>("code");
        QTest::newRow("network") << QByteArray("<er>network</er>") << "network";
        QTest::newRow("empty error") << QByteArray("<er> </er>") << "engine";
        QTest::newRow("empty final") << QByteArray("<ft>  </ft>") << "no-match";
    }

    void phoneErrors()
    {
        QFETCH(QByteArray, inner);
        QFETCH(QString, code);
        QSignalSpy failed(mVoice, &VoiceService::failed);
        ready();
        const uint sid = mVoice->start(0, mLongWav);
        QVERIFY(sid);
        result("<vr><sid>" + QByteArray::number(sid) + "</sid>" + inner + "</vr>");
        QCOMPARE(failed.size(), 1);
        QCOMPARE(failed[0][0].toString(), code);
        QCOMPARE(mVoice->state(), QStringLiteral("idle"));
        const int frames = mAudioChunks.size();
        QTest::qWait(80);
        QCOMPARE(mAudioChunks.size(), frames);
    }

    void resultsForAnotherSessionAreIgnored()
    {
        QSignalSpy partial(mVoice, &VoiceService::partialText);
        QSignalSpy failed(mVoice, &VoiceService::failed);
        ready();
        const uint sid = mVoice->start(0, mLongWav);
        result("<vr><sid>" + QByteArray::number(sid % 255 + 1) + "</sid><pt>other</pt></vr>");
        result("<vr><pt>no session id</pt></vr>");
        result("<vr><sid>" + QByteArray::number(sid % 255 + 1) + "</sid><er>x</er></vr>");
        QCOMPARE(partial.size(), 0);
        QCOMPARE(failed.size(), 0);
        QCOMPARE(mVoice->state(), QStringLiteral("listening"));
    }

    void resultFraming()
    {
        QSignalSpy partial(mVoice, &VoiceService::partialText);
        // Two results in pieces, the second starting in the first's last write
        result("<vr><st>re");
        QVERIFY(!mVoice->available());
        result("ady</st></vr><vr><st>unava", 185);
        QVERIFY(mVoice->available());
        result("ilable</st><er>language</er></vr>");
        QVERIFY(!mVoice->available());
        ready();
        // Junk is dropped (not valid XML), the next result still parses
        result("<vr><sid>abc</sid></vr>");
        result("<notvr/></vr>");
        QVERIFY(mVoice->available());
        // Over 4 KiB without </vr>: dropped, then parsing starts afresh
        result("<vr><st>unavailable</st><er>" + QByteArray(3000, 'x'));
        result(QByteArray(1200, 'y'));
        result("</er></vr>");   // the end of a dropped result: junk now
        QVERIFY(mVoice->available());
        const uint sid = mVoice->start(0, mLongWav);
        result("<vr><sid>" + QByteArray::number(sid) + "</sid><pt>still here</pt></vr>");
        QCOMPARE(partial.size(), 1);
    }

    void unsubscribeEndsTheSession()
    {
        QSignalSpy failed(mVoice, &VoiceService::failed);
        ready();
        QVERIFY(mVoice->start(0, mLongWav));
        QTest::qWait(60);
        mAudio->StopNotify();       // the phone went away
        QCOMPARE(failed.size(), 1);
        QCOMPARE(failed[0][0].toString(), QStringLiteral("disconnected"));
        QCOMPARE(mVoice->state(), QStringLiteral("idle"));
        QVERIFY(!mVoice->available());
        // and while recognising
        ready();
        QVERIFY(mVoice->start(0, mLongWav));
        QVERIFY(mVoice->stop());
        mEvent->StopNotify();
        QCOMPARE(failed.size(), 2);
        // idle: nothing to fail
        ready();
        mEvent->StopNotify();
        QCOMPARE(failed.size(), 2);
    }

    void sourceFailure()
    {
        QSignalSpy failed(mVoice, &VoiceService::failed);
        ready();
        QVERIFY(mVoice->start(0, mLongWav));
        mEvents.clear();
        // as MicSource reports a PulseAudio error
        QVERIFY(QMetaObject::invokeMethod(mVoice, "onSourceFailed", Q_ARG(QString, QStringLiteral("PulseAudio: test"))));
        QCOMPARE(failed.size(), 1);
        QCOMPARE(failed[0][0].toString(), QStringLiteral("source"));
        QVERIFY(events().contains("<st>cancel</st>"));
        // when not listening it is only logged
        QVERIFY(QMetaObject::invokeMethod(mVoice, "onSourceFailed", Q_ARG(QString, QStringLiteral("late"))));
        QCOMPARE(failed.size(), 1);
    }

    void wrongFrameSizeIsIgnored()
    {
        ready();
        QVERIFY(mVoice->start(0, mLongWav));
        QTest::qWait(30);
        mVoice->cancel();
        QVERIFY(mVoice->start(0, mLongWav));
        const int before = mAudioChunks.size();
        QVERIFY(QMetaObject::invokeMethod(mVoice, "onFrame", Q_ARG(QByteArray, QByteArray(100, 'x'))));
        mVoice->cancel();
        QVERIFY(QMetaObject::invokeMethod(mVoice, "onFrame", Q_ARG(QByteArray, QByteArray(FramePcmBytes, '\0'))));
        QCOMPARE(mAudioChunks.size(), before);
    }

    void sessionIdsWrapWithoutZero()
    {
        ready();
        QSet<uint> seen;
        uint prev = mVoice->start(0, mShortWav);
        mVoice->cancel();
        for (int i = 0; i < 300; i++) {
            const uint sid = mVoice->start(0, mShortWav);
            mVoice->cancel();
            QVERIFY(sid >= 1 && sid <= 255);
            QCOMPARE(sid, prev % 255 + 1);
            seen.insert(sid);
            prev = sid;
        }
        QCOMPARE(seen.size(), 255);
    }

    void eventsAreSplitAtTheMtu()
    {
        ready(23);      // not enough for audio, but the MTU is what events use too
        result("<vr><st>ready</st></vr>", 200);
        MDConfItem("/org/asteroidos/btsyncd/voice-language").set(QString(300, QChar('a')));
        const uint sid = mVoice->start(0, mLongWav);
        QVERIFY(sid);
        QVERIFY(mEvents.size() >= 2);
        for (const QByteArray &e : std::as_const(mEvents))
            QVERIFY(e.size() <= 197);
        QCOMPARE(events(), startXml(quint8(sid), 0, QString(300, QChar('a'))));
    }

    void wavSourceDirect()
    {
        // Odd sample counts are padded to whole frames
        const QString odd = file("odd.wav", wav(sine(FrameSamples + 10)));
        WavSource src(odd, nullptr);
        QList<QByteArray> frames;
        connect(&src, &VoiceSource::frame, this, [&frames](const QByteArray &f) { frames << f; });
        QSignalSpy finished(&src, &VoiceSource::finished);
        QString err;
        QVERIFY(src.start(err));
        QTRY_COMPARE(finished.size(), 1);
        QCOMPARE(frames.size(), 2);
        QCOMPARE(frames[1].size(), FramePcmBytes);
        QCOMPARE(frames[1].mid(20), QByteArray(FramePcmBytes - 20, '\0'));
        src.stop();
        // A source that is not a WAV
        WavSource bad(file("bad.wav", "RIFF....WAVX"), nullptr);
        QVERIFY(!bad.start(err));
        QCOMPARE(err, QStringLiteral("not a WAV file"));
        // VoiceSource::create picks the kind
        QObject parent;
        QVERIFY(qobject_cast<MicSource *>(VoiceSource::create(QString(), &parent)));
        QVERIFY(qobject_cast<MicSource *>(VoiceSource::create("mic", &parent)));
        QVERIFY(qobject_cast<WavSource *>(VoiceSource::create(odd, &parent)));
        MicSource mic(nullptr);
        QVERIFY(!mic.start(err));
        QCOMPARE(err, QStringLiteral("built without PulseAudio"));
        mic.stop();
    }

    /* VoiceProtocol beyond voice_test */

    void wavChunks()
    {
        QByteArray pcm, err;
        QString error;
        const QByteArray samples = sine(100);
        // A LIST chunk (odd length, padded) before "fmt "
        const QByteArray list = "LIST" + le32(5) + "INFOx" + QByteArray(1, '\0');
        QVERIFY(readWav(file("list.wav", wav(samples, 1, 1, 16000, 16, list)), pcm, error));
        QCOMPARE(pcm, samples);
        // WAVE_FORMAT_EXTENSIBLE
        QVERIFY(readWav(file("ext.wav", wav(samples, 0xfffe)), pcm, error));
        QCOMPARE(pcm, samples);
        // A data length past the end of the file (a recording cut short)
        QVERIFY(readWav(file("cut.wav", wav(samples, 1, 1, 16000, 16, QByteArray(), 100000)), pcm, error));
        QCOMPARE(pcm, samples);
        // An odd data length: whole samples only
        QVERIFY(readWav(file("oddlen.wav", wav(samples + "z", 1, 1, 16000, 16)), pcm, error));
        QCOMPARE(pcm, samples);
    }

    void badWavs_data()
    {
        QTest::addColumn<QByteArray>("content");
        QTest::addColumn<QString>("error");
        const QByteArray s = sine(10);
        QTest::newRow("short") << QByteArray("RIFF") << "not a WAV file";
        QTest::newRow("not RIFF") << ("RIFX" + wav(s).mid(4)) << "not a WAV file";
        QTest::newRow("stereo") << wav(s, 1, 2) << "need 16 kHz mono 16-bit PCM (format 1, 16000 Hz, 2 ch, 16 bit)";
        QTest::newRow("44.1 kHz") << wav(s, 1, 1, 44100) << "need 16 kHz mono 16-bit PCM (format 1, 44100 Hz, 1 ch, 16 bit)";
        QTest::newRow("8 bit") << wav(s, 1, 1, 16000, 8) << "need 16 kHz mono 16-bit PCM (format 1, 16000 Hz, 1 ch, 8 bit)";
        QTest::newRow("float") << wav(s, 3) << "need 16 kHz mono 16-bit PCM (format 3, 16000 Hz, 1 ch, 16 bit)";
        QTest::newRow("data before fmt") << ("RIFF" + le32(100) + "WAVE" + "data" + le32(4) + "abcd")
                                         << "need 16 kHz mono 16-bit PCM (format -1, 0 Hz, 0 ch, 0 bit)";
        QTest::newRow("no data") << ("RIFF" + le32(100) + "WAVE" + wav(s).mid(12, 24)) << "no data chunk";
        QTest::newRow("chunk past the end") << ("RIFF" + le32(100) + "WAVE" + "JUNK" + le32(0x7fffffff) + "xx")
                                            << "no data chunk";
    }

    void badWavs()
    {
        QFETCH(QByteArray, content);
        QFETCH(QString, error);
        QByteArray pcm;
        QString err;
        QVERIFY(!readWav(file("bad-case.wav", content), pcm, err));
        QCOMPARE(err, error);
    }

    void tooBigWav()
    {
        QFile f(mDir.filePath("big.wav"));
        QVERIFY(f.open(QIODevice::WriteOnly));
        QVERIFY(f.resize(16 * 1024 * 1024 + 1));
        f.close();
        QByteArray pcm;
        QString err;
        QVERIFY(!readWav(f.fileName(), pcm, err));
        QCOMPARE(err, QStringLiteral("too big"));
    }

    void escapeDropsControlCharacters()
    {
        QCOMPARE(escape(QString::fromUtf8("a\x01" "b\tc\nd\x1f" "e")), QString::fromUtf8("ab\tc\nde"));
        QCOMPARE(startXml(1, 2, "x<y"), QByteArray("<voice><st>start</st><sid>1</sid><id>2</id><co>adpcm</co>"
                                                    "<ra>16000</ra><la>x&lt;y</la></voice>"));
    }

    void adpcmEncoderAndDecoderAgree()
    {
        // The encoder predicts exactly what the decoder reconstructs, so
        // after every frame both states are equal (no drift), also at full
        // scale where predictor and step index saturate
        QList<qint16> signal;
        quint32 seed = 12345;
        for (int i = 0; i < 40 * FrameSamples; i++) {
            seed = seed * 1103515245u + 12345u;
            const int phase = i / FrameSamples;
            qint16 v;
            if (phase % 4 == 0)
                v = (i & 1) ? 32767 : -32768;           // full-scale square
            else if (phase % 4 == 1)
                v = qint16(int(seed >> 16) - 32768);    // noise
            else if (phase % 4 == 2)
                v = 0;                                  // silence after loud
            else
                v = qint16(20000 * qSin(i * 0.05));
            signal << v;
        }
        AdpcmState enc, dec;
        QByteArray coded(FrameSamples / 2, '\0');
        QList<qint16> out(FrameSamples);
        int maxIndex = 0;
        for (int f = 0; f < 40; f++) {
            adpcmEncode(enc, signal.constData() + f * FrameSamples, FrameSamples, reinterpret_cast<quint8 *>(coded.data()));
            adpcmDecode(dec, reinterpret_cast<const quint8 *>(coded.constData()), FrameSamples, out.data());
            QCOMPARE(dec.predictor, enc.predictor);
            QCOMPARE(dec.index, enc.index);
            QVERIFY(enc.index <= 88);
            maxIndex = qMax(maxIndex, int(enc.index));
        }
        QCOMPARE(maxIndex, 88);
        // A state from a corrupt header is clamped, not read past the table
        AdpcmState bad;
        bad.index = 200;
        qint16 pcm[2] = {1000, -1000};
        quint8 byte = 0;
        adpcmEncode(bad, pcm, 2, &byte);
        QVERIFY(bad.index <= 88);
        AdpcmState bad2;
        bad2.index = 255;
        adpcmDecode(bad2, &byte, 2, pcm);
        QVERIFY(bad2.index <= 88);
        // An odd count leaves the last sample out
        AdpcmState s;
        quint8 outByte = 0xaa;
        adpcmEncode(s, pcm, 1, &outByte);
        QCOMPARE(outByte, quint8(0xaa));
    }
};

BTSYNCD_TEST_MAIN(TestVoiceService)
#include "tst_voiceservice.moc"
