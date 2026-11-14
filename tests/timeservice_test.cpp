/*
 * Copyright (C) 2026 eos-mainline contributors
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

/*
 * FP-B1 (contract rows CR-27 and CR-28): the phone's time against Settings > Date & time, on a
 * private bus with a dconf-service (like weather_test) and a fake org.freedesktop.timedate1:
 *   - settings/auto-time (default true) false: the time the phone writes does not set the clock
 *     (timedate1 is not called); the 24-hour flag still follows its own switch
 *   - settings/auto-time-zone is read by the same helper family (default true)
 *   - time/phone-set is written true after the clock was set from the phone, once (a second
 *     time write does not write it again), and never written when the clock was left alone
 * Run as the other dconf tests (see dev-btsyncd.sh, DCONF_TESTS).
 */

#include "common.h"
#include "settime.h"
#include "timeservice.h"

#include <MDConfItem>
#include <QCoreApplication>
#include <QDBusConnection>
#include <QDBusMessage>
#include <QElapsedTimer>
#include <QMutex>
#include <QMutexLocker>
#include <QThread>
#include <QTimeZone>
#include <QVariantMap>
#include <cstdio>
#include <functional>

static int failures = 0;

#define CHECK(cond) do { if (!(cond)) { fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); failures++; } } while (0)

static const char *CHR_TIME = "/org/asteroidos/btsyncd/service5/char0";

class FakeTimedate : public QObject
{
    Q_OBJECT
    Q_CLASSINFO("D-Bus Interface", "org.freedesktop.timedate1")
public:
    QMutex lock;
    QList<qlonglong> calls;
    int count()
    {
        QMutexLocker l(&lock);
        return calls.size();
    }
    qlonglong last()
    {
        QMutexLocker l(&lock);
        return calls.isEmpty() ? 0 : calls.last();
    }

public slots:
    void SetTime(qlonglong usec, bool, bool)
    {
        QMutexLocker l(&lock);
        calls << usec;
    }
};

static void spin(int ms)
{
    QElapsedTimer t;
    t.start();
    while (t.elapsed() < ms)
        QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
}

static bool waitFor(const std::function<bool()> &cond, int ms = 10000)
{
    QElapsedTimer t;
    t.start();
    while (!cond() && t.elapsed() < ms)
        QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
    return cond();
}

static QString btsyncdName;

static bool write(QDBusConnection &driver, const QByteArray &value)
{
    QDBusMessage m = QDBusMessage::createMethodCall(btsyncdName, CHR_TIME, GATT_CHRC_IFACE, "WriteValue");
    QVariantMap options;
    options.insert("mtu", 23);
    m.setArguments({value, options});
    const bool ok = driver.call(m, QDBus::BlockWithGui, 5000).type() == QDBusMessage::ReplyMessage;
    spin(100);
    return ok;
}

static QVariant value(const char *key)
{
    // a fresh item: the key's current value
    return MDConfItem(QString::fromLatin1(key)).value();
}

// the usec timedate1 gets for a frame "2026-11-14 21:05:09" in the watch's zone
static qlonglong usecOf(int h, int m, int s)
{
    QDateTime t(QDate(2026, 11, 14), QTime(h, m, s));
    t.setTimeZone(QTimeZone::systemTimeZone());
    return t.toUTC().toSecsSinceEpoch() * 1000000LL;
}

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    if (qEnvironmentVariableIsEmpty("DBUS_SESSION_BUS_ADDRESS") || qEnvironmentVariableIsEmpty("DBUS_SYSTEM_BUS_ADDRESS")) {
        fprintf(stderr, "needs a private bus as session and system bus (see the top of the source)\n");
        return 2;
    }

    QThread fakeThread;
    fakeThread.start();
    FakeTimedate fake;
    fake.moveToThread(&fakeThread);
    QDBusConnection fakeConn = QDBusConnection::connectToBus(QDBusConnection::SystemBus, "fake-timedate");
    CHECK(fakeConn.registerObject("/org/freedesktop/timedate1", &fake, QDBusConnection::ExportAllSlots));
    CHECK(fakeConn.registerService("org.freedesktop.timedate1"));

    // the keys' names are the contract's
    CHECK(QByteArray(AutoTimeKey) == "/org/asteroidos/settings/auto-time");
    CHECK(QByteArray(AutoTimeZoneKey) == "/org/asteroidos/settings/auto-time-zone");
    CHECK(QByteArray(PhoneSetKey) == "/org/asteroidos/time/phone-set");

    // the starting state: nothing written by the user
    MDConfItem autoTime(AutoTimeKey), autoZone(AutoTimeZoneKey), phoneSet(PhoneSetKey);
    autoTime.unset();
    autoZone.unset();
    phoneSet.unset();
    spin(300);
    CHECK(autoTimeEnabled() && autoTimeZoneEnabled());   // default true

    TimeService time(5, QDBusConnection::systemBus());
    btsyncdName = QDBusConnection::systemBus().baseService();
    QDBusConnection driver = QDBusConnection::connectToBus(QDBusConnection::SessionBus, "driver");
    spin(200);

    // count what happens to time/phone-set (dconf tells every change of a value)
    int phoneSetChanges = 0;
    MDConfItem watched(PhoneSetKey);
    QObject::connect(&watched, &MDConfItem::valueChanged, [&] { phoneSetChanges++; });
    spin(200);

    const QByteArray frame = QByteArray::fromHex("7e0a0e150509") + char(1);   // 2026-11-14 21:05:09, 24 hours

    // ---- auto time on (the default): the clock is set, phone-set goes true, once
    CHECK(!value(PhoneSetKey).isValid());
    CHECK(write(driver, frame));
    CHECK(waitFor([&] { return fake.count() == 1; }));
    CHECK(fake.last() == usecOf(21, 5, 9));
    CHECK(waitFor([&] { return value(PhoneSetKey).isValid() && value(PhoneSetKey).toBool(); }));
    CHECK(waitFor([&] { return phoneSetChanges == 1; }));
    // a second time write: the clock again, the key not written again
    CHECK(write(driver, QByteArray::fromHex("7e0a0e150609")));
    CHECK(waitFor([&] { return fake.count() == 2; }));
    CHECK(fake.last() == usecOf(21, 6, 9));
    spin(500);
    CHECK(phoneSetChanges == 1);
    CHECK(value(PhoneSetKey).toBool());

    // ---- auto time off: the clock is left alone, phone-set is not written, the flag still follows its own switch
    MDConfItem fromPhone(TimeService::FromPhoneKey), use12(TimeService::Use12hKey);
    fromPhone.set(true);
    use12.set(true);
    phoneSet.set(false);   // as a fresh watch: not set from the phone yet
    autoTime.set(false);
    CHECK(waitFor([&] { return !autoTimeEnabled() && !value(PhoneSetKey).toBool(); }));
    spin(300);
    const int before = fake.count();
    const int changesBefore = phoneSetChanges;
    CHECK(write(driver, frame));   // flag 1: 24 hours
    spin(500);
    CHECK(fake.count() == before);
    CHECK(!value(PhoneSetKey).toBool());
    CHECK(phoneSetChanges == changesBefore);
    CHECK(waitFor([&] { return value(TimeService::Phone24hKey).isValid() && value(TimeService::Phone24hKey).toBool(); }));
    CHECK(waitFor([&] { return !value(TimeService::Use12hKey).toBool(); }));   // the format follows the phone's
    CHECK(!setSystemTimeFromPhone(QDateTime(QDate(2026, 11, 14), QTime(1, 2, 3))));
    CHECK(fake.count() == before);

    // ---- auto time on again: applied, phone-set true (the key was false)
    autoTime.set(true);
    CHECK(waitFor([&] { return autoTimeEnabled(); }));
    spin(300);
    CHECK(write(driver, frame));
    CHECK(waitFor([&] { return fake.count() == before + 1; }));
    CHECK(waitFor([&] { return value(PhoneSetKey).toBool(); }));
    // directly, as the current time service does
    CHECK(setSystemTimeFromPhone(QDateTime(QDate(2026, 11, 14), QTime(21, 7, 0), QTimeZone::systemTimeZone())));
    CHECK(waitFor([&] { return fake.count() == before + 2; }));

    // ---- the zone switch
    autoZone.set(false);
    CHECK(waitFor([&] { return !autoTimeZoneEnabled(); }));
    CHECK(autoTimeEnabled());   // independent of auto time
    autoZone.set(true);
    CHECK(waitFor([&] { return autoTimeZoneEnabled(); }));

    autoTime.unset();
    autoZone.unset();
    phoneSet.unset();
    fakeThread.quit();
    fakeThread.wait();
    if (failures)
        fprintf(stderr, "%d check(s) failed\n", failures);
    else
        fprintf(stderr, "timeservice_test: all checks passed\n");
    return failures ? 1 : 0;
}

#include "timeservice_test.moc"
