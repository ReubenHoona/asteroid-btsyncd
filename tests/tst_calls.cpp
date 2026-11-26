/*
 * The phone's call cards on the watch (notificationservice.cpp with callcontrol.h and
 * callobserver.h), against a fake lipstick on a private D-Bus:
 *   - a ringing call (CR-52: answer,reject,silence,reply): its buttons, the ring vibration of a
 *     call on the phone, its arrival time kept, the call log told (CallObserver::callCard)
 *   - answer, decline and end: on the watch's hands-free link when CallControl takes them (no
 *     feedback to the phone), else to the phone; "Answer on phone" (answer-phone) always to the
 *     phone as answer (CR-53); a typed reply to a ringing call goes to the phone (CR-53)
 *   - the call in progress (end) updates the same card quietly; the phone's removal ends it
 *     (CallObserver::callGone) and a dismissed card after a button is not a dismiss
 *   - the missed-call card of Gadgetbridge's fixture (missed_call, <wh>): CallObserver::missedCallCard
 *   - reconnections: a call card the phone does not post again within the grace is closed
 * Names and numbers are invented (555 numbers).
 */
#include "callcontrol.h"
#include "callobserver.h"
#include "fixtures.h"
#include "notificationservice.h"
#include "testmain.h"

#include <QDateTime>
#include <QTest>

class FakeCalls : public CallControl, public CallObserver
{
public:
    bool answers = false, hangsUp = false;
    int answerCalls = 0, hangUpCalls = 0;
    QStringList log;

    bool answerOnWatch() override { answerCalls++; return answers; }
    bool hangUp() override { hangUpCalls++; return hangsUp; }
    void callCard(int phoneId, const QString &summary, const QString &body, const QStringList &phoneActions,
                  const QString &packageName) override
    {
        log << QStringLiteral("card %1 %2|%3|%4|%5").arg(phoneId).arg(summary, body, phoneActions.join(','), packageName);
    }
    void callAction(int phoneId, const QString &action) override
    {
        log << QStringLiteral("action %1 %2").arg(phoneId).arg(action);
    }
    void callGone(int phoneId) override { log << QStringLiteral("gone %1").arg(phoneId); }
    void missedCallCard(int phoneId, const QString &summary, const QString &body, qint64 whenSeconds) override
    {
        log << QStringLiteral("missed %1 %2|%3|%4").arg(phoneId).arg(summary, body).arg(whenSeconds);
    }
};

class TestCalls : public QObject
{
    Q_OBJECT

private:
    FakePeers *mPeers = nullptr;
    FakeLipstick *mLip = nullptr;
    NotificationService *mService = nullptr;
    Characteristic *mUpdate = nullptr;
    NotificationsFeedbackChrc *mFeedback = nullptr;
    FakeCalls mCalls;
    QList<QByteArray> mChunks;

    QByteArray feedback() const
    {
        QByteArray all;
        for (const QByteArray &c : mChunks)
            all += c;
        return all;
    }

    void write(const QByteArray &xml) { mUpdate->WriteValue(xml, {{"mtu", 247}}); }

    static QByteArray ringing(int id, const QByteArray &extra = QByteArray())
    {
        return "<insert><id>" + QByteArray::number(id) + "</id><vb>ringtone</vb><an>Phone</an>"
               "<ai>ios-call-outline</ai><su>Ana Example</su><bo>Incoming call · Mobile</bo>"
               "<ac>answer,reject,silence,reply</ac><pr>5</pr><ca>call</ca>" + extra + "</insert>";
    }

    uint lipstickId(int phoneId) const { return mService->mKnownAndroidNotifs.value(phoneId, 0); }

    LipNotification lastNotify() const
    {
        const QList<LipNotification> calls = mLip->notifyCalls();
        return calls.isEmpty() ? LipNotification() : calls.last();
    }

    // A button of the card, as lipstick reports it; the feedback it gave (none: empty)
    QByteArray press(int phoneId, const QString &key)
    {
        mChunks.clear();
        mLip->emitAction(lipstickId(phoneId), key);
        (void)QTest::qWaitFor([this] { return feedback().endsWith("</action>"); }, 400);
        return feedback();
    }

private slots:
    void initTestCase()
    {
        if (!haveTestBus())
            QSKIP("no D-Bus session bus (run under testenv.sh --bus)");
        mPeers = new FakePeers;
        mLip = mPeers->lipstick();
        QVERIFY(mPeers->startLipstick());
        mService = new NotificationService(0, QDBusConnection::systemBus(), this);
        mService->setCallControl(&mCalls);
        mService->setCallObserver(&mCalls);
        mUpdate = mService->getCharacteristics()[0];
        mFeedback = qobject_cast<NotificationsFeedbackChrc *>(mService->getCharacteristics()[1]);
        QVERIFY(mFeedback);
        connect(mFeedback, &NotificationsFeedbackChrc::valueChanged, this, [this] {
            mChunks << mFeedback->property("Value").toByteArray();
        });
        mFeedback->StartNotify();
        QVERIFY(mService->phoneConnected());
    }

    void init()
    {
        mChunks.clear();
        mLip->reset();
        mCalls.log.clear();
        mCalls.answers = mCalls.hangsUp = false;
        mCalls.answerCalls = mCalls.hangUpCalls = 0;
    }

    void cleanupTestCase()
    {
        delete mService;
        mService = nullptr;
        delete mPeers;
    }

    void ringingCall()
    {
        write(ringing(51, "<sd>1</sd>"));
        const LipNotification n = lastNotify();
        QVERIFY(lipstickId(51));
        QCOMPARE(n.actions, (QStringList{"answer", "Answer", "reject", "Decline", "silence", "Silence", "reply", "Reply"}));
        QCOMPARE(n.summary, QStringLiteral("Ana Example"));
        // a call ringing on the phone only vibrates on the watch (f92d67f)
        QCOMPARE(n.hints.value("x-nemo-feedback").toString(), QStringLiteral("eos_phone_ringing"));
        QVERIFY(QDateTime::fromString(n.hints.value("x-nemo-timestamp").toString(), Qt::ISODate).isValid());
        QCOMPARE(n.hints.value("x-asteroid-category").toString(), QStringLiteral("call"));
        QCOMPARE(mCalls.log, QStringList{"card 51 Ana Example|Incoming call · Mobile|answer,reject,silence,reply|"});
        // the phone posts it again while it rings: the same card, the same time, the log told again
        const QString stamp = n.hints.value("x-nemo-timestamp").toString();
        QTest::qWait(1100);
        write(ringing(51, "<sd>1</sd>"));
        QCOMPARE(lastNotify().id, lipstickId(51));
        QCOMPARE(lastNotify().hints.value("x-nemo-timestamp").toString(), stamp);
        QCOMPARE(mCalls.log.size(), 2);
    }

    void answerOnTheWatch()
    {
        write(ringing(52));
        mCalls.answers = true;
        QCOMPARE(press(52, "answer"), QByteArray());
        QCOMPARE(mCalls.answerCalls, 1);
        QVERIFY(mCalls.log.contains("action 52 answer"));
        // lipstick closes the card after the button: that is not a dismiss
        mLip->emitClosed(lipstickId(52), 2);
        QTest::qWait(200);
        QVERIFY(mChunks.isEmpty());
    }

    void answerOnThePhone()
    {
        // no hands-free link (or "Answer calls on watch" off): the phone answers
        write(ringing(53));
        QCOMPARE(press(53, "answer"), QByteArray("<action><id>53</id><ty>answer</ty></action>"));
        QCOMPARE(mCalls.answerCalls, 1);
        // More > Call > Answer on phone (CR-53): the phone's answer, whatever the link
        write(ringing(54));
        mCalls.answers = true;
        QCOMPARE(press(54, "answer-phone"), QByteArray("<action><id>54</id><ty>answer</ty></action>"));
        QCOMPARE(mCalls.answerCalls, 1);
        QVERIFY(mCalls.log.contains("action 54 answer-phone"));
    }

    void declineAndEnd()
    {
        write(ringing(55));
        mCalls.hangsUp = true;
        QCOMPARE(press(55, "reject"), QByteArray());
        QCOMPARE(mCalls.hangUpCalls, 1);
        mCalls.hangsUp = false;
        write(ringing(56));
        QCOMPARE(press(56, "reject"), QByteArray("<action><id>56</id><ty>reject</ty></action>"));
        // the call in progress: the same card, quietly, with End call
        write("<insert><id>57</id><an>Phone</an><su>Ana Example</su><bo>Ongoing call</bo><ac>end</ac><og>1</og>"
              "<ca>call</ca></insert>");
        const uint card = lipstickId(57);
        QVERIFY(card);
        QCOMPARE(lastNotify().actions, (QStringList{"end", "End call"}));
        QCOMPARE(press(57, "end"), QByteArray("<action><id>57</id><ty>end</ty></action>"));
        mCalls.hangsUp = true;
        write("<insert><id>58</id><an>Phone</an><su>Ana Example</su><bo>Ongoing call</bo><ac>end</ac><og>1</og>"
              "<ca>call</ca></insert>");
        QCOMPARE(press(58, "end"), QByteArray());
        // every decline and end asks the hands-free link first
        QCOMPARE(mCalls.hangUpCalls, 4);
        QVERIFY(mCalls.log.contains("action 57 end") && mCalls.log.contains("action 58 end"));
    }

    void silenceAndReply()
    {
        write(ringing(59));
        QCOMPARE(press(59, "silence"), QByteArray("<action><id>59</id><ty>silence</ty></action>"));
        QCOMPARE(mCalls.answerCalls + mCalls.hangUpCalls, 0);
        // a typed reply to a ringing call: the phone declines it and sends the text (CR-53)
        mChunks.clear();
        QVERIFY(mService->reply(lipstickId(59), QStringLiteral("Call you back in 5")));
        QTRY_COMPARE(feedback(), QByteArray("<action><id>59</id><ty>reply</ty><tx>Call you back in 5</tx></action>"));
    }

    void answeredThenGone()
    {
        write(ringing(60));
        const uint card = lipstickId(60);
        // answered on the phone: the card becomes the call in progress, in place and quiet
        write("<insert><id>60</id><an>Phone</an><su>Ana Example</su><bo>Ongoing call</bo><ac>end</ac><og>1</og>"
              "<ca>call</ca></insert>");
        QCOMPARE(lastNotify().id, card);
        QCOMPARE(lastNotify().hints.value("x-nemo-feedback").toString(), QStringLiteral("notif_silent"));
        QCOMPARE(mCalls.log.last(), QStringLiteral("card 60 Ana Example|Ongoing call|end|"));
        // hung up: the phone removes it, the log hears it, the card goes (after the close delay)
        write("<removed><id>60</id></removed>");
        QVERIFY(mCalls.log.contains("gone 60"));
        QTRY_COMPARE(mLip->closeCalls(), QList<uint>{card});
        QCOMPARE(lipstickId(60), 0u);
        QVERIFY(mChunks.isEmpty());
    }

    void missedCallCard()
    {
        // Gadgetbridge's own frames (fixtures/missed-call-card.frames.txt): the card, its removal
        const QList<QByteArray> frames = fixtureFrames("missed-call-card.frames.txt");
        QCOMPARE(frames.size(), 2);
        write(frames.value(0));
        QCOMPARE(mCalls.log, QStringList{"missed 590801160 Sam Carter|Missed call · Mobile|1793200000"});
        const LipNotification n = lastNotify();
        QCOMPARE(n.actions, (QStringList{"callback", "Call"}));
        // ordered by the phone's time of the call (<wh>, CR-52)
        QCOMPARE(n.hints.value("x-nemo-timestamp").toString(),
                 QDateTime::fromSecsSinceEpoch(1793200000).toUTC().toString(Qt::ISODate));
        // its button is not a call action
        QCOMPARE(press(590801160, "callback"), QByteArray("<action><id>590801160</id><ty>callback</ty></action>"));
        QCOMPARE(mCalls.answerCalls + mCalls.hangUpCalls, 0);
        const uint card = lipstickId(590801160);
        write(frames.value(1));
        QTRY_VERIFY(mLip->closeCalls().contains(card));
        QCOMPARE(lipstickId(590801160), 0u);
    }

    void notACall()
    {
        write("<insert><id>61</id><pn>com.example.chat</pn><an>Chat</an><su>Ben Sample</su><bo>answer me</bo>"
              "<ac>reply,open</ac><ca>msg</ca></insert>");
        QVERIFY(lipstickId(61));
        QVERIFY(mCalls.log.isEmpty());
        QCOMPARE(press(61, "open"), QByteArray("<action><id>61</id><ty>open</ty></action>"));
        QVERIFY(mCalls.log.isEmpty());
        QCOMPARE(mCalls.answerCalls + mCalls.hangUpCalls, 0);
    }

    // The link drops while a call rings and comes back: the phone posts its live calls again
    // within the grace; a call card it does not post again ended meanwhile and goes
    void callsAfterAReconnection()
    {
        mService->setCallGraceMs(400);
        write(ringing(62));
        write(ringing(63));
        const uint stale = lipstickId(62), live = lipstickId(63);
        QTest::qWait(20);       // posted before the reconnection (ms clock)
        mService->setLinkConnected(false);
        QVERIFY(!mService->phoneConnected());
        mService->setLinkConnected(true);
        write(ringing(63));
        // (the call cards of the cases before are stale too)
        QTRY_VERIFY(mLip->closeCalls().contains(stale));
        QVERIFY(!mLip->closeCalls().contains(live));
        QCOMPARE(lipstickId(62), 0u);
        QCOMPARE(lipstickId(63), live);
        QVERIFY(mCalls.log.contains("gone 62"));
        QVERIFY(!mCalls.log.contains("gone 63"));
        // a later reconnection has a grace of its own
        mService->setCallGraceMs(3600 * 1000);
    }
};

BTSYNCD_TEST_MAIN(TestCalls)
#include "tst_calls.moc"
