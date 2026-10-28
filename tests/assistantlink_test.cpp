/*
 * Copyright (C) 2026 - Reuben Horner <reubarb.scons@gmail.com>
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program. If not, see <http://www.gnu.org/licenses/>.
 */

// The assistant relay follows the phone's BlueZ link (a bonded phone out of
// range keeps its subscriptions), on a private bus like notifications_test:
// dbus-run-session -- sh -c 'DBUS_SYSTEM_BUS_ADDRESS=$DBUS_SESSION_BUS_ADDRESS assistantlink_test'

#include "assistantservice.h"
#include "notificationservice.h"
#include "common.h"

#include <QCoreApplication>
#include <QDBusConnection>
#include <QDBusMessage>
#include <QElapsedTimer>
#include <QVariantMap>
#include <cstdio>
#include <functional>

static int failures = 0;

#define CHECK(cond) do { if (!(cond)) { fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); failures++; } } while (0)

static const char *CHR_FEEDBACK = "/org/asteroidos/btsyncd/service0/char1";
static const char *CHR_REQUEST = "/org/asteroidos/btsyncd/service10/char0";
static const char *CHR_REPLY = "/org/asteroidos/btsyncd/service10/char1";

class Collector : public QObject
{
    Q_OBJECT
public:
    QByteArray requests;
    QList<QPair<uint, QString>> failed;
public slots:
    void propertiesChanged(const QString &, const QVariantMap &changed, const QStringList &)
    {
        if (changed.contains("Value"))
            requests += changed.value("Value").toByteArray();
    }
    void onFailed(uint id, const QString &code) { failed << qMakePair(id, code); }
};

static bool waitFor(const std::function<bool()> &cond, int ms = 3000)
{
    QElapsedTimer t;
    t.start();
    while (!cond() && t.elapsed() < ms)
        QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
    return cond();
}

static void spin(int ms)
{
    waitFor([] { return false; }, ms);
}

static QString btsyncdName;

static QDBusMessage gattCall(QDBusConnection &driver, const char *path, const char *method, const QList<QVariant> &args)
{
    QDBusMessage m = QDBusMessage::createMethodCall(btsyncdName, path, GATT_CHRC_IFACE, method);
    m.setArguments(args);
    return driver.call(m, QDBus::BlockWithGui, 5000);
}

static QVariant api(QDBusConnection &driver, const char *method, const QList<QVariant> &args = {})
{
    QDBusMessage m = QDBusMessage::createMethodCall(SERVICE_NAME, BTSYNCD_ASSISTANT_PATH,
                                                    "org.asteroidos.btsyncd.Assistant", method);
    m.setArguments(args);
    const QDBusMessage r = driver.call(m, QDBus::BlockWithGui, 5000);
    CHECK(r.type() == QDBusMessage::ReplyMessage);
    return r.arguments().value(0);
}

static void writeReply(QDBusConnection &driver, const QByteArray &xml)
{
    QVariantMap options;
    options.insert("mtu", 247);
    CHECK(gattCall(driver, CHR_REPLY, "WriteValue", {xml, options}).type() == QDBusMessage::ReplyMessage);
}

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    if (qEnvironmentVariableIsEmpty("DBUS_SESSION_BUS_ADDRESS") || qEnvironmentVariableIsEmpty("DBUS_SYSTEM_BUS_ADDRESS")) {
        fprintf(stderr, "needs a private bus as session and system bus (see the top of the source)\n");
        return 2;
    }

    NotificationService notifications(0, QDBusConnection::systemBus());
    AssistantService assistant(10, QDBusConnection::systemBus(), &notifications);
    btsyncdName = QDBusConnection::systemBus().baseService();

    QDBusConnection driver = QDBusConnection::connectToBus(QDBusConnection::SessionBus, "driver");
    Collector collector;
    CHECK(driver.connect(btsyncdName, CHR_REQUEST, "org.freedesktop.DBus.Properties", "PropertiesChanged",
                         &collector, SLOT(propertiesChanged(QString, QVariantMap, QStringList))));
    CHECK(driver.connect(SERVICE_NAME, BTSYNCD_ASSISTANT_PATH, "org.asteroidos.btsyncd.Assistant", "Failed",
                         &collector, SLOT(onFailed(uint, QString))));
    spin(200);

    // The phone connects: link, feedback and relay subscriptions, "ready"
    notifications.setLinkConnected(true);
    CHECK(gattCall(driver, CHR_FEEDBACK, "StartNotify", {}).type() == QDBusMessage::ReplyMessage);
    CHECK(gattCall(driver, CHR_REQUEST, "StartNotify", {}).type() == QDBusMessage::ReplyMessage);
    CHECK(api(driver, "Available").toBool() == false);
    writeReply(driver, "<asr><st>ready</st></asr>");
    CHECK(api(driver, "Available").toBool() == true);

    // A question goes to the phone
    CHECK(api(driver, "Ask", {7u, QStringLiteral("{\"messages\":[]}")}).toBool() == true);
    CHECK(waitFor([&] { return collector.requests.endsWith("</ask>"); }));

    // The bonded phone goes out of range: BlueZ keeps the subscriptions (no
    // StopNotify), only the link says it. The question waiting fails at once
    // and the relay is not available: asteroid-assistant asks the server.
    notifications.setLinkConnected(false);
    CHECK(waitFor([&] { return !collector.failed.isEmpty(); }));
    CHECK(collector.failed.size() == 1 && collector.failed.value(0).first == 7u
          && collector.failed.value(0).second == QLatin1String("disconnected"));
    CHECK(api(driver, "Available").toBool() == false);
    collector.requests.clear();
    CHECK(api(driver, "Ask", {8u, QStringLiteral("{\"messages\":[]}")}).toBool() == false);
    spin(200);
    CHECK(collector.requests.isEmpty());

    // Back with the same subscriptions: available once the phone says so again
    notifications.setLinkConnected(true);
    spin(200);
    CHECK(api(driver, "Available").toBool() == false);
    writeReply(driver, "<asr><st>ready</st></asr>");
    CHECK(api(driver, "Available").toBool() == true);

    if (failures)
        fprintf(stderr, "%d check(s) failed\n", failures);
    else
        fprintf(stderr, "assistantlink_test: all checks passed\n");
    return failures ? 1 : 0;
}

#include "assistantlink_test.moc"
