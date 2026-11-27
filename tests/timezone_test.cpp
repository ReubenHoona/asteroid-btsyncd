/*
 * Copyright (C) 2026 eos-mainline contributors
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

/*
 * R5 E-07: the phone's time zone with its time (stock TimeService SET_TIMEZONE: the phone's zone
 * is set unless Automatic time zone is off; REEVALUATE_PHONE_TIME_ZONE when it is switched on
 * while the phone is connected), on a private bus with a dconf-service and a fake
 * org.freedesktop.timedate1 (SetTime, SetTimezone, its Timezone property):
 *   - the frames Gadgetbridge writes (fixtures/time-set-zone.frames.txt, from its
 *     AsteroidOSTimeAndStatusTest): the wall time read as the instant at the phone's offset,
 *     the zone name, a long write in parts, and the 6- and 7-byte frames of older phones
 *   - the zone set while auto-time-zone is on and only when it differs; a zone the user chose
 *     (auto-time-zone off) stays; switched on again with the phone there: the phone's last zone
 *   - a name this watch's zone database lacks: the Etc zone of a whole-hour offset, else nothing
 * The zones are invented or Etc zones. Run as the other dconf tests (DCONF_TESTS).
 */
#include "common.h"
#include "fixtures.h"
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
    Q_PROPERTY(QString Timezone READ timezone)
public:
    QMutex lock;
    QList<qlonglong> times;
    QStringList zones;
    QString zone = QStringLiteral("Etc/UTC");
    int timeCount()
    {
        QMutexLocker l(&lock);
        return times.size();
    }
    qlonglong lastTime()
    {
        QMutexLocker l(&lock);
        return times.isEmpty() ? 0 : times.last();
    }
    int zoneCount()
    {
        QMutexLocker l(&lock);
        return zones.size();
    }
    QString lastZone()
    {
        QMutexLocker l(&lock);
        return zones.isEmpty() ? QString() : zones.last();
    }
    // the user's choice in Settings (its own SetTimezone call)
    void choose(const QString &z)
    {
        QMutexLocker l(&lock);
        zone = z;
    }
    QString timezone()
    {
        QMutexLocker l(&lock);
        return zone;
    }

public slots:
    void SetTime(qlonglong usec, bool, bool)
    {
        QMutexLocker l(&lock);
        times << usec;
    }
    void SetTimezone(const QString &z, bool)
    {
        QMutexLocker l(&lock);
        zones << z;
        zone = z;
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

static bool write(QDBusConnection &driver, const QByteArray &value, int offset = 0)
{
    QDBusMessage m = QDBusMessage::createMethodCall(btsyncdName, CHR_TIME, GATT_CHRC_IFACE, "WriteValue");
    QVariantMap options;
    options.insert("mtu", 23);
    if (offset > 0)
        options.insert("offset", offset);
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

// a fixture line: the bytes as decimal numbers, space separated
static QByteArray bytesOf(const QByteArray &line)
{
    QByteArray out;
    for (const QByteArray &n : line.trimmed().split(' ')) {
        if (!n.isEmpty())
            out.append(char(n.toInt()));
    }
    return out;
}

static qlonglong usecUtc(const QDate &d, int h, int m, int s)
{
    return QDateTime(d, QTime(h, m, s), QTimeZone::utc()).toSecsSinceEpoch() * 1000000LL;
}

// a frame of the layout timeservice.h has: the wall time, the flag, the offset and the name
static QByteArray frameOf(int h, int flag, qint32 offset, const QByteArray &name)
{
    QByteArray f = QByteArray::fromHex("7e0a0e") + char(h) + char(5) + char(9) + char(flag);
    const quint32 u = quint32(offset);
    f += char(u & 0xff);
    f += char((u >> 8) & 0xff);
    f += char((u >> 16) & 0xff);
    f += char((u >> 24) & 0xff);
    f += char(name.size());
    return f + name;
}

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    if (qEnvironmentVariableIsEmpty("DBUS_SESSION_BUS_ADDRESS") || qEnvironmentVariableIsEmpty("DBUS_SYSTEM_BUS_ADDRESS")) {
        fprintf(stderr, "needs a private bus as session and system bus (see the top of the source)\n");
        return 2;
    }
    const QDate day(2026, 11, 14);

    // ---- Gadgetbridge's frames
    const QList<QByteArray> frames = fixtureFrames("time-set-zone.frames.txt");
    CHECK(frames.size() == 3);
    const QByteArray f1 = bytesOf(frames.value(0)), f2 = bytesOf(frames.value(1)), f3 = bytesOf(frames.value(2));
    TimeSetChrc::Frame fr;
    // 21:05:09 three hours ahead of UTC, 24 hours
    CHECK(f1.size() == 12 + 9 && TimeSetChrc::frameSize(f1) == f1.size());
    CHECK(TimeSetChrc::decode(f1, &fr));
    CHECK(fr.flag24 == 1 && fr.hasOffset && fr.offsetSecs == 3 * 3600 && fr.zoneId == QLatin1String("Etc/GMT-3"));
    CHECK(fr.time.toUTC() == QDateTime(day, QTime(18, 5, 9), QTimeZone::utc()));
    // five and a half hours ahead, 12 hours
    CHECK(TimeSetChrc::decode(f2, &fr));
    CHECK(fr.flag24 == 0 && fr.offsetSecs == 5 * 3600 + 1800 && fr.zoneId == QLatin1String("Asia/Kolkata"));
    CHECK(fr.time.toUTC() == QDateTime(day, QTime(15, 35, 9), QTimeZone::utc()));
    // an invented name five hours behind UTC: the next day in UTC
    CHECK(TimeSetChrc::decode(f3, &fr));
    CHECK(fr.offsetSecs == -5 * 3600 && fr.zoneId == QLatin1String("Mars/Olympus_Mons"));
    CHECK(fr.time.toUTC() == QDateTime(day.addDays(1), QTime(2, 5, 9), QTimeZone::utc()));
    // the frame's first part of a long write tells the whole size
    CHECK(TimeSetChrc::frameSize(f2.left(18)) == f2.size());
    CHECK(TimeSetChrc::frameSize(f1.left(7)) == 7 && TimeSetChrc::frameSize(f1.left(11)) == 11);
    // an older phone's frames: no zone, the watch's zone
    const QByteArray old = bytesOf(fixtureFrames("time-set.frames.txt").value(0));
    CHECK(old.size() == 7 && TimeSetChrc::decode(old, &fr));
    CHECK(!fr.hasOffset && fr.zoneId.isEmpty() && fr.flag24 == 1);
    CHECK(fr.time == QDateTime(day, QTime(21, 5, 9), QTimeZone::systemTimeZone()));
    CHECK(TimeSetChrc::decode(old.left(6), &fr) && !fr.hasOffset && fr.flag24 == -1);
    // a name longer than the bytes there: the offset alone
    QByteArray cut = f1.left(15);
    CHECK(TimeSetChrc::decode(cut, &fr) && fr.hasOffset && fr.zoneId.isEmpty());
    // an offset no zone has: none (the wall time in the watch's zone), the name still read
    CHECK(TimeSetChrc::decode(frameOf(21, 1, 20 * 3600, "Etc/GMT-3"), &fr) && !fr.hasOffset
          && fr.zoneId == QLatin1String("Etc/GMT-3") && fr.time == QDateTime(day, QTime(21, 5, 9), QTimeZone::systemTimeZone()));
    // a name with a space or a byte above 126: no name
    CHECK(TimeSetChrc::decode(frameOf(21, 1, 0, "Etc/ GMT"), &fr) && fr.zoneId.isEmpty() && fr.hasOffset);
    CHECK(TimeSetChrc::decode(frameOf(21, 1, 0, QByteArray("Etc/GMT\xe9")), &fr) && fr.zoneId.isEmpty());
    // not a date: refused as before
    QByteArray bad = f1;
    bad[1] = char(12);
    CHECK(!TimeSetChrc::decode(bad, &fr));

    // ---- the zone set for the phone's
    CHECK(zoneForPhone(QStringLiteral("Etc/GMT-3"), true, 3 * 3600) == QLatin1String("Etc/GMT-3"));
    CHECK(zoneForPhone(QStringLiteral("Asia/Kolkata"), true, 19800) == QLatin1String("Asia/Kolkata"));
    CHECK(zoneForPhone(QStringLiteral("Mars/Olympus_Mons"), true, -5 * 3600) == QLatin1String("Etc/GMT+5"));
    CHECK(zoneForPhone(QStringLiteral("Mars/Olympus_Mons"), true, 14 * 3600) == QLatin1String("Etc/GMT-14"));
    CHECK(zoneForPhone(QString(), true, 0) == QLatin1String("Etc/GMT"));
    // a half hour has no Etc zone; no offset, no zone
    CHECK(zoneForPhone(QStringLiteral("Mars/Olympus_Mons"), true, 19800).isEmpty());
    CHECK(zoneForPhone(QStringLiteral("Mars/Olympus_Mons"), false, 0).isEmpty());
    // a path is no name
    CHECK(zoneForPhone(QStringLiteral("../../etc/passwd"), true, 3600) == QLatin1String("Etc/GMT-1"));
    CHECK(zoneForPhone(QStringLiteral("/Etc/GMT-3"), false, 0).isEmpty());

    // ---- on the bus
    QThread fakeThread;
    fakeThread.start();
    FakeTimedate fake;
    fake.moveToThread(&fakeThread);
    QDBusConnection fakeConn = QDBusConnection::connectToBus(QDBusConnection::SystemBus, "fake-timedate");
    CHECK(fakeConn.registerObject("/org/freedesktop/timedate1", &fake,
                                  QDBusConnection::ExportAllSlots | QDBusConnection::ExportAllProperties));
    CHECK(fakeConn.registerService("org.freedesktop.timedate1"));

    CHECK(QByteArray(TimeService::PhoneZoneKey) == "/org/asteroidos/settings/phone-time-zone");
    MDConfItem autoTime(AutoTimeKey), autoZone(AutoTimeZoneKey), phoneSet(PhoneSetKey), phoneZone(TimeService::PhoneZoneKey);
    autoTime.unset();
    autoZone.unset();
    phoneSet.unset();
    phoneZone.unset();
    spin(300);
    CHECK(systemTimeZoneName() == QLatin1String("Etc/UTC"));

    TimeService time(5, QDBusConnection::systemBus());
    btsyncdName = QDBusConnection::systemBus().baseService();
    QDBusConnection driver = QDBusConnection::connectToBus(QDBusConnection::SessionBus, "driver");
    time.phoneConnectedChanged(true);
    spin(200);

    // auto time zone on (the default): the phone's zone, then its time as the instant it is
    CHECK(write(driver, f1));
    CHECK(waitFor([&] { return fake.zoneCount() == 1 && fake.timeCount() == 1; }));
    CHECK(fake.lastZone() == QLatin1String("Etc/GMT-3"));
    CHECK(fake.lastTime() == usecUtc(day, 18, 5, 9));
    CHECK(waitFor([&] { return value(TimeService::PhoneZoneKey).toString() == QLatin1String("Etc/GMT-3"); }));
    // the same zone again: not set again
    CHECK(write(driver, f1));
    CHECK(waitFor([&] { return fake.timeCount() == 2; }));
    spin(300);
    CHECK(fake.zoneCount() == 1);

    // a long write: the parts at their offsets, read when whole
    CHECK(write(driver, f2.left(18)));
    spin(300);
    CHECK(fake.timeCount() == 2 && fake.zoneCount() == 1);
    CHECK(write(driver, f2.mid(18), 18));
    CHECK(waitFor([&] { return fake.zoneCount() == 2 && fake.timeCount() == 3; }));
    CHECK(fake.lastZone() == QLatin1String("Asia/Kolkata"));
    CHECK(fake.lastTime() == usecUtc(day, 15, 35, 9));
    // a part that follows nothing: ignored
    CHECK(write(driver, f2.mid(18), 18));
    spin(300);
    CHECK(fake.zoneCount() == 2 && fake.timeCount() == 3);

    // auto time zone off and a zone the user chose: it stays; the phone's is kept, the clock still set
    autoZone.set(false);
    CHECK(waitFor([&] { return !autoTimeZoneEnabled(); }));
    spin(200);
    fake.choose(QStringLiteral("Etc/GMT+7"));
    CHECK(write(driver, f3));
    CHECK(waitFor([&] { return fake.timeCount() == 4; }));
    CHECK(fake.lastTime() == usecUtc(day.addDays(1), 2, 5, 9));
    CHECK(waitFor([&] { return value(TimeService::PhoneZoneKey).toString() == QLatin1String("Etc/GMT+5"); }));
    spin(300);
    CHECK(fake.zoneCount() == 2 && systemTimeZoneName() == QLatin1String("Etc/GMT+7"));

    // switched on with no phone: nothing until the phone's next time
    time.phoneConnectedChanged(false);
    autoZone.set(true);
    CHECK(waitFor([&] { return autoTimeZoneEnabled(); }));
    spin(500);
    CHECK(fake.zoneCount() == 2 && systemTimeZoneName() == QLatin1String("Etc/GMT+7"));
    // off and on again with the phone there: the phone's last zone at once
    autoZone.set(false);
    CHECK(waitFor([&] { return !autoTimeZoneEnabled(); }));
    spin(200);
    time.phoneConnectedChanged(true);
    autoZone.set(true);
    CHECK(waitFor([&] { return fake.zoneCount() == 3; }));
    CHECK(fake.lastZone() == QLatin1String("Etc/GMT+5"));

    // a name this watch lacks and a half-hour offset: the zone and the key stay, the clock is set
    CHECK(write(driver, frameOf(21, 1, 19800, "Mars/Olympus_Mons")));
    CHECK(waitFor([&] { return fake.timeCount() == 5; }));
    CHECK(fake.lastTime() == usecUtc(day, 15, 35, 9));
    spin(300);
    CHECK(fake.zoneCount() == 3 && value(TimeService::PhoneZoneKey).toString() == QLatin1String("Etc/GMT+5"));

    // an older phone's frame: no zone, the wall time in the watch's zone
    CHECK(write(driver, old));
    CHECK(waitFor([&] { return fake.timeCount() == 6; }));
    CHECK(fake.lastTime() == QDateTime(day, QTime(21, 5, 9), QTimeZone::systemTimeZone()).toUTC().toSecsSinceEpoch() * 1000000LL);
    spin(300);
    CHECK(fake.zoneCount() == 3);

    // auto time off, auto time zone on: the zone, not the clock (the two switches are apart)
    autoTime.set(false);
    CHECK(waitFor([&] { return !autoTimeEnabled(); }));
    spin(200);
    CHECK(write(driver, f1));
    CHECK(waitFor([&] { return fake.zoneCount() == 4; }));
    CHECK(fake.lastZone() == QLatin1String("Etc/GMT-3"));
    spin(300);
    CHECK(fake.timeCount() == 6);

    autoTime.unset();
    autoZone.unset();
    phoneSet.unset();
    phoneZone.unset();
    fakeThread.quit();
    fakeThread.wait();
    if (failures)
        fprintf(stderr, "%d check(s) failed\n", failures);
    else
        fprintf(stderr, "timezone_test: all checks passed\n");
    return failures ? 1 : 0;
}

#include "timezone_test.moc"
