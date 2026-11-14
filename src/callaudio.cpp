/*
 * Copyright (C) 2026 eos-mainline contributors
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

/*
 * Calls of the paired phone on the watch, as stock Wear OS (the Bluetooth
 * module's HeadsetClient and WearServices' HfpClientConnection): the watch is
 * the phone's hands-free unit through oFono (hfp_hf_bluez5, eos patches).
 *
 * - The call audio stays on the phone until the user answers on the watch or
 *   pulls the audio to it: the oFono card's AudioAllowed is false (the unit
 *   starts that way, OFONO_HFP_AUDIO_ALLOWED=0), so SCO from the phone is
 *   refused, and oFono tells an Android phone (AT+ANDROID=SINKAUDIOPOLICY).
 * - Answer: allow the audio, connect it (AT+BCC), answer (ATA). Answering on
 *   the phone (or "Answer calls on watch" off) keeps the audio there.
 * - Audio to the phone: withdraw the audio (oFono drops the SCO link), then
 *   allow it again so that the phone may route the call back.
 * - The last call gone: the audio is not allowed any more.
 * - Mute is local (the microphone in PulseAudio). The call volume is the
 *   Settings > Sound slider's key, /org/asteroidos/sound/call-volume
 *   (percent, steps 1..5 as AudioService's voice call stream): applied to
 *   the call's sink in PulseAudio and sent to the phone (AT+VGS) when the
 *   audio comes to the watch and whenever it changes; the phone's AT+VGS
 *   sets it too.
 * - After the service level connection: AT+NREC=0 when the phone has its
 *   own echo cancelling (the watch cancels its echo itself).
 * - "Answer calls on watch" (stock's pref_bluetoothHfp, default on) is the
 *   hands-free profile itself: with the phone's LE link up and no link, the
 *   watch connects it (stock keeps it connected), only to a phone bonded
 *   over BR/EDR so that it never starts a pairing, and only on a kernel
 *   with RFCOMM; off, the link is dropped and not made.
 * - Battery: no polling. oFono's signals, one single-shot timer for the
 *   connect attempts (at most 6 after the phone's LE link came) and one
 *   second for "audio to the phone".
 */

#include "callaudio.h"

#include <QDBusArgument>
#include <QDBusMetaType>
#include <QDBusObjectPath>
#include <QDBusPendingCall>
#include <QDBusServiceWatcher>
#include <QDBusVariant>
#include <QDebug>
#include <QJsonDocument>
#include <QJsonObject>
#include <QPointer>
#include <QProcess>
#include <QProcessEnvironment>
#include <QRegularExpression>

#include <MDConfItem>

#include <sys/socket.h>
#include <unistd.h>

#define OFONO_SERVICE          "org.ofono"
#define OFONO_MANAGER_IFACE    "org.ofono.Manager"
#define OFONO_MODEM_IFACE      "org.ofono.Modem"
#define OFONO_VCM_IFACE        "org.ofono.VoiceCallManager"
#define OFONO_CALL_IFACE       "org.ofono.VoiceCall"
#define OFONO_VOLUME_IFACE     "org.ofono.CallVolume"
#define OFONO_HANDSFREE_IFACE  "org.ofono.Handsfree"
#define OFONO_AUDIO_MGR_IFACE  "org.ofono.HandsfreeAudioManager"
#define OFONO_CARD_IFACE       "org.ofono.HandsfreeAudioCard"
#define HFP_AG_UUID            "0000111f-0000-1000-8000-00805f9b34fb"
#define BEARER_BREDR_IFACE     "org.bluez.Bearer.BREDR1"
// Settings > Bluetooth > "Answer calls on watch" (default on)
#define CALLS_ON_WATCH_KEY     "/org/asteroidos/btsyncd/calls-on-watch"
// Settings > Sound > "Call volume", percent of the maximum (default 4 of 5)
#define CALL_VOLUME_KEY        "/org/asteroidos/sound/call-volume"
#define CALL_VOLUME_DEFAULT    80

// The echo canceller's sink and source (pulseaudio module-bluetooth-policy,
// eos patch 0001): the call's far end and the watch's microphone
#define CALL_SINK   "bluetooth_ag_ec_sink"
#define CALL_SOURCE "bluetooth_ag_ec_source"

typedef QList<QPair<QString, QVariantMap>> ObjectList;

static QVariant unwrap(const QVariant &value)
{
    QVariant v = value;
    if (v.userType() == qMetaTypeId<QDBusVariant>())
        v = v.value<QDBusVariant>().variant();
    return v;
}

static QStringList toStringList(const QVariant &value)
{
    const QVariant v = unwrap(value);
    if (v.userType() == qMetaTypeId<QDBusArgument>())
        return qdbus_cast<QStringList>(v.value<QDBusArgument>());
    return v.toStringList();
}

static QVariantMap toMap(const QVariant &value)
{
    const QVariant v = unwrap(value);
    if (v.userType() == qMetaTypeId<QDBusArgument>())
        return qdbus_cast<QVariantMap>(v.value<QDBusArgument>());
    return v.toMap();
}

// a(oa{sv}), as oFono's GetModems, GetCalls and GetCards return
static ObjectList toObjectList(const QVariant &value)
{
    ObjectList list;
    const QVariant v = unwrap(value);
    if (v.userType() != qMetaTypeId<QDBusArgument>())
        return list;
    const QDBusArgument arg = v.value<QDBusArgument>();
    arg.beginArray();
    while (!arg.atEnd()) {
        QDBusObjectPath path;
        QVariantMap properties;
        arg.beginStructure();
        arg >> path >> properties;
        arg.endStructure();
        list.append(qMakePair(path.path(), properties));
    }
    arg.endArray();
    return list;
}

static QString objectPath(const QVariant &value)
{
    return unwrap(value).value<QDBusObjectPath>().path();
}

// /hfp/org/bluez/hci0/dev_00_11_22_33_44_55 -> 00:11:22:33:44:55
static QString modemAddress(const QString &path)
{
    const int i = path.lastIndexOf("/dev_");
    return i < 0 ? QString() : path.mid(i + 5).replace('_', ':').toUpper();
}

// The connect attempts' times divided in the tests (BTSYNCD_CALLS_TIME_DIV=100)
static int timeDiv()
{
    static const int div = qMax(1, qEnvironmentVariableIntValue("BTSYNCD_CALLS_TIME_DIV"));
    return div;
}

CallAudioDBusApi::CallAudioDBusApi(CallAudio *calls) : QObject(calls), mCalls(calls)
{
    connect(calls, &CallAudio::stateChanged, this, &CallAudioDBusApi::StateChanged);
}

QVariantMap CallAudioDBusApi::State()
{
    return mCalls->state();
}

bool CallAudioDBusApi::SetMuted(bool muted)
{
    return mCalls->setMuted(muted);
}

bool CallAudioDBusApi::AdjustVolume(int delta)
{
    return mCalls->adjustVolume(delta);
}

bool CallAudioDBusApi::SendTones(const QString &digits)
{
    return mCalls->sendTones(digits);
}

bool CallAudioDBusApi::SetRoute(const QString &route)
{
    if (route != QLatin1String("watch") && route != QLatin1String("phone"))
        return false;
    return mCalls->setAudioOnWatch(route == QLatin1String("watch"));
}

bool CallAudioDBusApi::PullToWatch()
{
    return mCalls->setAudioOnWatch(true);
}

CallAudio::CallAudio(QObject *parent)
    : QObject(parent), mBus(QDBusConnection::systemBus()),
      mEnabled(new MDConfItem(CALLS_ON_WATCH_KEY, this)),
      mCallVolume(new MDConfItem(CALL_VOLUME_KEY, this))
{
    mVolumePct = qBound(0, mCallVolume->value(CALL_VOLUME_DEFAULT).toInt(), 100);

    mWatcher = new QDBusServiceWatcher(OFONO_SERVICE, mBus,
                                       QDBusServiceWatcher::WatchForRegistration |
                                       QDBusServiceWatcher::WatchForUnregistration, this);
    connect(mWatcher, &QDBusServiceWatcher::serviceRegistered, this, &CallAudio::refresh);
    connect(mWatcher, &QDBusServiceWatcher::serviceUnregistered, this, [this] {
        qInfo() << "CallAudio: oFono gone";
        clearModem();
        mCard.clear();
        mAudioConnected = false;
        changed();
    });

    mBus.connect(OFONO_SERVICE, "/", OFONO_MANAGER_IFACE, "ModemAdded", this, SLOT(onModemAdded(QDBusMessage)));
    mBus.connect(OFONO_SERVICE, "/", OFONO_MANAGER_IFACE, "ModemRemoved", this, SLOT(onModemRemoved(QDBusMessage)));
    mBus.connect(OFONO_SERVICE, "/", OFONO_AUDIO_MGR_IFACE, "CardAdded", this, SLOT(onCardAdded(QDBusMessage)));
    mBus.connect(OFONO_SERVICE, "/", OFONO_AUDIO_MGR_IFACE, "CardRemoved", this, SLOT(onCardRemoved(QDBusMessage)));

    mConnectTimer.setSingleShot(true);
    connect(&mConnectTimer, &QTimer::timeout, this, &CallAudio::connectHandsfree);
    connect(mEnabled, &MDConfItem::valueChanged, this, &CallAudio::onSettingChanged);
    connect(mCallVolume, &MDConfItem::valueChanged, this, [this] {
        mVolumePct = qBound(0, mCallVolume->value(CALL_VOLUME_DEFAULT).toInt(), 100);
        applyVolume();
        changed();
    });

    QDBusConnection session = QDBusConnection::sessionBus();
    if (!session.registerObject(BTSYNCD_CALLAUDIO_PATH, new CallAudioDBusApi(this),
                                QDBusConnection::ExportAllSlots | QDBusConnection::ExportAllSignals))
        qWarning() << "CallAudio: cannot register" << BTSYNCD_CALLAUDIO_PATH << "on the session bus";

    refresh();
}

bool CallAudio::enabled() const
{
    return mEnabled->value(true).toBool();
}

bool CallAudio::available() const
{
    return !mModem.isEmpty() && mModemInterfaces.contains(OFONO_VCM_IFACE);
}

// The watch holds the call audio: a call, and the SCO link to the watch
bool CallAudio::onWatch() const
{
    return available() && !mCalls.isEmpty() && mAudioConnected;
}

// The call the view shows: a ringing or waiting one first (it needs the
// user), else the newest
QString CallAudio::mainCall() const
{
    const QString ringing = ringingCall();
    if (!ringing.isEmpty())
        return ringing;
    return mCallOrder.isEmpty() ? QString() : mCallOrder.last();
}

QString CallAudio::ringingCall() const
{
    for (const QString &path : mCallOrder) {
        const QString &s = mCalls.value(path).state;
        if (s == "incoming" || s == "waiting")
            return path;
    }
    return QString();
}

int CallAudio::volumePercent() const
{
    return mVolumePct;
}

// 1..VolumeMax, stock's lowest call step is 1
int CallAudio::volumeStep() const
{
    return qBound(1, qRound(mVolumePct * VolumeMax / 100.0), VolumeMax);
}

QVariantMap CallAudio::state() const
{
    const bool watch = onWatch();
    QVariantMap map;
    map.insert("available", watch);
    map.insert("muted", mMuted);
    map.insert("route", watch ? QStringLiteral("watch") : QStringLiteral("phone"));
    // the phone's own call volume while the call is on the phone (cvol), else the watch's step
    map.insert("volume", watch ? volumeStep() : mPhoneVolume);
    map.insert("volumeMax", watch ? int(VolumeMax) : mPhoneVolumeMax);
    map.insert("callState", mCalls.value(mainCall()).state);
    map.insert("link", available());
    return map;
}

void CallAudio::changed()
{
    const QVariantMap now = state();
    if (now == mLast)
        return;
    mLast = now;
    emit stateChanged(now);
}

void CallAudio::refresh()
{
    QDBusMessage reply = mBus.call(QDBusMessage::createMethodCall(OFONO_SERVICE, "/", OFONO_MANAGER_IFACE, "GetModems"),
                                   QDBus::Block, 3000);
    if (reply.type() != QDBusMessage::ReplyMessage || reply.arguments().isEmpty()) {
        clearModem();
        changed();
        return;
    }
    clearModem();
    for (const auto &m : toObjectList(reply.arguments().at(0))) {
        if (m.second.value("Type").toString() == "hfp") {
            setModem(m.first, m.second);
            break;
        }
    }

    reply = mBus.call(QDBusMessage::createMethodCall(OFONO_SERVICE, "/", OFONO_AUDIO_MGR_IFACE, "GetCards"),
                      QDBus::Block, 3000);
    mCard.clear();
    mAudioConnected = false;
    if (reply.type() == QDBusMessage::ReplyMessage && !reply.arguments().isEmpty())
        for (const auto &c : toObjectList(reply.arguments().at(0)))
            setCard(c.first, c.second);
    changed();
}

void CallAudio::setModem(const QString &path, const QVariantMap &properties)
{
    if (!mModem.isEmpty() && mModem != path)
        return;
    const bool fresh = mModem.isEmpty();
    mModem = path;
    if (fresh) {
        qInfo() << "CallAudio: hands-free link" << path;
        mBus.connect(OFONO_SERVICE, mModem, OFONO_MODEM_IFACE, "PropertyChanged", this, SLOT(onModemPropertyChanged(QDBusMessage)));
        mBus.connect(OFONO_SERVICE, mModem, OFONO_VCM_IFACE, "CallAdded", this, SLOT(onCallAdded(QDBusMessage)));
        mBus.connect(OFONO_SERVICE, mModem, OFONO_VCM_IFACE, "CallRemoved", this, SLOT(onCallRemoved(QDBusMessage)));
        mBus.connect(OFONO_SERVICE, mModem, OFONO_VOLUME_IFACE, "PropertyChanged", this, SLOT(onVolumePropertyChanged(QDBusMessage)));
        mNrecDone = false;
    }
    mModemInterfaces = toStringList(properties.value("Interfaces"));
    watchModemInterfaces();
    mConnectTimer.stop();
    mConnectDelay = 0;
    mConnectTries = 0;
    // "Answer calls on watch" went off while the phone was making the link
    if (!enabled())
        disconnectHandsfree();
}

void CallAudio::clearModem()
{
    if (mModem.isEmpty())
        return;
    mBus.disconnect(OFONO_SERVICE, mModem, OFONO_MODEM_IFACE, "PropertyChanged", this, SLOT(onModemPropertyChanged(QDBusMessage)));
    mBus.disconnect(OFONO_SERVICE, mModem, OFONO_VCM_IFACE, "CallAdded", this, SLOT(onCallAdded(QDBusMessage)));
    mBus.disconnect(OFONO_SERVICE, mModem, OFONO_VCM_IFACE, "CallRemoved", this, SLOT(onCallRemoved(QDBusMessage)));
    mBus.disconnect(OFONO_SERVICE, mModem, OFONO_VOLUME_IFACE, "PropertyChanged", this, SLOT(onVolumePropertyChanged(QDBusMessage)));
    for (const QString &c : mCallOrder)
        mBus.disconnect(OFONO_SERVICE, c, OFONO_CALL_IFACE, "PropertyChanged", this, SLOT(onCallPropertyChanged(QDBusMessage)));
    qInfo() << "CallAudio: hands-free link gone";
    mModem.clear();
    mModemInterfaces.clear();
    mCalls.clear();
    mCallOrder.clear();
    mOfonoVolume = -1;
    mAppliedVolume = -1;
    if (mMuted) {
        mMuted = false;
        muteMicrophone(false);
    }
    scheduleConnect(true);
}

// The atoms come up one by one after the service level connection
void CallAudio::watchModemInterfaces()
{
    if (mModemInterfaces.contains(OFONO_VCM_IFACE) && mCalls.isEmpty()) {
        const QDBusMessage reply = mBus.call(QDBusMessage::createMethodCall(OFONO_SERVICE, mModem, OFONO_VCM_IFACE, "GetCalls"),
                                             QDBus::Block, 3000);
        if (reply.type() == QDBusMessage::ReplyMessage && !reply.arguments().isEmpty()) {
            for (const auto &c : toObjectList(reply.arguments().at(0)))
                addCall(c.first, c.second);
            // A call already up when the link came: its audio stays where
            // it is (stock: the route is not allowed)
            if (!mCalls.isEmpty())
                setAudioAllowed(false);
        }
    }
    if (mModemInterfaces.contains(OFONO_HANDSFREE_IFACE) && !mNrecDone) {
        mNrecDone = true;
        const QDBusMessage reply = mBus.call(QDBusMessage::createMethodCall(OFONO_SERVICE, mModem, OFONO_HANDSFREE_IFACE, "GetProperties"),
                                             QDBus::Block, 3000);
        if (reply.type() == QDBusMessage::ReplyMessage && !reply.arguments().isEmpty() &&
            toMap(reply.arguments().at(0)).value("EchoCancelingNoiseReduction").toBool()) {
            qInfo() << "CallAudio: the phone's echo cancelling off (AT+NREC=0)";
            QDBusMessage nrec = QDBusMessage::createMethodCall(OFONO_SERVICE, mModem, OFONO_HANDSFREE_IFACE, "SetProperty");
            nrec << QStringLiteral("EchoCancelingNoiseReduction") << QVariant::fromValue(QDBusVariant(false));
            mBus.call(nrec, QDBus::NoBlock);
        }
    }
    changed();
}

void CallAudio::onModemAdded(const QDBusMessage &msg)
{
    if (msg.arguments().size() < 2)
        return;
    const QVariantMap properties = toMap(msg.arguments().at(1));
    if (properties.value("Type").toString() == "hfp" && mModem.isEmpty())
        setModem(objectPath(msg.arguments().at(0)), properties);
    changed();
}

void CallAudio::onModemRemoved(const QDBusMessage &msg)
{
    if (msg.arguments().isEmpty() || objectPath(msg.arguments().at(0)) != mModem)
        return;
    clearModem();
    changed();
}

void CallAudio::onModemPropertyChanged(const QDBusMessage &msg)
{
    if (msg.arguments().size() < 2 || msg.arguments().at(0).toString() != "Interfaces")
        return;
    mModemInterfaces = toStringList(msg.arguments().at(1));
    if (!mModemInterfaces.contains(OFONO_VCM_IFACE)) {
        for (const QString &c : mCallOrder)
            mBus.disconnect(OFONO_SERVICE, c, OFONO_CALL_IFACE, "PropertyChanged", this, SLOT(onCallPropertyChanged(QDBusMessage)));
        mCalls.clear();
        mCallOrder.clear();
    }
    watchModemInterfaces();
}

void CallAudio::addCall(const QString &path, const QVariantMap &properties)
{
    if (!mCalls.contains(path)) {
        mBus.connect(OFONO_SERVICE, path, OFONO_CALL_IFACE, "PropertyChanged", this, SLOT(onCallPropertyChanged(QDBusMessage)));
        mCallOrder.append(path);
    }
    Call &c = mCalls[path];
    c.state = properties.value("State").toString();
    qInfo() << "CallAudio: call" << path << c.state;
}

void CallAudio::onCallAdded(const QDBusMessage &msg)
{
    if (msg.arguments().size() < 2)
        return;
    addCall(objectPath(msg.arguments().at(0)), toMap(msg.arguments().at(1)));
    changed();
}

void CallAudio::onCallRemoved(const QDBusMessage &msg)
{
    if (msg.arguments().isEmpty())
        return;
    const QString path = objectPath(msg.arguments().at(0));
    if (!mCalls.contains(path))
        return;
    mBus.disconnect(OFONO_SERVICE, path, OFONO_CALL_IFACE, "PropertyChanged", this, SLOT(onCallPropertyChanged(QDBusMessage)));
    mCalls.remove(path);
    mCallOrder.removeAll(path);
    qInfo() << "CallAudio: call" << path << "gone";
    if (mCalls.isEmpty()) {
        // The last call gone: no audio on the watch until the next answer
        setAudioAllowed(false);
        if (mMuted) {
            mMuted = false;
            muteMicrophone(false);
        }
    }
    changed();
}

void CallAudio::onCallPropertyChanged(const QDBusMessage &msg)
{
    if (msg.arguments().size() < 2 || !mCalls.contains(msg.path()))
        return;
    if (msg.arguments().at(0).toString() == "State") {
        mCalls[msg.path()].state = unwrap(msg.arguments().at(1)).toString();
        qInfo() << "CallAudio: call" << msg.path() << mCalls[msg.path()].state;
    }
    changed();
}

// The phone's AT+VGS (or our own SetProperty coming back): the call volume
void CallAudio::onVolumePropertyChanged(const QDBusMessage &msg)
{
    if (msg.arguments().size() < 2 || msg.arguments().at(0).toString() != "SpeakerVolume")
        return;
    const int percent = qBound(0, unwrap(msg.arguments().at(1)).toInt(), 100);
    mOfonoVolume = percent;
    if (percent == mVolumePct)
        return;
    mVolumePct = percent;
    // One setting with the Settings slider; its change applies the sink
    mCallVolume->set(percent);
    applyVolume();
    changed();
}

void CallAudio::setCard(const QString &path, const QVariantMap &properties)
{
    // One phone: its card (the modem's address when both are known)
    const QString remote = properties.value("RemoteAddress").toString().toUpper();
    if (!mModem.isEmpty() && !remote.isEmpty() && remote != modemAddress(mModem))
        return;
    if (!mCard.isEmpty())
        mBus.disconnect(OFONO_SERVICE, mCard, OFONO_CARD_IFACE, "PropertyChanged", this, SLOT(onCardPropertyChanged(QDBusMessage)));
    mCard = path;
    mBus.connect(OFONO_SERVICE, mCard, OFONO_CARD_IFACE, "PropertyChanged", this, SLOT(onCardPropertyChanged(QDBusMessage)));
    mAudioAllowed = properties.value("AudioAllowed", true).toBool();
    mAudioConnected = properties.value("AudioConnected").toBool();
    qInfo() << "CallAudio: audio card" << path << "allowed" << mAudioAllowed << "connected" << mAudioConnected;
}

void CallAudio::onCardAdded(const QDBusMessage &msg)
{
    if (msg.arguments().size() < 2)
        return;
    setCard(objectPath(msg.arguments().at(0)), toMap(msg.arguments().at(1)));
    changed();
}

void CallAudio::onCardRemoved(const QDBusMessage &msg)
{
    if (msg.arguments().isEmpty() || objectPath(msg.arguments().at(0)) != mCard)
        return;
    mBus.disconnect(OFONO_SERVICE, mCard, OFONO_CARD_IFACE, "PropertyChanged", this, SLOT(onCardPropertyChanged(QDBusMessage)));
    mCard.clear();
    mAudioConnected = false;
    mAppliedVolume = -1;
    changed();
}

void CallAudio::onCardPropertyChanged(const QDBusMessage &msg)
{
    if (msg.arguments().size() < 2 || msg.path() != mCard)
        return;
    const QString name = msg.arguments().at(0).toString();
    const bool value = unwrap(msg.arguments().at(1)).toBool();
    if (name == "AudioAllowed") {
        mAudioAllowed = value;
    } else if (name == "AudioConnected") {
        mAudioConnected = value;
        qInfo() << "CallAudio: call audio" << (value ? "on the watch" : "off the watch");
        // The next time the audio comes, the sink and the phone get the level again
        mAppliedVolume = -1;
        if (value)
            applyVolume();
    }
    changed();
}

// In order with Connect and Answer that follow: the bus delivers one
// client's calls in the order they were sent
void CallAudio::setAudioAllowed(bool allowed)
{
    if (mCard.isEmpty())
        return;
    QDBusMessage set = QDBusMessage::createMethodCall(OFONO_SERVICE, mCard, OFONO_CARD_IFACE, "SetProperty");
    set << QStringLiteral("AudioAllowed") << QVariant::fromValue(QDBusVariant(allowed));
    mBus.call(set, QDBus::NoBlock);
    mAudioAllowed = allowed;
}

// Ask for the audio (AT+BCC; the phone then opens the SCO link)
void CallAudio::connectAudio()
{
    if (mCard.isEmpty())
        return;
    QDBusPendingCall pending = mBus.asyncCall(QDBusMessage::createMethodCall(OFONO_SERVICE, mCard, OFONO_CARD_IFACE, "Connect"));
    auto *watcher = new QDBusPendingCallWatcher(pending, this);
    connect(watcher, &QDBusPendingCallWatcher::finished, this, [](QDBusPendingCallWatcher *w) {
        if (w->isError())
            qInfo() << "CallAudio: audio connect:" << w->error().message();
        w->deleteLater();
    });
}

bool CallAudio::answerOnWatch()
{
    const QString ringing = ringingCall();
    if (!enabled() || !available() || ringing.isEmpty() || mCard.isEmpty())
        return false;
    qInfo() << "CallAudio: answer on the watch" << ringing;
    setAudioAllowed(true);
    connectAudio();
    mBus.call(QDBusMessage::createMethodCall(OFONO_SERVICE, ringing, OFONO_CALL_IFACE, "Answer"), QDBus::NoBlock);
    return true;
}

bool CallAudio::hangUp()
{
    const QString path = mainCall();
    if (!available() || path.isEmpty())
        return false;
    qInfo() << "CallAudio: hang up" << path << mCalls.value(path).state;
    mBus.call(QDBusMessage::createMethodCall(OFONO_SERVICE, path, OFONO_CALL_IFACE, "Hangup"), QDBus::NoBlock);
    return true;
}

bool CallAudio::setAudioOnWatch(bool watch)
{
    if (!available() || mCard.isEmpty() || mCalls.isEmpty())
        return false;
    if (watch && !enabled())
        return false;
    qInfo() << "CallAudio: call audio to" << (watch ? "the watch" : "the phone");
    if (watch) {
        setAudioAllowed(true);
        connectAudio();
    } else {
        // oFono drops the SCO link; the phone takes the audio back. Then
        // allowed again, as stock, so that the phone may route it back
        setAudioAllowed(false);
        QTimer::singleShot(1000, this, [this] {
            if (!mCalls.isEmpty())
                setAudioAllowed(true);
        });
    }
    return true;
}

bool CallAudio::setMuted(bool muted)
{
    if (mCalls.isEmpty())
        return false;
    mMuted = muted;
    muteMicrophone(muted);
    changed();
    return true;
}

// The echo canceller's source, and the microphone itself in case the call
// runs without the echo canceller (it failed to load)
void CallAudio::muteMicrophone(bool muted)
{
    pactl(QStringLiteral("set-source-mute " CALL_SOURCE " %1").arg(muted ? 1 : 0));
    pactl(QStringLiteral("set-source-mute @DEFAULT_SOURCE@ %1").arg(muted ? 1 : 0));
}

// One step of the call volume, with the audio on the watch (with it on the
// phone the volume is the phone's: a Gadgetbridge command, not here)
bool CallAudio::adjustVolume(int delta)
{
    if (!onWatch())
        return adjustPhoneVolume(delta);
    const int step = qBound(1, volumeStep() + delta, int(VolumeMax));
    mVolumePct = step * 100 / VolumeMax;
    mCallVolume->set(mVolumePct);
    applyVolume();
    changed();
    return true;
}

// The call's far end on the watch: the echo canceller's sink; and the phone
// (AT+VGS) unless it is where the level came from
void CallAudio::applyVolume()
{
    if (!mAudioConnected || mVolumePct == mAppliedVolume)
        return;
    mAppliedVolume = mVolumePct;
    pactl(QStringLiteral("set-sink-volume " CALL_SINK " %1%").arg(mVolumePct));
    if (mVolumePct != mOfonoVolume && available() && mModemInterfaces.contains(OFONO_VOLUME_IFACE)) {
        mOfonoVolume = mVolumePct;
        QDBusMessage set = QDBusMessage::createMethodCall(OFONO_SERVICE, mModem, OFONO_VOLUME_IFACE, "SetProperty");
        set << QStringLiteral("SpeakerVolume") << QVariant::fromValue(QDBusVariant(QVariant::fromValue<uchar>(mVolumePct)));
        mBus.call(set, QDBus::NoBlock);
    }
}

// DTMF digits of the keypad (AT+VTS): 0-9 * # A-D, a call in progress
bool CallAudio::sendTones(const QString &digits)
{
    static const QRegularExpression valid("^[0-9*#A-D]{1,32}$");
    const QString tones = digits.toUpper();
    const QString path = mainCall();
    if (!available() || !mModemInterfaces.contains(OFONO_VCM_IFACE) || path.isEmpty()
            || mCalls.value(path).state != QLatin1String("active") || !valid.match(tones).hasMatch())
        return false;
    QDBusMessage send = QDBusMessage::createMethodCall(OFONO_SERVICE, mModem, OFONO_VCM_IFACE, "SendTones");
    send << tones;
    mBus.call(send, QDBus::NoBlock);
    return true;
}

// pactl (pulseaudio-misc) in the user session; BTSYNCD_PACTL replaces it in
// the tests
void CallAudio::pactl(const QString &args)
{
    const QString cmd = QProcessEnvironment::systemEnvironment().value("BTSYNCD_PACTL", "pactl");
    QStringList list = args.split(' ', Qt::SkipEmptyParts);
    QProcess::startDetached(cmd, list);
}

bool CallAudio::adjustPhoneVolume(int delta)
{
    if (delta == 0 || !mRelay || !mRelay->relayHas(QStringLiteral("cvol")))
        return false;
    mPhoneVolumeDelta = qBound(-VolumeMax, mPhoneVolumeDelta + delta, int(VolumeMax));
    sendPhoneVolume();
    return true;
}

// One act at a time, one step each (the act steps by 1): the crown's detents that come while it
// is on its way are sent as it answers
void CallAudio::sendPhoneVolume()
{
    if (mPhoneVolumeBusy || mPhoneVolumeDelta == 0 || !mRelay)
        return;
    const int step = mPhoneVolumeDelta > 0 ? 1 : -1;
    mPhoneVolumeDelta -= step;
    mPhoneVolumeBusy = true;
    const QPointer<CallAudio> self(this);
    const bool sent = mRelay->relayAct(QByteArrayLiteral("{\"a\":\"cvol\",\"d\":") + QByteArray::number(step) + '}',
                                       [self](const QString &json) {
        if (!self)
            return;
        self->mPhoneVolumeBusy = false;
        self->onPhoneVolume(json);
        self->sendPhoneVolume();
    });
    if (!sent) {
        mPhoneVolumeBusy = false;
        mPhoneVolumeDelta = 0;
    }
}

void CallAudio::onPhoneVolume(const QString &json)
{
    const QJsonObject o = QJsonDocument::fromJson(json.toUtf8()).object();
    const int v = o.value(QStringLiteral("v")).toInt(-1);
    const int max = o.value(QStringLiteral("max")).toInt(0);
    if (!o.value(QStringLiteral("ok")).toBool() || max < 1 || max > 100 || v < 0 || v > max) {
        // a refusal or a timeout: the steps still waiting are dropped
        mPhoneVolumeDelta = 0;
        return;
    }
    mPhoneVolume = v;
    mPhoneVolumeMax = max;
    qInfo() << "CallAudio: the phone's call volume" << v << "of" << max;
    changed();
}

void CallAudio::setPhoneLink(bool connected)
{
    mPhoneLink = connected;
    // the phone's volume is the phone's: not kept across its absence
    if (!connected && (mPhoneVolume >= 0 || mPhoneVolumeMax > 0)) {
        mPhoneVolume = -1;
        mPhoneVolumeMax = 0;
        mPhoneVolumeDelta = 0;
        changed();
    }
    scheduleConnect(true);
}

// "Answer calls on watch" switched: the profile itself comes or goes
void CallAudio::onSettingChanged()
{
    if (enabled()) {
        scheduleConnect(true);
    } else {
        mConnectTimer.stop();
        disconnectHandsfree();
    }
    changed();
}

// A kernel without RFCOMM (a build before CONFIG_BT_RFCOMM) cannot carry the
// hands-free profile: no attempts, no failing connections for the phone to
// see. The socket also loads the module (bt-proto-3) when there is one.
// BTSYNCD_RFCOMM=0/1 stands in for the kernel in the tests.
bool CallAudio::rfcommAvailable()
{
    if (qEnvironmentVariableIsSet("BTSYNCD_RFCOMM"))
        return qEnvironmentVariableIntValue("BTSYNCD_RFCOMM") != 0;
    const int af_bluetooth = 31, btproto_rfcomm = 3;
    const int fd = ::socket(af_bluetooth, SOCK_STREAM, btproto_rfcomm);
    if (fd < 0)
        return false;
    ::close(fd);
    return true;
}

// The next attempt, 5 s after the phone's LE link came (fresh), then 30 s
// doubling; at most ConnectAttempts of them, so that a phone that does not
// take the profile (its "Phone calls" off for the watch) is not asked all day
void CallAudio::scheduleConnect(bool fresh)
{
    if (fresh) {
        mConnectDelay = 0;
        mConnectTries = 0;
    }
    if (!mPhoneLink || available() || !enabled() || mConnectTries >= ConnectAttempts) {
        mConnectTimer.stop();
        mConnectDelay = 0;
        return;
    }
    if (!rfcommAvailable()) {
        if (fresh)
            qInfo() << "CallAudio: this kernel has no RFCOMM: no hands-free link";
        mConnectTimer.stop();
        return;
    }
    mConnectDelay = mConnectDelay == 0 ? 5000 : qMin(mConnectDelay == 5000 ? 30000 : mConnectDelay * 2, 600000);
    mConnectTries++;
    mConnectTimer.start(mConnectDelay / timeDiv());
}

// Connect the hands-free profile to the connected phone: one bonded over
// BR/EDR (no pairing ever starts from here) that offers the audio gateway
void CallAudio::connectHandsfree()
{
    if (!mPhoneLink || available() || !enabled())
        return;

    const QDBusMessage reply = mBus.call(QDBusMessage::createMethodCall(BLUEZ_SERVICE_NAME, "/", DBUS_OM_IFACE, "GetManagedObjects"),
                                         QDBus::Block, 3000);
    QString device;
    if (reply.type() == QDBusMessage::ReplyMessage && !reply.arguments().isEmpty()) {
        const QDBusArgument arg = reply.arguments().at(0).value<QDBusArgument>();
        arg.beginMap();
        while (!arg.atEnd()) {
            QDBusObjectPath path;
            QMap<QString, QVariantMap> interfaces;
            arg.beginMapEntry();
            arg >> path >> interfaces;
            arg.endMapEntry();
            const QVariantMap dev = interfaces.value(DEVICE_MANAGER_IFACE);
            if (dev.isEmpty() || !dev.value("Connected").toBool())
                continue;
            if (!interfaces.value(BEARER_BREDR_IFACE).value("Bonded").toBool())
                continue;
            if (!toStringList(dev.value("UUIDs")).contains(HFP_AG_UUID, Qt::CaseInsensitive))
                continue;
            device = path.path();
        }
        arg.endMap();
    }

    if (device.isEmpty()) {
        qInfo() << "CallAudio: no connected phone bonded over BR/EDR with the audio gateway profile";
    } else {
        qInfo() << "CallAudio: connecting the hands-free profile to" << device;
        QDBusMessage connectProfile = QDBusMessage::createMethodCall(BLUEZ_SERVICE_NAME, device, DEVICE_MANAGER_IFACE, "ConnectProfile");
        connectProfile << QStringLiteral(HFP_AG_UUID);
        QDBusPendingCall pending = mBus.asyncCall(connectProfile, 30000);
        auto *watcher = new QDBusPendingCallWatcher(pending, this);
        connect(watcher, &QDBusPendingCallWatcher::finished, this, [](QDBusPendingCallWatcher *w) {
            if (w->isError())
                qInfo() << "CallAudio: hands-free connect:" << w->error().message();
            w->deleteLater();
        });
    }
    // Try again later unless the link comes up (setModem stops it)
    scheduleConnect(false);
}

// "Answer calls on watch" off: drop the link (oFono's modem is the device
// under /hfp)
void CallAudio::disconnectHandsfree()
{
    if (mModem.isEmpty() || !mModem.startsWith(QLatin1String("/hfp/")))
        return;
    const QString device = mModem.mid(4);
    qInfo() << "CallAudio: dropping the hands-free profile of" << device;
    QDBusMessage drop = QDBusMessage::createMethodCall(BLUEZ_SERVICE_NAME, device, DEVICE_MANAGER_IFACE, "DisconnectProfile");
    drop << QStringLiteral(HFP_AG_UUID);
    QDBusPendingCall pending = mBus.asyncCall(drop, 30000);
    auto *watcher = new QDBusPendingCallWatcher(pending, this);
    connect(watcher, &QDBusPendingCallWatcher::finished, this, [](QDBusPendingCallWatcher *w) {
        if (w->isError())
            qInfo() << "CallAudio: hands-free disconnect:" << w->error().message();
        w->deleteLater();
    });
}
