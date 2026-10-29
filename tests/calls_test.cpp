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

/*
 * Tests of the calls on the watch (CallAudio) with a fake oFono and BlueZ on
 * a private D-Bus (cmake -DBTSYNCD_TESTS=ON), run like notifications_test:
 *   dbus-run-session -- sh -c 'DBUS_SYSTEM_BUS_ADDRESS=$DBUS_SESSION_BUS_ADDRESS calls_test'
 *   - no oFono, then oFono with a hands-free link: available, the phone's
 *     echo cancelling switched off (AT+NREC=0) once
 *   - a ringing call; Answer: audio allowed, audio connected, then ATA, in
 *     that order (stock)
 *   - audio on the watch: the call volume applied locally; mute and volume
 *     (local, and the volume sent to the phone); the phone's AT+VGS
 *   - audio to the phone (withdrawn, then allowed again) and back
 *   - hang up; the last call gone: audio withdrawn, microphone unmuted
 *   - the link gone with the phone's LE link up: the hands-free profile is
 *     connected to the phone bonded over BR/EDR (never to a phone that is
 *     not), and not while the link is up
 *   - a call already active when the link comes: its audio stays put
 */

#include "callaudio.h"
#include "common.h"

#include <QCoreApplication>
#include <QDBusAbstractAdaptor>
#include <QDBusArgument>
#include <QDBusConnection>
#include <QDBusMessage>
#include <QDBusMetaType>
#include <QDBusObjectPath>
#include <QDBusVariant>
#include <QElapsedTimer>
#include <QFile>
#include <QMutex>
#include <QMutexLocker>
#include <QTemporaryDir>
#include <QThread>
#include <cstdio>
#include <functional>

static int failures = 0;

#define CHECK(cond) do { if (!(cond)) { fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); failures++; } } while (0)

static const char *MODEM = "/hfp/org/bluez/hci0/dev_00_00_00_00_00_01";
static const char *CALL1 = "/hfp/org/bluez/hci0/dev_00_00_00_00_00_01/voicecall01";
static const char *CARD = "/card_1";
static const char *DEVICE = "/org/bluez/hci0/dev_00_00_00_00_00_01";
static const char *HFP_AG = "0000111f-0000-1000-8000-00805f9b34fb";

struct OfonoObject {
    QDBusObjectPath path;
    QVariantMap properties;
};
Q_DECLARE_METATYPE(OfonoObject)
typedef QList<OfonoObject> OfonoObjectList;
Q_DECLARE_METATYPE(OfonoObjectList)

QDBusArgument &operator<<(QDBusArgument &arg, const OfonoObject &o)
{
    arg.beginStructure();
    arg << o.path << o.properties;
    arg.endStructure();
    return arg;
}

const QDBusArgument &operator>>(const QDBusArgument &arg, OfonoObject &o)
{
    arg.beginStructure();
    arg >> o.path >> o.properties;
    arg.endStructure();
    return arg;
}

typedef QMap<QString, QVariantMap> Interfaces;
typedef QMap<QDBusObjectPath, Interfaces> Objects;
Q_DECLARE_METATYPE(Interfaces)
Q_DECLARE_METATYPE(Objects)

// What the fakes were asked, in order
static QMutex logLock;
static QStringList calls;
static void logCall(const QString &s)
{
    QMutexLocker l(&logLock);
    calls << s;
}
static QStringList takeCalls()
{
    QMutexLocker l(&logLock);
    QStringList c = calls;
    calls.clear();
    return c;
}
static bool logged(const QString &s)
{
    QMutexLocker l(&logLock);
    return calls.join(",").contains(s);
}
static int loggedCount()
{
    QMutexLocker l(&logLock);
    return calls.size();
}

// The state the fake oFono reports
static QMutex stateLock;
static bool callPresent = false;
static QString callState = "incoming";
static bool cardPresent = true;
static bool bredrBonded = true;

class FakeManager : public QDBusAbstractAdaptor
{
    Q_OBJECT
    Q_CLASSINFO("D-Bus Interface", "org.ofono.Manager")
public:
    explicit FakeManager(QObject *parent) : QDBusAbstractAdaptor(parent) {}
public slots:
    OfonoObjectList GetModems()
    {
        QVariantMap p;
        p.insert("Type", "hfp");
        p.insert("Online", true);
        p.insert("Interfaces", QStringList() << "org.ofono.VoiceCallManager" << "org.ofono.CallVolume" << "org.ofono.Handsfree");
        return OfonoObjectList() << OfonoObject{QDBusObjectPath(MODEM), p};
    }
};

class FakeAudioManager : public QDBusAbstractAdaptor
{
    Q_OBJECT
    Q_CLASSINFO("D-Bus Interface", "org.ofono.HandsfreeAudioManager")
public:
    explicit FakeAudioManager(QObject *parent) : QDBusAbstractAdaptor(parent) {}
public slots:
    OfonoObjectList GetCards()
    {
        QMutexLocker l(&stateLock);
        if (!cardPresent)
            return OfonoObjectList();
        QVariantMap p;
        p.insert("RemoteAddress", "00:00:00:00:00:01");
        p.insert("LocalAddress", "00:00:00:00:00:02");
        p.insert("AudioAllowed", false);
        p.insert("AudioConnected", false);
        return OfonoObjectList() << OfonoObject{QDBusObjectPath(CARD), p};
    }
};

class FakeCallManager : public QDBusAbstractAdaptor
{
    Q_OBJECT
    Q_CLASSINFO("D-Bus Interface", "org.ofono.VoiceCallManager")
public:
    explicit FakeCallManager(QObject *parent) : QDBusAbstractAdaptor(parent) {}
public slots:
    OfonoObjectList GetCalls()
    {
        QMutexLocker l(&stateLock);
        if (!callPresent)
            return OfonoObjectList();
        QVariantMap p;
        p.insert("State", callState);
        p.insert("LineIdentification", "+15550100");
        return OfonoObjectList() << OfonoObject{QDBusObjectPath(CALL1), p};
    }
};

class FakeVolume : public QDBusAbstractAdaptor
{
    Q_OBJECT
    Q_CLASSINFO("D-Bus Interface", "org.ofono.CallVolume")
public:
    explicit FakeVolume(QObject *parent) : QDBusAbstractAdaptor(parent) {}
public slots:
    QVariantMap GetProperties()
    {
        QVariantMap p;
        p.insert("SpeakerVolume", QVariant::fromValue<uchar>(50));
        p.insert("MicrophoneVolume", QVariant::fromValue<uchar>(50));
        p.insert("Muted", false);
        return p;
    }
    void SetProperty(const QString &name, const QDBusVariant &value)
    {
        logCall(QString("volume %1 %2").arg(name).arg(value.variant().toInt()));
    }
};

class FakeHandsfree : public QDBusAbstractAdaptor
{
    Q_OBJECT
    Q_CLASSINFO("D-Bus Interface", "org.ofono.Handsfree")
public:
    explicit FakeHandsfree(QObject *parent) : QDBusAbstractAdaptor(parent) {}
public slots:
    QVariantMap GetProperties()
    {
        QVariantMap p;
        p.insert("EchoCancelingNoiseReduction", true);
        return p;
    }
    void SetProperty(const QString &name, const QDBusVariant &value)
    {
        if (name == "EchoCancelingNoiseReduction" && !value.variant().toBool())
            logCall("nrec off");
    }
};

class FakeCall : public QDBusAbstractAdaptor
{
    Q_OBJECT
    Q_CLASSINFO("D-Bus Interface", "org.ofono.VoiceCall")
public:
    explicit FakeCall(QObject *parent) : QDBusAbstractAdaptor(parent) {}
public slots:
    void Answer() { logCall("call answer"); }
    void Hangup() { logCall("call hangup"); }
};

class FakeCard : public QDBusAbstractAdaptor
{
    Q_OBJECT
    Q_CLASSINFO("D-Bus Interface", "org.ofono.HandsfreeAudioCard")
public:
    explicit FakeCard(QObject *parent) : QDBusAbstractAdaptor(parent) {}
public slots:
    void SetProperty(const QString &name, const QDBusVariant &value)
    {
        logCall(QString("card %1 %2").arg(name).arg(value.variant().toBool() ? "true" : "false"));
    }
    void Connect() { logCall("card connect"); }
};

class FakeObjectManager : public QDBusAbstractAdaptor
{
    Q_OBJECT
    Q_CLASSINFO("D-Bus Interface", "org.freedesktop.DBus.ObjectManager")
public:
    explicit FakeObjectManager(QObject *parent) : QDBusAbstractAdaptor(parent) {}
public slots:
    Objects GetManagedObjects()
    {
        QMutexLocker l(&stateLock);
        QVariantMap dev;
        dev.insert("Connected", true);
        dev.insert("UUIDs", QStringList() << "0000180f-0000-1000-8000-00805f9b34fb" << HFP_AG);
        QVariantMap bredr;
        bredr.insert("Bonded", bredrBonded);
        Interfaces ifaces;
        ifaces.insert("org.bluez.Device1", dev);
        ifaces.insert("org.bluez.Bearer.BREDR1", bredr);
        Objects o;
        o.insert(QDBusObjectPath(DEVICE), ifaces);
        return o;
    }
};

class FakeDevice : public QDBusAbstractAdaptor
{
    Q_OBJECT
    Q_CLASSINFO("D-Bus Interface", "org.bluez.Device1")
public:
    explicit FakeDevice(QObject *parent) : QDBusAbstractAdaptor(parent) {}
public slots:
    void ConnectProfile(const QString &uuid) { logCall("bluez connect " + uuid); }
};

class Collector : public QObject
{
    Q_OBJECT
public:
    QList<QVariantMap> changes;
public slots:
    void callChanged(const QDBusMessage &msg)
    {
        changes << qdbus_cast<QVariantMap>(msg.arguments().value(0).value<QDBusArgument>());
    }
};

static void spin(int ms)
{
    QElapsedTimer t;
    t.start();
    while (t.elapsed() < ms)
        QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
}

static bool waitFor(std::function<bool()> cond, int ms = 5000)
{
    QElapsedTimer t;
    t.start();
    while (t.elapsed() < ms) {
        if (cond())
            return true;
        QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
    }
    return cond();
}

static QVariant api(QDBusConnection &driver, const QString &method, const QVariantList &args = {})
{
    QDBusMessage m = QDBusMessage::createMethodCall(SERVICE_NAME, BTSYNCD_CALLS_PATH, BTSYNCD_CALLS_IFACE, method);
    m.setArguments(args);
    const QDBusMessage r = driver.call(m, QDBus::BlockWithGui, 5000);
    CHECK(r.type() == QDBusMessage::ReplyMessage);
    return r.arguments().value(0);
}

static QVariantMap getCall(QDBusConnection &driver)
{
    return qdbus_cast<QVariantMap>(api(driver, "GetCall").value<QDBusArgument>());
}

static void emitSignal(QDBusConnection &conn, const QString &path, const QString &iface, const QString &name,
                       const QVariantList &args)
{
    QDBusMessage s = QDBusMessage::createSignal(path, iface, name);
    s.setArguments(args);
    conn.send(s);
}

static QVariant v(const QVariant &value)
{
    return QVariant::fromValue(QDBusVariant(value));
}

static QStringList pactlLog(const QString &file)
{
    QFile f(file);
    if (!f.open(QIODevice::ReadOnly))
        return QStringList();
    return QString::fromUtf8(f.readAll()).split('\n', Qt::SkipEmptyParts);
}

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    if (qEnvironmentVariableIsEmpty("DBUS_SESSION_BUS_ADDRESS") || qEnvironmentVariableIsEmpty("DBUS_SYSTEM_BUS_ADDRESS")) {
        fprintf(stderr, "needs a private bus as session and system bus (see the top of the source)\n");
        return 2;
    }
    qDBusRegisterMetaType<OfonoObject>();
    qDBusRegisterMetaType<OfonoObjectList>();
    qDBusRegisterMetaType<Interfaces>();
    qDBusRegisterMetaType<Objects>();

    // pactl replaced by a script that logs its arguments
    QTemporaryDir tmp;
    const QString pactlFile = tmp.path() + "/pactl.log";
    const QString pactl = tmp.path() + "/pactl";
    {
        QFile f(pactl);
        if (!f.open(QIODevice::WriteOnly))
            return 2;
        f.write(QString("#!/bin/sh\necho \"$*\" >> %1\n").arg(pactlFile).toUtf8());
        f.close();
        f.setPermissions(QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner);
    }
    qputenv("BTSYNCD_PACTL", pactl.toUtf8());

    QThread fakeThread;
    fakeThread.start();
    // One object per path, an adaptor per interface (as oFono and BlueZ)
    QObject ofonoRoot, modem, call1, card, bluezRoot, device;
    new FakeManager(&ofonoRoot);
    new FakeAudioManager(&ofonoRoot);
    new FakeCallManager(&modem);
    new FakeVolume(&modem);
    new FakeHandsfree(&modem);
    new FakeCall(&call1);
    new FakeCard(&card);
    new FakeObjectManager(&bluezRoot);
    new FakeDevice(&device);
    const QList<QObject *> fakes = {&ofonoRoot, &modem, &call1, &card, &bluezRoot, &device};
    for (QObject *o : fakes)
        o->moveToThread(&fakeThread);

    QDBusConnection bluez = QDBusConnection::connectToBus(QDBusConnection::SessionBus, "fake-bluez");
    CHECK(bluez.registerObject("/", &bluezRoot, QDBusConnection::ExportAdaptors));
    CHECK(bluez.registerObject(DEVICE, &device, QDBusConnection::ExportAdaptors));
    CHECK(bluez.registerService("org.bluez"));

    QDBusConnection driver = QDBusConnection::connectToBus(QDBusConnection::SessionBus, "driver");

    CallAudio callAudio;
    Collector collector;
    CHECK(driver.connect(SERVICE_NAME, BTSYNCD_CALLS_PATH, BTSYNCD_CALLS_IFACE, "CallChanged",
                         &collector, SLOT(callChanged(QDBusMessage))));
    CHECK(QDBusConnection::sessionBus().registerService(SERVICE_NAME));
    spin(200);

    // No oFono yet
    CHECK(getCall(driver).value("available").toBool() == false);
    CHECK(api(driver, "Answer").toBool() == false);

    // oFono with the hands-free link to the phone
    QDBusConnection ofono = QDBusConnection::connectToBus(QDBusConnection::SessionBus, "fake-ofono");
    CHECK(ofono.registerObject("/", &ofonoRoot, QDBusConnection::ExportAdaptors));
    CHECK(ofono.registerObject(MODEM, &modem, QDBusConnection::ExportAdaptors));
    CHECK(ofono.registerObject(CALL1, &call1, QDBusConnection::ExportAdaptors));
    CHECK(ofono.registerObject(CARD, &card, QDBusConnection::ExportAdaptors));
    CHECK(ofono.registerService("org.ofono"));
    CHECK(waitFor([&] { return getCall(driver).value("available").toBool(); }));
    CHECK(waitFor([&] { return logged("nrec off"); }));
    CHECK(getCall(driver).value("state").toString().isEmpty());
    CHECK(getCall(driver).value("volume").toInt() == 50);
    // The link is up: nothing to connect
    callAudio.setPhoneLink(true);
    spin(300);
    CHECK(takeCalls() == QStringList() << "nrec off");

    // A ringing call
    {
        QVariantMap p;
        p.insert("State", "incoming");
        p.insert("LineIdentification", "+15550100");
        QMutexLocker l(&stateLock);
        callPresent = true;
        callState = "incoming";
        l.unlock();
        emitSignal(ofono, MODEM, "org.ofono.VoiceCallManager", "CallAdded",
                   {QVariant::fromValue(QDBusObjectPath(CALL1)), p});
    }
    CHECK(waitFor([&] { return getCall(driver).value("state").toString() == "incoming"; }));
    CHECK(getCall(driver).value("number").toString() == "+15550100");
    CHECK(getCall(driver).value("audioOnWatch").toBool() == false);
    CHECK(!collector.changes.isEmpty() && collector.changes.last().value("state").toString() == "incoming");

    // Answer on the watch: allowed, connected, then ATA (stock's order)
    CHECK(api(driver, "Answer").toBool() == true);
    CHECK(waitFor([&] { return loggedCount() >= 3; }));
    CHECK(takeCalls() == QStringList() << "card AudioAllowed true" << "card connect" << "call answer");

    // The phone answers and opens the audio
    emitSignal(ofono, CALL1, "org.ofono.VoiceCall", "PropertyChanged", {QString("State"), v("active")});
    emitSignal(ofono, CARD, "org.ofono.HandsfreeAudioCard", "PropertyChanged", {QString("AudioConnected"), v(true)});
    CHECK(waitFor([&] { QVariantMap c = getCall(driver); return c.value("state").toString() == "active" && c.value("audioOnWatch").toBool(); }));
    CHECK(getCall(driver).value("activeSince").toLongLong() > 0);
    // The call volume, local
    CHECK(waitFor([&] { return pactlLog(pactlFile).contains("set-sink-volume bluetooth_ag_ec_sink 50%"); }));

    // Mute (local) and the volume (local and to the phone)
    CHECK(api(driver, "SetMuted", {true}).toBool() == true);
    CHECK(waitFor([&] { return pactlLog(pactlFile).contains("set-source-mute bluetooth_ag_ec_source 1"); }));
    CHECK(getCall(driver).value("muted").toBool() == true);
    CHECK(api(driver, "SetVolume", {80}).toBool() == true);
    CHECK(waitFor([&] { return logged("volume SpeakerVolume 80"); }));
    CHECK(waitFor([&] { return pactlLog(pactlFile).contains("set-sink-volume bluetooth_ag_ec_sink 80%"); }));
    takeCalls();
    // The phone's AT+VGS
    emitSignal(ofono, MODEM, "org.ofono.CallVolume", "PropertyChanged",
               {QString("SpeakerVolume"), v(QVariant::fromValue<uchar>(40))});
    CHECK(waitFor([&] { return getCall(driver).value("volume").toInt() == 40; }));
    CHECK(waitFor([&] { return pactlLog(pactlFile).contains("set-sink-volume bluetooth_ag_ec_sink 40%"); }));

    // Audio to the phone: withdrawn (oFono drops the link), then allowed again
    CHECK(api(driver, "SetAudioOnWatch", {false}).toBool() == true);
    emitSignal(ofono, CARD, "org.ofono.HandsfreeAudioCard", "PropertyChanged", {QString("AudioConnected"), v(false)});
    CHECK(waitFor([&] { return getCall(driver).value("audioOnWatch").toBool() == false; }));
    CHECK(waitFor([&] { return loggedCount() >= 2; }, 3000));
    CHECK(takeCalls() == QStringList() << "card AudioAllowed false" << "card AudioAllowed true");
    // and back to the watch
    CHECK(api(driver, "SetAudioOnWatch", {true}).toBool() == true);
    CHECK(waitFor([&] { return loggedCount() >= 2; }));
    CHECK(takeCalls() == QStringList() << "card AudioAllowed true" << "card connect");

    // End the call; the phone removes it: audio withdrawn, mic unmuted
    CHECK(api(driver, "Hangup").toBool() == true);
    CHECK(waitFor([&] { return logged("call hangup"); }));
    takeCalls();
    {
        QMutexLocker l(&stateLock);
        callPresent = false;
    }
    emitSignal(ofono, MODEM, "org.ofono.VoiceCallManager", "CallRemoved", {QVariant::fromValue(QDBusObjectPath(CALL1))});
    CHECK(waitFor([&] { return getCall(driver).value("state").toString().isEmpty(); }));
    CHECK(waitFor([&] { return logged("card AudioAllowed false"); }));
    CHECK(waitFor([&] { return pactlLog(pactlFile).contains("set-source-mute bluetooth_ag_ec_source 0"); }));
    CHECK(getCall(driver).value("muted").toBool() == false);
    CHECK(api(driver, "Hangup").toBool() == false);
    takeCalls();

    // The link goes while the phone's LE link stays: connect it again (5 s)
    emitSignal(ofono, "/", "org.ofono.Manager", "ModemRemoved", {QVariant::fromValue(QDBusObjectPath(MODEM))});
    CHECK(waitFor([&] { return getCall(driver).value("available").toBool() == false; }));
    CHECK(waitFor([&] { return logged(QString("bluez connect ") + HFP_AG); }, 8000));
    takeCalls();

    // The link back with a call already active: its audio stays put
    {
        QMutexLocker l(&stateLock);
        callPresent = true;
        callState = "active";
    }
    {
        QVariantMap p;
        p.insert("Type", "hfp");
        p.insert("Interfaces", QStringList() << "org.ofono.VoiceCallManager" << "org.ofono.CallVolume" << "org.ofono.Handsfree");
        emitSignal(ofono, "/", "org.ofono.Manager", "ModemAdded", {QVariant::fromValue(QDBusObjectPath(MODEM)), p});
    }
    CHECK(waitFor([&] { return getCall(driver).value("state").toString() == "active"; }));
    CHECK(waitFor([&] { return logged("card AudioAllowed false"); }));
    CHECK(getCall(driver).value("audioOnWatch").toBool() == false);
    // No connect attempts while the link is up
    takeCalls();
    spin(6000);
    CHECK(!logged("bluez connect"));

    // A phone not bonded over BR/EDR: never connected (no pairing from here)
    {
        QMutexLocker l(&stateLock);
        bredrBonded = false;
        callPresent = false;
    }
    emitSignal(ofono, "/", "org.ofono.Manager", "ModemRemoved", {QVariant::fromValue(QDBusObjectPath(MODEM))});
    CHECK(waitFor([&] { return getCall(driver).value("available").toBool() == false; }));
    takeCalls();
    spin(6500);
    CHECK(!logged("bluez connect"));

    // The phone's LE link gone: nothing more
    callAudio.setPhoneLink(false);

    fakeThread.quit();
    fakeThread.wait();
    fprintf(stderr, "%s: %d failures\n", argv[0], failures);
    return failures ? 1 : 0;
}

#include "calls_test.moc"
