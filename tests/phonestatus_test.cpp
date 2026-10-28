/*
 * Tests of the phone status service (protocol version 4) on a private D-Bus
 * (cmake -DBTSYNCD_TESTS=ON), run like notifications_test:
 *   dbus-run-session -- sh -c 'DBUS_SYSTEM_BUS_ADDRESS=$DBUS_SESSION_BUS_ADDRESS phonestatus_test'
 *   - capabilities
 *   - the phone's battery and DND (split writes) -> session API and signals;
 *     the notification service follows the DND
 *   - the watch's DND to the phone; switched while no phone listened: sent
 *     when it listens again, and the phone's older state does not undo it
 *   - a bonded phone out of range (BlueZ keeps its subscriptions, the link
 *     goes): disconnected, no card (Wear OS); the watch's DND waits and goes
 *     out after the phone's setup when it is back
 *   - the quiet "Phone disconnected" card (opt-in) after the phone is gone a
 *     while, closed when it is back
 */

#include "phonestatusservice.h"
#include "notificationservice.h"
#include "common.h"

#include <QCoreApplication>
#include <QDBusConnection>
#include <QDBusMessage>
#include <QElapsedTimer>
#include <QMutex>
#include <QMutexLocker>
#include <QThread>
#include <QVariantMap>
#include <cstdio>
#include <functional>

static int failures = 0;

#define CHECK(cond) do { if (!(cond)) { fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); failures++; } } while (0)

static const char *CHR_FEEDBACK = "/org/asteroidos/btsyncd/service0/char1";
static const char *CHR_STATUS = "/org/asteroidos/btsyncd/service9/char0";
static const char *CHR_REQUESTS = "/org/asteroidos/btsyncd/service9/char1";
static const char *CHR_PCAPS = "/org/asteroidos/btsyncd/service9/char2";

class FakeLipstick : public QObject
{
    Q_OBJECT
    Q_CLASSINFO("D-Bus Interface", "org.freedesktop.Notifications")
public:
    QMutex lock;
    QStringList summaries;
    QList<QVariantMap> hints;
    QList<uint> closed;
    uint nextId = 500;

public slots:
    uint Notify(const QString &, uint replacesId, const QString &, const QString &summary,
                const QString &, const QStringList &, const QVariantMap &h, int)
    {
        QMutexLocker l(&lock);
        summaries << summary;
        hints << h;
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
    QByteArray requests;
    QList<bool> connectedChanges, dndChanges;
    QList<int> batteryChanges;
public slots:
    void propertiesChanged(const QString &, const QVariantMap &changed, const QStringList &)
    {
        if(changed.contains("Value"))
            requests += changed.value("Value").toByteArray();
    }
    void connectedChanged(bool c) { connectedChanges << c; }
    void batteryChanged(int level, bool) { batteryChanges << level; }
    void dndChanged(bool on) { dndChanges << on; }
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

static void writeStatus(QDBusConnection &driver, const QByteArray &value)
{
    QVariantMap options;
    options.insert("mtu", 23);
    CHECK(gattCall(driver, CHR_STATUS, "WriteValue", {value, options}).type() == QDBusMessage::ReplyMessage);
}

static QVariant api(QDBusConnection &driver, const char *method, const QList<QVariant> &args = {})
{
    QDBusMessage m = QDBusMessage::createMethodCall(SERVICE_NAME, BTSYNCD_PHONE_PATH, BTSYNCD_PHONE_IFACE, method);
    m.setArguments(args);
    const QDBusMessage r = driver.call(m, QDBus::BlockWithGui, 5000);
    CHECK(r.type() == QDBusMessage::ReplyMessage);
    return r.arguments().value(0);
}

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    if(qEnvironmentVariableIsEmpty("DBUS_SESSION_BUS_ADDRESS") || qEnvironmentVariableIsEmpty("DBUS_SYSTEM_BUS_ADDRESS")) {
        fprintf(stderr, "needs a private bus as session and system bus (see the top of the source)\n");
        return 2;
    }

    QThread fakeThread;
    fakeThread.start();
    FakeLipstick fake;
    fake.moveToThread(&fakeThread);
    QDBusConnection fakeConn = QDBusConnection::connectToBus(QDBusConnection::SessionBus, "fake-lipstick");
    CHECK(fakeConn.registerObject("/org/freedesktop/Notifications", &fake, QDBusConnection::ExportAllSlots));
    CHECK(fakeConn.registerService("org.freedesktop.Notifications"));

    NotificationService notifications(0, QDBusConnection::systemBus());
    PhoneStatusService phone(9, QDBusConnection::systemBus(), &notifications);
    phone.setDisconnectNoticeMs(800);
    btsyncdName = QDBusConnection::systemBus().baseService();

    QDBusConnection driver = QDBusConnection::connectToBus(QDBusConnection::SessionBus, "driver");
    Collector collector;
    CHECK(driver.connect(btsyncdName, CHR_REQUESTS, "org.freedesktop.DBus.Properties", "PropertiesChanged",
                         &collector, SLOT(propertiesChanged(QString, QVariantMap, QStringList))));
    CHECK(driver.connect(SERVICE_NAME, BTSYNCD_PHONE_PATH, BTSYNCD_PHONE_IFACE, "ConnectedChanged",
                         &collector, SLOT(connectedChanged(bool))));
    CHECK(driver.connect(SERVICE_NAME, BTSYNCD_PHONE_PATH, BTSYNCD_PHONE_IFACE, "BatteryChanged",
                         &collector, SLOT(batteryChanged(int, bool))));
    CHECK(driver.connect(SERVICE_NAME, BTSYNCD_PHONE_PATH, BTSYNCD_PHONE_IFACE, "DoNotDisturbChanged",
                         &collector, SLOT(dndChanged(bool))));
    spin(200);

    {
        const QDBusMessage r = gattCall(driver, CHR_PCAPS, "ReadValue", {QVariantMap()});
        CHECK(r.arguments().value(0).toByteArray() == "<capabilities><version>1</version><battery>1</battery><dnd>1</dnd></capabilities>");
    }
    CHECK(api(driver, "BatteryLevel").toInt() == -1);
    CHECK(api(driver, "Connected").toBool() == false);
    CHECK(api(driver, "DoNotDisturb").toBool() == false);

    // The phone connects (feedback + requests), and says how it is, in pieces
    CHECK(gattCall(driver, CHR_FEEDBACK, "StartNotify", {}).type() == QDBusMessage::ReplyMessage);
    CHECK(gattCall(driver, CHR_REQUESTS, "StartNotify", {}).type() == QDBusMessage::ReplyMessage);
    CHECK(waitFor([&] { return collector.connectedChanges == QList<bool>() << true; }));
    CHECK(api(driver, "Connected").toBool() == true);
    writeStatus(driver, "<ps><v>4</v><bl>83</bl>");
    writeStatus(driver, "<bc>1</bc><dnd>priority</dnd></ps>");
    CHECK(waitFor([&] { return !collector.batteryChanges.isEmpty() && !collector.dndChanges.isEmpty(); }));
    CHECK(collector.batteryChanges == QList<int>() << 83);
    CHECK(collector.dndChanges == QList<bool>() << true);
    CHECK(api(driver, "BatteryLevel").toInt() == 83);
    CHECK(api(driver, "Charging").toBool() == true);
    CHECK(api(driver, "DoNotDisturb").toBool() == true);
    CHECK(notifications.doNotDisturb());
    CHECK(phone.phoneVersion() == 4);
    // From the phone: nothing goes back to it
    spin(200);
    CHECK(collector.requests.isEmpty());
    // The same battery again: no signal
    writeStatus(driver, "<ps><bl>83</bl><bc>1</bc></ps>");
    spin(200);
    CHECK(collector.batteryChanges.size() == 1);

    // The user switches DND off on the watch: the phone is told
    CHECK(api(driver, "SetDoNotDisturb", {false}).toBool() == true);
    CHECK(waitFor([&] { return collector.requests.endsWith("</req>"); }));
    CHECK(collector.requests == "<req><dnd>off</dnd></req>");
    CHECK(!notifications.doNotDisturb());
    // The phone's status from before it followed: does not undo it
    writeStatus(driver, "<ps><dnd>priority</dnd></ps>");
    spin(200);
    CHECK(api(driver, "DoNotDisturb").toBool() == false);
    // The phone followed; later it switches DND on by itself
    writeStatus(driver, "<ps><dnd>off</dnd></ps>");
    writeStatus(driver, "<ps><dnd>alarms</dnd></ps>");
    CHECK(waitFor([&] { return api(driver, "DoNotDisturb").toBool() == true; }));

    // The phone stops listening; the watch's DND changes meanwhile
    collector.requests.clear();
    CHECK(gattCall(driver, CHR_REQUESTS, "StopNotify", {}).type() == QDBusMessage::ReplyMessage);
    CHECK(api(driver, "SetDoNotDisturb", {false}).toBool() == false);
    spin(200);
    CHECK(collector.requests.isEmpty());
    // Back: it gets the watch's choice, and its own old state is ignored
    CHECK(gattCall(driver, CHR_REQUESTS, "StartNotify", {}).type() == QDBusMessage::ReplyMessage);
    CHECK(waitFor([&] { return collector.requests.endsWith("</req>"); }));
    CHECK(collector.requests == "<req><dnd>off</dnd></req>");
    writeStatus(driver, "<ps><dnd>priority</dnd></ps>");
    spin(200);
    CHECK(api(driver, "DoNotDisturb").toBool() == false);

    // A bonded phone goes out of range: BlueZ keeps its subscriptions (no
    // StopNotify), the link says it is gone. Like Wear OS: no card.
    phone.setDisconnectNoticeEnabled(false);
    collector.requests.clear();
    int changes = collector.connectedChanges.size();
    int cards = 0;
    {
        QMutexLocker l(&fake.lock);
        cards = fake.summaries.size();
    }
    notifications.setLinkConnected(true);   // the link is known from now on
    spin(100);
    CHECK(collector.connectedChanges.size() == changes);   // it was there already
    notifications.setLinkConnected(false);
    CHECK(waitFor([&] { return collector.connectedChanges.size() == changes + 1; }));
    CHECK(collector.connectedChanges.last() == false);
    CHECK(api(driver, "Connected").toBool() == false);
    // the watch's DND meanwhile is not sent into the void
    CHECK(api(driver, "SetDoNotDisturb", {true}).toBool() == false);
    spin(1200);   // longer than the notice delay of this test
    CHECK(collector.requests.isEmpty());
    {
        QMutexLocker l(&fake.lock);
        CHECK(fake.summaries.size() == cards);
    }
    // Back with the same subscription: connected; the DND after the phone's setup
    notifications.setLinkConnected(true);
    CHECK(waitFor([&] { return collector.connectedChanges.size() == changes + 2; }));
    CHECK(collector.connectedChanges.last() == true);
    CHECK(api(driver, "Connected").toBool() == true);
    spin(500);
    CHECK(collector.requests.isEmpty());
    CHECK(waitFor([&] { return collector.requests.endsWith("</req>"); }, NotificationService::PhoneSetupMs + 3000));
    CHECK(collector.requests == "<req><dnd>on</dnd></req>");
    writeStatus(driver, "<ps><dnd>priority</dnd></ps>");   // the phone follows
    spin(200);
    CHECK(api(driver, "DoNotDisturb").toBool() == true);

    // The phone goes away (opt-in notice): after a while a quiet card, gone when it is back
    phone.setDisconnectNoticeEnabled(true);
    int notified = 0;
    {
        QMutexLocker l(&fake.lock);
        notified = fake.summaries.size();
    }
    CHECK(gattCall(driver, CHR_FEEDBACK, "StopNotify", {}).type() == QDBusMessage::ReplyMessage);
    CHECK(waitFor([&] { return collector.connectedChanges.size() == changes + 3; }));
    CHECK(collector.connectedChanges.last() == false);
    spin(300);
    {
        QMutexLocker l(&fake.lock);
        CHECK(fake.summaries.size() == notified);
    }
    CHECK(waitFor([&] { QMutexLocker l(&fake.lock); return fake.summaries.size() == notified + 1; }));
    uint notice = 0;
    {
        QMutexLocker l(&fake.lock);
        CHECK(fake.summaries.last() == "Phone disconnected");
        CHECK(fake.hints.last().value("x-asteroid-phone-notice").toBool());
        CHECK(fake.hints.last().value("x-nemo-feedback").toString() == "notif_silent");
        CHECK(!fake.hints.last().contains("x-nemo-preview-body"));
        notice = fake.nextId - 1;
    }
    CHECK(gattCall(driver, CHR_FEEDBACK, "StartNotify", {}).type() == QDBusMessage::ReplyMessage);
    CHECK(waitFor([&] { QMutexLocker l(&fake.lock); return fake.closed.contains(notice); }));
    // A quick reconnection: no card
    CHECK(gattCall(driver, CHR_FEEDBACK, "StopNotify", {}).type() == QDBusMessage::ReplyMessage);
    spin(300);
    CHECK(gattCall(driver, CHR_FEEDBACK, "StartNotify", {}).type() == QDBusMessage::ReplyMessage);
    spin(900);
    {
        QMutexLocker l(&fake.lock);
        CHECK(fake.summaries.size() == notified + 1);
    }

    fakeThread.quit();
    fakeThread.wait();
    if(failures)
        fprintf(stderr, "%d check(s) failed\n", failures);
    else
        fprintf(stderr, "phonestatus_test: all checks passed\n");
    return failures ? 1 : 0;
}

#include "phonestatus_test.moc"
