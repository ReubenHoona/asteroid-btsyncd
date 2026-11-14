/*
 * Copyright (C) 2026 eos-mainline contributors
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

/*
 * Tests of "Share heart rate" (heartrateservice.h) on a private bus with a dconf-service, like
 * weather_test, with a fake BlueZ (the LE advertising manager) and a fake heart rate sensor:
 *   - nothing is advertised and the sensor is off while the toggle is off; a subscription is refused
 *   - toggle on: the advertisement of 0x180D is registered with BlueZ; a subscriber starts the
 *     sensor; each reading goes out as a Heart Rate Measurement (flags 0x06/0x04, 8 or 16 bits)
 *   - at most one notification per 800 ms, none for a reading older than 3 s
 *   - the last subscriber leaving, or the toggle going off, stops the sensor; off unregisters
 *   - no timer in the service
 */

#include "common.h"
#include "heartrateservice.h"

#include <MDConfItem>
#include <QCoreApplication>
#include <QDBusArgument>
#include <QDBusConnection>
#include <QDBusContext>
#include <QDBusMessage>
#include <QDBusObjectPath>
#include <QElapsedTimer>
#include <QTimer>
#include <QVariantMap>
#include <cstdio>
#include <functional>

static int failures = 0;

#define CHECK(cond) do { if (!(cond)) { fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); failures++; } } while (0)

static const char *CHR_MEASURE = "/org/asteroidos/btsyncd/service11/char0";
static const char *CHR_LOCATION = "/org/asteroidos/btsyncd/service11/char1";

static QString btsyncdName;

class FakeAdvertisingManager : public QObject, public QDBusContext
{
    Q_OBJECT
    Q_CLASSINFO("D-Bus Interface", "org.bluez.LEAdvertisingManager1")
public:
    QStringList registered, unregistered;
    bool fail = false;

public slots:
    void RegisterAdvertisement(const QDBusObjectPath &path, const QVariantMap &)
    {
        if (fail) {
            sendErrorReply(QStringLiteral("org.bluez.Error.Failed"), QStringLiteral("Maximum advertisements reached"));
            return;
        }
        registered << path.path();
    }
    void UnregisterAdvertisement(const QDBusObjectPath &path) { unregistered << path.path(); }
};

class FakeSource : public HeartRateSource
{
    Q_OBJECT
public:
    int starts = 0, stops = 0;
    bool running = false;
    void start() override { starts++; running = true; }
    void stop() override { stops++; running = false; }
    void feed(int bpm, bool contact, int age) { emit reading(bpm, contact, age); }
};

class Collector : public QObject
{
    Q_OBJECT
public:
    QList<QByteArray> values;
public slots:
    void propertiesChanged(const QString &, const QVariantMap &c, const QStringList &)
    {
        if (c.contains("Value"))
            values << c.value("Value").toByteArray();
    }
};

static bool waitFor(const std::function<bool()> &cond, int ms = 10000)
{
    QElapsedTimer t;
    t.start();
    while (!cond() && t.elapsed() < ms)
        QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
    return cond();
}

static void spin(int ms) { waitFor([] { return false; }, ms); }

#define EVENTUALLY(cond) CHECK(waitFor([&] { return (cond); }))

static QDBusMessage gattCall(QDBusConnection &driver, const char *path, const char *method, const QList<QVariant> &args = {})
{
    QDBusMessage m = QDBusMessage::createMethodCall(btsyncdName, path, GATT_CHRC_IFACE, method);
    m.setArguments(args);
    return driver.call(m, QDBus::BlockWithGui, 5000);
}

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    if (qEnvironmentVariableIsEmpty("DBUS_SESSION_BUS_ADDRESS") || qEnvironmentVariableIsEmpty("DBUS_SYSTEM_BUS_ADDRESS")) {
        fprintf(stderr, "needs a private bus as session and system bus (see the top of the source)\n");
        return 2;
    }

    // the notification value
    CHECK(HeartRateService::measurement(72, true) == QByteArray("\x06\x48", 2));
    CHECK(HeartRateService::measurement(255, true) == QByteArray("\x06\xff", 2));
    CHECK(HeartRateService::measurement(256, true) == QByteArray("\x07\x00\x01", 3));
    CHECK(HeartRateService::measurement(300, true) == QByteArray("\x07\x2c\x01", 3));
    CHECK(HeartRateService::measurement(70000, true) == QByteArray("\x07\xff\xff", 3));
    CHECK(HeartRateService::measurement(0, false) == QByteArray("\x04\x00", 2));
    CHECK(HeartRateService::measurement(72, false) == QByteArray("\x04\x00", 2));   // no skin, no rate
    CHECK(HeartRateService::measurement(-3, true) == QByteArray("\x06\x00", 2));

    // the fake BlueZ on the bus the service advertises on
    QDBusConnection bluez = QDBusConnection::connectToBus(QDBusConnection::SystemBus, "fake-bluez");
    FakeAdvertisingManager manager;
    CHECK(bluez.registerObject("/org/bluez/hci0", &manager, QDBusConnection::ExportAllSlots));
    CHECK(bluez.registerService("org.bluez"));

    MDConfItem share(SHARE_HEART_RATE_KEY);
    share.set(false);
    EVENTUALLY(!MDConfItem(SHARE_HEART_RATE_KEY).value(true).toBool());

    FakeSource *source = new FakeSource;
    HeartRateService hrs(11, QDBusConnection::systemBus(), source);
    btsyncdName = QDBusConnection::systemBus().baseService();
    QDBusConnection driver = QDBusConnection::connectToBus(QDBusConnection::SessionBus, "driver");
    Collector collector;
    CHECK(driver.connect(btsyncdName, CHR_MEASURE, "org.freedesktop.DBus.Properties", "PropertiesChanged",
                         &collector, SLOT(propertiesChanged(QString, QVariantMap, QStringList))));
    spin(200);

    CHECK(hrs.getUuid().toLower() == "0000180d-0000-1000-8000-00805f9b34fb");
    CHECK(hrs.getCharacteristics().size() == 2);
    CHECK(hrs.getCharacteristics().value(0)->getUuid() == "00002a37-0000-1000-8000-00805f9b34fb");
    CHECK(hrs.getCharacteristics().value(0)->getFlags() == QStringList({"notify"}));       // no pairing for gym equipment
    CHECK(hrs.getCharacteristics().value(1)->getUuid() == "00002a38-0000-1000-8000-00805f9b34fb");
    CHECK(hrs.getCharacteristics().value(1)->getFlags() == QStringList({"read"}));
    // no timer: a notification per reading of the sensor
    CHECK(hrs.findChildren<QTimer *>().isEmpty());

    // The body sensor location: wrist (2)
    {
        const QDBusMessage r = gattCall(driver, CHR_LOCATION, "ReadValue", {QVariantMap()});
        CHECK(r.type() == QDBusMessage::ReplyMessage && r.arguments().value(0).toByteArray() == QByteArray("\x02", 1));
    }

    // ---- the toggle is off ----
    CHECK(!hrs.sharing() && !hrs.advertising() && !hrs.measuring());
    spin(300);
    CHECK(manager.registered.isEmpty());
    {
        const QDBusMessage r = gattCall(driver, CHR_MEASURE, "StartNotify");
        CHECK(r.type() == QDBusMessage::ErrorMessage && r.errorName() == "org.bluez.Error.NotPermitted");
    }
    CHECK(!hrs.notifying() && source->starts == 0 && !source->running);
    source->feed(72, true, 0);
    spin(100);
    CHECK(collector.values.isEmpty());

    // ---- on: advertised, nobody listening, the sensor stays off ----
    share.set(true);
    EVENTUALLY(manager.registered.size() == 1);
    CHECK(manager.registered.value(0) == "/org/asteroidos/btsyncd/advertisement_hrs");
    EVENTUALLY(hrs.advertising());
    CHECK(hrs.sharing() && !hrs.measuring() && source->starts == 0);
    // what BlueZ reads from the advertisement
    {
        QDBusMessage m = QDBusMessage::createMethodCall(btsyncdName, HeartRateAdvertisement::Path, "org.freedesktop.DBus.Properties", "GetAll");
        m.setArguments({QString(LE_ADVERTISEMENT_IFACE)});
        const QDBusMessage r = driver.call(m, QDBus::BlockWithGui, 5000);
        CHECK(r.type() == QDBusMessage::ReplyMessage);
        const QVariantMap p = qdbus_cast<QVariantMap>(r.arguments().value(0));
        CHECK(p.value("Type").toString() == "peripheral");
        CHECK(p.value("ServiceUUIDs").toStringList() == QStringList({"0000180d-0000-1000-8000-00805f9b34fb"}));
        CHECK(p.value("Discoverable").toBool() == true);
        CHECK(p.value("Includes").toStringList() == QStringList({"local-name"}));
        CHECK(p.value("MinInterval").toUInt() == 1000 && p.value("MaxInterval").toUInt() == 1500);   // slow, for the battery
    }
    source->feed(72, true, 0);
    spin(100);
    CHECK(collector.values.isEmpty());

    // ---- a central subscribes ----
    CHECK(gattCall(driver, CHR_MEASURE, "StartNotify").type() == QDBusMessage::ReplyMessage);
    EVENTUALLY(source->running && hrs.measuring() && hrs.notifying());
    CHECK(source->starts == 1);
    source->feed(72, true, 0);
    EVENTUALLY(collector.values.size() == 1);
    CHECK(collector.values.value(0) == QByteArray("\x06\x48", 2));
    // faster than 800 ms: dropped
    source->feed(73, true, 0);
    spin(100);
    CHECK(collector.values.size() == 1);
    spin(HeartRateService::MinNotifyIntervalMs);
    source->feed(74, true, 0);
    EVENTUALLY(collector.values.size() == 2);
    CHECK(collector.values.value(1) == QByteArray("\x06\x4a", 2));
    // a reading older than 3 s (sensorfw replays the last one at a start) is not sent; 3 s is
    spin(HeartRateService::MinNotifyIntervalMs + 50);
    source->feed(75, true, HeartRateService::MaxReadingAgeMs + 1);
    spin(100);
    CHECK(collector.values.size() == 2);
    source->feed(76, true, HeartRateService::MaxReadingAgeMs);
    EVENTUALLY(collector.values.size() == 3);
    CHECK(collector.values.value(2) == QByteArray("\x06\x4c", 2));
    // above 255: 16 bits; the sensor off the skin: contact not detected, rate 0
    spin(HeartRateService::MinNotifyIntervalMs + 50);
    source->feed(300, true, 0);
    EVENTUALLY(collector.values.size() == 4);
    CHECK(collector.values.value(3) == QByteArray("\x07\x2c\x01", 3));
    spin(HeartRateService::MinNotifyIntervalMs + 50);
    source->feed(0, false, 0);
    EVENTUALLY(collector.values.size() == 5);
    CHECK(collector.values.value(4) == QByteArray("\x04\x00", 2));

    // ---- the toggle goes off while a central listens ----
    share.set(false);
    EVENTUALLY(manager.unregistered.size() == 1 && !hrs.advertising());
    CHECK(manager.unregistered.value(0) == "/org/asteroidos/btsyncd/advertisement_hrs");
    EVENTUALLY(!source->running);
    CHECK(source->stops == 1 && !hrs.measuring());
    spin(HeartRateService::MinNotifyIntervalMs + 50);
    source->feed(80, true, 0);
    spin(100);
    CHECK(collector.values.size() == 5);
    // back on: advertised again, and the central BlueZ still counts gets its values again
    share.set(true);
    EVENTUALLY(manager.registered.size() == 2 && source->running);
    CHECK(source->starts == 2);
    spin(HeartRateService::MinNotifyIntervalMs + 50);
    source->feed(81, true, 0);
    EVENTUALLY(collector.values.size() == 6);
    CHECK(collector.values.value(5) == QByteArray("\x06\x51", 2));

    // ---- the last central leaves ----
    CHECK(gattCall(driver, CHR_MEASURE, "StopNotify").type() == QDBusMessage::ReplyMessage);
    EVENTUALLY(!source->running && !hrs.notifying());
    CHECK(hrs.advertising());          // still advertised: the toggle is on
    spin(HeartRateService::MinNotifyIntervalMs + 50);
    source->feed(82, true, 0);
    spin(100);
    CHECK(collector.values.size() == 6);

    // ---- BlueZ cannot advertise: not advertising, and the toggle works again afterwards ----
    manager.fail = true;
    share.set(false);
    EVENTUALLY(manager.unregistered.size() == 2);
    share.set(true);
    EVENTUALLY(!hrs.advertising() && manager.registered.size() == 2);
    spin(200);
    CHECK(!hrs.advertising());
    manager.fail = false;
    share.set(false);
    spin(300);
    share.set(true);
    EVENTUALLY(hrs.advertising() && manager.registered.size() == 3);

    if (failures)
        fprintf(stderr, "%d check(s) failed\n", failures);
    else
        fprintf(stderr, "heartrate_test: all checks passed\n");
    return failures ? 1 : 0;
}

#include "heartrate_test.moc"
