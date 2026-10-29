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
 * Calls of the paired phone on the watch, as stock Wear OS (the Bluetooth
 * module's HeadsetClient and WearServices' HfpClientConnection): the watch is
 * the phone's hands-free unit through oFono (hfp_hf_bluez5, eos patches).
 *
 * - The call audio stays on the phone until the user answers on the watch or
 *   pulls the audio to it: the oFono card's AudioAllowed is false (the unit
 *   starts that way, OFONO_HFP_AUDIO_ALLOWED=0), so SCO from the phone is
 *   refused, and oFono tells an Android phone (AT+ANDROID=SINKAUDIOPOLICY).
 * - Answer: allow the audio, connect it (AT+BCC), answer (ATA).
 * - Audio to the phone: withdraw the audio (oFono drops the SCO link), then
 *   allow it again so that the phone may route the call back.
 * - The last call gone: the audio is not allowed any more.
 * - Mute is local (the microphone in PulseAudio). The volume is local (the
 *   call's sink in PulseAudio) and sent to the phone (AT+VGS); the phone's
 *   AT+VGS sets it too.
 * - After the service level connection: AT+NREC=0 when the phone has its
 *   own echo cancelling (the watch cancels its echo itself).
 * - With the phone's LE link up and no hands-free link, the watch connects
 *   the hands-free profile (stock keeps it connected), only to a phone
 *   bonded over BR/EDR, so that it never starts a pairing.
 */

#include "callaudio.h"

#include <QDBusArgument>
#include <QDBusMetaType>
#include <QDBusObjectPath>
#include <QDBusPendingCall>
#include <QDBusServiceWatcher>
#include <QDBusVariant>
#include <QDateTime>
#include <QDebug>
#include <QProcess>
#include <QProcessEnvironment>

#include <MDConfItem>

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
// The stock "Answer calls on watch" setting (default on)
#define CALLS_ON_WATCH_KEY     "/org/asteroidos/btsyncd/calls-on-watch"

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

CallsDBusApi::CallsDBusApi(CallAudio *calls) : QObject(calls), mCalls(calls)
{
    connect(calls, &CallAudio::callChanged, this, &CallsDBusApi::CallChanged);
}

QVariantMap CallsDBusApi::GetCall()
{
    return mCalls->call();
}

bool CallsDBusApi::Answer()
{
    return mCalls->answer();
}

bool CallsDBusApi::Hangup()
{
    return mCalls->hangup();
}

bool CallsDBusApi::SetMuted(bool muted)
{
    return mCalls->setMuted(muted);
}

bool CallsDBusApi::SetVolume(int volume)
{
    return mCalls->setVolume(volume);
}

bool CallsDBusApi::SetAudioOnWatch(bool onWatch)
{
    return mCalls->setAudioOnWatch(onWatch);
}

CallAudio::CallAudio(QObject *parent)
    : QObject(parent), mBus(QDBusConnection::systemBus()),
      mEnabled(new MDConfItem(CALLS_ON_WATCH_KEY, this))
{
    mWatcher = new QDBusServiceWatcher(OFONO_SERVICE, mBus,
                                       QDBusServiceWatcher::WatchForRegistration |
                                       QDBusServiceWatcher::WatchForUnregistration, this);
    connect(mWatcher, &QDBusServiceWatcher::serviceRegistered, this, &CallAudio::refresh);
    connect(mWatcher, &QDBusServiceWatcher::serviceUnregistered, this, [this] {
        qInfo() << "CallAudio: oFono gone";
        clearModem();
        mCard.clear();
        mAudioConnected = false;
        callsChanged();
    });

    mBus.connect(OFONO_SERVICE, "/", OFONO_MANAGER_IFACE, "ModemAdded", this, SLOT(onModemAdded(QDBusMessage)));
    mBus.connect(OFONO_SERVICE, "/", OFONO_MANAGER_IFACE, "ModemRemoved", this, SLOT(onModemRemoved(QDBusMessage)));
    mBus.connect(OFONO_SERVICE, "/", OFONO_AUDIO_MGR_IFACE, "CardAdded", this, SLOT(onCardAdded(QDBusMessage)));
    mBus.connect(OFONO_SERVICE, "/", OFONO_AUDIO_MGR_IFACE, "CardRemoved", this, SLOT(onCardRemoved(QDBusMessage)));

    mConnectTimer.setSingleShot(true);
    connect(&mConnectTimer, &QTimer::timeout, this, &CallAudio::connectHandsfree);
    connect(mEnabled, &MDConfItem::valueChanged, this, [this] { scheduleConnect(true); });

    QDBusConnection session = QDBusConnection::sessionBus();
    if (!session.registerObject(BTSYNCD_CALLS_PATH, new CallsDBusApi(this),
                                QDBusConnection::ExportAllSlots | QDBusConnection::ExportAllSignals))
        qWarning() << "CallAudio: cannot register" << BTSYNCD_CALLS_PATH << "on the session bus";

    refresh();
}

bool CallAudio::available() const
{
    return !mModem.isEmpty() && mModemInterfaces.contains(OFONO_VCM_IFACE);
}

// The call the view shows: a ringing or waiting one first (it needs the
// user), else the newest
QString CallAudio::mainCall() const
{
    for (const QString &path : mCallOrder) {
        const QString &s = mCalls.value(path).state;
        if (s == "incoming" || s == "waiting")
            return path;
    }
    return mCallOrder.isEmpty() ? QString() : mCallOrder.last();
}

QVariantMap CallAudio::call() const
{
    QVariantMap map;
    const Call c = mCalls.value(mainCall());
    map.insert("available", available());
    map.insert("state", c.state);
    map.insert("number", c.number);
    map.insert("name", c.name);
    map.insert("activeSince", c.activeSince);
    map.insert("audioOnWatch", mAudioConnected);
    map.insert("muted", mMuted);
    map.insert("volume", mVolume < 0 ? 60 : mVolume);
    return map;
}

void CallAudio::callsChanged()
{
    const QVariantMap now = call();
    if (now == mLast)
        return;
    mLast = now;
    emit callChanged(now);
}

void CallAudio::refresh()
{
    QDBusMessage reply = mBus.call(QDBusMessage::createMethodCall(OFONO_SERVICE, "/", OFONO_MANAGER_IFACE, "GetModems"),
                                   QDBus::Block, 3000);
    if (reply.type() != QDBusMessage::ReplyMessage || reply.arguments().isEmpty()) {
        clearModem();
        callsChanged();
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
    callsChanged();
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
    mVolume = -1;
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
    if (mModemInterfaces.contains(OFONO_VOLUME_IFACE) && mVolume < 0) {
        const QDBusMessage reply = mBus.call(QDBusMessage::createMethodCall(OFONO_SERVICE, mModem, OFONO_VOLUME_IFACE, "GetProperties"),
                                             QDBus::Block, 3000);
        if (reply.type() == QDBusMessage::ReplyMessage && !reply.arguments().isEmpty())
            mVolume = toMap(reply.arguments().at(0)).value("SpeakerVolume").toInt();
        applyVolume();
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
    callsChanged();
}

void CallAudio::onModemAdded(const QDBusMessage &msg)
{
    if (msg.arguments().size() < 2)
        return;
    const QVariantMap properties = toMap(msg.arguments().at(1));
    if (properties.value("Type").toString() == "hfp" && mModem.isEmpty())
        setModem(objectPath(msg.arguments().at(0)), properties);
    callsChanged();
}

void CallAudio::onModemRemoved(const QDBusMessage &msg)
{
    if (msg.arguments().isEmpty() || objectPath(msg.arguments().at(0)) != mModem)
        return;
    clearModem();
    callsChanged();
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
    c.number = properties.value("LineIdentification").toString();
    c.name = properties.value("Name").toString();
    if (c.state == "active" && c.activeSince == 0)
        c.activeSince = QDateTime::currentMSecsSinceEpoch();
    qInfo() << "CallAudio: call" << path << c.state;
}

void CallAudio::onCallAdded(const QDBusMessage &msg)
{
    if (msg.arguments().size() < 2)
        return;
    addCall(objectPath(msg.arguments().at(0)), toMap(msg.arguments().at(1)));
    callsChanged();
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
    callsChanged();
}

void CallAudio::onCallPropertyChanged(const QDBusMessage &msg)
{
    if (msg.arguments().size() < 2 || !mCalls.contains(msg.path()))
        return;
    Call &c = mCalls[msg.path()];
    const QString name = msg.arguments().at(0).toString();
    const QVariant value = unwrap(msg.arguments().at(1));
    if (name == "State") {
        c.state = value.toString();
        if (c.state == "active" && c.activeSince == 0)
            c.activeSince = QDateTime::currentMSecsSinceEpoch();
        qInfo() << "CallAudio: call" << msg.path() << c.state;
    } else if (name == "LineIdentification") {
        c.number = value.toString();
    } else if (name == "Name") {
        c.name = value.toString();
    }
    callsChanged();
}

void CallAudio::onVolumePropertyChanged(const QDBusMessage &msg)
{
    if (msg.arguments().size() < 2 || msg.arguments().at(0).toString() != "SpeakerVolume")
        return;
    // The phone's AT+VGS (or our own SetProperty coming back)
    mVolume = unwrap(msg.arguments().at(1)).toInt();
    applyVolume();
    callsChanged();
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
    callsChanged();
}

void CallAudio::onCardRemoved(const QDBusMessage &msg)
{
    if (msg.arguments().isEmpty() || objectPath(msg.arguments().at(0)) != mCard)
        return;
    mBus.disconnect(OFONO_SERVICE, mCard, OFONO_CARD_IFACE, "PropertyChanged", this, SLOT(onCardPropertyChanged(QDBusMessage)));
    mCard.clear();
    mAudioConnected = false;
    callsChanged();
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
        if (value)
            applyVolume();
    }
    callsChanged();
}

void CallAudio::setAudioAllowed(bool allowed)
{
    if (mCard.isEmpty())
        return;
    QDBusMessage set = QDBusMessage::createMethodCall(OFONO_SERVICE, mCard, OFONO_CARD_IFACE, "SetProperty");
    set << QStringLiteral("AudioAllowed") << QVariant::fromValue(QDBusVariant(allowed));
    // Blocking: the audio must be allowed before Connect and Answer follow
    const QDBusMessage reply = mBus.call(set, QDBus::Block, 3000);
    if (reply.type() == QDBusMessage::ErrorMessage)
        qWarning() << "CallAudio: AudioAllowed" << allowed << reply.errorMessage();
    else
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

bool CallAudio::answer()
{
    QString ringing;
    for (const QString &path : mCallOrder)
        if (mCalls.value(path).state == "incoming" || mCalls.value(path).state == "waiting")
            ringing = path;
    if (!available() || ringing.isEmpty())
        return false;
    qInfo() << "CallAudio: answer on the watch" << ringing;
    setAudioAllowed(true);
    connectAudio();
    mBus.call(QDBusMessage::createMethodCall(OFONO_SERVICE, ringing, OFONO_CALL_IFACE, "Answer"), QDBus::NoBlock);
    return true;
}

bool CallAudio::hangup()
{
    const QString path = mainCall();
    if (!available() || path.isEmpty())
        return false;
    qInfo() << "CallAudio: hang up" << path << mCalls.value(path).state;
    mBus.call(QDBusMessage::createMethodCall(OFONO_SERVICE, path, OFONO_CALL_IFACE, "Hangup"), QDBus::NoBlock);
    return true;
}

bool CallAudio::setAudioOnWatch(bool onWatch)
{
    if (!available() || mCard.isEmpty() || mCalls.isEmpty())
        return false;
    qInfo() << "CallAudio: call audio to" << (onWatch ? "the watch" : "the phone");
    if (onWatch) {
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
    callsChanged();
    return true;
}

// The echo canceller's source, and the microphone itself in case the call
// runs without the echo canceller (it failed to load)
void CallAudio::muteMicrophone(bool muted)
{
    pactl(QStringLiteral("set-source-mute " CALL_SOURCE " %1").arg(muted ? 1 : 0));
    pactl(QStringLiteral("set-source-mute @DEFAULT_SOURCE@ %1").arg(muted ? 1 : 0));
}

bool CallAudio::setVolume(int volume)
{
    if (!available())
        return false;
    volume = qBound(0, volume, 100);
    mVolume = volume;
    applyVolume();
    if (mModemInterfaces.contains(OFONO_VOLUME_IFACE)) {
        QDBusMessage set = QDBusMessage::createMethodCall(OFONO_SERVICE, mModem, OFONO_VOLUME_IFACE, "SetProperty");
        set << QStringLiteral("SpeakerVolume") << QVariant::fromValue(QDBusVariant(QVariant::fromValue<uchar>(volume)));
        mBus.call(set, QDBus::NoBlock);
    }
    callsChanged();
    return true;
}

// The call's far end on the watch: the echo canceller's sink
void CallAudio::applyVolume()
{
    if (mVolume < 0 || mVolume == mAppliedVolume)
        return;
    mAppliedVolume = mVolume;
    pactl(QStringLiteral("set-sink-volume " CALL_SINK " %1%").arg(mVolume));
}

// pactl (pulseaudio-misc) in the user session; BTSYNCD_PACTL replaces it in
// the tests
void CallAudio::pactl(const QString &args)
{
    const QString cmd = QProcessEnvironment::systemEnvironment().value("BTSYNCD_PACTL", "pactl");
    QStringList list = args.split(' ', Qt::SkipEmptyParts);
    QProcess::startDetached(cmd, list);
}

void CallAudio::setPhoneLink(bool connected)
{
    mPhoneLink = connected;
    scheduleConnect(true);
}

void CallAudio::scheduleConnect(bool soon)
{
    if (!mPhoneLink || available() || !mEnabled->value(true).toBool()) {
        mConnectTimer.stop();
        mConnectDelay = 0;
        return;
    }
    if (soon)
        mConnectDelay = 0;
    // 5 s after the phone came, then 30 s, doubling up to 10 min
    mConnectDelay = mConnectDelay == 0 ? 5000 : qMin(mConnectDelay == 5000 ? 30000 : mConnectDelay * 2, 600000);
    mConnectTimer.start(mConnectDelay);
}

// Connect the hands-free profile to the connected phone: one bonded over
// BR/EDR (no pairing ever starts from here) that offers the audio gateway
void CallAudio::connectHandsfree()
{
    if (!mPhoneLink || available() || !mEnabled->value(true).toBool())
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
