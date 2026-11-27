/*
 * Copyright (C) 2026 - eos
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

/*
 * The profile and goals from the phone (M4 WO-11) on a private bus with a
 * dconf-service (cross: the image's mlite and dconf; natively the in-memory
 * stub): Gadgetbridge's frames (fixtures/health-profile.frames.txt) written
 * to the health request characteristic over D-Bus as BlueZ does, in pieces
 * of a 23-byte MTU, with the watch's own edits between them; the keys read
 * back through other MDConfItems (as Settings and sensorlogd read them), with
 * dconf's types. Every profile here is invented.
 */

#include "common.h"
#include "fixtures.h"
#include "healthprofile.h"
#include "healthservice.h"

#include <MDConfItem>
#include <QCoreApplication>
#include <QDBusConnection>
#include <QDBusMessage>
#include <QElapsedTimer>
#include <QHash>
#include <QVariantMap>
#include <cstdio>
#include <functional>

using namespace HealthProfile;

static int failures = 0;

#define CHECK(cond) do { if (!(cond)) { fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); failures++; } } while (0)

static QString btsyncdName;
static QString requestPath;

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

// A long write as BlueZ hands it over: pieces of MTU - 3 bytes with their offsets
static bool write(QDBusConnection &driver, const QByteArray &value)
{
    for (int pos = 0; pos < value.size(); pos += 20) {
        QDBusMessage m = QDBusMessage::createMethodCall(btsyncdName, requestPath, GATT_CHRC_IFACE, "WriteValue");
        QVariantMap options;
        options.insert("mtu", 23);
        options.insert("offset", pos);
        m.setArguments({value.mid(pos, 20), options});
        if (driver.call(m, QDBus::BlockWithGui, 5000).type() != QDBusMessage::ReplyMessage)
            return false;
    }
    spin(50);
    return true;
}

// One long-lived item per key (a new one for every poll subscribes and unsubscribes each time)
static MDConfItem *item(const char *key)
{
    static QHash<QByteArray, MDConfItem *> items;
    MDConfItem *&i = items[key];
    if (!i)
        i = new MDConfItem(QString::fromLatin1(key));
    return i;
}

static QVariant value(const char *key) { return item(key)->value(); }

static bool watchIs(const QString &gender, int heightCm, double weightKg, const QString &born, int steps, int sleep)
{
    return value(GenderKey).toString() == gender && value(HeightKey).toInt() == heightCm
        && value(WeightKey).toDouble() == weightKg && value(BirthDateKey).toString() == born
        && value(StepsGoalKey).toInt() == steps && value(SleepGoalKey).toInt() == sleep;
}

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    if (qEnvironmentVariableIsEmpty("DBUS_SESSION_BUS_ADDRESS") || qEnvironmentVariableIsEmpty("DBUS_SYSTEM_BUS_ADDRESS")) {
        fprintf(stderr, "needs a private bus as session and system bus (see the top of the source)\n");
        return 2;
    }
    const QList<QByteArray> frames = fixtureFrames("health-profile.frames.txt");
    CHECK(frames.size() == 5);
    if (frames.size() != 5)
        return 1;

    HealthService health(6, QDBusConnection::systemBus());
    btsyncdName = QDBusConnection::systemBus().baseService();
    requestPath = health.getCharacteristics().value(0)->getPath().path();
    int notifications = 0;
    QObject::connect(qobject_cast<HealthDataChrc *>(health.getCharacteristics().value(1)), &HealthDataChrc::valueChanged,
                     [&notifications] { notifications++; });
    QDBusConnection driver = QDBusConnection::connectToBus(QDBusConnection::SessionBus, "driver");
    spin(200);

    // the watch as Settings and the Health app left it
    item(GenderKey)->set(QStringLiteral("male"));
    item(HeightKey)->set(175);
    item(WeightKey)->set(75.0);
    item(StepsGoalKey)->set(10000);
    EVENTUALLY(value(StepsGoalKey).toInt() == 10000);

    // 1: the phone's first profile: all of it
    CHECK(write(driver, frames[0]));
    EVENTUALLY(watchIs("female", 168, 61.0, "1991-04-17", 9000, 450));
    // with the types the readers expect (healthprofile.h)
    CHECK(value(GenderKey).typeId() == QMetaType::QString);
    CHECK(value(HeightKey).typeId() == QMetaType::Int);
    CHECK(value(WeightKey).typeId() == QMetaType::Double);
    CHECK(value(BirthDateKey).typeId() == QMetaType::QString);
    CHECK(value(StepsGoalKey).typeId() == QMetaType::Int);
    CHECK(value(SleepGoalKey).typeId() == QMetaType::Int);
    CHECK(value(PhoneProfileKey).toString().size() == 64);

    // the user's own edits on the watch, then the same profile at the next connection: they stay
    item(WeightKey)->set(65.5);
    item(StepsGoalKey)->set(11000);
    EVENTUALLY(value(StepsGoalKey).toInt() == 11000 && value(WeightKey).toDouble() == 65.5);
    CHECK(write(driver, frames[1]));
    spin(300);
    CHECK(watchIs("female", 168, 65.5, "1991-04-17", 11000, 450));

    // 3: the weight changed on the phone: the profile, not the unchanged steps goal
    CHECK(write(driver, frames[2]));
    EVENTUALLY(watchIs("female", 168, 63.0, "1991-04-17", 11000, 450));
    // 4: only the height and the steps goal
    CHECK(write(driver, frames[3]));
    EVENTUALLY(watchIs("female", 181, 63.0, "1991-04-17", 12000, 450));
    // 5: what the watch does not take is left out
    CHECK(write(driver, frames[4]));
    EVENTUALLY(watchIs("other", 181, 63.0, "1991-04-17", 9000, 450));

    // a message that is no profile changes nothing
    CHECK(write(driver, "<prof><gd>female</hc></prof>"));
    spin(300);
    CHECK(watchIs("other", 181, 63.0, "1991-04-17", 9000, 450));

    // never an answer on the data characteristic
    spin(100);
    CHECK(notifications == 0);

    if (failures)
        fprintf(stderr, "%d failure(s)\n", failures);
    else
        printf("healthprofile_test: all passed\n");
    return failures ? 1 : 0;
}
