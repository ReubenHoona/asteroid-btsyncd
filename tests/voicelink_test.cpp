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

// A voice reply recognised by the phone ends when the phone's BlueZ link
// drops (a bonded phone keeps its subscriptions), on a private bus like
// notifications_test:
// dbus-run-session -- sh -c 'DBUS_SYSTEM_BUS_ADDRESS=$DBUS_SESSION_BUS_ADDRESS voicelink_test'

#include "voiceservice.h"
#include "notificationservice.h"
#include "common.h"

#include <QCoreApplication>
#include <QDBusConnection>
#include <QDBusMessage>
#include <QElapsedTimer>
#include <QFile>
#include <QTemporaryDir>
#include <QVariantMap>
#include <cstdio>
#include <functional>

static int failures = 0;

#define CHECK(cond) do { if (!(cond)) { fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); failures++; } } while (0)

static const char *CHR_FEEDBACK = "/org/asteroidos/btsyncd/service0/char1";
static const char *CHR_EVENT = "/org/asteroidos/btsyncd/service7/char0";
static const char *CHR_AUDIO = "/org/asteroidos/btsyncd/service7/char1";
static const char *CHR_RESULT = "/org/asteroidos/btsyncd/service7/char2";

class Collector : public QObject
{
    Q_OBJECT
public:
    QStringList errors;
    int audio = 0;
public slots:
    void error(const QString &c) { errors << c; }
    void audioChanged(const QString &, const QVariantMap &changed, const QStringList &)
    {
        if (changed.contains("Value"))
            audio++;
    }
};

static bool waitFor(const std::function<bool()> &cond, int ms = 3000)
{
    QElapsedTimer t;
    t.start();
    while (!cond() && t.elapsed() < ms)
        QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
    return cond();
}

static QString systemName, sessionName;

static QDBusMessage gattCall(QDBusConnection &driver, const char *path, const char *method, const QList<QVariant> &args)
{
    QDBusMessage m = QDBusMessage::createMethodCall(systemName, path, GATT_CHRC_IFACE, method);
    m.setArguments(args);
    return driver.call(m, QDBus::BlockWithGui, 5000);
}

static QVariant api(QDBusConnection &driver, const char *method, const QList<QVariant> &args = {})
{
    QDBusMessage m = QDBusMessage::createMethodCall(sessionName, BTSYNCD_VOICE_PATH, BTSYNCD_VOICE_IFACE, method);
    m.setArguments(args);
    const QDBusMessage r = driver.call(m, QDBus::BlockWithGui, 5000);
    CHECK(r.type() == QDBusMessage::ReplyMessage);
    return r.arguments().value(0);
}

// 16 kHz mono 16-bit silence
static bool writeWav(const QString &path, int seconds)
{
    const quint32 data = quint32(seconds) * 16000 * 2;
    QByteArray h("RIFF");
    auto le32 = [&h](quint32 v) { for (int i = 0; i < 4; i++) h.append(char((v >> (8 * i)) & 0xff)); };
    auto le16 = [&h](quint16 v) { h.append(char(v & 0xff)); h.append(char(v >> 8)); };
    le32(36 + data);
    h += "WAVEfmt ";
    le32(16); le16(1); le16(1); le32(16000); le32(32000); le16(2); le16(16);
    h += "data";
    le32(data);
    QFile f(path);
    if (!f.open(QIODevice::WriteOnly))
        return false;
    f.write(h);
    f.write(QByteArray(int(data), '\0'));
    return true;
}

int main(int argc, char **argv)
{
    qputenv("BTSYNCD_VOICE_RECOGNITION", "phone");
    QCoreApplication app(argc, argv);
    if (qEnvironmentVariableIsEmpty("DBUS_SESSION_BUS_ADDRESS") || qEnvironmentVariableIsEmpty("DBUS_SYSTEM_BUS_ADDRESS")) {
        fprintf(stderr, "needs a private bus as session and system bus (see the top of the source)\n");
        return 2;
    }
    QTemporaryDir dir;
    const QString wav = dir.filePath("speech.wav");
    CHECK(writeWav(wav, 5));

    NotificationService notifications(0, QDBusConnection::systemBus());
    VoiceService voice(7, QDBusConnection::systemBus(), &notifications);
    systemName = QDBusConnection::systemBus().baseService();
    sessionName = QDBusConnection::sessionBus().baseService();

    QDBusConnection driver = QDBusConnection::connectToBus(QDBusConnection::SessionBus, "driver");
    Collector c;
    CHECK(driver.connect(sessionName, BTSYNCD_VOICE_PATH, BTSYNCD_VOICE_IFACE, "Error", &c, SLOT(error(QString))));
    CHECK(driver.connect(systemName, CHR_AUDIO, "org.freedesktop.DBus.Properties", "PropertiesChanged",
                         &c, SLOT(audioChanged(QString, QVariantMap, QStringList))));
    waitFor([] { return false; }, 200);

    // The phone is there: link, subscriptions, it does voice replies
    notifications.setLinkConnected(true);
    CHECK(gattCall(driver, CHR_FEEDBACK, "StartNotify", {}).type() == QDBusMessage::ReplyMessage);
    CHECK(gattCall(driver, CHR_EVENT, "StartNotify", {}).type() == QDBusMessage::ReplyMessage);
    CHECK(gattCall(driver, CHR_AUDIO, "StartNotify", {}).type() == QDBusMessage::ReplyMessage);
    QVariantMap options;
    options.insert("mtu", 247);
    CHECK(gattCall(driver, CHR_RESULT, "WriteValue", {QByteArray("<vr><st>ready</st></vr>"), options}).type()
          == QDBusMessage::ReplyMessage);
    CHECK(api(driver, "Available").toBool());
    // the assistant's "auto" choice: the phone itself listens
    CHECK(api(driver, "PhoneAvailable").toBool());

    // A reply is being spoken (streamed to the phone) when the link drops;
    // "auto" (the default): the phone's recogniser, as it is there
    qputenv("BTSYNCD_VOICE_RECOGNITION", "auto");
    CHECK(api(driver, "Start", {0u, wav}).toUInt() != 0);
    CHECK(waitFor([&] { return c.audio > 10; }));
    CHECK(api(driver, "State").toString() == "listening");
    notifications.setLinkConnected(false);
    CHECK(waitFor([&] { return !c.errors.isEmpty(); }, 2000));
    CHECK(c.errors == QStringList() << "disconnected");
    CHECK(api(driver, "State").toString() == "idle");
    // and nothing more is streamed into the void
    const int sent = c.audio;
    waitFor([] { return false; }, 500);
    CHECK(c.audio <= sent + 1);
    CHECK(!api(driver, "Available").toBool());
    CHECK(!api(driver, "PhoneAvailable").toBool());

    if (failures)
        fprintf(stderr, "%d check(s) failed\n", failures);
    else
        fprintf(stderr, "voicelink_test: all checks passed\n");
    return failures ? 1 : 0;
}

#include "voicelink_test.moc"
