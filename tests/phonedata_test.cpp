/*
 * Copyright (C) 2026 eos-mainline contributors
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

/*
 * Tests of what the phone's time and status writes do on the watch (W2-gb-data's frames),
 * on a private bus with a dconf-service, like weather_test:
 *   - the time characteristic's 7th byte (24-hour flag): decoded, kept, and applied to
 *     use-12h-format only while time-format-from-phone is on (also when it is switched on later)
 *   - <lc> in the phone status: the phone's language at dconf phone-locale
 */

#include "common.h"
#include "fixtures.h"
#include "notificationservice.h"
#include "phonestatusservice.h"
#include "timeservice.h"

#include <MDConfItem>
#include <QCoreApplication>
#include <QDBusConnection>
#include <QDBusMessage>
#include <QElapsedTimer>
#include <QHash>
#include <QTimeZone>
#include <QVariantMap>
#include <cstdio>
#include <functional>

static int failures = 0;

#define CHECK(cond) do { if (!(cond)) { fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); failures++; } } while (0)

static const char *CHR_TIME = "/org/asteroidos/btsyncd/service5/char0";
static const char *CHR_STATUS = "/org/asteroidos/btsyncd/service9/char0";

static QString btsyncdName;

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

#define EVENTUALLY(cond) CHECK(waitFor([&] { return (cond); }))

static bool write(QDBusConnection &driver, const char *path, const QByteArray &value)
{
    QDBusMessage m = QDBusMessage::createMethodCall(btsyncdName, path, GATT_CHRC_IFACE, "WriteValue");
    QVariantMap options;
    options.insert("mtu", 23);
    m.setArguments({value, options});
    const bool ok = driver.call(m, QDBus::BlockWithGui, 5000).type() == QDBusMessage::ReplyMessage;
    spin(50);
    return ok;
}

// One long-lived item per key: a new MDConfItem for every poll of EVENTUALLY subscribes and unsubscribes on the bus
// each time, which under qemu starves the writes the test waits for
static QVariant value(const char *key)
{
    static QHash<QByteArray, MDConfItem *> items;
    MDConfItem *&item = items[key];
    if (!item)
        item = new MDConfItem(key);
    return item->value();
}

// "126 10 14 21 5 9 1" as the fixture lists the bytes
static QByteArray bytesOf(const QByteArray &line)
{
    QByteArray out;
    for (const QByteArray &n : line.split(' '))
        out.append(char(n.toInt()));
    return out;
}

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    if (qEnvironmentVariableIsEmpty("DBUS_SESSION_BUS_ADDRESS") || qEnvironmentVariableIsEmpty("DBUS_SYSTEM_BUS_ADDRESS")) {
        fprintf(stderr, "needs a private bus as session and system bus (see the top of the source)\n");
        return 2;
    }

    // ---- the decoder, with the frames ----
    const QList<QByteArray> frames = fixtureFrames("time-set.frames.txt");
    CHECK(frames.size() == 2);
    {
        QDateTime t;
        int flag = -2;
        const QByteArray b24 = bytesOf(frames.value(0));
        CHECK(b24.size() == 7);
        CHECK(TimeSetChrc::decode(b24, &t, &flag));
        // year 1900 + 126, month 10 + 1 (a byte of 0..11), day, hour, minute, second, 24 h
        CHECK(t.date() == QDate(2026, 11, 14) && t.time() == QTime(21, 5, 9) && flag == 1);
        CHECK(TimeSetChrc::decode(bytesOf(frames.value(1)), &t, &flag) && flag == 0);
        // six bytes (an older Gadgetbridge): the time alone, no flag
        CHECK(TimeSetChrc::decode(b24.left(6), &t, &flag) && flag == -1 && t.time() == QTime(21, 5, 9));
        // a seventh byte that is neither 0 nor 1 is no flag
        QByteArray two = b24;
        two[6] = char(2);
        CHECK(TimeSetChrc::decode(two, &t, &flag) && flag == -1);
        two[6] = char(255);
        CHECK(TimeSetChrc::decode(two, &t, &flag) && flag == -1);
        // too short, or not a date
        CHECK(!TimeSetChrc::decode(b24.left(5), &t, &flag) && flag == -1);
        QByteArray bad = b24;
        bad[1] = char(12);   // month 13
        CHECK(!TimeSetChrc::decode(bad, &t, &flag));
        bad = b24;
        bad[2] = char(32);
        CHECK(!TimeSetChrc::decode(bad, &t, &flag));
    }

    NotificationService notifications(0, QDBusConnection::systemBus());
    // The format keys as the settings leave them: not from the phone, 12 hours
    MDConfItem fromPhone(TimeService::FromPhoneKey);
    MDConfItem use12(TimeService::Use12hKey);
    fromPhone.set(false);
    use12.set(true);
    EVENTUALLY(value(TimeService::Use12hKey).toBool() == true);
    TimeService time(5, QDBusConnection::systemBus());
    PhoneStatusService phone(9, QDBusConnection::systemBus(), &notifications);
    btsyncdName = QDBusConnection::systemBus().baseService();
    QDBusConnection driver = QDBusConnection::connectToBus(QDBusConnection::SessionBus, "driver");
    spin(200);

    // ---- the 24-hour flag ----
    // time sync is not following the phone's format: the flag is kept, use-12h-format stays
    CHECK(write(driver, CHR_TIME, bytesOf(frames.value(0))));
    EVENTUALLY(value(TimeService::Phone24hKey).isValid() && value(TimeService::Phone24hKey).toBool() == true);
    spin(300);
    CHECK(value(TimeService::Use12hKey).toBool() == true);
    // switched on: the phone's last flag (24 hours) applies at once
    fromPhone.set(true);
    EVENTUALLY(value(TimeService::Use12hKey).toBool() == false);
    // the phone changes to 12 hours: the next time write carries it
    CHECK(write(driver, CHR_TIME, bytesOf(frames.value(1))));
    EVENTUALLY(value(TimeService::Use12hKey).toBool() == true && value(TimeService::Phone24hKey).toBool() == false);
    CHECK(write(driver, CHR_TIME, bytesOf(frames.value(0))));
    EVENTUALLY(value(TimeService::Use12hKey).toBool() == false);
    // a time write without the flag changes nothing
    CHECK(write(driver, CHR_TIME, bytesOf("126 10 14 21 6 9")));
    spin(300);
    CHECK(value(TimeService::Use12hKey).toBool() == false && value(TimeService::Phone24hKey).toBool() == true);
    // a flag that is none changes nothing
    CHECK(write(driver, CHR_TIME, bytesOf("126 10 14 21 6 9 2")));
    spin(300);
    CHECK(value(TimeService::Use12hKey).toBool() == false);
    // switched off: the user's own choice stays and the phone does not touch it
    fromPhone.set(false);
    use12.set(true);
    EVENTUALLY(value(TimeService::Use12hKey).toBool() == true);
    CHECK(write(driver, CHR_TIME, bytesOf(frames.value(0))));
    spin(400);
    CHECK(value(TimeService::Use12hKey).toBool() == true);

    // ---- the phone's language ----
    const QList<QByteArray> status = fixtureFrames("phone-status-locale.frames.txt");
    CHECK(status.size() == 2 && status.value(0).contains("<lc>en-NZ</lc>"));
    CHECK(!value(PhoneStatusService::PhoneLocaleKey).isValid());
    CHECK(write(driver, CHR_STATUS, status.value(0)));
    EVENTUALLY(value(PhoneStatusService::PhoneLocaleKey).toString() == "en-NZ");
    // the rest of that status is applied as before (the battery)
    CHECK(phone.batteryLevel() == 80);
    CHECK(write(driver, CHR_STATUS, status.value(1)));
    EVENTUALLY(value(PhoneStatusService::PhoneLocaleKey).toString() == "pt-BR");
    CHECK(phone.batteryLevel() == 80);
    // an underscore tag, a script and region
    CHECK(write(driver, CHR_STATUS, "<ps><lc>en_GB</lc></ps>"));
    EVENTUALLY(value(PhoneStatusService::PhoneLocaleKey).toString() == "en-GB");
    CHECK(write(driver, CHR_STATUS, "<ps><lc>zh-Hant-TW</lc></ps>"));
    EVENTUALLY(value(PhoneStatusService::PhoneLocaleKey).toString() == "zh-Hant-TW");
    // what is no tag changes nothing
    for (const char *bad : {"<ps><lc></lc></ps>", "<ps><lc>x</lc></ps>", "<ps><lc>en NZ</lc></ps>", "<ps><lc>en-NZ;rm -rf</lc></ps>",
                            "<ps><lc>12-34</lc></ps>", "<ps><lc>en-toolongsubtag9</lc></ps>", "<ps><lc>../etc</lc></ps>"}) {
        CHECK(write(driver, CHR_STATUS, bad));
        spin(150);
        if (value(PhoneStatusService::PhoneLocaleKey).toString() != "zh-Hant-TW") {
            fprintf(stderr, "FAIL: %s changed the language\n", bad);
            failures++;
        }
    }
    // 35 characters at most
    QString t35 = "en";
    while (t35.size() + 9 <= 35)
        t35 += "-aaaaaaaa";
    t35 += "-bbbbb";
    CHECK(t35.size() == 35 && PhoneStatusService::validLocaleTag(t35));
    CHECK(!PhoneStatusService::validLocaleTag(t35 + "b"));
    CHECK(PhoneStatusService::validLocaleTag("en") && PhoneStatusService::validLocaleTag("fil") && PhoneStatusService::validLocaleTag("es-419"));
    CHECK(!PhoneStatusService::validLocaleTag("e") && !PhoneStatusService::validLocaleTag("english") && !PhoneStatusService::validLocaleTag("en-"));

    if (failures)
        fprintf(stderr, "%d check(s) failed\n", failures);
    else
        fprintf(stderr, "phonedata_test: all checks passed\n");
    return failures ? 1 : 0;
}
