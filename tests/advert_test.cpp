/*
 * Copyright (C) 2026 eos-mainline contributors
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

/*
 * Tests of the advertising schedule of the wearui-w2 wave (W2-btsyncd-notif) on a private bus with a
 * fake BlueZ (cmake -DBTSYNCD_TESTS=ON), run like notifications_test with
 * BTSYNCD_ADVERT_TIME_DIV=100 (the schedule's times divided by 100, so that its nine minutes take five seconds):
 *   dbus-run-session -- sh -c 'DBUS_SYSTEM_BUS_ADDRESS=$DBUS_SESSION_BUS_ADDRESS BTSYNCD_ADVERT_TIME_DIV=100 advert_test'
 *   - the stages: fast for the first 10 s, then 10, 20, 40, 80, 160, 320 and 540 s, the intervals
 *     only growing, within what BlueZ takes (20 ms to 10.24 s)
 *   - the advertisement's MinInterval and MaxInterval follow the stage
 *   - with a fake BlueZ: registered fast at start, seven new registrations at the stages' times
 *     and no more, none after the phone connected, fast again when it left, and BlueZ's own interval
 *     when it does not take ours
 */

#include "advertisement.h"
#include "bluezmanager.h"
#include "common.h"

#include <QCoreApplication>
#include <QDBusAbstractAdaptor>
#include <QDBusConnection>
#include <QDBusContext>
#include <QDBusMessage>
#include <QDBusMetaType>
#include <QDBusObjectPath>
#include <QElapsedTimer>
#include <QMutex>
#include <QMutexLocker>
#include <QThread>
#include <QVariantMap>
#include <cstdio>
#include <functional>
#include <memory>

static int failures = 0;

#define CHECK(cond) do { if (!(cond)) { fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); failures++; } } while (0)

typedef QMap<QString, QVariantMap> IfaceMap;
typedef QMap<QDBusObjectPath, IfaceMap> ObjMap;
Q_DECLARE_METATYPE(IfaceMap)
Q_DECLARE_METATYPE(ObjMap)

static const char *ADAPTER = "/org/bluez/hci0";
static const char *DEVICE = "/org/bluez/hci0/dev_phone";

// BlueZ's object manager: an adapter that is powered and a device that is (not) connected
class FakeObjects : public QObject
{
    Q_OBJECT
    Q_CLASSINFO("D-Bus Interface", "org.freedesktop.DBus.ObjectManager")
public:
    QMutex lock;
    bool connected = false;

public slots:
    ObjMap GetManagedObjects()
    {
        QMutexLocker l(&lock);
        ObjMap objects;
        IfaceMap adapter;
        QVariantMap props;
        props.insert("Powered", true);
        adapter.insert(ADAPTER_IFACE, props);
        adapter.insert(GATT_MANAGER_IFACE, QVariantMap());
        adapter.insert(LE_ADVERTISING_MANAGER_IFACE, QVariantMap());
        objects.insert(QDBusObjectPath(ADAPTER), adapter);
        IfaceMap device;
        QVariantMap dev;
        dev.insert("Connected", connected);
        dev.insert("Alias", "Phone");
        dev.insert("Paired", true);
        device.insert(DEVICE_MANAGER_IFACE, dev);
        objects.insert(QDBusObjectPath(DEVICE), device);
        return objects;
    }
};

// What the adapter's LE advertising manager was asked, with the advertisement's intervals at the time
class FakeAdvertising : public QObject, public QDBusContext
{
    Q_OBJECT
    Q_CLASSINFO("D-Bus Interface", LE_ADVERTISING_MANAGER_IFACE)
public:
    QMutex lock;
    QObject *advertisement = nullptr;
    QStringList calls;                  // "register 100-150", "unregister"
    // an error for a registration with these intervals (BlueZ or the controller does not take them)
    bool refuseIntervals = false;
    int registers() const { int n = 0; for(const QString &c : calls) n += c.startsWith("register") ? 1 : 0; return n; }

public slots:
    void RegisterAdvertisement(const QDBusObjectPath &, const QVariantMap &)
    {
        QMutexLocker l(&lock);
        const uint min = advertisement->property("MinInterval").toUInt(), max = advertisement->property("MaxInterval").toUInt();
        if(refuseIntervals && min != 1280) {
            calls << QString("refused %1-%2").arg(min).arg(max);
            sendErrorReply("org.bluez.Error.InvalidArguments", "Invalid arguments in method call");
            return;
        }
        calls << QString("register %1-%2").arg(min).arg(max);
    }
    void UnregisterAdvertisement(const QDBusObjectPath &)
    {
        QMutexLocker l(&lock);
        calls << "unregister";
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

static int registerCount(FakeAdvertising &fake)
{
    QMutexLocker l(&fake.lock);
    return fake.registers();
}

static QStringList callsOf(FakeAdvertising &fake)
{
    QMutexLocker l(&fake.lock);
    return fake.calls;
}

static void setConnected(QDBusConnection &fakeConn, FakeObjects &objects, bool connected)
{
    {
        QMutexLocker l(&objects.lock);
        objects.connected = connected;
    }
    QDBusMessage s = QDBusMessage::createSignal(DEVICE, "org.freedesktop.DBus.Properties", "PropertiesChanged");
    QVariantMap changed;
    changed.insert("Connected", connected);
    s << QString(DEVICE_MANAGER_IFACE) << changed << QStringList();
    fakeConn.send(s);
}

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    if(qEnvironmentVariableIsEmpty("DBUS_SYSTEM_BUS_ADDRESS") || qEnvironmentVariableIntValue("BTSYNCD_ADVERT_TIME_DIV") != 100) {
        fprintf(stderr, "needs a private bus as system bus and BTSYNCD_ADVERT_TIME_DIV=100 (see the top of the source)\n");
        return 2;
    }
    qDBusRegisterMetaType<IfaceMap>();
    qDBusRegisterMetaType<ObjMap>();

    // ---- The stages
    CHECK(Advertisement::stageCount() == 8);
    CHECK(Advertisement::stage(0).afterMs == 0 && Advertisement::stage(0).minMs == 100 && Advertisement::stage(0).maxMs == 150);
    const int starts[] = { 0, 10000, 20000, 40000, 80000, 160000, 320000, 540000 };
    for(int i = 0; i < 8; i++) {
        CHECK(Advertisement::stage(i).afterMs == starts[i]);
        CHECK(Advertisement::stageAt(starts[i]) == i);
        CHECK(Advertisement::stageAt(starts[i] + 1) == i);
        if(i > 0)
            CHECK(Advertisement::stageAt(starts[i] - 1) == i - 1);
        CHECK(Advertisement::stage(i).minMs >= 20 && Advertisement::stage(i).maxMs <= 10240);
        CHECK(Advertisement::stage(i).minMs <= Advertisement::stage(i).maxMs);
        if(i > 0) {
            CHECK(Advertisement::stage(i).minMs > Advertisement::stage(i - 1).minMs);
            CHECK(Advertisement::stage(i).maxMs > Advertisement::stage(i - 1).maxMs);
        }
    }
    CHECK(Advertisement::stageAt(0) == 0 && Advertisement::stageAt(9999) == 0);       // fast for the first 10 s
    CHECK(Advertisement::stageAt(-5) == 0);
    CHECK(Advertisement::stageAt(539999) == 6 && Advertisement::stageAt(540000) == 7);   // the cap after nine minutes
    CHECK(Advertisement::stageAt(100000000) == 7);
    CHECK(Advertisement::stage(7).afterMs == 9 * 60 * 1000);

    QThread fakeThread;
    fakeThread.start();
    QDBusConnection fakeConn = QDBusConnection::connectToBus(QDBusConnection::SystemBus, "fake-bluez");
    FakeObjects objects;
    FakeAdvertising advertising;
    objects.moveToThread(&fakeThread);
    advertising.moveToThread(&fakeThread);
    CHECK(fakeConn.registerObject("/", &objects, QDBusConnection::ExportAllSlots));
    CHECK(fakeConn.registerObject(ADAPTER, &advertising, QDBusConnection::ExportAllSlots));
    CHECK(fakeConn.registerService("org.bluez"));

    // ---- The advertisement's properties follow the stage
    {
        Advertisement advert(QDBusConnection::systemBus());
        QDBusConnection driver = QDBusConnection::connectToBus(QDBusConnection::SystemBus, "driver");
        const auto interval = [&](const char *name) {
            QDBusMessage m = QDBusMessage::createMethodCall(QDBusConnection::systemBus().baseService(), ADVERTISEMENT_PATH_BASE,
                                                            "org.freedesktop.DBus.Properties", "Get");
            m << QString(LE_ADVERTISEMENT_IFACE) << QString(name);
            const QDBusMessage r = driver.call(m, QDBus::BlockWithGui, 5000);
            CHECK(r.type() == QDBusMessage::ReplyMessage);
            return r.arguments().value(0).value<QDBusVariant>().variant().toUInt();
        };
        CHECK(advert.stageIndex() == 0 && !advert.defaultIntervals());
        CHECK(interval("MinInterval") == 100 && interval("MaxInterval") == 150);
        advert.setStage(3);
        CHECK(interval("MinInterval") == 800 && interval("MaxInterval") == 1000);
        advert.setStage(99);
        CHECK(advert.stageIndex() == 7 && interval("MinInterval") == 2500 && interval("MaxInterval") == 3000);
        advert.setStage(-3);
        CHECK(advert.stageIndex() == 0);
        advert.useDefaultIntervals();
        CHECK(interval("MinInterval") == 1280 && interval("MaxInterval") == 1280);
    }

    // ---- The schedule against a fake BlueZ
    {
        // (the adapter path answers the advertising manager only: BlueZManager's registration of its GATT
        // application fails and is logged, which this test does not care about)
        Advertisement advert(QDBusConnection::systemBus());
        advertising.advertisement = &advert;
        BlueZManager bm(QDBusObjectPath("/org/asteroidos/btsyncd"), advert.getPath());
        bm.setAdvertisement(&advert);
        QElapsedTimer clock;
        clock.start();
        // registered at the start with the fast intervals
        CHECK(waitFor([&] { return registerCount(advertising) >= 1; }));
        CHECK(callsOf(advertising).first() == "register 100-150");
        CHECK(!bm.connected());
        // then seven steps (the times divided by 100: 0.1 s, 0.2, 0.4, 0.8, 1.6, 3.2 and 5.4 s), one new
        // registration each, with the next stage's interval
        const QStringList expected = QStringList() << "register 100-150" << "unregister" << "register 200-300" << "unregister" << "register 400-600"
            << "unregister" << "register 800-1000" << "unregister" << "register 1000-1500" << "unregister" << "register 1500-2000"
            << "unregister" << "register 2000-2500" << "unregister" << "register 2500-3000";
        CHECK(waitFor([&] { return registerCount(advertising) == 8; }, 9000));
        CHECK(clock.elapsed() >= 5000);          // 540 s / 100, from the start
        CHECK(callsOf(advertising) == expected);
        // and no more: the cap is the cap
        spin(1500);
        CHECK(registerCount(advertising) == 8);

        // the phone connects: the schedule ends, nothing new is registered
        setConnected(fakeConn, objects, true);
        CHECK(waitFor([&] { return bm.connected(); }));
        spin(300);
        CHECK(registerCount(advertising) == 8);
        // it leaves: fast again, then the steps again
        setConnected(fakeConn, objects, false);
        CHECK(waitFor([&] { return registerCount(advertising) == 9; }));
        CHECK(callsOf(advertising).last() == "register 100-150");
        CHECK(waitFor([&] { return registerCount(advertising) == 10; }));
        CHECK(callsOf(advertising).last() == "register 200-300");
        // it comes back half way: no more steps
        setConnected(fakeConn, objects, true);
        CHECK(waitFor([&] { return bm.connected(); }));
        const int stopped = registerCount(advertising);
        spin(1500);
        CHECK(registerCount(advertising) == stopped);
        setConnected(fakeConn, objects, false);
        CHECK(waitFor([&] { return registerCount(advertising) == stopped + 1; }));
        CHECK(callsOf(advertising).last() == "register 100-150");
        advertising.advertisement = nullptr;
    }

    // ---- BlueZ (or the controller) does not take the intervals: its own interval from then on, once
    {
        {
            QMutexLocker l(&advertising.lock);
            advertising.calls.clear();
            advertising.refuseIntervals = true;
        }
        setConnected(fakeConn, objects, false);
        Advertisement advert(QDBusConnection::systemBus());
        advertising.advertisement = &advert;
        BlueZManager bm(QDBusObjectPath("/org/asteroidos/btsyncd"), advert.getPath());
        bm.setAdvertisement(&advert);
        CHECK(waitFor([&] { return registerCount(advertising) >= 1; }, 5000));
        CHECK(callsOf(advertising).value(0) == "refused 100-150");
        CHECK(callsOf(advertising).value(1) == "register 1280-1280");
        CHECK(advert.defaultIntervals());
        // no steps after that
        spin(2000);
        CHECK(registerCount(advertising) == 1);
        CHECK(advert.stageIndex() == 0);
        advertising.advertisement = nullptr;
    }

    fakeThread.quit();
    fakeThread.wait();
    if(failures)
        fprintf(stderr, "%d check(s) failed\n", failures);
    else
        fprintf(stderr, "advert_test: all checks passed\n");
    return failures ? 1 : 0;
}

#include "advert_test.moc"
