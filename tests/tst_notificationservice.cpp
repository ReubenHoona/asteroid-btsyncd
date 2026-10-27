/*
 * The notification service against a fake lipstick on a private D-Bus:
 *   - phone -> watch <insert> / <removed> parsing (NotificationsUpdateChrc),
 *     split over writes, and what is posted to lipstick (actions, hints,
 *     vibration, resync, calls keeping their time, a lost notification
 *     posted again)
 *   - watch -> phone feedback (<action>): dismiss, actions, typed replies,
 *     silence, in MTU - 3 byte pieces, and nothing after a phone removal or
 *     after another action
 *   - the canned replies (<replies>) and the capabilities read
 *   - finding the phone's notifications again after a restart (restoreKnown)
 *   - the wake hold around a write
 * Phone ids, texts and numbers are synthetic.
 */
#include "notificationservice.h"
#include "backgroundactivity.h"
#include "mdconfitem.h"
#include "testmain.h"
#include "wakehold.h"

#include <QDateTime>
#include <QTest>
#include <QXmlStreamReader>

static const char REPLIES_KEY[] = "/org/asteroidos/notifications/canned-replies";

class TestNotificationService : public QObject
{
    Q_OBJECT

private:
    FakePeers *mPeers = nullptr;
    FakeLipstick *mLip = nullptr;
    NotificationService *mService = nullptr;
    Characteristic *mUpdate = nullptr;
    NotificationsFeedbackChrc *mFeedback = nullptr;
    Characteristic *mReplies = nullptr;
    Characteristic *mCaps = nullptr;
    QList<QByteArray> mChunks;

    // One phone write, or several of `piece` bytes (a phone that splits a
    // long value itself). The last write keeps the whole closing tag: the
    // parser needs it in one write (see closingTagSplitOverWrites)
    void write(Characteristic *chrc, const QByteArray &xml, int mtu = 0, int piece = 0)
    {
        QVariantMap options;
        if (mtu > 0)
            options.insert("mtu", mtu);
        if (piece <= 0)
            piece = xml.size();
        int pos = 0;
        while (xml.size() - pos > piece + 12) {
            chrc->WriteValue(xml.mid(pos, piece), options);
            pos += piece;
        }
        chrc->WriteValue(xml.mid(pos), options);
    }

    QByteArray feedback() const
    {
        QByteArray all;
        for (const QByteArray &c : mChunks)
            all += c;
        return all;
    }

    // Waits for the feedback to reach `expected` (bytes the phone joins)
    bool waitFeedback(const QByteArray &expected)
    {
        (void)QTest::qWaitFor([&] { return feedback().size() >= expected.size(); }, 3000);
        return feedback() == expected;
    }

    uint lipstickId(int phoneId) const { return mService->mKnownAndroidNotifs.value(phoneId, 0); }

    LipNotification lastNotify() const
    {
        const QList<LipNotification> calls = mLip->notifyCalls();
        return calls.isEmpty() ? LipNotification() : calls.last();
    }

    static QByteArray insert(int id, const QByteArray &inner)
    {
        return "<insert><pn>com.example.chat</pn><id>" + QByteArray::number(id) + "</id><an>Example Chat</an>"
               "<ai>ios-chatbubbles</ai>" + inner + "</insert>";
    }

private slots:
    void initTestCase()
    {
        if (!haveTestBus())
            QSKIP("no D-Bus session bus (run under dbus-run-session)");
        mPeers = new FakePeers;
        mLip = mPeers->lipstick();
        QVERIFY(mPeers->startLipstick());
        mService = new NotificationService(0, QDBusConnection::systemBus(), this);
        const QList<Characteristic *> chrcs = mService->getCharacteristics();
        QCOMPARE(chrcs.size(), 4);
        mUpdate = chrcs[0];
        mFeedback = qobject_cast<NotificationsFeedbackChrc *>(chrcs[1]);
        mReplies = chrcs[2];
        mCaps = chrcs[3];
        QVERIFY(mFeedback);
        QCOMPARE(mUpdate->getUuid(), QStringLiteral(NOTI_UPD_UUID));
        QCOMPARE(mFeedback->getUuid(), QStringLiteral(NOTI_FDB_UUID));
        QCOMPARE(mReplies->getUuid(), QStringLiteral(NOTI_RPL_UUID));
        QCOMPARE(mCaps->getUuid(), QStringLiteral(NOTI_CAP_UUID));
        QCOMPARE(mService->getCharacteristicPaths().size(), 4);
        QCOMPARE(mUpdate->getService(), mService->getPath());
        connect(mFeedback, &NotificationsFeedbackChrc::valueChanged, this, [this] {
            mChunks << mFeedback->property("Value").toByteArray();
        });
        // The first restoreKnown() ran in the constructor
        QCOMPARE(mLip->getNotificationsCalls(), 1);
    }

    void init()
    {
        mChunks.clear();
        mLip->reset();
        mService->mKnownAndroidNotifs.clear();
        // back to the 23 bytes a link starts with
        mService->setMtu(23);
    }

    void cleanupTestCase()
    {
        delete mService;
        mService = nullptr;
        delete mPeers;
    }

    void capabilities()
    {
        const QByteArray caps = "<capabilities><version>2</version>"
                                "<actions>dismiss,reply,open,mute,answer,reject,silence,end</actions>"
                                "<replies>1</replies></capabilities>";
        QCOMPARE(mCaps->ReadValue({}), caps);
        QCOMPARE(mCaps->ReadValue({{"offset", 14}}), caps.mid(14));
    }

    void insertPostsToLipstick()
    {
        const int runsBefore = BackgroundActivity::runs();
        write(mUpdate, insert(7, "<su>Alex</su><bo>Lunch &amp; a &lt;walk&gt;?</bo><vb>normal</vb>"), 0, 20);
        QCOMPARE(mLip->notifyCalls().size(), 1);
        const LipNotification n = lastNotify();
        QCOMPARE(n.appName, QStringLiteral("Example Chat"));
        QCOMPARE(n.id, 0u);
        QCOMPARE(n.appIcon, QStringLiteral("ios-chatbubbles"));
        QCOMPARE(n.summary, QStringLiteral("Alex"));
        QCOMPARE(n.body, QStringLiteral("Lunch & a <walk>?"));
        QVERIFY(n.actions.isEmpty());
        QCOMPARE(n.timeout, 0);
        QCOMPARE(n.hints.value("x-nemo-owner").toString(), QStringLiteral("asteroid-btsyncd"));
        QCOMPARE(n.hints.value("x-asteroid-phone-id").toString(), QStringLiteral("7"));
        QCOMPARE(n.hints.value("x-nemo-preview-summary").toString(), QStringLiteral("Alex"));
        QCOMPARE(n.hints.value("x-nemo-preview-body").toString(), QStringLiteral("Lunch & a <walk>?"));
        QCOMPARE(n.hints.value("x-nemo-feedback").toString(), QStringLiteral("notif_normal"));
        QCOMPARE(n.hints.value("urgency").toInt(), 3);
        QVERIFY(!n.hints.contains("x-asteroid-phone-actions"));
        QVERIFY(!n.hints.contains("x-nemo-timestamp"));

        const uint id = lipstickId(7);
        QVERIFY(id != 0);
        QCOMPARE(mLip->shown().value(id).summary, QStringLiteral("Alex"));
        // lipstick stamps it now; btsyncd remembers that for a resync
        QVERIFY(QDateTime::fromString(mService->timestamp(id), Qt::ISODate).isValid());
        // The write held the system awake
        QVERIFY(BackgroundActivity::runs() > runsBefore || BackgroundActivity::lastRunning());
        WakeHold::release(0);
    }

    void vibration_data()
    {
        QTest::addColumn<QByteArray>("vb");
        QTest::addColumn<QString>("feedback");
        QTest::newRow("absent (old phones)") << QByteArray() << "notif_strong";
        QTest::newRow("none") << QByteArray("<vb>none</vb>") << "notif_silent";
        QTest::newRow("normal") << QByteArray("<vb>normal</vb>") << "notif_normal";
        QTest::newRow("strong") << QByteArray("<vb>strong</vb>") << "notif_strong";
        QTest::newRow("ringtone") << QByteArray("<vb>ringtone</vb>") << "ringtone";
        QTest::newRow("unknown") << QByteArray("<vb>loud</vb>") << "notif_strong";
    }

    void vibration()
    {
        QFETCH(QByteArray, vb);
        QFETCH(QString, feedback);
        write(mUpdate, insert(8, "<su>S</su>" + vb));
        QCOMPARE(lastNotify().hints.value("x-nemo-feedback").toString(), feedback);
    }

    void updateReplacesTheSameNotification()
    {
        write(mUpdate, insert(9, "<su>One</su>"));
        const uint id = lipstickId(9);
        QVERIFY(id);
        write(mUpdate, insert(9, "<su>Two</su><x-new>skipped</x-new>"));
        QCOMPARE(mLip->notifyCalls().size(), 2);
        QCOMPARE(lastNotify().id, id);
        QCOMPARE(lastNotify().summary, QStringLiteral("Two"));
        QCOMPARE(lipstickId(9), id);
        QCOMPARE(mService->mKnownAndroidNotifs.size(), 1);
    }

    void actionsBecomeButtons()
    {
        write(mUpdate, insert(10, "<su>S</su><ac>reply, open,dismiss,,mute,later</ac>"));
        const LipNotification n = lastNotify();
        QCOMPARE(n.actions, (QStringList{"reply", "Reply", "open", "Open on phone", "mute", "Mute app",
                                         "later", "later"}));
        QCOMPARE(n.hints.value("x-asteroid-phone-actions").toString(),
                 QStringLiteral("reply, open,dismiss,mute,later"));
        QVERIFY(!n.hints.contains("x-nemo-timestamp"));     // not a call
    }

    void callKeepsItsTime()
    {
        write(mUpdate, insert(11, "<su>+15550100</su><bo>Incoming call</bo><vb>ringtone</vb>"
                                  "<ac>answer,reject,silence</ac>"), 185);
        LipNotification n = lastNotify();
        QCOMPARE(n.actions, (QStringList{"answer", "Answer", "reject", "Decline", "silence", "Silence"}));
        const QString stamp = n.hints.value("x-nemo-timestamp").toString();
        QVERIFY(QDateTime::fromString(stamp, Qt::ISODate).isValid());
        const uint id = lipstickId(11);
        QCOMPARE(mService->callTimestamp(id), stamp);
        QCOMPARE(mService->timestamp(id), stamp);

        QTest::qWait(1100);     // a later update (now silenced on the phone)
        write(mUpdate, insert(11, "<su>+15550100</su><bo>Incoming call</bo><vb>none</vb><ac>answer,reject</ac>"));
        n = lastNotify();
        QCOMPARE(n.id, id);
        QCOMPARE(n.hints.value("x-nemo-timestamp").toString(), stamp);
        QCOMPARE(mService->callTimestamp(0), QString());
        QCOMPARE(mService->timestamp(0), QString());
    }

    void resyncIsSilentAndKeepsItsPlace()
    {
        write(mUpdate, insert(12, "<su>Old</su><bo>news</bo><vb>strong</vb>"));
        const uint id = lipstickId(12);
        const QString posted = mService->timestamp(id);
        QVERIFY(!posted.isEmpty());

        // After a reconnect the phone sends it again with <rs>1</rs>
        write(mUpdate, insert(12, "<su>Old</su><bo>news</bo><vb>strong</vb><rs> 1 </rs>"));
        const LipNotification n = lastNotify();
        QCOMPARE(n.id, id);
        QCOMPARE(n.hints.value("x-nemo-feedback").toString(), QStringLiteral("notif_silent"));
        QCOMPARE(n.hints.value("x-nemo-feedback-suppressed").toBool(), true);
        QVERIFY(!n.hints.contains("x-nemo-preview-summary"));
        QVERIFY(!n.hints.contains("x-nemo-preview-body"));
        QCOMPARE(n.hints.value("x-nemo-timestamp").toString(), posted);

        // A new notification in a resync has no time to keep
        write(mUpdate, insert(13, "<su>New</su><rs>1</rs>"));
        QVERIFY(!lastNotify().hints.contains("x-nemo-timestamp"));
        // <rs>0</rs> is a normal notification
        write(mUpdate, insert(14, "<su>N</su><rs>0</rs>"));
        QCOMPARE(lastNotify().hints.value("x-nemo-preview-summary").toString(), QStringLiteral("N"));
    }

    void goneNotificationIsPostedAgain()
    {
        write(mUpdate, insert(15, "<su>A</su><ac>reply</ac>"));
        const uint id = lipstickId(15);
        write(mUpdate, insert(15, "<su>A</su><rs>1</rs>"));      // carries the old time
        QVERIFY(lastNotify().hints.contains("x-nemo-timestamp"));
        mLip->setReplaceGone(true);     // closed behind btsyncd's back
        const int before = mLip->notifyCalls().size();
        write(mUpdate, insert(15, "<su>B</su><rs>1</rs>"));
        const QList<LipNotification> calls = mLip->notifyCalls();
        QCOMPARE(calls.size(), before + 2);
        QCOMPARE(calls[before].id, id);
        QCOMPARE(calls[before + 1].id, 0u);
        QVERIFY(!calls[before + 1].hints.contains("x-nemo-timestamp"));     // its own new time
        QCOMPARE(calls[before + 1].summary, QStringLiteral("B"));
        const uint newId = lipstickId(15);
        QVERIFY(newId && newId != id);
        QCOMPARE(mService->timestamp(id), QString());
    }

    void goneCallKeepsItsTime()
    {
        write(mUpdate, insert(16, "<su>Call</su><ac>answer,reject</ac>"));
        const QString stamp = lastNotify().hints.value("x-nemo-timestamp").toString();
        mLip->setReplaceGone(true);
        write(mUpdate, insert(16, "<su>Call</su><ac>answer,reject</ac>"));
        QCOMPARE(lastNotify().id, 0u);
        QCOMPARE(lastNotify().hints.value("x-nemo-timestamp").toString(), stamp);
    }

    void insertWithoutIdOrRootIsHarmless()
    {
        write(mUpdate, "<notinsert><id>1</id></insert>");
        QCOMPARE(mLip->notifyCalls().size(), 0);
        // the reader was reset: the next one parses
        write(mUpdate, insert(17, "<su>ok</su>"));
        QCOMPARE(lastNotify().summary, QStringLiteral("ok"));
        // A removal with another root is ignored too
        write(mUpdate, "<gone><id>17</id></removed>");
        QVERIFY(mLip->closeCalls().isEmpty());
        QVERIFY(lipstickId(17));
    }

    void removedClosesWithoutFeedback()
    {
        write(mUpdate, insert(18, "<su>S</su><ac>reply</ac>"));
        const uint id = lipstickId(18);
        write(mUpdate, "<removed><id>18</id></removed>", 0, 7);
        QCOMPARE(mLip->closeCalls(), QList<uint>{id});
        QCOMPARE(lipstickId(18), 0u);
        QCOMPARE(mService->timestamp(id), QString());
        // lipstick then reports the close: not a dismiss for the phone
        mLip->emitClosed(id, 3);
        QTest::qWait(200);
        QVERIFY(mChunks.isEmpty());

        // An id the watch does not know: nothing to close
        write(mUpdate, "<removed><id>999</id></removed>");
        QCOMPARE(mLip->closeCalls().size(), 1);
    }

    void dismissOnTheWatch()
    {
        write(mUpdate, insert(19, "<su>S</su>"), 30);
        const uint id = lipstickId(19);
        mLip->emitClosed(id, 2);    // dismissed by the user
        QVERIFY2(waitFeedback("<action><id>19</id><ty>dismiss</ty></action>"), feedback());
        for (const QByteArray &c : std::as_const(mChunks))
            QVERIFY(c.size() <= 27);    // MTU 30 - 3
        QVERIFY(mChunks.size() >= 2);
        QCOMPARE(lipstickId(19), 0u);
    }

    void expiredIsNotADismiss()
    {
        write(mUpdate, insert(20, "<su>S</su>"));
        const uint id = lipstickId(20);
        mLip->emitClosed(id, 1);    // expired
        QTRY_COMPARE(lipstickId(20), 0u);
        QVERIFY(mChunks.isEmpty());
        // A close of a notification btsyncd never posted
        mLip->emitClosed(12345, 2);
        QTest::qWait(100);
        QVERIFY(mChunks.isEmpty());
    }

    void actionThenCloseSendsOnlyTheAction()
    {
        write(mUpdate, insert(21, "<su>S</su><ac>open,mute</ac>"), 100);
        const uint id = lipstickId(21);
        mLip->emitAction(id, "open");
        QVERIFY2(waitFeedback("<action><id>21</id><ty>open</ty></action>"), feedback());
        QCOMPARE(mChunks.size(), 1);    // fits in MTU 100 - 3
        // lipstick closes a notification after any action: no dismiss
        mLip->emitClosed(id, 2);
        QTRY_COMPARE(lipstickId(21), 0u);
        QTest::qWait(100);
        QCOMPARE(feedback(), QByteArray("<action><id>21</id><ty>open</ty></action>"));
    }

    void replyAndDefaultActionsAreNotSent()
    {
        write(mUpdate, insert(22, "<su>S</su><ac>reply</ac>"));
        const uint id = lipstickId(22);
        mLip->emitAction(id, "reply");      // the text comes through Reply()
        mLip->emitAction(id, "default");    // a tap on the notification
        mLip->emitAction(4242, "open");     // not ours
        QTest::qWait(200);
        QVERIFY(mChunks.isEmpty());
        QCOMPARE(lipstickId(22), id);
    }

    void typedReply()
    {
        write(mUpdate, insert(23, "<su>S</su><ac>reply</ac>"), 64);
        const uint id = lipstickId(23);
        NotificationsDBusApi *api = mService->findChild<NotificationsDBusApi *>();
        QVERIFY(api);
        QVERIFY(api->Reply(id, QStringLiteral("  On my way <3 & ü  ")));
        // Escaped, trimmed, UTF-8, in pieces of MTU - 3
        const QByteArray expected = QStringLiteral("<action><id>23</id><ty>reply</ty><tx>On my way &lt;3 &amp; ü</tx></action>").toUtf8();
        QVERIFY2(waitFeedback(expected), feedback());
        for (const QByteArray &c : std::as_const(mChunks))
            QVERIFY(c.size() <= 61);
        QXmlStreamReader r(feedback());
        QVERIFY(r.readNextStartElement());
        QCOMPARE(r.name(), QLatin1String("action"));
        // Done on the watch: closed, and its close is not a dismiss
        QTRY_COMPARE(mLip->closeCalls(), QList<uint>{id});
        QCOMPARE(lipstickId(23), 0u);
        mChunks.clear();
        mLip->emitClosed(id, 2);
        QTest::qWait(200);
        QVERIFY(mChunks.isEmpty());
    }

    void replyNeedsTextAndAKnownNotification()
    {
        write(mUpdate, insert(24, "<su>S</su><ac>reply</ac>"));
        const uint id = lipstickId(24);
        NotificationsDBusApi *api = mService->findChild<NotificationsDBusApi *>();
        QVERIFY(!api->Reply(id, QStringLiteral("   ")));
        QVERIFY(!api->Reply(id + 1000, QStringLiteral("text")));
        QVERIFY(!mService->reply(0, QStringLiteral("text")));
        QTest::qWait(100);
        QVERIFY(mChunks.isEmpty());
        QCOMPARE(lipstickId(24), id);
    }

    void replyOverTheSessionBus()
    {
        // The launcher's call: org.asteroidos.btsyncd.Notifications.Reply
        write(mUpdate, insert(25, "<su>S</su><ac>reply</ac>"), 247);
        const uint id = lipstickId(25);
        QDBusMessage call = QDBusMessage::createMethodCall(SERVICE_NAME, BTSYNCD_NOTIF_PATH, BTSYNCD_NOTIF_IFACE, "Reply");
        call << id << QStringLiteral("Yes");
        const QDBusMessage reply = callAsPeer(call);
        QCOMPARE(reply.type(), QDBusMessage::ReplyMessage);
        QCOMPARE(reply.arguments().value(0).toBool(), true);
        QVERIFY2(waitFeedback("<action><id>25</id><ty>reply</ty><tx>Yes</tx></action>"), feedback());
    }

    void silenceARingingCall()
    {
        write(mUpdate, insert(26, "<su>+15550100</su><vb>ringtone</vb><ac>answer,reject,silence</ac>"), 100);
        const uint id = lipstickId(26);
        const QString stamp = lastNotify().hints.value("x-nemo-timestamp").toString();
        const int before = mLip->notifyCalls().size();
        NotificationsDBusApi *api = mService->findChild<NotificationsDBusApi *>();
        QVERIFY(api->Silence(id));
        QVERIFY2(waitFeedback("<action><id>26</id><ty>silence</ty></action>"), feedback());
        // and the watch stops vibrating: the same notification, feedback suppressed
        QTRY_COMPARE(mLip->notifyCalls().size(), before + 1);
        const LipNotification n = lastNotify();
        QCOMPARE(n.id, id);
        QCOMPARE(n.hints.value("x-nemo-feedback-suppressed").toBool(), true);
        QCOMPARE(n.hints.value("x-nemo-timestamp").toString(), stamp);
        QCOMPARE(n.actions, (QStringList{"answer", "Answer", "reject", "Decline", "silence", "Silence"}));
        // The call keeps ringing on the phone until answered or declined
        QCOMPARE(lipstickId(26), id);
        mLip->emitAction(id, "answer");
        QVERIFY2(waitFeedback("<action><id>26</id><ty>silence</ty></action><action><id>26</id><ty>answer</ty></action>"),
                 feedback());
    }

    void silenceWithoutThePhonesSilence()
    {
        // An older phone offers no "silence": only the watch goes quiet
        write(mUpdate, insert(27, "<su>Call</su><ac>answer,reject</ac>"));
        const uint id = lipstickId(27);
        const int before = mLip->notifyCalls().size();
        QVERIFY(mService->silence(id));
        QTRY_COMPARE(mLip->notifyCalls().size(), before + 1);
        QTest::qWait(100);
        QVERIFY(mChunks.isEmpty());
        // Not a call: nothing to update; unknown: false
        write(mUpdate, insert(28, "<su>Chat</su><ac>reply</ac>"));
        const int chat = mLip->notifyCalls().size();
        QVERIFY(mService->silence(lipstickId(28)));
        QTest::qWait(100);
        QCOMPARE(mLip->notifyCalls().size(), chat);
        QVERIFY(!mService->silence(987654));
    }

    void cannedReplies()
    {
        MDConfItem::reset();
        write(mReplies, "<replies><r> OK </r><r>   </r><x>skipped</x><r>On my way</r><r>Can&apos;t talk</r></replies>",
              50, 20);
        QCOMPARE(MDConfItem::store().value(REPLIES_KEY).toStringList(),
                 (QStringList{"OK", "On my way", "Can't talk"}));
        QCOMPARE(MDConfItem::sets(), 1);
        // The MTU of the replies write is used for feedback
        write(mUpdate, insert(29, "<su>S</su>"));
        mLip->emitClosed(lipstickId(29), 2);
        QVERIFY(waitFeedback("<action><id>29</id><ty>dismiss</ty></action>"));
        QCOMPARE(mChunks.size(), 1);    // 43 bytes in MTU 50 - 3

        // None: back to the launcher's defaults
        write(mReplies, "<replies></replies>");
        QVERIFY(!MDConfItem::store().contains(REPLIES_KEY));
        QCOMPARE(MDConfItem::unsets(), 1);
        // Another root changes nothing
        write(mReplies, "<other><r>x</r></other></replies>");
        QCOMPARE(MDConfItem::sets() + MDConfItem::unsets(), 2);
        // and the next list parses from a clean reader
        write(mReplies, "<replies><r>Later</r></replies>");
        QCOMPARE(MDConfItem::store().value(REPLIES_KEY).toStringList(), QStringList{"Later"});
    }

    void restoreAfterARestart()
    {
        auto ours = [](uint id, int phoneId, const QString &actions, const QString &stamp) {
            LipNotification n;
            n.appName = "Example Chat";
            n.id = id;
            n.summary = "S" + QString::number(id);
            n.hints.insert("x-nemo-owner", "asteroid-btsyncd");
            if (phoneId)
                n.hints.insert("x-asteroid-phone-id", QString::number(phoneId));
            if (!actions.isEmpty())
                n.hints.insert("x-asteroid-phone-actions", actions);
            if (!stamp.isEmpty())
                n.hints.insert("x-nemo-timestamp", stamp);
            return n;
        };
        LipNotification other = ours(600, 31, "", "");
        other.hints.insert("x-nemo-owner", "someone-else");
        LipNotification badId = ours(601, 0, "", "");
        badId.hints.insert("x-asteroid-phone-id", "not-a-number");
        mLip->setShown({ours(500, 31, "reply", "2026-10-01T10:00:00Z"),
                        ours(400, 31, "reply", "2026-10-01T09:00:00Z"),     // an older duplicate
                        ours(510, 32, "answer,reject,silence", "2026-10-01T11:00:00Z"),
                        ours(520, 33, "", ""),
                        ours(530, 0, "", ""),
                        badId, other});
        // Stale knowledge the restart must drop
        mService->mKnownAndroidNotifs.insert(34, 777);

        // lipstick (re)starts: its service watcher triggers restoreKnown()
        mPeers->stopLipstick();
        const int gets = mLip->getNotificationsCalls();
        QVERIFY(mPeers->startLipstick());
        QTRY_COMPARE(mLip->getNotificationsCalls(), gets + 1);

        QCOMPARE(mService->mKnownAndroidNotifs.size(), 3);
        QCOMPARE(lipstickId(31), 500u);
        QCOMPARE(lipstickId(32), 510u);
        QCOMPARE(lipstickId(33), 520u);
        QCOMPARE(lipstickId(34), 0u);
        QTRY_COMPARE(mLip->closeCalls(), QList<uint>{400});
        QCOMPARE(mService->timestamp(500), QStringLiteral("2026-10-01T10:00:00Z"));
        QCOMPARE(mService->callTimestamp(510), QStringLiteral("2026-10-01T11:00:00Z"));
        QCOMPARE(mService->callTimestamp(500), QString());

        // The phone's update finds it, a restored call can be silenced
        write(mUpdate, insert(31, "<su>again</su><ac>reply</ac>"));
        QCOMPARE(lastNotify().id, 500u);
        const int before = mLip->notifyCalls().size();
        QVERIFY(mService->silence(510));
        QVERIFY(waitFeedback("<action><id>32</id><ty>silence</ty></action>"));
        QTRY_COMPARE(mLip->notifyCalls().size(), before + 1);
        QCOMPARE(lastNotify().id, 510u);
        QCOMPARE(lastNotify().hints.value("x-nemo-feedback-suppressed").toBool(), true);
    }

    void restoreWithoutLipstickKeepsWhatItKnows()
    {
        mService->mKnownAndroidNotifs.insert(35, 800);
        mPeers->stopLipstick();
        mService->restoreKnown();
        QCOMPARE(lipstickId(35), 800u);
        QVERIFY(mPeers->startLipstick());
        QTRY_VERIFY(mLip->getNotificationsCalls() > 0);
        // lipstick shows nothing of ours: forgotten
        QTRY_COMPARE(lipstickId(35), 0u);
    }

    void mtuOption()
    {
        write(mUpdate, insert(36, "<su>S</su>"), 0);   // no mtu: keeps the last one
        mService->setMtu(-5);
        mService->setMtu(0);
        mLip->emitClosed(lipstickId(36), 2);
        QVERIFY(waitFeedback("<action><id>36</id><ty>dismiss</ty></action>"));
        for (const QByteArray &c : std::as_const(mChunks))
            QVERIFY(c.size() <= 20);
        // A huge MTU still sends at most 509 bytes a notification
        mChunks.clear();
        mFeedback->send(QByteArray(1200, 'x'), 1000);
        QCOMPARE(mChunks.size(), 3);
        QCOMPARE(mChunks[0].size(), 509);
        QCOMPARE(mChunks[2].size(), 1200 - 2 * 509);
        mChunks.clear();
        mFeedback->send(QByteArray(), 100);
        QVERIFY(mChunks.isEmpty());
    }

    void junkAfterAnInsertLosesTheNextOne()
    {
        // Static review finding 6 (notificationservice.cpp, mReader.addData):
        // a write that does not end exactly with "</insert>" (here a stray
        // newline after it) is never parsed nor cleared, so the next
        // notification is parsed from the stale start: the phone's second
        // notification is lost and the first one is shown again.
        write(mUpdate, insert(37, "<su>first</su>") + "\n");
        write(mUpdate, insert(38, "<su>second</su>"));
        QCOMPARE(lastNotify().summary, QStringLiteral("second"));
        QVERIFY(lipstickId(38) != 0);
    }

    void closingTagSplitOverWrites()
    {
        // PROTOCOL-notification-actions.md: "A value may span several writes
        // [...]; the receiver joins them until the closing tag". The parser
        // only looks at the end of each write, so a phone that splits its
        // writes inside "</insert>" loses that notification (and, as above,
        // garbles the next one). BlueZ joins a long (prepared) write into one
        // WriteValue, so today's Gadgetbridge is not hit; a client that
        // chunks itself (as it does for voice results) would be.
        const QByteArray xml = insert(39, "<su>split</su>");
        mUpdate->WriteValue(xml.left(xml.size() - 4), {});
        mUpdate->WriteValue(xml.right(4), {});      // "ert>"
        QCOMPARE(mLip->notifyCalls().size(), 1);
        // the same for a removal
        mService->mKnownAndroidNotifs.insert(40, 4000);
        mLip->reset();
        mUpdate->WriteValue("<removed><id>40</id></rem", {});
        mUpdate->WriteValue("oved>", {});
        QCOMPARE(mLip->closeCalls(), QList<uint>{4000});
    }
};

BTSYNCD_TEST_MAIN(TestNotificationService)
#include "tst_notificationservice.moc"
