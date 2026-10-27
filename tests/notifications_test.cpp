/*
 * Tests of the notification service (protocol version 3) on a private D-Bus
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
 */

#include "notificationservice.h"
#include "common.h"

#include <QCoreApplication>
#include <QDBusConnection>
#include <QDBusMessage>
#include <QDBusMetaType>
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

struct Posted {
    QString appName, summary, body;
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
    uint Notify(const QString &appName, uint replacesId, const QString &, const QString &summary,
                const QString &body, const QStringList &actions, const QVariantMap &hints, int)
    {
        QMutexLocker l(&lock);
        Posted p;
        p.appName = appName;
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
    QList<bool> findPhoneChanges;
public slots:
    void propertiesChanged(const QString &, const QVariantMap &changed, const QStringList &)
    {
        if(changed.contains("Value"))
            feedback += changed.value("Value").toByteArray();
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
        CHECK(caps.contains("<version>3</version>"));
        CHECK(caps.contains("<findphone>1</findphone>"));
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

    fakeThread.quit();
    fakeThread.wait();
    if(failures)
        fprintf(stderr, "%d check(s) failed\n", failures);
    else
        fprintf(stderr, "notifications_test: all checks passed\n");
    return failures ? 1 : 0;
}

#include "notifications_test.moc"
