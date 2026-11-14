/*
 * Copyright (C) 2026 eos-mainline contributors
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

/*
 * Tests of the calls on the watch (CallAudio) with a fake oFono and BlueZ on
 * a private D-Bus (cmake -DBTSYNCD_TESTS=ON). The settings parts need a dconf
 * service on that bus (they say so and are skipped without), as weather_test:
 *   BTSYNCD_CALLS_TIME_DIV=100 BTSYNCD_RFCOMM=1 dbus-run-session -- sh -c 'DBUS_SYSTEM_BUS_ADDRESS=$DBUS_SESSION_BUS_ADDRESS calls_test'
 *   - no oFono: the contract's State() with nothing, every command refused
 *   - oFono with a hands-free link: the phone's echo cancelling off (AT+NREC=0) once
 *   - a ringing call; answerOnWatch: audio allowed, audio connected, then ATA, in that order
 *   - the audio on the watch: State (available, route watch, volume step of 5), the call volume
 *     kept in the Settings > Sound key (percent), applied to the sink and sent to the phone
 *     (AT+VGS); AdjustVolume steps and limits (1..5); the phone's AT+VGS
 *   - mute (local), DTMF digits (valid ones only), SetRoute and PullToWatch (the audio to the
 *     phone is withdrawn then allowed again)
 *   - hang up; the last call gone: audio withdrawn, microphone unmuted
 *   - the hands-free profile connected to the phone bonded over BR/EDR after the LE link, at
 *     most six times (no endless retries), never to a phone that is not bonded, not while the
 *     link is up, not without RFCOMM in the kernel
 *   - "Answer calls on watch" off: the profile dropped, no attempts, the answer goes to the phone
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
#include <MDConfItem>
#include <cstdio>
#include <functional>

static int failures = 0;

#define CHECK(cond) do { if (!(cond)) { fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); failures++; } } while (0)

static const char *MODEM = "/hfp/org/bluez/hci0/dev_00_00_00_00_00_01";
static const char *CALL1 = "/hfp/org/bluez/hci0/dev_00_00_00_00_00_01/voicecall01";
static const char *CARD = "/card_1";
static const char *DEVICE = "/org/bluez/hci0/dev_00_00_00_00_00_01";
static const char *HFP_AG = "0000111f-0000-1000-8000-00805f9b34fb";
static const char *KEY_ENABLED = "/org/asteroidos/btsyncd/calls-on-watch";
static const char *KEY_VOLUME = "/org/asteroidos/sound/call-volume";

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
static int loggedTimes(const QString &s)
{
    QMutexLocker l(&logLock);
    int n = 0;
    for (const QString &c : calls)
        if (c.contains(s))
            n++;
    return n;
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
    void SendTones(const QString &tones) { logCall("tones " + tones); }
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
    void DisconnectProfile(const QString &uuid) { logCall("bluez disconnect " + uuid); }
};

class Collector : public QObject
{
    Q_OBJECT
public:
    QList<QVariantMap> changes;
public slots:
    void stateChanged(const QDBusMessage &msg)
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
    QDBusMessage m = QDBusMessage::createMethodCall(SERVICE_NAME, BTSYNCD_CALLAUDIO_PATH, BTSYNCD_CALLAUDIO_IFACE, method);
    m.setArguments(args);
    const QDBusMessage r = driver.call(m, QDBus::BlockWithGui, 5000);
    CHECK(r.type() == QDBusMessage::ReplyMessage);
    return r.arguments().value(0);
}

static QVariantMap getState(QDBusConnection &driver)
{
    return qdbus_cast<QVariantMap>(api(driver, "State").value<QDBusArgument>());
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
    // The kernel of the tests has RFCOMM (changed below)
    qputenv("BTSYNCD_RFCOMM", "1");

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

    // Settings: a dconf service on the bus, or those parts are skipped
    MDConfItem probe("/org/asteroidos/btsyncd/calls-test-probe");
    probe.set(1);
    spin(500);
    MDConfItem observer("/org/asteroidos/btsyncd/calls-test-probe");
    const bool haveDconf = observer.value(0).toInt() == 1;
    fprintf(stderr, "dconf service: %s\n", haveDconf ? "yes" : "no (the settings parts are skipped)");
    MDConfItem enabledItem(KEY_ENABLED), volumeObserver(KEY_VOLUME);

    CallAudio callAudio;
    CallControl *control = &callAudio;
    Collector collector;
    CHECK(driver.connect(SERVICE_NAME, BTSYNCD_CALLAUDIO_PATH, BTSYNCD_CALLAUDIO_IFACE, "StateChanged",
                         &collector, SLOT(stateChanged(QDBusMessage))));
    CHECK(QDBusConnection::sessionBus().registerService(SERVICE_NAME));
    spin(200);

    // No oFono yet: the contract's keys, and every command refused
    {
        const QVariantMap s = getState(driver);
        CHECK(s.value("available").toBool() == false);
        CHECK(s.value("muted").toBool() == false);
        CHECK(s.value("route").toString() == "phone");
        CHECK(s.value("volume").toInt() == -1);
        CHECK(s.value("volumeMax").toInt() == 0);
        CHECK(s.value("callState").toString().isEmpty());
        CHECK(s.value("link").toBool() == false);
    }
    CHECK(control->answerOnWatch() == false);
    CHECK(control->hangUp() == false);
    CHECK(api(driver, "SetMuted", {true}).toBool() == false);
    CHECK(api(driver, "AdjustVolume", {1}).toBool() == false);
    CHECK(api(driver, "SendTones", {QString("5")}).toBool() == false);
    CHECK(api(driver, "SetRoute", {QString("watch")}).toBool() == false);
    CHECK(api(driver, "PullToWatch").toBool() == false);

    // oFono with the hands-free link to the phone
    QDBusConnection ofono = QDBusConnection::connectToBus(QDBusConnection::SessionBus, "fake-ofono");
    CHECK(ofono.registerObject("/", &ofonoRoot, QDBusConnection::ExportAdaptors));
    CHECK(ofono.registerObject(MODEM, &modem, QDBusConnection::ExportAdaptors));
    CHECK(ofono.registerObject(CALL1, &call1, QDBusConnection::ExportAdaptors));
    CHECK(ofono.registerObject(CARD, &card, QDBusConnection::ExportAdaptors));
    CHECK(ofono.registerService("org.ofono"));
    CHECK(waitFor([&] { return getState(driver).value("link").toBool(); }));
    CHECK(waitFor([&] { return logged("nrec off"); }));
    CHECK(getState(driver).value("callState").toString().isEmpty());
    // The link without a call: nothing on the watch yet
    CHECK(getState(driver).value("available").toBool() == false);
    // The link is up: nothing to connect
    callAudio.setPhoneLink(true);
    spin(500);
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
    CHECK(waitFor([&] { return getState(driver).value("callState").toString() == "incoming"; }));
    CHECK(getState(driver).value("available").toBool() == false);
    CHECK(getState(driver).value("route").toString() == "phone");
    CHECK(!collector.changes.isEmpty() && collector.changes.last().value("callState").toString() == "incoming");

    // Answer on the watch: allowed, connected, then ATA (stock's order)
    CHECK(control->answerOnWatch() == true);
    CHECK(waitFor([&] { return loggedCount() >= 3; }));
    CHECK(takeCalls() == QStringList() << "card AudioAllowed true" << "card connect" << "call answer");

    // The phone answers and opens the audio: on the watch, the stored call volume (80 % = step 4)
    // goes to the sink and to the phone
    emitSignal(ofono, CALL1, "org.ofono.VoiceCall", "PropertyChanged", {QString("State"), v("active")});
    emitSignal(ofono, CARD, "org.ofono.HandsfreeAudioCard", "PropertyChanged", {QString("AudioConnected"), v(true)});
    CHECK(waitFor([&] { QVariantMap s = getState(driver); return s.value("callState").toString() == "active" && s.value("available").toBool(); }));
    {
        const QVariantMap s = getState(driver);
        CHECK(s.value("route").toString() == "watch");
        CHECK(s.value("volume").toInt() == 4);
        CHECK(s.value("volumeMax").toInt() == 5);
        CHECK(s.value("muted").toBool() == false);
    }
    CHECK(waitFor([&] { return pactlLog(pactlFile).contains("set-sink-volume bluetooth_ag_ec_sink 80%"); }));
    CHECK(waitFor([&] { return logged("volume SpeakerVolume 80"); }));
    takeCalls();

    // One step up: 100 % (step 5), the sink, the phone, the Settings key
    CHECK(api(driver, "AdjustVolume", {1}).toBool() == true);
    CHECK(waitFor([&] { return getState(driver).value("volume").toInt() == 5; }));
    CHECK(waitFor([&] { return pactlLog(pactlFile).contains("set-sink-volume bluetooth_ag_ec_sink 100%"); }));
    CHECK(waitFor([&] { return logged("volume SpeakerVolume 100"); }));
    if (haveDconf)
        CHECK(waitFor([&] { return volumeObserver.value(0).toInt() == 100; }));
    takeCalls();
    // At the top it stays there: nothing more is sent
    CHECK(api(driver, "AdjustVolume", {1}).toBool() == true);
    spin(300);
    CHECK(getState(driver).value("volume").toInt() == 5);
    CHECK(takeCalls().isEmpty());
    // Many steps down end at the lowest step, 1 (20 %), not at zero
    CHECK(api(driver, "AdjustVolume", {-9}).toBool() == true);
    CHECK(waitFor([&] { return getState(driver).value("volume").toInt() == 1; }));
    CHECK(waitFor([&] { return pactlLog(pactlFile).contains("set-sink-volume bluetooth_ag_ec_sink 20%"); }));
    CHECK(waitFor([&] { return logged("volume SpeakerVolume 20"); }));
    if (haveDconf)
        CHECK(waitFor([&] { return volumeObserver.value(0).toInt() == 20; }));
    // Steps of one: 3 up is step 4 (80 %)
    CHECK(api(driver, "AdjustVolume", {3}).toBool() == true);
    CHECK(waitFor([&] { return getState(driver).value("volume").toInt() == 4; }));
    CHECK(waitFor([&] { return logged("volume SpeakerVolume 80"); }));
    takeCalls();

    // The Settings slider moves the call volume during the call
    if (haveDconf) {
        volumeObserver.set(40);
        CHECK(waitFor([&] { return getState(driver).value("volume").toInt() == 2; }));
        CHECK(waitFor([&] { return pactlLog(pactlFile).contains("set-sink-volume bluetooth_ag_ec_sink 40%"); }));
        CHECK(waitFor([&] { return logged("volume SpeakerVolume 40"); }));
        volumeObserver.set(80);
        CHECK(waitFor([&] { return getState(driver).value("volume").toInt() == 4; }));
        takeCalls();
    }

    // The phone's AT+VGS: the volume follows, the phone is not told back
    emitSignal(ofono, MODEM, "org.ofono.CallVolume", "PropertyChanged",
               {QString("SpeakerVolume"), v(QVariant::fromValue<uchar>(60))});
    CHECK(waitFor([&] { return getState(driver).value("volume").toInt() == 3; }));
    CHECK(waitFor([&] { return pactlLog(pactlFile).contains("set-sink-volume bluetooth_ag_ec_sink 60%"); }));
    spin(300);
    CHECK(!logged("volume SpeakerVolume"));
    if (haveDconf)
        CHECK(waitFor([&] { return volumeObserver.value(0).toInt() == 60; }));

    // Mute (local)
    CHECK(api(driver, "SetMuted", {true}).toBool() == true);
    CHECK(waitFor([&] { return pactlLog(pactlFile).contains("set-source-mute bluetooth_ag_ec_source 1"); }));
    CHECK(getState(driver).value("muted").toBool() == true);
    CHECK(waitFor([&] { return !collector.changes.isEmpty() && collector.changes.last().value("muted").toBool(); }));

    // The keypad: digits, * # and A-D (lower case too); nothing else
    takeCalls();
    CHECK(api(driver, "SendTones", {QString("12#*")}).toBool() == true);
    CHECK(waitFor([&] { return logged("tones 12#*"); }));
    CHECK(api(driver, "SendTones", {QString("a9")}).toBool() == true);
    CHECK(waitFor([&] { return logged("tones A9"); }));
    takeCalls();
    CHECK(api(driver, "SendTones", {QString("12x")}).toBool() == false);
    CHECK(api(driver, "SendTones", {QString("")}).toBool() == false);
    CHECK(api(driver, "SendTones", {QString(33, QChar('1'))}).toBool() == false);
    CHECK(api(driver, "SendTones", {QString("1;ATD")}).toBool() == false);
    spin(300);
    CHECK(!logged("tones"));

    // Audio to the phone: withdrawn (oFono drops the link), then allowed again
    CHECK(api(driver, "SetRoute", {QString("bogus")}).toBool() == false);
    CHECK(api(driver, "SetRoute", {QString("phone")}).toBool() == true);
    emitSignal(ofono, CARD, "org.ofono.HandsfreeAudioCard", "PropertyChanged", {QString("AudioConnected"), v(false)});
    CHECK(waitFor([&] { QVariantMap s = getState(driver); return s.value("available").toBool() == false && s.value("route").toString() == "phone"; }));
    CHECK(getState(driver).value("volume").toInt() == -1);
    CHECK(waitFor([&] { return loggedCount() >= 2; }, 3000));
    CHECK(takeCalls() == QStringList() << "card AudioAllowed false" << "card AudioAllowed true");
    // and back to the watch
    CHECK(api(driver, "PullToWatch").toBool() == true);
    CHECK(waitFor([&] { return loggedCount() >= 2; }));
    CHECK(takeCalls() == QStringList() << "card AudioAllowed true" << "card connect");
    emitSignal(ofono, CARD, "org.ofono.HandsfreeAudioCard", "PropertyChanged", {QString("AudioConnected"), v(true)});
    CHECK(waitFor([&] { return getState(driver).value("route").toString() == "watch"; }));
    // The level is applied again, as the audio comes
    CHECK(getState(driver).value("volume").toInt() == 3);
    CHECK(waitFor([&] { int n = 0; for (const QString &l : pactlLog(pactlFile)) if (l == "set-sink-volume bluetooth_ag_ec_sink 60%") n++; return n >= 2; }));
    takeCalls();
    CHECK(api(driver, "SetRoute", {QString("watch")}).toBool() == true);
    CHECK(waitFor([&] { return loggedCount() >= 2; }));
    takeCalls();

    // End the call; the phone removes it: audio withdrawn, mic unmuted
    CHECK(control->hangUp() == true);
    CHECK(waitFor([&] { return logged("call hangup"); }));
    takeCalls();
    {
        QMutexLocker l(&stateLock);
        callPresent = false;
    }
    emitSignal(ofono, MODEM, "org.ofono.VoiceCallManager", "CallRemoved", {QVariant::fromValue(QDBusObjectPath(CALL1))});
    CHECK(waitFor([&] { return getState(driver).value("callState").toString().isEmpty(); }));
    CHECK(waitFor([&] { return logged("card AudioAllowed false"); }));
    CHECK(waitFor([&] { return pactlLog(pactlFile).contains("set-source-mute bluetooth_ag_ec_source 0"); }));
    CHECK(getState(driver).value("muted").toBool() == false);
    CHECK(control->hangUp() == false);
    CHECK(api(driver, "SendTones", {QString("1")}).toBool() == false);
    emitSignal(ofono, CARD, "org.ofono.HandsfreeAudioCard", "PropertyChanged", {QString("AudioConnected"), v(false)});
    takeCalls();

    // The link goes while the phone's LE link stays: connect again (the first attempt at 5 s), at most
    // six times when the phone does not take it
    emitSignal(ofono, "/", "org.ofono.Manager", "ModemRemoved", {QVariant::fromValue(QDBusObjectPath(MODEM))});
    CHECK(waitFor([&] { return getState(driver).value("link").toBool() == false; }));
    CHECK(waitFor([&] { return logged(QString("bluez connect ") + HFP_AG); }, 8000));
    CHECK(waitFor([&] { return loggedTimes("bluez connect") == CallAudio::ConnectAttempts; }, 20000));
    spin(5000);
    CHECK(loggedTimes("bluez connect") == CallAudio::ConnectAttempts);
    // A new LE link starts over
    takeCalls();
    callAudio.setPhoneLink(false);
    callAudio.setPhoneLink(true);
    CHECK(waitFor([&] { return loggedTimes("bluez connect") == CallAudio::ConnectAttempts; }, 20000));
    spin(1000);
    CHECK(loggedTimes("bluez connect") == CallAudio::ConnectAttempts);
    takeCalls();

    // A kernel without RFCOMM: no attempts at all
    qputenv("BTSYNCD_RFCOMM", "0");
    callAudio.setPhoneLink(false);
    callAudio.setPhoneLink(true);
    spin(2000);
    CHECK(!logged("bluez connect"));
    qputenv("BTSYNCD_RFCOMM", "1");

    // "Answer calls on watch" off: no attempts; on again: attempts
    if (haveDconf) {
        enabledItem.set(false);
        spin(500);
        takeCalls();
        callAudio.setPhoneLink(false);
        callAudio.setPhoneLink(true);
        spin(2500);
        CHECK(!logged("bluez connect"));
        enabledItem.set(true);
        CHECK(waitFor([&] { return logged("bluez connect"); }, 5000));
        callAudio.setPhoneLink(false);   // stop the attempts for the next part
        spin(200);
        takeCalls();
        callAudio.setPhoneLink(true);
    }
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
    CHECK(waitFor([&] { return getState(driver).value("callState").toString() == "active"; }));
    CHECK(waitFor([&] { return logged("card AudioAllowed false"); }));
    CHECK(getState(driver).value("available").toBool() == false);
    CHECK(getState(driver).value("route").toString() == "phone");
    // No connect attempts while the link is up
    takeCalls();
    spin(6000);
    CHECK(!logged("bluez connect"));

    // "Answer calls on watch" off with the link up: the profile is dropped and the answer (and
    // the audio) stay with the phone
    if (haveDconf) {
        // a second, ringing call
        {
            QVariantMap p;
            p.insert("State", "waiting");
            emitSignal(ofono, MODEM, "org.ofono.VoiceCallManager", "CallAdded",
                       {QVariant::fromValue(QDBusObjectPath(CALL1)), p});
        }
        CHECK(waitFor([&] { return getState(driver).value("callState").toString() == "waiting"; }));
        takeCalls();
        enabledItem.set(false);
        CHECK(waitFor([&] { return logged(QString("bluez disconnect ") + HFP_AG); }));
        CHECK(control->answerOnWatch() == false);
        CHECK(api(driver, "SetRoute", {QString("watch")}).toBool() == false);
        spin(300);
        CHECK(!logged("card") && !logged("call answer"));
        // Declining and ending are the same call whoever holds the audio
        enabledItem.set(true);
        spin(500);
        CHECK(control->answerOnWatch() == true);
        CHECK(waitFor([&] { return loggedCount() >= 3; }));
        takeCalls();
        emitSignal(ofono, MODEM, "org.ofono.VoiceCallManager", "CallRemoved", {QVariant::fromValue(QDBusObjectPath(CALL1))});
        spin(300);
    }

    // A phone not bonded over BR/EDR: never connected (no pairing from here)
    {
        QMutexLocker l(&stateLock);
        bredrBonded = false;
        callPresent = false;
    }
    emitSignal(ofono, "/", "org.ofono.Manager", "ModemRemoved", {QVariant::fromValue(QDBusObjectPath(MODEM))});
    CHECK(waitFor([&] { return getState(driver).value("link").toBool() == false; }));
    takeCalls();
    spin(6000);
    CHECK(!logged("bluez connect"));

    // The phone's LE link gone: nothing more
    callAudio.setPhoneLink(false);

    fakeThread.quit();
    fakeThread.wait();
    fprintf(stderr, "%s: %d failures\n", argv[0], failures);
    return failures ? 1 : 0;
}

#include "calls_test.moc"
