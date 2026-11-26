/*
 * The assistant relay's actions on the phone (assistantservice.h, CR-49, CR-50), the way the
 * phone's Gadgetbridge drives them, on a private D-Bus:
 *   - every act the apps send (dial, call, sms, clist, cnums, cinfo, cstar, cdel, cphoto, convs,
 *     msgs, msend, open, cvol, wxrefresh) goes to the phone as exactly the frame Gadgetbridge's
 *     own tests record (fixtures/*-requests.frames.txt), in notifications of MTU - 3 bytes
 *   - every result Gadgetbridge records (fixtures/*-results.frames.txt) reaches the app unchanged
 *     (ActResult on the session bus), whatever the pieces it is written in
 *   - how long each act waits (CR-50: wifipw 125 s, msend and sms 40 s, the lists, a contact's
 *     card and photo 20 s, every other act 15 s), in real time for 15 s and 20 s: the timeout
 *     result, the cancel sent to the phone, a late result dropped
 *   - the size limits: 8192 bytes for a question or an act, a reply of 8192 bytes on the wire
 *     (Gadgetbridge's MAX_REPLY_WIRE_BYTES, a 6000-byte contact photo) arrives whole, a longer
 *     one without its end is dropped and the next reply still parses
 *   - btsyncd's own acts (PhoneRelay::relayAct): their own ids, never on the session bus
 *   - reconnections: what waits ends "disconnected", the actions and the MTU are the next
 *     connection's, and a reply cut by the disconnection does not swallow the phone's status
 * Names, numbers and addresses are the invented ones of the fixtures (555 numbers).
 */
#include "assistantprotocol.h"
#include "assistantservice.h"
#include "fixtures.h"
#include "notificationservice.h"
#include "testmain.h"

#include <QDBusConnection>
#include <QElapsedTimer>
#include <QHash>
#include <QRegularExpression>
#include <QTest>

// Everything the phone of FP-G2/FP-G3/FP-G4 lists, and a few more
static const char ALL_ACTS[] = "nav,contacts,sms,call,hangup,dial,clist,cnums,cinfo,cstar,cdel,cphoto,convs,msgs,"
                               "msend,mread,open,cvol,wxrefresh,wifipw,rsvp,cam";

// The XML entities Gadgetbridge and Qt write, undone by hand (not by the parser under test)
static QString unescapeXml(const QByteArray &in)
{
    QString s = QString::fromUtf8(in);
    s.replace(QLatin1String("&quot;"), QLatin1String("\""));
    s.replace(QLatin1String("&apos;"), QLatin1String("'"));
    s.replace(QLatin1String("&lt;"), QLatin1String("<"));
    s.replace(QLatin1String("&gt;"), QLatin1String(">"));
    s.replace(QLatin1String("&amp;"), QLatin1String("&"));
    return s;
}

struct Frame {
    bool valid = false;
    uint id = 0;
    QString json;       // the <js> content, unescaped
};

// <act><id>N</id><js>...</js></act> or <acr>...</acr>
static Frame parseFrame(const QByteArray &frame)
{
    static const QRegularExpression re(QStringLiteral("^<(act|acr)><id>(\\d+)</id><js>(.*)</js></\\1>$"),
                                       QRegularExpression::DotMatchesEverythingOption);
    const QRegularExpressionMatch m = re.match(QString::fromUtf8(frame));
    Frame f;
    if (!m.hasMatch())
        return f;
    f.valid = true;
    f.id = m.captured(2).toUInt();
    f.json = unescapeXml(m.captured(3).toUtf8());
    return f;
}

static QString actOf(const QString &json)
{
    static const QRegularExpression re(QStringLiteral("\"a\":\"([a-z]+)\""));
    return re.match(json).captured(1);
}

class TestRelay : public QObject
{
    Q_OBJECT

private:
    FakePeers *mPeers = nullptr;
    NotificationService *mNotifications = nullptr;
    AssistantService *mRelay = nullptr;
    AssistantRequestsChrc *mRequests = nullptr;
    Characteristic *mReply = nullptr;
    NotificationsFeedbackChrc *mFeedback = nullptr;
    QList<QByteArray> mSent;                 // the notifications of D001, one each
    struct Result { uint id; QString json; qint64 at; };
    QList<Result> mResults;                  // AssistantService::actResult
    QList<QPair<uint, QString>> mBusResults; // ActResult on the session bus
    QElapsedTimer mClock;
    QHash<uint, QString> mRequestJson;       // the fixtures' acts by id

    QByteArray sent() const
    {
        QByteArray all;
        for (const QByteArray &s : mSent)
            all += s;
        return all;
    }

    // The phone's write, in pieces of `piece` bytes as Gadgetbridge splits a long one
    void reply(const QByteArray &xml, int mtu = 247, int piece = 244)
    {
        QVariantMap options;
        if (mtu > 0)
            options.insert("mtu", mtu);
        for (int pos = 0; pos < xml.size(); pos += piece)
            mReply->WriteValue(xml.mid(pos, piece), options);
    }

    void status(int mtu = 247) { reply(QByteArray("<asr><st>ready</st><ac>") + ALL_ACTS + "</ac></asr>", mtu); }

    void connectPhone()
    {
        mNotifications->setLinkConnected(true);
        if (!mFeedback->notifying())
            mFeedback->StartNotify();
        if (!mRequests->notifying())
            mRequests->StartNotify();
        status();
    }

    QList<Result> resultsFor(uint id) const
    {
        QList<Result> out;
        for (const Result &r : mResults)
            if (r.id == id)
                out << r;
        return out;
    }

    void loadRequests(const char *fixture)
    {
        for (const QByteArray &frame : fixtureFrames(fixture)) {
            const Frame f = parseFrame(frame);
            if (f.valid)
                mRequestJson.insert(f.id, f.json);
        }
    }

    static void addFrameRows(const char *fixture)
    {
        int line = 0;
        for (const QByteArray &frame : fixtureFrames(fixture))
            QTest::newRow(qPrintable(QStringLiteral("%1:%2").arg(QLatin1String(fixture)).arg(++line))) << frame;
    }

public slots:
    void onBusActResult(uint id, const QString &json) { mBusResults << qMakePair(id, json); }

private slots:
    void initTestCase()
    {
        if (!haveTestBus())
            QSKIP("no D-Bus session bus (run under testenv.sh --bus)");
        mPeers = new FakePeers;
        QVERIFY(mPeers->startLipstick());
        mNotifications = new NotificationService(0, QDBusConnection::systemBus(), this);
        mRelay = new AssistantService(10, QDBusConnection::systemBus(), mNotifications, this);
        const QList<Characteristic *> chrcs = mRelay->getCharacteristics();
        QCOMPARE(chrcs.size(), 3);
        mRequests = qobject_cast<AssistantRequestsChrc *>(chrcs[0]);
        mReply = chrcs[1];
        QVERIFY(mRequests);
        QCOMPARE(mRequests->getUuid(), QStringLiteral(ASSISTANT_REQUEST_UUID));
        QCOMPARE(mReply->getUuid(), QStringLiteral(ASSISTANT_REPLY_UUID));
        mFeedback = qobject_cast<NotificationsFeedbackChrc *>(mNotifications->getCharacteristics()[1]);
        QVERIFY(mFeedback);
        connect(mRequests, &AssistantRequestsChrc::valueChanged, this, [this] {
            mSent << mRequests->property("Value").toByteArray();
        });
        mClock.start();
        connect(mRelay, &AssistantService::actResult, this, [this](uint id, const QString &json) {
            mResults << Result{id, json, mClock.elapsed()};
        });
        // the apps' view: ActResult on the session bus, from another connection
        QDBusConnection observer = QDBusConnection::connectToBus(QDBusConnection::SessionBus, QStringLiteral("observer"));
        QVERIFY(observer.connect(QDBusConnection::sessionBus().baseService(), QStringLiteral(BTSYNCD_ASSISTANT_PATH),
                                 QStringLiteral("org.asteroidos.btsyncd.Assistant"), QStringLiteral("ActResult"), this,
                                 SLOT(onBusActResult(uint, QString))));
        loadRequests("contacts-requests.frames.txt");
        loadRequests("phone-core-requests.frames.txt");
        loadRequests("messages-fp-g4-requests.frames.txt");
        QCOMPARE(mRequestJson.size(), 21);
        connectPhone();
        QCOMPARE(mRelay->actions().size(), QString::fromLatin1(ALL_ACTS).split(',').size());
    }

    void init()
    {
        if (!mRelay->actions().contains(QStringLiteral("dial")))
            connectPhone();
        mSent.clear();
        mResults.clear();
        mBusResults.clear();
    }

    void cleanupTestCase()
    {
        delete mRelay;
        mRelay = nullptr;
        delete mNotifications;
        mNotifications = nullptr;
        delete mPeers;
    }

    // CR-50
    void actTimeouts_data()
    {
        QTest::addColumn<QString>("act");
        QTest::addColumn<int>("ms");
        QTest::newRow("wifipw") << "wifipw" << 125000;
        for (const char *a : {"msend", "sms"})
            QTest::newRow(a) << a << 40000;
        for (const char *a : {"convs", "msgs", "clist", "cnums", "cinfo", "cphoto"})
            QTest::newRow(a) << a << 20000;
        for (const char *a : {"dial", "call", "hangup", "cstar", "cdel", "open", "cvol", "wxrefresh", "nav", "contacts",
                              "mread", "rsvp", "cam"})
            QTest::newRow(a) << a << 15000;
    }

    void actTimeouts()
    {
        QFETCH(QString, act);
        QFETCH(int, ms);
        QCOMPARE(AssistantProtocol::actTimeoutMs(act), ms);
    }

    // Watch to phone: the frame Gadgetbridge parses in its own tests, byte for byte
    void requestsAreTheFramesGadgetbridgeRecords_data()
    {
        QTest::addColumn<QByteArray>("frame");
        addFrameRows("contacts-requests.frames.txt");
        addFrameRows("phone-core-requests.frames.txt");
        addFrameRows("messages-fp-g4-requests.frames.txt");
    }

    void requestsAreTheFramesGadgetbridgeRecords()
    {
        QFETCH(QByteArray, frame);
        const Frame f = parseFrame(frame);
        QVERIFY(f.valid);
        QVERIFY(mRelay->actions().contains(actOf(f.json)));
        // through the session bus API, as the apps call it
        QDBusMessage call = QDBusMessage::createMethodCall(QDBusConnection::sessionBus().baseService(),
                                                           QStringLiteral(BTSYNCD_ASSISTANT_PATH),
                                                           QStringLiteral("org.asteroidos.btsyncd.Assistant"),
                                                           QStringLiteral("Act"));
        call << f.id << f.json;
        const QDBusMessage r = callAsPeer(call);
        QCOMPARE(r.type(), QDBusMessage::ReplyMessage);
        QCOMPARE(r.arguments().value(0).toBool(), true);
        QCOMPARE(sent(), frame);
        for (const QByteArray &s : std::as_const(mSent))
            QVERIFY(s.size() <= 244);       // MTU 247 - 3
        // the same id again while it waits: refused
        QVERIFY(!mRelay->act(f.id, f.json.toUtf8()));
        mSent.clear();
        QVERIFY(mRelay->cancelAct(f.id));
        QCOMPARE(sent(), "<act><id>" + QByteArray::number(f.id) + "</id><cancel/></act>");
        QVERIFY(!mRelay->cancelAct(f.id));
    }

    // Phone to watch: the phone's result is the app's, unchanged; in the smallest pieces too
    void resultsReachTheAppsUnchanged_data()
    {
        QTest::addColumn<QByteArray>("frame");
        addFrameRows("contacts-results.frames.txt");
        addFrameRows("phone-core-results.frames.txt");
        addFrameRows("messages-fp-g4-results.frames.txt");
    }

    void resultsReachTheAppsUnchanged()
    {
        QFETCH(QByteArray, frame);
        const Frame f = parseFrame(frame);
        QVERIFY(f.valid);
        QVERIFY2(mRequestJson.contains(f.id), "a result whose act is not in the request fixtures");
        QVERIFY(mRelay->act(f.id, mRequestJson.value(f.id).toUtf8()));
        reply(frame, 23, 20);
        QCOMPARE(resultsFor(f.id).size(), 1);
        QCOMPARE(resultsFor(f.id).value(0).json, f.json);
        QTRY_COMPARE(mBusResults.size(), 1);
        QCOMPARE(mBusResults.value(0).first, f.id);
        QCOMPARE(mBusResults.value(0).second, f.json);
        // a second result for it is nobody's
        reply(frame);
        QTest::qWait(50);
        QCOMPARE(resultsFor(f.id).size(), 1);
        QCOMPARE(mBusResults.size(), 1);
    }

    // The smallest MTU (no mtu option, BlueZ < 5.62: 23) and the largest notification (509)
    void requestsInPiecesOfTheMtu()
    {
        status(0);      // a write without the mtu option keeps the last MTU
        status(23);
        const QByteArray frame = fixtureFrames("contacts-requests.frames.txt").value(5);   // open contact, 136 bytes
        const Frame f = parseFrame(frame);
        QVERIFY(mRelay->act(f.id, f.json.toUtf8()));
        QVERIFY(mSent.size() >= 6);
        for (const QByteArray &s : std::as_const(mSent))
            QVERIFY(s.size() <= 20);
        QCOMPARE(sent(), frame);
        QVERIFY(mRelay->cancelAct(f.id));
        status(1000);
        mSent.clear();
        const QByteArray big = "{\"a\":\"open\",\"w\":\"mail\",\"to\":\"" + QByteArray(2000, 'x') + "\"}";
        QVERIFY(mRelay->act(90, big));
        QCOMPARE(mSent.value(0).size(), 509);
        QCOMPARE(sent(), AssistantProtocol::actXml(90, big));
        QVERIFY(mRelay->cancelAct(90));
        status();
    }

    // CR-50 in real time: 15 s and 20 s, the timeout result, the cancel the phone gets
    void timeoutsInRealTime()
    {
        const qint64 t0 = mClock.elapsed();
        QVERIFY(mRelay->act(21, mRequestJson.value(21).toUtf8()));     // dial
        QVERIFY(mRelay->act(43, mRequestJson.value(43).toUtf8()));     // cstar
        QVERIFY(mRelay->act(42, mRequestJson.value(42).toUtf8()));     // cinfo
        QVERIFY(mRelay->act(45, mRequestJson.value(45).toUtf8()));     // cphoto
        QVERIFY(mRelay->act(23, mRequestJson.value(23).toUtf8()));     // sms: 40 s
        // and one of btsyncd's own (the Weather app's refresh, CR-40)
        QStringList done;
        QVERIFY(mRelay->relayAct("{\"a\":\"wxrefresh\"}", [&done](const QString &json) { done << json; }));
        mSent.clear();
        const QString timeout = QStringLiteral("{\"ok\":false,\"er\":\"timeout\"}");
        // a coarse timer may fire up to 5 % early: nothing before 14 s
        QTest::qWait(int(14000 - (mClock.elapsed() - t0)));
        QVERIFY(mResults.isEmpty());
        QVERIFY(done.isEmpty());
        QTRY_VERIFY_WITH_TIMEOUT(resultsFor(21).size() == 1 && resultsFor(43).size() == 1 && done.size() == 1, 3000);
        for (uint id : {21u, 43u}) {
            const Result r = resultsFor(id).value(0);
            QCOMPARE(r.json, timeout);
            QVERIFY2(r.at - t0 >= 14250 && r.at - t0 < 16500, qPrintable(QString::number(r.at - t0)));
            QVERIFY(sent().contains("<act><id>" + QByteArray::number(id) + "</id><cancel/></act>"));
        }
        QCOMPARE(done.value(0), timeout);
        // 20 s for the card and the photo
        QVERIFY(resultsFor(42).isEmpty() && resultsFor(45).isEmpty());
        QTest::qWait(int(18900 - (mClock.elapsed() - t0)));
        QVERIFY(resultsFor(42).isEmpty() && resultsFor(45).isEmpty());
        QTRY_VERIFY_WITH_TIMEOUT(resultsFor(42).size() == 1 && resultsFor(45).size() == 1, 3500);
        for (uint id : {42u, 45u}) {
            const Result r = resultsFor(id).value(0);
            QCOMPARE(r.json, timeout);
            QVERIFY2(r.at - t0 >= 19000 && r.at - t0 < 21500, qPrintable(QString::number(r.at - t0)));
            QVERIFY(sent().contains("<act><id>" + QByteArray::number(id) + "</id><cancel/></act>"));
        }
        // the text message still waits (40 s); a late result of a timed-out act is dropped
        QVERIFY(resultsFor(23).isEmpty());
        reply("<acr><id>21</id><js>{&quot;ok&quot;:true}</js></acr>");
        QTest::qWait(50);
        QCOMPARE(resultsFor(21).size(), 1);
        QVERIFY(mRelay->cancelAct(23));
        // every timeout went to the apps; btsyncd's own act did not
        QTRY_COMPARE(mBusResults.size(), 4);
        for (const auto &r : std::as_const(mBusResults))
            QVERIFY(r.first < AssistantService::InternalIdBase);
        QCOMPARE(done.size(), 1);
    }

    void sizeLimits()
    {
        // a question or an act of up to 8192 bytes goes out, a longer one is refused
        const QByteArray head = "{\"a\":\"open\",\"w\":\"mail\",\"to\":\"";
        QByteArray json = head + QByteArray(8192 - head.size() - 2, 'm') + "\"}";
        QCOMPARE(json.size(), 8192);
        QVERIFY(mRelay->act(91, json));
        QVERIFY(mRelay->cancelAct(91));
        QVERIFY(!mRelay->act(92, json.insert(head.size(), 'm')));
        QCOMPARE(json.size(), 8193);
        QVERIFY(mRelay->available());
        QVERIFY(mRelay->ask(93, QByteArray(8192, 'q')));
        QVERIFY(mRelay->cancel(93));
        QVERIFY(!mRelay->ask(94, QByteArray(8193, 'q')));

        // a contact photo at Gadgetbridge's limit (AsteroidOSContactPhoto.MAX_JPEG_BYTES 6000:
        // 8000 bytes of base64), in 20-byte writes (MTU 23)
        mSent.clear();
        QVERIFY(mRelay->act(45, mRequestJson.value(45).toUtf8()));
        const QByteArray photo(8000, 'A');
        const QByteArray frame = "<acr><id>45</id><js>{&quot;ok&quot;:true,&quot;j&quot;:&quot;" + photo
                                 + "&quot;}</js></acr>";
        QVERIFY(frame.size() <= 8192);
        reply(frame, 23, 20);
        QCOMPARE(resultsFor(45).size(), 1);
        QCOMPARE(resultsFor(45).value(0).json, "{\"ok\":true,\"j\":\"" + QString::fromLatin1(photo) + "\"}");

        // exactly 8192 bytes on the wire (MAX_REPLY_WIRE_BYTES)
        QVERIFY(mRelay->act(24, mRequestJson.value(24).toUtf8()));
        QByteArray exact = "<acr><id>24</id><js>{&quot;ok&quot;:true,&quot;c&quot;:[],&quot;h&quot;:&quot;";
        const QByteArray tail = "&quot;}</js></acr>";
        exact += QByteArray(8192 - exact.size() - tail.size(), 'h') + tail;
        QCOMPARE(exact.size(), 8192);
        reply(exact, 23, 20);
        QCOMPARE(resultsFor(24).size(), 1);
        QVERIFY(resultsFor(24).value(0).json.startsWith(QLatin1String("{\"ok\":true,\"c\":[],\"h\":\"hhh")));

        // more than 8192 bytes without its end: dropped (the act still waits), the rest that
        // follows is no reply, and the next reply parses
        QVERIFY(mRelay->act(41, mRequestJson.value(41).toUtf8()));
        QByteArray tooLong = "<acr><id>41</id><js>{&quot;ok&quot;:true,&quot;h&quot;:&quot;" + QByteArray(8400, 'h')
                             + "&quot;}</js></acr>";
        reply(tooLong, 23, 20);
        QTest::qWait(50);
        QVERIFY(resultsFor(41).isEmpty());
        reply("<acr><id>41</id><js>{&quot;ok&quot;:true,&quot;same&quot;:true}</js></acr>");
        QCOMPARE(resultsFor(41).size(), 1);
        QCOMPARE(resultsFor(41).value(0).json, QStringLiteral("{\"ok\":true,\"same\":true}"));
        QVERIFY(mRelay->actions().contains(QStringLiteral("clist")));
    }

    // btsyncd's own acts (the call volume of CallAudio, the Weather refresh): ids of their own,
    // their result to the caller only
    void ownActsAreNotForTheSessionBus()
    {
        QString got;
        int calls = 0;
        QVERIFY(mRelay->relayHas(QStringLiteral("cvol")));
        QVERIFY(mRelay->relayAct("{\"a\":\"cvol\",\"d\":-1}", [&](const QString &json) { got = json; calls++; }));
        const Frame f = parseFrame(sent());
        QVERIFY(f.valid);
        QVERIFY(f.id >= AssistantService::RelayIdBase);
        QCOMPARE(f.json, QStringLiteral("{\"a\":\"cvol\",\"d\":-1}"));
        reply("<acr><id>" + QByteArray::number(f.id) + "</id><js>{&quot;ok&quot;:true,&quot;v&quot;:2,&quot;max&quot;:5}</js></acr>");
        QCOMPARE(calls, 1);
        QCOMPARE(got, QStringLiteral("{\"ok\":true,\"v\":2,\"max\":5}"));
        QTest::qWait(100);
        // (the bus may still bring the results of the case before)
        for (const auto &r : std::as_const(mBusResults))
            QVERIFY(r.first < AssistantService::InternalIdBase);
        // an act the phone does not list is not sent, and its callback never runs
        status();
        reply("<asr><st>ready</st><ac>nav,sms</ac></asr>");
        mSent.clear();
        QVERIFY(!mRelay->relayHas(QStringLiteral("cvol")));
        QVERIFY(!mRelay->relayAct("{\"a\":\"cvol\"}", [&](const QString &) { calls++; }));
        QVERIFY(mSent.isEmpty());
        QCOMPARE(calls, 1);
        status();
    }

    // The link drops: what waits ends at once, the phone's next connection starts afresh
    void reconnection()
    {
        QVERIFY(mRelay->act(24, mRequestJson.value(24).toUtf8()));
        QVERIFY(mRelay->act(62, mRequestJson.value(62).toUtf8()));
        mNotifications->setLinkConnected(false);
        QCOMPARE(resultsFor(24).size(), 1);
        QCOMPARE(resultsFor(62).size(), 1);
        QCOMPARE(resultsFor(24).value(0).json, QStringLiteral("{\"ok\":false,\"er\":\"disconnected\"}"));
        QVERIFY(mRelay->actions().isEmpty());
        QVERIFY(!mRelay->available());
        QVERIFY(!mRelay->act(25, mRequestJson.value(25).toUtf8()));
        // back: nothing until the phone says what it does, with the MTU of this connection
        mNotifications->setLinkConnected(true);
        QVERIFY(mRelay->actions().isEmpty());
        reply(QByteArray("<asr><st>ready</st><ac>") + ALL_ACTS + "</ac></asr>", 0);   // no mtu option
        QVERIFY(mRelay->actions().contains(QStringLiteral("cnums")));
        mSent.clear();
        QVERIFY(mRelay->act(25, mRequestJson.value(25).toUtf8()));
        for (const QByteArray &s : std::as_const(mSent))
            QVERIFY(s.size() <= 20);        // the 23 of a new link, not the 247 of the last one
        QVERIFY(mRelay->cancelAct(25));
        // unsubscribed (the phone's Gadgetbridge stopped): the same
        status();
        QVERIFY(mRelay->act(26, mRequestJson.value(26).toUtf8()));
        mRequests->StopNotify();
        QCOMPARE(resultsFor(26).value(0).json, QStringLiteral("{\"ok\":false,\"er\":\"disconnected\"}"));
        QVERIFY(mRelay->actions().isEmpty());
        mRequests->StartNotify();
        status();
        QVERIFY(mRelay->actions().contains(QStringLiteral("cvol")));
    }

    // A result cut by the disconnection (the phone walked away while it wrote a long contact
    // list): the phone writes its status first at the next connection, and the relay must take it
    void replyCutByADisconnection()
    {
        const QByteArray result = fixtureFrames("contacts-results.frames.txt").value(0);
        QVERIFY(result.size() > 300);
        QVERIFY(mRelay->act(41, mRequestJson.value(41).toUtf8()));
        reply(result.left(244));                    // one write of several
        mNotifications->setLinkConnected(false);
        QCOMPARE(resultsFor(41).value(0).json, QStringLiteral("{\"ok\":false,\"er\":\"disconnected\"}"));
        mNotifications->setLinkConnected(true);
        status();
        QVERIFY2(mRelay->actions().contains(QStringLiteral("clist")),
                 "the status of the new connection was taken for the rest of the cut reply");
        QVERIFY(mRelay->available());
        // and the relay works: the list again, whole
        QVERIFY(mRelay->act(41, mRequestJson.value(41).toUtf8()));
        reply(result);
        QCOMPARE(resultsFor(41).size(), 2);
        QCOMPARE(resultsFor(41).value(1).json, parseFrame(result).json);

        // the same when the phone unsubscribes in the middle (Gadgetbridge restarted)
        QVERIFY(mRelay->act(41, mRequestJson.value(41).toUtf8()));
        reply(result.left(100));
        mRequests->StopNotify();
        mRequests->StartNotify();
        status();
        QVERIFY(mRelay->actions().contains(QStringLiteral("clist")));
    }
};

BTSYNCD_TEST_MAIN(TestRelay)
#include "tst_relay.moc"
