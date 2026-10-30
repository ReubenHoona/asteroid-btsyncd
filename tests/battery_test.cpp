/*
 * Tests of the battery service (Battery Level and Battery Level Status) with a
 * fake mce on a private D-Bus (cmake -DBTSYNCD_TESTS=ON), run like
 * notifications_test:
 *   dbus-run-session -- sh -c 'DBUS_SYSTEM_BUS_ADDRESS=$DBUS_SESSION_BUS_ADDRESS battery_test'
 *   - the Battery Level Status bytes for every charger state, battery state
 *     and battery status mce has
 *   - mce's values at the start; plugged in, charging, full, paused on the
 *     charger, unplugged, low, critical, unknown, as mce signals them
 *   - a notification only when the value changes: none for the same state
 *     again, none for the level, none while nothing happens
 */

#include "batteryservice.h"
#include "common.h"

#include <QCoreApplication>
#include <QDBusConnection>
#include <QDBusMessage>
#include <QDBusVariant>
#include <QElapsedTimer>
#include <QMutex>
#include <QMutexLocker>
#include <QThread>
#include <QVariantMap>
#include <cstdio>
#include <functional>

static int failures = 0;

#define CHECK(cond) do { if (!(cond)) { fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); failures++; } } while (0)

static const char *CHR_LEVEL = "/org/asteroidos/btsyncd/service3/char0";
static const char *CHR_STATUS = "/org/asteroidos/btsyncd/service3/char1";

// mce's request interface, as asteroid-btsyncd asks it
class FakeMce : public QObject
{
    Q_OBJECT
    Q_CLASSINFO("D-Bus Interface", "com.nokia.mce.request")
public:
    QMutex lock;
    int level = 58;
    QString charger = "off", state = "discharging", status = "ok";

public slots:
    int get_battery_level() { QMutexLocker l(&lock); return level; }
    QString get_charger_state() { QMutexLocker l(&lock); return charger; }
    QString get_battery_state() { QMutexLocker l(&lock); return state; }
    QString get_battery_status() { QMutexLocker l(&lock); return status; }
};

class Collector : public QObject
{
    Q_OBJECT
public:
    QList<QByteArray> values;
public slots:
    void propertiesChanged(const QString &iface, const QVariantMap &changed, const QStringList &)
    {
        if(iface == GATT_CHRC_IFACE && changed.contains("Value"))
            values << changed.value("Value").toByteArray();
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

static QByteArray readValue(QDBusConnection &driver, const char *path)
{
    QDBusMessage m = QDBusMessage::createMethodCall(btsyncdName, path, GATT_CHRC_IFACE, "ReadValue");
    m.setArguments({QVariantMap()});
    const QDBusMessage r = driver.call(m, QDBus::BlockWithGui, 5000);
    CHECK(r.type() == QDBusMessage::ReplyMessage);
    return r.arguments().value(0).toByteArray();
}

static QVariant property(QDBusConnection &driver, const char *path, const char *name)
{
    QDBusMessage m = QDBusMessage::createMethodCall(btsyncdName, path, "org.freedesktop.DBus.Properties", "Get");
    m.setArguments({GATT_CHRC_IFACE, name});
    const QDBusMessage r = driver.call(m, QDBus::BlockWithGui, 5000);
    CHECK(r.type() == QDBusMessage::ReplyMessage);
    return r.arguments().value(0).value<QDBusVariant>().variant();
}

static QDBusConnection *mceConn;

// a signal as mce sends it (from the connection that owns com.nokia.mce)
static void mce(const char *name, const QVariant &arg)
{
    QDBusMessage m = QDBusMessage::createSignal("/com/nokia/mce/signal", "com.nokia.mce.signal", name);
    m << arg;
    CHECK(mceConn->send(m));
}

static QByteArray hex(const char *h)
{
    return QByteArray::fromHex(h);
}

// no two notifications in a row with the same value
static bool allChanges(const QList<QByteArray> &values)
{
    for(int i = 1; i < values.size(); i++)
        if(values[i] == values[i - 1])
            return false;
    return true;
}

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);

    // The bytes: Flags 0, then the Power State, little endian
    CHECK(BatteryLvlStatusChrc::encode("off", "discharging", "ok") == hex("00c100"));       // present, discharging: active, good
    CHECK(BatteryLvlStatusChrc::encode("off", "not_charging", "ok") == hex("00c100"));
    CHECK(BatteryLvlStatusChrc::encode("on", "charging", "ok") == hex("00a300"));           // wired, charging, good
    CHECK(BatteryLvlStatusChrc::encode("on", "full", "full") == hex("00e300"));             // wired, discharging: inactive, good
    CHECK(BatteryLvlStatusChrc::encode("on", "not_charging", "ok") == hex("00e340"));       // wired, inactive, fault: other
    CHECK(BatteryLvlStatusChrc::encode("on", "discharging", "ok") == hex("00c300"));        // wired, discharging: active
    CHECK(BatteryLvlStatusChrc::encode("off", "charging", "ok") == hex("00a100"));
    CHECK(BatteryLvlStatusChrc::encode("off", "full", "full") == hex("00e100"));
    CHECK(BatteryLvlStatusChrc::encode("off", "discharging", "low") == hex("004101"));      // low
    CHECK(BatteryLvlStatusChrc::encode("off", "discharging", "empty") == hex("00c101"));    // critical
    CHECK(BatteryLvlStatusChrc::encode("on", "charging", "low") == hex("002301"));
    CHECK(BatteryLvlStatusChrc::encode("unknown", "unknown", "unknown") == hex("000500"));  // present, wired unknown
    CHECK(BatteryLvlStatusChrc::encode("", "", "") == hex("000500"));
    CHECK(BatteryLvlStatusChrc::encode("bogus", "bogus", "bogus") == hex("000500"));
    CHECK(BatteryLvlStatusChrc::encode("on", "unknown", "ok") == hex("008300"));
    CHECK(BatteryLvlStatusChrc::encode("unknown", "discharging", "ok") == hex("00c500"));

    if(qEnvironmentVariableIsEmpty("DBUS_SESSION_BUS_ADDRESS") || qEnvironmentVariableIsEmpty("DBUS_SYSTEM_BUS_ADDRESS")) {
        fprintf(stderr, "needs a private bus as session and system bus (see the top of the source)\n");
        return 2;
    }

    QThread fakeThread;
    fakeThread.start();
    FakeMce fake;
    fake.moveToThread(&fakeThread);
    QDBusConnection fakeConn = QDBusConnection::connectToBus(QDBusConnection::SessionBus, "fake-mce");
    mceConn = &fakeConn;
    CHECK(fakeConn.registerObject("/com/nokia/mce/request", &fake, QDBusConnection::ExportAllSlots));
    CHECK(fakeConn.registerService("com.nokia.mce"));

    BatteryService battery(3, QDBusConnection::systemBus());
    btsyncdName = QDBusConnection::systemBus().baseService();

    QDBusConnection driver = QDBusConnection::connectToBus(QDBusConnection::SessionBus, "driver");
    Collector level, status;
    CHECK(driver.connect(btsyncdName, CHR_LEVEL, "org.freedesktop.DBus.Properties", "PropertiesChanged",
                         &level, SLOT(propertiesChanged(QString, QVariantMap, QStringList))));
    CHECK(driver.connect(btsyncdName, CHR_STATUS, "org.freedesktop.DBus.Properties", "PropertiesChanged",
                         &status, SLOT(propertiesChanged(QString, QVariantMap, QStringList))));

    // The characteristic, in the battery service next to the level
    CHECK(property(driver, CHR_STATUS, "UUID").toString() == "00002bed-0000-1000-8000-00805f9b34fb");
    CHECK(property(driver, CHR_STATUS, "Flags").toStringList() == QStringList({"encrypt-authenticated-read", "encrypt-authenticated-notify"}));
    CHECK(property(driver, CHR_LEVEL, "UUID").toString() == "00002a19-0000-1000-8000-00805f9b34fb");

    // mce's values at the start: on the battery, 58 %
    CHECK(waitFor([&] { return readValue(driver, CHR_STATUS) == hex("00c100"); }));
    CHECK(waitFor([&] { return readValue(driver, CHR_LEVEL) == QByteArray(1, 58); }));
    spin(300);
    level.values.clear();
    status.values.clear();

    // Plugged in: mce says the charger, then the battery charges
    mce("charger_state_ind", "on");
    CHECK(waitFor([&] { return status.values.size() == 1; }));
    CHECK(status.values.value(0) == hex("00c300"));
    mce("battery_state_ind", "charging");
    CHECK(waitFor([&] { return status.values.size() == 2; }));
    CHECK(status.values.value(1) == hex("00a300"));
    CHECK(readValue(driver, CHR_STATUS) == hex("00a300"));
    // the level rises: the level is notified, the status is not
    mce("battery_level_ind", 59);
    CHECK(waitFor([&] { return level.values.size() == 1; }));
    CHECK(level.values.value(0) == QByteArray(1, 59));
    // the same again (or what makes the same bytes): nothing
    mce("battery_level_ind", 59);
    mce("charger_state_ind", "on");
    mce("battery_state_ind", "charging");
    mce("battery_status_ind", "ok");
    spin(400);
    CHECK(status.values.size() == 2);
    CHECK(level.values.size() == 1);

    // Full on the charger: discharging: inactive, the charger connected
    mce("battery_level_ind", 100);
    mce("battery_state_ind", "full");
    CHECK(waitFor([&] { return status.values.size() == 3; }));
    CHECK(status.values.value(2) == hex("00e300"));
    mce("battery_status_ind", "full");   // still "good"
    spin(400);
    CHECK(status.values.size() == 3);

    // On the charger without charging (paused: too hot, too cold, given up)
    mce("battery_state_ind", "not_charging");
    CHECK(waitFor([&] { return status.values.size() == 4; }));
    CHECK(status.values.value(3) == hex("00e340"));
    mce("battery_state_ind", "charging");
    CHECK(waitFor([&] { return status.values.size() == 5; }));
    CHECK(status.values.value(4) == hex("00a300"));

    // Unplugged: the charger goes, then the battery discharges
    mce("charger_state_ind", "off");
    CHECK(waitFor([&] { return status.values.size() == 6; }));
    CHECK(status.values.value(5) == hex("00a100"));
    mce("battery_state_ind", "discharging");
    CHECK(waitFor([&] { return status.values.size() == 7; }));
    CHECK(status.values.value(6) == hex("00c100"));
    // "not charging" without the charger is discharging too
    mce("battery_state_ind", "not_charging");
    spin(400);
    CHECK(status.values.size() == 7);

    // Low, then critical (mce's low and empty)
    mce("battery_status_ind", "low");
    CHECK(waitFor([&] { return status.values.size() == 8; }));
    CHECK(status.values.value(7) == hex("004101"));
    mce("battery_status_ind", "empty");
    CHECK(waitFor([&] { return status.values.size() == 9; }));
    CHECK(status.values.value(8) == hex("00c101"));

    // mce does not know the charger
    mce("charger_state_ind", "unknown");
    CHECK(waitFor([&] { return status.values.size() == 10; }));
    CHECK(status.values.value(9) == hex("00c501"));

    // Plugged in again, everything at once (one mce update): the last value is
    // the state, no value twice in a row
    mce("charger_state_ind", "on");
    mce("battery_state_ind", "charging");
    mce("battery_status_ind", "ok");
    CHECK(waitFor([&] { return status.values.value(status.values.size() - 1) == hex("00a300"); }));
    spin(300);
    CHECK(status.values.size() >= 11 && status.values.size() <= 13);
    CHECK(status.values.last() == hex("00a300"));
    CHECK(readValue(driver, CHR_STATUS) == hex("00a300"));
    CHECK(allChanges(status.values));

    // Nothing happens: nothing is sent (no timer)
    const int levels = level.values.size(), statuses = status.values.size();
    spin(1500);
    CHECK(level.values.size() == levels);
    CHECK(status.values.size() == statuses);

    fakeThread.quit();
    fakeThread.wait();
    if(failures)
        fprintf(stderr, "%d check(s) failed\n", failures);
    else
        fprintf(stderr, "battery_test: all checks passed (%d status notifications)\n", int(status.values.size()));
    return failures ? 1 : 0;
}

#include "battery_test.moc"
