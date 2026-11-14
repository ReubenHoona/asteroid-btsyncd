/*
 * Copyright (C) 2026 eos-mainline contributors
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

/*
 * FP-B1 (contract row CR-26): Settings > Developer options > "Vibrate on connectivity change"
 * (dconf btsyncd/connectivity-vibrate, default false) on a private bus with a dconf-service
 * and a fake ngfd (com.nokia.NonGraphicFeedback1.Backend on the system bus):
 *   - off (the default): nothing plays when the link changes
 *   - on: ui_confirm when the phone connects, ui_reject when it disconnects, a haptic only
 *     (media.vibra true); the same state again plays nothing
 *   - the first state given (the link at start) plays nothing, also while on
 * Run as the other dconf tests (see dev-btsyncd.sh, DCONF_TESTS).
 */

#include "connectivityfeedback.h"

#include <MDConfItem>
#include <QCoreApplication>
#include <QDBusConnection>
#include <QElapsedTimer>
#include <QStringList>
#include <QVariantMap>
#include <cstdio>
#include <functional>

static int failures = 0;

#define CHECK(cond) do { if (!(cond)) { fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); failures++; } } while (0)

class FakeNgfd : public QObject
{
    Q_OBJECT
    Q_CLASSINFO("D-Bus Interface", "com.nokia.NonGraphicFeedback1")
public:
    QStringList events;
    QVariantMap lastProps;
    uint next = 1;

public slots:
    uint Play(const QString &event, const QVariantMap &props)
    {
        events << event;
        lastProps = props;
        return next++;
    }
};

static void spin(int ms)
{
    QElapsedTimer t;
    t.start();
    while (t.elapsed() < ms)
        QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
}

static bool waitFor(const std::function<bool()> &cond, int ms = 5000)
{
    QElapsedTimer t;
    t.start();
    while (!cond() && t.elapsed() < ms)
        QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
    return cond();
}

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    if (qEnvironmentVariableIsEmpty("DBUS_SESSION_BUS_ADDRESS") || qEnvironmentVariableIsEmpty("DBUS_SYSTEM_BUS_ADDRESS")) {
        fprintf(stderr, "needs a private bus as session and system bus (see the top of the source)\n");
        return 2;
    }

    CHECK(QByteArray(ConnectivityFeedback::Key) == "/org/asteroidos/btsyncd/connectivity-vibrate");
    CHECK(QByteArray(ConnectivityFeedback::ConnectEvent) == "ui_confirm");
    CHECK(QByteArray(ConnectivityFeedback::DisconnectEvent) == "ui_reject");

    FakeNgfd ngfd;
    QDBusConnection fakeConn = QDBusConnection::connectToBus(QDBusConnection::SystemBus, "fake-ngfd");
    CHECK(fakeConn.registerObject("/com/nokia/NonGraphicFeedback1", &ngfd, QDBusConnection::ExportAllSlots));
    CHECK(fakeConn.registerService("com.nokia.NonGraphicFeedback1.Backend"));

    MDConfItem key(ConnectivityFeedback::Key);
    key.unset();
    spin(300);
    CHECK(!ConnectivityFeedback::enabled());

    // off: nothing, whatever the link does
    ConnectivityFeedback feedback;
    feedback.setPhoneLink(false);   // the link at start
    feedback.setPhoneLink(true);
    feedback.setPhoneLink(false);
    spin(400);
    CHECK(ngfd.events.isEmpty());

    // on: a haptic for each change
    key.set(true);
    CHECK(waitFor([] { return ConnectivityFeedback::enabled(); }));
    feedback.setPhoneLink(true);
    CHECK(waitFor([&] { return ngfd.events.size() == 1; }));
    CHECK(ngfd.events.value(0) == "ui_confirm");
    CHECK(ngfd.lastProps.value("media.vibra").toBool());
    feedback.setPhoneLink(true);   // no change
    spin(300);
    CHECK(ngfd.events.size() == 1);
    feedback.setPhoneLink(false);
    CHECK(waitFor([&] { return ngfd.events.size() == 2; }));
    CHECK(ngfd.events.value(1) == "ui_reject");
    CHECK(ngfd.lastProps.value("media.vibra").toBool());

    // the first state plays nothing, also while on
    ConnectivityFeedback second;
    second.setPhoneLink(true);
    spin(300);
    CHECK(ngfd.events.size() == 2);
    second.setPhoneLink(false);
    CHECK(waitFor([&] { return ngfd.events.size() == 3; }));
    CHECK(ngfd.events.value(2) == "ui_reject");

    // off again
    key.set(false);
    CHECK(waitFor([] { return !ConnectivityFeedback::enabled(); }));
    feedback.setPhoneLink(true);
    spin(400);
    CHECK(ngfd.events.size() == 3);

    key.unset();
    if (failures)
        fprintf(stderr, "%d check(s) failed\n", failures);
    else
        fprintf(stderr, "connectivity_test: all checks passed\n");
    return failures ? 1 : 0;
}

#include "connectivity_test.moc"
