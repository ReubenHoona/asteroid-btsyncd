/*
 * Tests of the notification service (protocol versions 3 and 4) on a private D-Bus
 * (cmake -DBTSYNCD_TESTS=ON). Run it with a private bus that serves as both
 * the session and the system bus:
 *   dbus-run-session -- sh -c 'DBUS_SYSTEM_BUS_ADDRESS=$DBUS_SESSION_BUS_ADDRESS notifications_test'
 * A fake lipstick (org.freedesktop.Notifications, in its own thread) records
 * what btsyncd posts; the test writes to the GATT characteristics the way
 * BlueZ does and reads the feedback notifications.
 *   - capabilities: version 3
 *   - <sr> and <ax>: hints and app-N actions; ActionInvoked app-1 -> ty=app ix=1
 *   - an insert without the new elements posts as before
 *   - find my phone: refused without a subscribed phone; start/stop feedback;
 *     <findphone>stop</findphone> from the phone; FindPhoneChanged signals
 *   - the find-my-watch alert's "found" action
 * Version 4:
 *   - conversations: one card per thread through re-posts under new ids,
 *     quiet in-place updates, a new card (banner, vibration) for new
 *     messages, removal + insert keeps the card, delayed close
 *   - importance low, only alert once, ongoing: quiet
 *   - alarms (vb=alarm, category), Do Not Disturb
 *   - app icons: stored as SVG, used by name, bad icons refused
 *   - a dismiss while no phone is subscribed waits for it; its resync does
 *     not bring the card back
 *   - a call card (end, or answer/reject) the phone does not post again within the
 *     grace of a (re)connection is closed (a call that ended while the watch was
 *     away); one posted again stays, as does every other card
 *   - the same while the link is gone but BlueZ kept the subscription (a
 *     bonded phone): sent after the phone's setup once the link is back
 *   - keys: the same phone notification under a new phone id updates its card;
 *     the phone's list after a resync (<sync>) closes the cards not in it
 *     (not ones posted meanwhile), also in several writes
 *   - framing: writes joined until the closing tag (also split inside it, as
 *     long-write chunks with offsets, two messages in one write); the rest of
 *     a write lost at a disconnection or followed by a new message is dropped
 *     and the next notification still comes
 */

#include "notificationservice.h"
#include "common.h"

#include <QCoreApplication>
#include <QDBusConnection>
#include <QDBusMessage>
#include <QDBusMetaType>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QElapsedTimer>
#include <QMutex>
#include <QMutexLocker>
#include <QThread>
#include <QVariantMap>
#include <cstdio>
#include <functional>

static int failures = 0;

#define CHECK(cond) do { if (!(cond)) { fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); failures++; } } while (0)

static const char *CHR_UPDATE = "/org/asteroidos/btsyncd/service0/char0";
static const char *CHR_FEEDBACK = "/org/asteroidos/btsyncd/service0/char1";
static const char *CHR_CAPS = "/org/asteroidos/btsyncd/service0/char3";
static const char *CHR_ICON = "/org/asteroidos/btsyncd/service0/char4";

struct Posted {
    QString appName, appIcon, summary, body;
    uint replacesId = 0;
    QStringList actions;
    QVariantMap hints;
};

// lipstick's org.freedesktop.Notifications, enough for btsyncd
class FakeLipstick : public QObject
{
    Q_OBJECT
    Q_CLASSINFO("D-Bus Interface", "org.freedesktop.Notifications")
public:
    QMutex lock;
    QList<Posted> posted;
    QList<uint> closed;
    uint nextId = 100;

public slots:
    uint Notify(const QString &appName, uint replacesId, const QString &appIcon, const QString &summary,
                const QString &body, const QStringList &actions, const QVariantMap &hints, int)
    {
        QMutexLocker l(&lock);
        Posted p;
        p.appName = appName;
        p.appIcon = appIcon;
        p.replacesId = replacesId;
        p.summary = summary;
        p.body = body;
        p.actions = actions;
        p.hints = hints;
        posted << p;
        return replacesId ? replacesId : nextId++;
    }
    void CloseNotification(uint id)
    {
        QMutexLocker l(&lock);
        closed << id;
    }
};

class Collector : public QObject
{
    Q_OBJECT
public:
    QByteArray feedback;
    int largestPiece = 0;
    QList<bool> findPhoneChanges;
public slots:
    void propertiesChanged(const QString &, const QVariantMap &changed, const QStringList &)
    {
        if(changed.contains("Value")) {
            const QByteArray piece = changed.value("Value").toByteArray();
            feedback += piece;
            largestPiece = qMax(largestPiece, int(piece.size()));
        }
    }
    void findPhoneChanged(bool ringing) { findPhoneChanges << ringing; }
};

static void spin(int ms)
{
    QElapsedTimer t;
    t.start();
    while(t.elapsed() < ms)
        QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
}

static bool waitFor(const std::function<bool()> &cond, int ms = 3000)
{
    QElapsedTimer t;
    t.start();
    while(!cond() && t.elapsed() < ms)
        QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
    return cond();
}

static QString btsyncdName;

static QDBusMessage gattCall(QDBusConnection &driver, const char *path, const char *method, const QList<QVariant> &args)
{
    QDBusMessage m = QDBusMessage::createMethodCall(btsyncdName, path, GATT_CHRC_IFACE, method);
    m.setArguments(args);
    return driver.call(m, QDBus::BlockWithGui, 5000);
}

static void write(QDBusConnection &driver, const QByteArray &value)
{
    QVariantMap options;
    options.insert("mtu", 185);
    const QDBusMessage r = gattCall(driver, CHR_UPDATE, "WriteValue", {value, options});
    CHECK(r.type() == QDBusMessage::ReplyMessage);
}

// One chunk of a long write (BlueZ passes its offset)
static void writeAt(QDBusConnection &driver, const QByteArray &value, int offset)
{
    QVariantMap options;
    options.insert("mtu", 185);
    if(offset)
        options.insert("offset", (quint16) offset);
    const QDBusMessage r = gattCall(driver, CHR_UPDATE, "WriteValue", {value, options});
    CHECK(r.type() == QDBusMessage::ReplyMessage);
}

static int postedCount(FakeLipstick &fake)
{
    QMutexLocker l(&fake.lock);
    return fake.posted.size();
}

static Posted lastPosted(FakeLipstick &fake)
{
    QMutexLocker l(&fake.lock);
    return fake.posted.isEmpty() ? Posted() : fake.posted.last();
}

static bool wasClosed(FakeLipstick &fake, uint id)
{
    QMutexLocker l(&fake.lock);
    return fake.closed.contains(id);
}

static void emitActionInvoked(QDBusConnection &fakeConn, uint id, const QString &key)
{
    QDBusMessage s = QDBusMessage::createSignal("/org/freedesktop/Notifications", "org.freedesktop.Notifications", "ActionInvoked");
    s << id << key;
    fakeConn.send(s);
}

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    if(qEnvironmentVariableIsEmpty("DBUS_SESSION_BUS_ADDRESS") || qEnvironmentVariableIsEmpty("DBUS_SYSTEM_BUS_ADDRESS")) {
        fprintf(stderr, "needs a private bus as session and system bus (see the top of the source)\n");
        return 2;
    }

    // The fake lipstick answers from its own thread: btsyncd calls it blocking
    QThread fakeThread;
    fakeThread.start();
    FakeLipstick fake;
    fake.moveToThread(&fakeThread);
    QDBusConnection fakeConn = QDBusConnection::connectToBus(QDBusConnection::SessionBus, "fake-lipstick");
    CHECK(fakeConn.registerObject("/org/freedesktop/Notifications", &fake, QDBusConnection::ExportAllSlots));
    CHECK(fakeConn.registerService("org.freedesktop.Notifications"));

    NotificationService service(0, QDBusConnection::systemBus());
    btsyncdName = QDBusConnection::systemBus().baseService();

    QDBusConnection driver = QDBusConnection::connectToBus(QDBusConnection::SessionBus, "driver");
    Collector collector;
    CHECK(driver.connect(btsyncdName, CHR_FEEDBACK, "org.freedesktop.DBus.Properties", "PropertiesChanged",
                         &collector, SLOT(propertiesChanged(QString, QVariantMap, QStringList))));
    CHECK(driver.connect(SERVICE_NAME, BTSYNCD_NOTIF_PATH, BTSYNCD_NOTIF_IFACE, "FindPhoneChanged",
                         &collector, SLOT(findPhoneChanged(bool))));
    spin(200);

    // Capabilities
    {
        const QDBusMessage r = gattCall(driver, CHR_CAPS, "ReadValue", {QVariantMap()});
        CHECK(r.type() == QDBusMessage::ReplyMessage);
        const QByteArray caps = r.arguments().value(0).toByteArray();
        CHECK(caps.contains("<version>4</version>"));
        CHECK(caps.contains("<findphone>1</findphone>"));
        CHECK(caps.contains("<sync>1</sync>"));
        CHECK(caps.contains("found"));
    }

    // Find my phone without a subscribed phone: refused
    {
        QDBusMessage m = QDBusMessage::createMethodCall(SERVICE_NAME, BTSYNCD_NOTIF_PATH, BTSYNCD_NOTIF_IFACE, "FindPhone");
        m << true;
        const QDBusMessage r = driver.call(m, QDBus::BlockWithGui, 5000);
        CHECK(r.type() == QDBusMessage::ReplyMessage && r.arguments().value(0).toBool() == false);
    }

    // The phone subscribes to the feedback, as BlueZ does
    CHECK(gattCall(driver, CHR_FEEDBACK, "StartNotify", {}).type() == QDBusMessage::ReplyMessage);

    // A message with suggested replies and two app actions (escaped text)
    write(driver, "<insert><id>42</id><vb>normal</vb><pn>org.example.chat</pn><an>Chat</an>"
                  "<su>Alice</su><bo>Lunch?</bo><ac>reply,open,mute</ac>"
                  "<ax><a>Mark as read</a><a>Archive &amp; more</a></ax>"
                  "<sr><r>Sure!</r><r>  Not\ntoday  </r><r></r></sr></insert>");
    CHECK(waitFor([&] { return postedCount(fake) == 1; }));
    {
        const Posted p = lastPosted(fake);
        CHECK(p.summary == "Alice");
        CHECK(p.actions == (QStringList() << "reply" << "Reply" << "open" << "Open on phone" << "mute" << "Mute app"
                                          << "app-0" << "Mark as read" << "app-1" << "Archive & more"));
        CHECK(p.hints.value("x-asteroid-app-actions").toString() == "Mark as read\nArchive & more");
        CHECK(p.hints.value("x-asteroid-smart-replies").toString() == "Sure!\nNot today");
        CHECK(p.hints.value("x-asteroid-phone-actions").toString() == "reply,open,mute");
        CHECK(p.hints.value("x-asteroid-phone-id").toString() == "42");
    }

    // The watch runs the second app action: feedback ty=app ix=1 for phone id 42
    collector.feedback.clear();
    emitActionInvoked(fakeConn, 100, "app-1");
    CHECK(waitFor([&] { return collector.feedback.endsWith("</action>"); }));
    CHECK(collector.feedback == "<action><id>42</id><ty>app</ty><ix>1</ix></action>");
    fprintf(stderr, "app action feedback: %s\n", collector.feedback.constData());

    // An out-of-range app action is not one: no feedback as app
    collector.feedback.clear();
    write(driver, "<insert><id>43</id><vb>none</vb><an>Mail</an><su>Bob</su><bo>Hi</bo><ac>open,mute</ac></insert>");
    CHECK(waitFor([&] { return postedCount(fake) == 2; }));
    {
        // Without the new elements: as before
        const Posted p = lastPosted(fake);
        CHECK(p.actions == (QStringList() << "open" << "Open on phone" << "mute" << "Mute app"));
        CHECK(!p.hints.contains("x-asteroid-app-actions"));
        CHECK(!p.hints.contains("x-asteroid-smart-replies"));
    }
    emitActionInvoked(fakeConn, 101, "app-7");
    CHECK(waitFor([&] { return collector.feedback.endsWith("</action>"); }));
    CHECK(collector.feedback == "<action><id>43</id><ty>app-7</ty></action>");

    // Find my watch alert with "found"
    collector.feedback.clear();
    write(driver, "<insert><id>-77</id><vb>ringtone</vb><an>Incoming call</an><su>Gadgetbridge</su><ac>found</ac></insert>");
    CHECK(waitFor([&] { return postedCount(fake) == 3; }));
    CHECK(lastPosted(fake).actions == (QStringList() << "found" << "Found it"));
    // the watch itself rings (ngfd's ringtone, with sound)
    CHECK(lastPosted(fake).hints.value("x-nemo-feedback").toString() == "ringtone");
    emitActionInvoked(fakeConn, 102, "found");
    CHECK(waitFor([&] { return collector.feedback.endsWith("</action>"); }));
    CHECK(collector.feedback == "<action><id>-77</id><ty>found</ty></action>");

    // Find my phone: start, the phone stops it, start again, the watch stops it
    collector.feedback.clear();
    collector.findPhoneChanges.clear();
    {
        QDBusMessage m = QDBusMessage::createMethodCall(SERVICE_NAME, BTSYNCD_NOTIF_PATH, BTSYNCD_NOTIF_IFACE, "FindPhone");
        m << true;
        const QDBusMessage r = driver.call(m, QDBus::BlockWithGui, 5000);
        CHECK(r.type() == QDBusMessage::ReplyMessage && r.arguments().value(0).toBool() == true);
    }
    CHECK(waitFor([&] { return collector.feedback.endsWith("</action>") && !collector.findPhoneChanges.isEmpty(); }));
    CHECK(collector.feedback == "<action><id>0</id><ty>findphone</ty><tx>start</tx></action>");
    CHECK(collector.findPhoneChanges == QList<bool>() << true);
    {
        QDBusMessage m = QDBusMessage::createMethodCall(SERVICE_NAME, BTSYNCD_NOTIF_PATH, BTSYNCD_NOTIF_IFACE, "FindingPhone");
        const QDBusMessage r = driver.call(m, QDBus::BlockWithGui, 5000);
        CHECK(r.arguments().value(0).toBool() == true);
    }
    write(driver, "<findphone>stop</findphone>");
    CHECK(waitFor([&] { return collector.findPhoneChanges.size() == 2; }));
    CHECK(collector.findPhoneChanges == QList<bool>() << true << false);

    // A notification after the findphone element still parses (no leftovers)
    write(driver, "<insert><id>44</id><vb>none</vb><an>Chat</an><su>Carol</su><bo>Ok</bo><ac>open</ac></insert>");
    CHECK(waitFor([&] { return postedCount(fake) == 4; }));
    CHECK(lastPosted(fake).summary == "Carol");

    collector.feedback.clear();
    {
        QDBusMessage m = QDBusMessage::createMethodCall(SERVICE_NAME, BTSYNCD_NOTIF_PATH, BTSYNCD_NOTIF_IFACE, "FindPhone");
        m << true;
        driver.call(m, QDBus::BlockWithGui, 5000);
        collector.feedback.clear();
        QDBusMessage s = QDBusMessage::createMethodCall(SERVICE_NAME, BTSYNCD_NOTIF_PATH, BTSYNCD_NOTIF_IFACE, "FindPhone");
        s << false;
        driver.call(s, QDBus::BlockWithGui, 5000);
    }
    CHECK(waitFor([&] { return collector.feedback.endsWith("</action>") && collector.findPhoneChanges.size() == 4; }));
    CHECK(collector.feedback == "<action><id>0</id><ty>findphone</ty><tx>stop</tx></action>");

    // The phone disconnects while ringing: not ringing any more
    {
        QDBusMessage m = QDBusMessage::createMethodCall(SERVICE_NAME, BTSYNCD_NOTIF_PATH, BTSYNCD_NOTIF_IFACE, "FindPhone");
        m << true;
        driver.call(m, QDBus::BlockWithGui, 5000);
    }
    CHECK(waitFor([&] { return collector.findPhoneChanges.size() == 5; }));
    CHECK(gattCall(driver, CHR_FEEDBACK, "StopNotify", {}).type() == QDBusMessage::ReplyMessage);
    CHECK(waitFor([&] { return collector.findPhoneChanges.size() == 6; }));
    CHECK(collector.findPhoneChanges.value(5) == false);

    // ---- Protocol version 4 ----
    CHECK(gattCall(driver, CHR_FEEDBACK, "StartNotify", {}).type() == QDBusMessage::ReplyMessage);
    {
        const QDBusMessage r = gattCall(driver, CHR_CAPS, "ReadValue", {QVariantMap()});
        const QByteArray caps = r.arguments().value(0).toByteArray();
        CHECK(caps.contains("<version>4</version>"));
        CHECK(caps.contains("<threads>1</threads>"));
        CHECK(caps.contains("<icons>48</icons>"));
        CHECK(caps.contains("<iconcache>"));
        // The token stays while the cache does
        const QByteArray again = gattCall(driver, CHR_CAPS, "ReadValue", {QVariantMap()}).arguments().value(0).toByteArray();
        CHECK(caps == again);
        fprintf(stderr, "capabilities: %s\n", caps.constData());
    }

    // A conversation: two messages from Dana, one card
    int before = postedCount(fake);
    write(driver, "<insert><id>50</id><vb>normal</vb><pn>org.example.chat</pn><an>Chat</an><su>Dana</su>"
                  "<bo>Are you coming?</bo><ac>reply,open,mute</ac><th>c0ffee01</th>"
                  "<ms><m><s>Dana</s><t>Hi</t></m><m><s>Dana</s><t>Are you coming?</t></m></ms><pr>4</pr></insert>");
    CHECK(waitFor([&] { return postedCount(fake) == before + 1; }));
    uint dana = 0;
    {
        const Posted p = lastPosted(fake);
        CHECK(p.replacesId == 0);
        CHECK(p.body == "Hi\nAre you coming?");
        CHECK(p.hints.value("x-nemo-preview-body").toString() == "Are you coming?");
        CHECK(p.hints.value("x-nemo-feedback").toString() == "notif_normal");
        CHECK(p.hints.value("x-asteroid-thread").toString() == "c0ffee01");
        QMutexLocker l(&fake.lock);
        dana = fake.nextId - 1;
    }
    // The app re-posts it (new phone id, same content): updated in place, quietly
    write(driver, "<insert><id>51</id><vb>normal</vb><pn>org.example.chat</pn><an>Chat</an><su>Dana</su>"
                  "<bo>Are you coming?</bo><ac>reply,open,mute</ac><th>c0ffee01</th>"
                  "<ms><m><s>Dana</s><t>Hi</t></m><m><s>Dana</s><t>Are you coming?</t></m></ms></insert>");
    CHECK(waitFor([&] { return postedCount(fake) == before + 2; }));
    {
        const Posted p = lastPosted(fake);
        CHECK(p.replacesId == dana);
        CHECK(!p.hints.contains("x-nemo-preview-body"));
        CHECK(p.hints.value("x-nemo-feedback").toString() == "notif_silent");
        CHECK(p.hints.value("x-nemo-feedback-suppressed").toBool());
        CHECK(p.hints.value("x-asteroid-phone-id").toString() == "51");
    }
    // ...and removes the old id: the card stays
    int closedBefore = 0;
    {
        QMutexLocker l(&fake.lock);
        closedBefore = fake.closed.size();
    }
    write(driver, "<removed><id>50</id></removed>");
    spin(NotificationService::CloseDelayMs + 300);
    {
        QMutexLocker l(&fake.lock);
        CHECK(fake.closed.size() == closedBefore);
    }
    // A new message: the card comes anew, at the top, with banner and vibration
    write(driver, "<insert><id>52</id><vb>normal</vb><pn>org.example.chat</pn><an>Chat</an><su>Dana</su>"
                  "<bo>Hello?</bo><ac>reply,open,mute</ac><th>c0ffee01</th>"
                  "<ms><m><s>Dana</s><t>Hi</t></m><m><s>Dana</s><t>Are you coming?</t></m>"
                  "<m><s>Me</s><t>Yes</t></m><m><s>Dana</s><t>Hello?</t></m></ms></insert>");
    CHECK(waitFor([&] { return postedCount(fake) == before + 3; }));
    {
        const Posted p = lastPosted(fake);
        CHECK(p.replacesId == 0);
        CHECK(p.body == "Dana: Hi\nDana: Are you coming?\nMe: Yes\nDana: Hello?");
        CHECK(p.hints.value("x-nemo-preview-body").toString() == "Hello?");
        CHECK(p.hints.value("x-nemo-feedback").toString() == "notif_normal");
        QMutexLocker l(&fake.lock);
        CHECK(fake.closed.size() == closedBefore + 1 && fake.closed.last() == dana);
        dana = fake.nextId - 1;
        closedBefore = fake.closed.size();
    }
    // Removed and posted again in a moment (Messenger): the card stays
    write(driver, "<removed><id>52</id></removed>");
    spin(300);
    write(driver, "<insert><id>53</id><vb>normal</vb><pn>org.example.chat</pn><an>Chat</an><su>Dana</su>"
                  "<bo>Hello?</bo><ac>reply,open,mute</ac><th>c0ffee01</th>"
                  "<ms><m><s>Dana</s><t>Hi</t></m><m><s>Dana</s><t>Are you coming?</t></m>"
                  "<m><s>Me</s><t>Yes</t></m><m><s>Dana</s><t>Hello?</t></m></ms></insert>");
    CHECK(waitFor([&] { return postedCount(fake) == before + 4; }));
    CHECK(lastPosted(fake).replacesId == dana);
    spin(NotificationService::CloseDelayMs + 300);
    {
        QMutexLocker l(&fake.lock);
        CHECK(fake.closed.size() == closedBefore);
    }
    // The watch dismisses it: the phone hears about its current id
    collector.feedback.clear();
    {
        QDBusMessage s = QDBusMessage::createSignal("/org/freedesktop/Notifications", "org.freedesktop.Notifications", "NotificationClosed");
        s << dana << 2u;
        fakeConn.send(s);
    }
    CHECK(waitFor([&] { return collector.feedback.endsWith("</action>"); }));
    CHECK(collector.feedback == "<action><id>53</id><ty>dismiss</ty></action>");
    // Removed for good: closed after the delay
    write(driver, "<insert><id>54</id><vb>normal</vb><an>Chat</an><su>Eve</su><bo>Bye</bo><th>c0ffee02</th></insert>");
    CHECK(waitFor([&] { return postedCount(fake) == before + 5; }));
    uint eve = 0;
    {
        QMutexLocker l(&fake.lock);
        eve = fake.nextId - 1;
        closedBefore = fake.closed.size();
    }
    write(driver, "<removed><id>54</id></removed>");
    spin(200);
    {
        QMutexLocker l(&fake.lock);
        CHECK(fake.closed.size() == closedBefore);
    }
    CHECK(waitFor([&] { QMutexLocker l(&fake.lock); return fake.closed.size() == closedBefore + 1 && fake.closed.last() == eve; }));

    // Unimportant (Android importance low): no banner, no vibration
    write(driver, "<insert><id>60</id><vb>strong</vb><an>Shop</an><su>Sale</su><bo>Today only</bo><pr>2</pr></insert>");
    CHECK(waitFor([&] { return postedCount(fake) == before + 6; }));
    CHECK(!lastPosted(fake).hints.contains("x-nemo-preview-body"));
    CHECK(lastPosted(fake).hints.value("x-nemo-feedback").toString() == "notif_silent");

    // Only alert once: new content updates in place, quietly
    write(driver, "<insert><id>61</id><vb>normal</vb><an>Files</an><su>Upload</su><bo>10%</bo><th>up01</th><oa>1</oa></insert>");
    CHECK(waitFor([&] { return postedCount(fake) == before + 7; }));
    uint upload = 0;
    {
        QMutexLocker l(&fake.lock);
        upload = fake.nextId - 1;
    }
    CHECK(lastPosted(fake).hints.value("x-nemo-feedback").toString() == "notif_normal");
    write(driver, "<insert><id>62</id><vb>normal</vb><an>Files</an><su>Upload</su><bo>50%</bo><th>up01</th><oa>1</oa></insert>");
    CHECK(waitFor([&] { return postedCount(fake) == before + 8; }));
    CHECK(lastPosted(fake).replacesId == upload);
    CHECK(lastPosted(fake).hints.value("x-nemo-feedback").toString() == "notif_silent");
    CHECK(lastPosted(fake).body == "50%");
    // Ongoing: the same
    write(driver, "<insert><id>63</id><vb>normal</vb><an>Files</an><su>Upload</su><bo>90%</bo><th>up01</th><og>1</og></insert>");
    CHECK(waitFor([&] { return postedCount(fake) == before + 9; }));
    CHECK(lastPosted(fake).replacesId == upload);
    CHECK(!lastPosted(fake).hints.contains("x-nemo-preview-body"));

    // A phone alarm going off: the alarm vibration without sound (the phone
    // rings, as Wear OS), and its category for the launcher
    write(driver, "<insert><id>70</id><vb>alarm</vb><pn>com.example.clock</pn><an>Clock</an><su>Alarm</su><bo>07:00</bo>"
                  "<ac>open,mute</ac><ax><a>Snooze</a><a>Dismiss</a></ax><ca>alarm</ca><pr>5</pr></insert>");
    CHECK(waitFor([&] { return postedCount(fake) == before + 10; }));
    {
        const Posted p = lastPosted(fake);
        CHECK(p.hints.value("x-nemo-feedback").toString() == "eos_phone_alarm");
        CHECK(p.hints.value("x-asteroid-category").toString() == "alarm");
        CHECK(p.hints.value("x-asteroid-app-actions").toString() == "Snooze\nDismiss");
    }

    // Do Not Disturb: quiet, but the alarm still rings
    service.setDoNotDisturb(true);
    write(driver, "<insert><id>71</id><vb>strong</vb><an>Mail</an><su>Frank</su><bo>Report</bo></insert>");
    CHECK(waitFor([&] { return postedCount(fake) == before + 11; }));
    CHECK(lastPosted(fake).hints.value("x-nemo-feedback").toString() == "notif_silent");
    CHECK(!lastPosted(fake).hints.contains("x-nemo-preview-body"));
    write(driver, "<insert><id>72</id><vb>alarm</vb><an>Clock</an><su>Timer</su><bo>Time's up</bo><ca>alarm</ca></insert>");
    CHECK(waitFor([&] { return postedCount(fake) == before + 12; }));
    CHECK(lastPosted(fake).hints.value("x-nemo-feedback").toString() == "eos_phone_alarm");
    service.setDoNotDisturb(false);

    // App icons: one icon in two writes, then a notification that uses it
    {
        static const char *png1x1 = "iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAYAAAAfFcSJAAAADUlEQVR42mNkYPhfDwAChwGA60e6kgAAAABJRU5ErkJggg==";
        const QByteArray icon = QByteArray("<icon><pn>org.example.chat</pn><h>ab12cd34</h><d>") + png1x1 + "</d></icon>";
        QVariantMap options;
        options.insert("mtu", 185);
        CHECK(gattCall(driver, CHR_ICON, "WriteValue", {icon.left(60), options}).type() == QDBusMessage::ReplyMessage);
        CHECK(gattCall(driver, CHR_ICON, "WriteValue", {icon.mid(60), options}).type() == QDBusMessage::ReplyMessage);
        const QString path = NotificationService::iconPath("org.example.chat", "ab12cd34");
        CHECK(waitFor([&] { return QFile::exists(path); }));
        QFile f(path);
        CHECK(f.open(QIODevice::ReadOnly));
        const QByteArray svg = f.readAll();
        CHECK(svg.startsWith("<svg") && svg.contains(QByteArray("data:image/png;base64,") + png1x1));
        // Not an icon: bad package, not a PNG, not base64
        CHECK(!NotificationService::storeIcon("../evil", "ab", png1x1));
        CHECK(!NotificationService::storeIcon("org.example.x", "ab", QByteArray("aGVsbG8=")));
        CHECK(!NotificationService::storeIcon("org.example.x", "ab", QByteArray("<svg/>")));
        CHECK(!NotificationService::storeIcon("org.example.x", "AB!", png1x1));

        write(driver, "<insert><id>80</id><vb>none</vb><pn>org.example.chat</pn><an>Chat</an><ai>ios-chatbubbles-outline</ai>"
                      "<su>Gus</su><bo>Yo</bo><ic>ab12cd34</ic></insert>");
        CHECK(waitFor([&] { return postedCount(fake) == before + 13; }));
        QString shown;
        {
            QMutexLocker l(&fake.lock);
            shown = fake.posted.last().appIcon;
        }
        // The launcher's Icon looks in /usr/share/icons/asteroid/NAME.svg
        CHECK(shown.startsWith("../"));
        const QString resolved = QDir::cleanPath(QStringLiteral("/usr/share/icons/asteroid/") + shown + ".svg");
        fprintf(stderr, "icon name %s -> %s\n", qPrintable(shown), qPrintable(resolved));
        // Lexically "/../../../PATH"; the kernel stops ".." at the root
        QString fromRoot = resolved;
        while(fromRoot.startsWith(QLatin1String("/../")))
            fromRoot = fromRoot.mid(3);
        CHECK(fromRoot == path);
        CHECK(QFile::exists(QStringLiteral("/") + shown + ".svg"));
        // An icon the watch does not have: the icon name
        write(driver, "<insert><id>81</id><vb>none</vb><pn>org.example.chat</pn><an>Chat</an><ai>ios-chatbubbles-outline</ai>"
                      "<su>Gus</su><bo>Yo</bo><ic>ffff0000</ic></insert>");
        CHECK(waitFor([&] { return postedCount(fake) == before + 14; }));
        {
            QMutexLocker l(&fake.lock);
            CHECK(fake.posted.last().appIcon == "ios-chatbubbles-outline");
        }
    }

    // Dismissed while the phone is away: sent when it is back, and its
    // resync does not bring the card back
    write(driver, "<insert><id>90</id><vb>none</vb><an>Mail</an><su>Hal</su><bo>Hi</bo></insert>");
    CHECK(waitFor([&] { return postedCount(fake) == before + 15; }));
    uint hal = 0;
    {
        QMutexLocker l(&fake.lock);
        hal = fake.nextId - 1;
    }
    CHECK(gattCall(driver, CHR_FEEDBACK, "StopNotify", {}).type() == QDBusMessage::ReplyMessage);
    collector.feedback.clear();
    {
        QDBusMessage s = QDBusMessage::createSignal("/org/freedesktop/Notifications", "org.freedesktop.Notifications", "NotificationClosed");
        s << hal << 2u;
        fakeConn.send(s);
    }
    spin(300);
    CHECK(collector.feedback.isEmpty());
    write(driver, "<insert><id>90</id><vb>none</vb><an>Mail</an><su>Hal</su><bo>Hi</bo><rs>1</rs></insert>");
    spin(300);
    CHECK(postedCount(fake) == before + 15);
    CHECK(gattCall(driver, CHR_FEEDBACK, "StartNotify", {}).type() == QDBusMessage::ReplyMessage);
    CHECK(waitFor([&] { return collector.feedback.endsWith("</action>"); }, 4000));
    CHECK(collector.feedback == "<action><id>90</id><ty>dismiss</ty></action>");

    // The link goes, BlueZ keeps the subscription (a bonded phone out of range):
    // a dismiss waits as well, and goes out once the phone is back and set up
    service.setLinkConnected(true);
    CHECK(service.phoneConnected());
    write(driver, "<insert><id>91</id><vb>none</vb><an>Mail</an><su>Ida</su><bo>Hi</bo></insert>");
    CHECK(waitFor([&] { return postedCount(fake) == before + 16; }));
    uint ida = 0;
    {
        QMutexLocker l(&fake.lock);
        ida = fake.nextId - 1;
    }
    service.setLinkConnected(false);
    CHECK(!service.phoneConnected());
    collector.feedback.clear();
    {
        QDBusMessage s = QDBusMessage::createSignal("/org/freedesktop/Notifications", "org.freedesktop.Notifications", "NotificationClosed");
        s << ida << 2u;
        fakeConn.send(s);
    }
    spin(300);
    CHECK(collector.feedback.isEmpty());
    service.setLinkConnected(true);
    CHECK(service.phoneConnected());
    spin(1000);
    CHECK(collector.feedback.isEmpty());   // the phone is still setting itself up
    CHECK(waitFor([&] { return collector.feedback.endsWith("</action>"); }, NotificationService::PhoneSetupMs + 3000));
    CHECK(collector.feedback == "<action><id>91</id><ty>dismiss</ty></action>");

    // Calls after a (re)connection: the phone never lists them in its resync, and a call
    // that ended while the watch was away (down, out of range) would keep its card, and
    // the launcher's in-call view, up for hours. A call card the phone does not post again
    // within the grace of a (re)connection goes; one it posts again (a live call) stays,
    // and so does anything but a call. Closed by the watch: no dismiss to the phone.
    service.setCallGraceMs(700);
    {
        const int calls = postedCount(fake);
        collector.feedback.clear();
        write(driver, "<insert><id>120</id><vb>none</vb><an>Ongoing call</an><su>Kim</su><ac>end</ac></insert>");
        write(driver, "<insert><id>121</id><vb>ringtone</vb><an>Phone</an><su>Lou</su><ac>answer,reject,silence</ac></insert>");
        write(driver, "<insert><id>122</id><vb>none</vb><an>Mail</an><su>Max</su><bo>Hi</bo></insert>");
        CHECK(waitFor([&] { return postedCount(fake) == calls + 3; }));
        uint kim = 0, lou = 0, max = 0;
        {
            QMutexLocker l(&fake.lock);
            kim = fake.nextId - 3;
            lou = fake.nextId - 2;
            max = fake.nextId - 1;
        }
        spin(1000);
        CHECK(!wasClosed(fake, kim) && !wasClosed(fake, lou));   // posted on this connection: live
        service.setLinkConnected(false);
        service.setLinkConnected(true);
        // The phone posts the live call again (its state changed, or it re-asserts calls)
        write(driver, "<insert><id>120</id><vb>none</vb><an>Ongoing call</an><su>Kim</su><ac>end</ac></insert>");
        CHECK(waitFor([&] { return postedCount(fake) == calls + 4; }));
        spin(300);
        CHECK(!wasClosed(fake, lou));                            // not before the grace is over
        CHECK(waitFor([&] { return wasClosed(fake, lou); }, 3000));
        spin(300);
        CHECK(!wasClosed(fake, kim));
        CHECK(!wasClosed(fake, max));
        // Nothing from the phone about the call at the next reconnection: it is over
        service.setLinkConnected(false);
        service.setLinkConnected(true);
        CHECK(waitFor([&] { return wasClosed(fake, kim); }, 3000));
        CHECK(!wasClosed(fake, max));
        CHECK(collector.feedback.isEmpty());
    }
    service.setCallGraceMs(NotificationService::CallGraceMs);

    // Framing. Split inside the closing tag: still posted
    int count = postedCount(fake);
    write(driver, "<insert><id>95</id><vb>none</vb><an>Mail</an><su>Lee</su><bo>Split</bo></ins");
    spin(200);
    CHECK(postedCount(fake) == count);
    write(driver, "ert>");
    CHECK(waitFor([&] { return postedCount(fake) == count + 1; }));
    CHECK(lastPosted(fake).summary == "Lee");
    CHECK(lastPosted(fake).body == "Split");
    // A long write: chunks with offsets, the second starting with a child element
    {
        const QByteArray xml = "<insert><id>96</id><vb>none</vb><an>Mail</an><su>Lee</su><bo>Chunks</bo><rs>1</rs></insert>";
        writeAt(driver, xml.left(40), 0);
        writeAt(driver, xml.mid(40, 30), 40);
        writeAt(driver, xml.mid(70), 70);
        CHECK(waitFor([&] { return postedCount(fake) == count + 2; }));
        CHECK(lastPosted(fake).body == "Chunks");
    }
    // The rest of a write never came (the phone went mid-write), then a new
    // notification: the stale part is dropped, the new one comes as it is
    write(driver, "<insert><id>97</id><vb>none</vb><an>Mail</an><su>Lost</su>");
    write(driver, "<insert><id>98</id><vb>none</vb><an>Mail</an><su>Lee</su><bo>Sent while away</bo><rs>1</rs></insert>");
    CHECK(waitFor([&] { return postedCount(fake) == count + 3; }));
    CHECK(lastPosted(fake).summary == "Lee");
    CHECK(lastPosted(fake).body == "Sent while away");
    spin(200);
    CHECK(postedCount(fake) == count + 3);
    // ... also when the disconnection is all that tells
    write(driver, "<insert><id>99</id><vb>none</vb><an>Mail</an><su>Cut</su>");
    service.setLinkConnected(false);
    service.setLinkConnected(true);
    write(driver, "<bo>tail</bo></insert>");   // the end of the cut one: not a notification
    write(driver, "<insert><id>100</id><vb>none</vb><an>Mail</an><su>Lee</su><bo>Next</bo></insert>");
    CHECK(waitFor([&] { return postedCount(fake) == count + 4; }));
    CHECK(lastPosted(fake).body == "Next");
    spin(200);
    CHECK(postedCount(fake) == count + 4);
    // Two messages in one write, and whitespace between writes
    write(driver, "<insert><id>101</id><vb>none</vb><an>Mail</an><su>Lee</su><bo>One</bo></insert>\n"
                  "<removed><id>100</id></removed>\n");
    CHECK(waitFor([&] { return postedCount(fake) == count + 5; }));
    CHECK(lastPosted(fake).body == "One");

    // A reply from the watch, as on Wear OS: the text goes to the phone and
    // the card stays; the phone's update with the reply replaces it in
    // place without an alert; the next message alerts again
    {
        const char *kimFirst = "<insert><id>195</id><vb>normal</vb><pn>org.example.chat</pn><an>Chat</an><su>Kim</su>"
                               "<bo>are you coming?</bo><ac>reply,open</ac><th>c0ffee19</th>"
                               "<ms><m><s>Kim</s><t>are you coming?</t></m></ms></insert>";
        before = postedCount(fake);
        write(driver, kimFirst);
        CHECK(waitFor([&] { return postedCount(fake) == before + 1; }));
        uint kim = 0;
        int closedNow = 0;
        {
            QMutexLocker l(&fake.lock);
            kim = fake.nextId - 1;
            closedNow = fake.closed.size();
        }
        collector.feedback.clear();
        collector.largestPiece = 0;
        QDBusMessage m = QDBusMessage::createMethodCall(SERVICE_NAME, BTSYNCD_NOTIF_PATH, BTSYNCD_NOTIF_IFACE, "Reply");
        m << kim << QStringLiteral("  OK  ");
        const QDBusMessage r = driver.call(m, QDBus::BlockWithGui, 5000);
        CHECK(r.type() == QDBusMessage::ReplyMessage && r.arguments().value(0).toBool());
        CHECK(waitFor([&] { return collector.feedback.endsWith("</action>"); }));
        CHECK(collector.feedback == "<action><id>195</id><ty>reply</ty><tx>OK</tx></action>");
        // the phone wrote with an MTU of 185: one notification
        CHECK(collector.largestPiece == collector.feedback.size());
        // a long reply (a voice one) whole, in pieces of at most MTU - 3
        collector.feedback.clear();
        collector.largestPiece = 0;
        const QString longText = QStringLiteral("I am on my way, see you at eight \u00e9t\u00e9 ") + QString(200, QLatin1Char('x'));
        QDBusMessage m2 = QDBusMessage::createMethodCall(SERVICE_NAME, BTSYNCD_NOTIF_PATH, BTSYNCD_NOTIF_IFACE, "Reply");
        m2 << kim << longText;
        CHECK(driver.call(m2, QDBus::BlockWithGui, 5000).arguments().value(0).toBool());
        CHECK(waitFor([&] { return collector.feedback.endsWith("</action>"); }));
        CHECK(QString::fromUtf8(collector.feedback) == QStringLiteral("<action><id>195</id><ty>reply</ty><tx>") + longText.trimmed() + QStringLiteral("</tx></action>"));
        CHECK(collector.largestPiece <= 182 && collector.feedback.size() > 182);
        spin(300);
        {
            QMutexLocker l(&fake.lock);
            CHECK(fake.closed.size() == closedNow);
        }
        // the phone's update, with the reply
        write(driver, "<insert><id>195</id><vb>normal</vb><pn>org.example.chat</pn><an>Chat</an><su>Kim</su>"
                      "<bo>OK</bo><ac>reply,open</ac><th>c0ffee19</th>"
                      "<ms><m><s>Kim</s><t>are you coming?</t></m><m><s>Me</s><t>OK</t></m></ms></insert>");
        CHECK(waitFor([&] { return postedCount(fake) == before + 2; }));
        {
            const Posted p = lastPosted(fake);
            CHECK(p.replacesId == kim);
            CHECK(p.body == "Kim: are you coming?\nMe: OK");
            CHECK(!p.hints.contains("x-nemo-preview-body"));
            CHECK(p.hints.value("x-nemo-feedback").toString() == "notif_silent");
            QMutexLocker l(&fake.lock);
            CHECK(fake.closed.size() == closedNow);
        }
        // Kim answers: a banner and a vibration again
        write(driver, "<insert><id>195</id><vb>normal</vb><pn>org.example.chat</pn><an>Chat</an><su>Kim</su>"
                      "<bo>great</bo><ac>reply,open</ac><th>c0ffee19</th>"
                      "<ms><m><s>Kim</s><t>are you coming?</t></m><m><s>Me</s><t>OK</t></m>"
                      "<m><s>Kim</s><t>great</t></m></ms></insert>");
        CHECK(waitFor([&] { return postedCount(fake) == before + 3; }));
        {
            const Posted p = lastPosted(fake);
            CHECK(p.hints.value("x-nemo-preview-body").toString() == "great");
            CHECK(p.hints.value("x-nemo-feedback").toString() == "notif_normal");
        }
    }

    // Keys: "Jo: Coffee?" sent again under a new phone id (a resync after the phone
    // forgot the id) updates its card instead of a second one
    count = postedCount(fake);
    uint jo = 0, max = 0, ann = 0;
    write(driver, "<insert><id>300</id><vb>none</vb><an>Notitest</an><su>Jo</su><bo>Coffee?</bo><ky>00000000000000aa</ky></insert>");
    CHECK(waitFor([&] { return postedCount(fake) == count + 1; }));
    {
        QMutexLocker l(&fake.lock);
        jo = fake.nextId - 1;
    }
    write(driver, "<insert><id>301</id><vb>none</vb><an>Notitest</an><su>Jo</su><bo>Coffee?</bo><ky>00000000000000aa</ky><rs>1</rs></insert>");
    CHECK(waitFor([&] { return postedCount(fake) == count + 2; }));
    CHECK(lastPosted(fake).replacesId == jo);
    // "Max", a card without a key; the phone's list has only Jo (under its new id): Max goes
    write(driver, "<insert><id>302</id><vb>none</vb><an>Notitest</an><su>Max</su><bo>Dinner tonight?</bo></insert>");
    CHECK(waitFor([&] { return postedCount(fake) == count + 3; }));
    {
        QMutexLocker l(&fake.lock);
        max = fake.nextId - 1;
    }
    service.setSyncGraceMs(0);
    write(driver, "<sync><n><id>301</id><ky>00000000000000aa</ky></n></sync>");
    CHECK(waitFor([&] { QMutexLocker l(&fake.lock); return fake.closed.contains(max); }));
    spin(300);
    {
        QMutexLocker l(&fake.lock);
        CHECK(!fake.closed.contains(jo));
    }
    // A card posted while the list was on its way stays
    service.setSyncGraceMs(60000);
    write(driver, "<insert><id>303</id><vb>none</vb><an>Notitest</an><su>Ann</su><bo>New</bo></insert>");
    CHECK(waitFor([&] { return postedCount(fake) == count + 4; }));
    {
        QMutexLocker l(&fake.lock);
        ann = fake.nextId - 1;
    }
    write(driver, "<sync><n><id>301</id><ky>00000000000000aa</ky></n></sync>");
    spin(500);
    {
        QMutexLocker l(&fake.lock);
        CHECK(!fake.closed.contains(ann));
    }
    // ... and goes with a later list that does not have it, here in two writes
    service.setSyncGraceMs(0);
    write(driver, "<sync><n><id>30");
    write(driver, "1</id></n></sync>");
    CHECK(waitFor([&] { QMutexLocker l(&fake.lock); return fake.closed.contains(ann); }));
    // Jo stays (by its id now), and its removal under the new id closes it
    {
        QMutexLocker l(&fake.lock);
        CHECK(!fake.closed.contains(jo));
    }
    write(driver, "<removed><id>301</id></removed>");
    CHECK(waitFor([&] { QMutexLocker l(&fake.lock); return fake.closed.contains(jo); }));

    // The decisions on their own
    {
        PhoneNotification n;
        n.vibrate = "normal";
        CHECK(PostPlan::decide(n, false, true, false).kind == PostPlan::New);
        CHECK(PostPlan::decide(n, false, true, false).vibrate);
        CHECK(PostPlan::decide(n, true, true, false).kind == PostPlan::Repost);
        CHECK(PostPlan::decide(n, true, false, false).kind == PostPlan::Update);
        CHECK(!PostPlan::decide(n, true, false, false).vibrate);
        n.vibrate = "none";
        CHECK(PostPlan::decide(n, false, true, false).preview && !PostPlan::decide(n, false, true, false).vibrate);
        n.vibrate = "ringtone";
        n.phoneActions = QStringList() << "answer" << "reject";
        CHECK(PostPlan::decide(n, true, true, false).kind == PostPlan::Update);
        CHECK(PostPlan::decide(n, true, true, false).vibrate);
        CHECK(!PostPlan::decide(n, true, true, true).vibrate);
        n.phoneActions = QStringList() << "found";
        CHECK(PostPlan::decide(n, false, true, true).vibrate);
        n.phoneActions.clear();
    }
    // The vibration (ngfd event) of each <vb>: a call or an alarm on the phone
    // only vibrates on the watch, the find-my-watch alert rings it
    {
        PhoneNotification n;
        CHECK(PostPlan::feedbackEvent(n) == "notif_strong");
        n.vibrate = "normal";
        CHECK(PostPlan::feedbackEvent(n) == "notif_normal");
        n.vibrate = "strong";
        CHECK(PostPlan::feedbackEvent(n) == "notif_strong");
        n.vibrate = "ringtone";
        n.phoneActions = QStringList() << "answer" << "reject";
        CHECK(PostPlan::feedbackEvent(n) == "eos_phone_ringing");
        n.phoneActions.clear();
        CHECK(PostPlan::feedbackEvent(n) == "eos_phone_ringing");
        n.phoneActions = QStringList() << "found";
        CHECK(PostPlan::feedbackEvent(n) == "ringtone");
        n.phoneActions.clear();
        n.vibrate = "alarm";
        CHECK(PostPlan::feedbackEvent(n) == "eos_phone_alarm");
    }
    {
        PhoneNotification n;
        n.vibrate = "strong";
        n.resync = true;
        CHECK(!PostPlan::decide(n, false, true, false).preview);
        CHECK(PostPlan::decide(n, true, true, false).kind == PostPlan::Update);
    }

    fakeThread.quit();
    fakeThread.wait();
    if(failures)
        fprintf(stderr, "%d check(s) failed\n", failures);
    else
        fprintf(stderr, "notifications_test: all checks passed\n");
    return failures ? 1 : 0;
}

#include "notifications_test.moc"
