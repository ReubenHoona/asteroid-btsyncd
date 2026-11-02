/*
 * Tests of the modes on the phone status service (capabilities version 2)
 * on a private D-Bus (cmake -DBTSYNCD_TESTS=ON), run like notifications_test:
 *   dbus-run-session -- sh -c 'DBUS_SYSTEM_BUS_ADDRESS=$DBUS_SESSION_BUS_ADDRESS modes_test'
 * with a dconf database of its own (the dconf keys are the watch's):
 *   - the watch's Bedtime goes to the phone as its Do Not Disturb; the
 *     switch between DND and Bedtime (the launcher's modes engine: the new
 *     one on first, then the other off) sends nothing
 *   - the phone's DND on: a watch in Bedtime stays in Bedtime; its DND off
 *     ends the watch's Bedtime; <bed> from a phone that can tell
 *   - Bedtime and Theater silence phone notifications as DND does; alarms
 *     come through
 *   - the phone away while Bedtime changes: sent when it is back, and its
 *     own older state does not undo it
 */

#include "phonestatusservice.h"
#include "notificationservice.h"
#include "common.h"

#include <MDConfItem>
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
static const char *CHR_UPDATE = "/org/asteroidos/btsyncd/service0/char0";
static const char *CHR_STATUS = "/org/asteroidos/btsyncd/service9/char0";
static const char *CHR_REQUESTS = "/org/asteroidos/btsyncd/service9/char1";

class FakeLipstick : public QObject
{
    Q_OBJECT
    Q_CLASSINFO("D-Bus Interface", "org.freedesktop.Notifications")
public:
    QMutex lock;
    QStringList summaries;
    QList<QVariantMap> hints;
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
    void CloseNotification(uint) {}
};

class Collector : public QObject
{
    Q_OBJECT
public:
    QByteArray requests;
public slots:
    void propertiesChanged(const QString &, const QVariantMap &changed, const QStringList &)
    {
        if(changed.contains("Value"))
            requests += changed.value("Value").toByteArray();
    }
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

static void write(QDBusConnection &driver, const char *chr, const QByteArray &value)
{
    QVariantMap options;
    options.insert("mtu", 512);
    CHECK(gattCall(driver, chr, "WriteValue", {value, options}).type() == QDBusMessage::ReplyMessage);
}

// the dconf keys as the quick panel and the modes engine write them
static MDConfItem *dndKey, *bedtimeKey, *theaterKey;
static void setKey(MDConfItem *key, bool on)
{
    key->set(on);
    key->sync();
}

static int lastNotification(FakeLipstick &fake, QVariantMap *hints)
{
    QMutexLocker l(&fake.lock);
    if(hints && !fake.hints.isEmpty())
        *hints = fake.hints.last();
    return fake.summaries.size();
}

// A message from the phone; true if it alerted (a banner: lipstick's preview)
static bool notifyAlerts(QDBusConnection &driver, FakeLipstick &fake, int id, const char *vibrate)
{
    const int before = lastNotification(fake, nullptr);
    const QByteArray xml = QByteArray("<insert><pn>org.example.chat</pn><id>") + QByteArray::number(id)
            + "</id><an>Chat</an><su>Someone</su><bo>A message</bo><vb>" + vibrate + "</vb></insert>";
    write(driver, CHR_UPDATE, xml);
    QVariantMap hints;
    CHECK(waitFor([&] { return lastNotification(fake, &hints) > before; }));
    return hints.contains("x-nemo-preview-body") || hints.contains("x-nemo-preview-summary");
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

    dndKey = new MDConfItem(PhoneStatusService::DndKey);
    bedtimeKey = new MDConfItem(PhoneStatusService::BedtimeKey);
    theaterKey = new MDConfItem(PhoneStatusService::TheaterKey);
    setKey(dndKey, false);
    setKey(bedtimeKey, false);
    setKey(theaterKey, false);

    NotificationService notifications(0, QDBusConnection::systemBus());
    notifications.setOnCharger(false);
    PhoneStatusService phone(9, QDBusConnection::systemBus(), &notifications);
    phone.setDisconnectNoticeEnabled(false);
    btsyncdName = QDBusConnection::systemBus().baseService();

    QDBusConnection driver = QDBusConnection::connectToBus(QDBusConnection::SessionBus, "driver");
    Collector collector;
    CHECK(driver.connect(btsyncdName, CHR_REQUESTS, "org.freedesktop.DBus.Properties", "PropertiesChanged",
                         &collector, SLOT(propertiesChanged(QString, QVariantMap, QStringList))));
    spin(200);

    // The phone connects and says it is not quiet
    CHECK(gattCall(driver, CHR_FEEDBACK, "StartNotify", {}).type() == QDBusMessage::ReplyMessage);
    CHECK(gattCall(driver, CHR_REQUESTS, "StartNotify", {}).type() == QDBusMessage::ReplyMessage);
    CHECK(waitFor([&] { return phone.connected(); }));
    write(driver, CHR_STATUS, "<ps><v>4</v><dnd>off</dnd></ps>");
    spin(300);
    CHECK(!phone.quiet());
    CHECK(collector.requests.isEmpty());

    // Not quiet: a message alerts
    CHECK(notifyAlerts(driver, fake, 1, "normal"));

    // Bedtime on the watch: the phone's DND comes on (as GB 44 understands)
    setKey(bedtimeKey, true);
    CHECK(waitFor([&] { return collector.requests.endsWith("</req>"); }));
    CHECK(collector.requests == "<req><dnd>on</dnd></req>");
    CHECK(phone.bedtime() && phone.quiet() && !phone.doNotDisturb());
    CHECK(notifications.modeQuiet());
    // ...and a message does not alert; an alarm on the phone does
    CHECK(!notifyAlerts(driver, fake, 2, "normal"));
    CHECK(notifyAlerts(driver, fake, 3, "alarm"));
    // The phone follows: its DND on keeps the watch in Bedtime
    write(driver, CHR_STATUS, "<ps><dnd>priority</dnd></ps>");
    spin(300);
    CHECK(phone.bedtime() && !phone.doNotDisturb());
    CHECK(!dndKey->value(false).toBool());

    // Do Not Disturb on the watch while in Bedtime: DND on, then (the
    // engine) Bedtime off; the phone stays quiet, nothing is sent
    collector.requests.clear();
    setKey(dndKey, true);
    setKey(bedtimeKey, false);
    spin(PhoneStatusService::QuietSettleMs + 500);
    CHECK(collector.requests.isEmpty());
    CHECK(phone.doNotDisturb() && !phone.bedtime() && phone.quiet());
    CHECK(!notifications.modeQuiet() && notifications.doNotDisturb());
    // ...and back to Bedtime (Bedtime on, then DND off): nothing either
    setKey(bedtimeKey, true);
    setKey(dndKey, false);
    spin(PhoneStatusService::QuietSettleMs + 500);
    CHECK(collector.requests.isEmpty());
    CHECK(phone.bedtime() && !phone.doNotDisturb());

    // The phone's DND goes off (its Bedtime ended, or the user): the watch's
    // Bedtime ends too, and nothing goes back
    write(driver, CHR_STATUS, "<ps><dnd>off</dnd></ps>");
    CHECK(waitFor([&] { return !bedtimeKey->value(false).toBool(); }));
    CHECK(!phone.quiet());
    CHECK(!notifications.modeQuiet());
    spin(PhoneStatusService::QuietSettleMs + 300);
    CHECK(collector.requests.isEmpty());

    // The phone's DND comes on: the watch's DND (not Bedtime)
    write(driver, CHR_STATUS, "<ps><dnd>alarms</dnd></ps>");
    CHECK(waitFor([&] { return dndKey->value(false).toBool(); }));
    CHECK(!phone.bedtime());
    write(driver, CHR_STATUS, "<ps><dnd>off</dnd></ps>");
    CHECK(waitFor([&] { return !dndKey->value(false).toBool(); }));

    // A phone that can tell its Bedtime: <bed> sets the watch's, nothing back
    write(driver, CHR_STATUS, "<ps><v>5</v><bed>on</bed><dnd>priority</dnd></ps>");
    CHECK(waitFor([&] { return bedtimeKey->value(false).toBool(); }));
    CHECK(!dndKey->value(false).toBool());
    write(driver, CHR_STATUS, "<ps><bed>off</bed><dnd>off</dnd></ps>");
    CHECK(waitFor([&] { return !bedtimeKey->value(false).toBool(); }));
    spin(PhoneStatusService::QuietSettleMs + 300);
    CHECK(collector.requests.isEmpty());

    // Theater mode: notifications silent, nothing to the phone (not synced)
    setKey(theaterKey, true);
    CHECK(waitFor([&] { return notifications.modeQuiet(); }));
    CHECK(!notifyAlerts(driver, fake, 4, "normal"));
    spin(PhoneStatusService::QuietSettleMs + 300);
    CHECK(collector.requests.isEmpty());
    CHECK(!phone.quiet());
    setKey(theaterKey, false);
    CHECK(waitFor([&] { return !notifications.modeQuiet(); }));
    CHECK(notifyAlerts(driver, fake, 5, "normal"));

    // The phone goes out of range; Bedtime comes on meanwhile: not sent into
    // the void; back, after its setup: sent, and its older state ignored
    collector.requests.clear();
    notifications.setLinkConnected(true);
    spin(100);
    notifications.setLinkConnected(false);
    CHECK(waitFor([&] { return !phone.connected(); }));
    setKey(bedtimeKey, true);
    spin(PhoneStatusService::QuietSettleMs + 500);
    CHECK(collector.requests.isEmpty());
    CHECK(phone.bedtime());
    spin(1000);
    notifications.setLinkConnected(true);
    CHECK(waitFor([&] { return phone.connected(); }));
    // the phone sets itself up and says its DND is off (from before)
    write(driver, CHR_STATUS, "<ps><v>4</v><dnd>off</dnd></ps>");
    spin(300);
    CHECK(phone.bedtime());
    CHECK(waitFor([&] { return collector.requests.endsWith("</req>"); }, NotificationService::PhoneSetupMs + 3000));
    CHECK(collector.requests == "<req><dnd>on</dnd></req>");
    write(driver, CHR_STATUS, "<ps><dnd>priority</dnd></ps>");   // the phone follows
    spin(300);
    CHECK(phone.bedtime() && !phone.doNotDisturb());
    // Away again: Bedtime to DND and back meanwhile: nothing to send once back
    collector.requests.clear();
    notifications.setLinkConnected(false);
    CHECK(waitFor([&] { return !phone.connected(); }));
    setKey(dndKey, true);
    setKey(bedtimeKey, false);
    spin(PhoneStatusService::QuietSettleMs + 300);
    setKey(bedtimeKey, true);
    setKey(dndKey, false);
    spin(PhoneStatusService::QuietSettleMs + 300);
    notifications.setLinkConnected(true);
    CHECK(waitFor([&] { return phone.connected(); }));
    spin(NotificationService::PhoneSetupMs + 1000);
    CHECK(collector.requests.isEmpty());
    CHECK(phone.bedtime() && !phone.doNotDisturb());

    // ...and the user ends it on the watch: the phone's DND off
    collector.requests.clear();
    setKey(bedtimeKey, false);
    CHECK(waitFor([&] { return collector.requests.endsWith("</req>"); }));
    CHECK(collector.requests == "<req><dnd>off</dnd></req>");

    fakeThread.quit();
    fakeThread.wait();
    delete dndKey;
    delete bedtimeKey;
    delete theaterKey;
    if(failures)
        fprintf(stderr, "%d check(s) failed\n", failures);
    else
        fprintf(stderr, "modes_test: all checks passed\n");
    return failures ? 1 : 0;
}

#include "modes_test.moc"
