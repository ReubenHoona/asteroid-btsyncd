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
#include <cmath>
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
    int levels = 0;
    QByteArray events;
public slots:
    void error(const QString &c) { errors << c; }
    void level(double) { levels++; }
    void eventChanged(const QString &, const QVariantMap &changed, const QStringList &)
    {
        if (changed.contains("Value"))
            events += changed.value("Value").toByteArray();
    }
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

// 16 kHz mono: room noise, 1.2 s of speech-like sound at the watch's level, then the room again
static bool writeSpeechWav(const QString &path)
{
    QByteArray pcm;
    quint32 seed = 1;
    auto noise = [&seed] { seed = seed * 1103515245u + 12345u; return (int((seed >> 16) & 0x7fff) - 16384) / 16384.0; };
    for (int t = 0; t < 16000 * 4; t++) {
        double v = 1500 + 13 * noise();                         // a small DC offset and the room (-68 dBFS)
        if (t >= 8000 && t < 8000 + 19200)
            v += 260 * std::sin(t * 0.13) * (1 + 0.3 * noise());  // -42 dBFS speech
        const qint16 x = qint16(v);
        pcm.append(char(x & 0xff));
        pcm.append(char((x >> 8) & 0xff));
    }
    QByteArray h("RIFF");
    auto le32 = [&h](quint32 v) { for (int i = 0; i < 4; i++) h.append(char((v >> (8 * i)) & 0xff)); };
    auto le16 = [&h](quint16 v) { h.append(char(v & 0xff)); h.append(char(v >> 8)); };
    le32(36 + pcm.size());
    h += "WAVEfmt ";
    le32(16); le16(1); le16(1); le32(16000); le32(32000); le16(2); le16(16);
    h += "data";
    le32(pcm.size());
    QFile f(path);
    if (!f.open(QIODevice::WriteOnly))
        return false;
    f.write(h + pcm);
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

    /* The phone's path ends by itself (2026-10-31: it listened to the limit): the watch
     * finds the end of speech and tells the phone, which then gives the final text */
    const QString speech = dir.filePath("speech.wav");
    CHECK(writeSpeechWav(speech));
    CHECK(driver.connect(sessionName, BTSYNCD_VOICE_PATH, BTSYNCD_VOICE_IFACE, "Level", &c, SLOT(level(double))));
    CHECK(driver.connect(systemName, CHR_EVENT, "org.freedesktop.DBus.Properties", "PropertiesChanged",
                         &c, SLOT(eventChanged(QString, QVariantMap, QStringList))));
    notifications.setLinkConnected(true);
    CHECK(gattCall(driver, CHR_RESULT, "WriteValue", {QByteArray("<vr><st>ready</st></vr>"), options}).type()
          == QDBusMessage::ReplyMessage);
    CHECK(api(driver, "PhoneAvailable").toBool());
    // the default is the watch's recogniser again: unset is "watch"
    qunsetenv("BTSYNCD_VOICE_RECOGNITION");
    c.events.clear();
    QVariantMap opt;
    opt.insert("endpoint_ms", 800);
    opt.insert("max_ms", 8000);
    opt.insert("no_speech_ms", 5000);
    QElapsedTimer t;
    t.start();
    CHECK(api(driver, "StartWith", {0u, QStringLiteral("phone:") + speech, opt}).toUInt() != 0);
    CHECK(waitFor([&] { return c.events.contains("<st>start</st>"); }, 2000));
    // speech ends at 1.7 s: the end event about 0.8 s later, long before the 4 s file ends
    CHECK(waitFor([&] { return c.events.contains("<st>end</st>"); }, 6000));
    const qint64 endMs = t.elapsed();
    fprintf(stderr, "voicelink_test: the watch ended the phone's session after %lld ms, %d level signals\n", endMs, c.levels);
    CHECK(endMs > 2000 && endMs < 3700);
    CHECK(c.levels > 20);
    CHECK(api(driver, "State").toString() == "recognising");
    CHECK(api(driver, "Cancel").toBool());

    if (failures)
        fprintf(stderr, "%d check(s) failed\n", failures);
    else
        fprintf(stderr, "voicelink_test: all checks passed\n");
    return failures ? 1 : 0;
}

#include "voicelink_test.moc"
