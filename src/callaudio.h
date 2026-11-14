/*
 * Copyright (C) 2026 eos-mainline contributors
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef CALLAUDIO_H
#define CALLAUDIO_H

#include <QDBusConnection>
#include <QDBusMessage>
#include <QObject>
#include <QStringList>
#include <QTimer>
#include <QVariantMap>

#include "callcontrol.h"
#include "common.h"

class CallAudio;
class MDConfItem;
class QDBusServiceWatcher;

/*
 * The call view's interface (session bus, BTSYNCD_CALLAUDIO_PATH), the
 * contract of the launcher's qml/calls/CallAudio.qml:
 *   State() -> a{sv} and StateChanged(a{sv}), keys
 *     available   the watch holds the call audio (speaker and microphone)
 *     muted       the watch's microphone is muted (local, as stock)
 *     route       "watch" or "phone"
 *     volume      the call volume step, 1..volumeMax; -1 unknown
 *     volumeMax   5 (stock's STREAM_VOICE_CALL steps); 0 unknown
 *     callState   oFono's state of the call: "dialing", "alerting", "active",
 *                 "held", "disconnected", also "incoming" and "waiting"; ""
 *                 without a call
 *     link        the hands-free link to the phone is up (extra)
 *   SetMuted(b), AdjustVolume(i delta), SendTones(s), SetRoute(s),
 *   PullToWatch()
 * Each method answers whether it did something.
 */
class CallAudioDBusApi : public QObject
{
    Q_OBJECT
    Q_CLASSINFO("D-Bus Interface", BTSYNCD_CALLAUDIO_IFACE)

public:
    explicit CallAudioDBusApi(CallAudio *calls);

public slots:
    QVariantMap State();
    bool SetMuted(bool muted);
    bool AdjustVolume(int delta);
    bool SendTones(const QString &digits);
    bool SetRoute(const QString &route);
    bool PullToWatch();

signals:
    void StateChanged(const QVariantMap &state);

private:
    CallAudio *mCalls;
};

class CallAudio : public QObject, public CallControl
{
    Q_OBJECT

public:
    // The steps of the call volume (stock: STREAM_VOICE_CALL 1..5) and the
    // percent kept in dconf for each (the Settings > Sound slider's)
    static const int VolumeMax = 5;
    // Hands-free connect attempts after the phone's LE link came, without
    // one in between that worked: 5 s, then 30 s doubling (no endless polling)
    static const int ConnectAttempts = 6;

    explicit CallAudio(QObject *parent = nullptr);

    QVariantMap state() const;
    // CallControl
    bool answerOnWatch() override;
    bool hangUp() override;
    bool setMuted(bool muted);
    bool adjustVolume(int delta);
    bool sendTones(const QString &digits);
    bool setAudioOnWatch(bool onWatch);

    // The phone's Bluetooth LE link (BlueZManager): with it up, the watch
    // connects the hands-free link as stock keeps it connected
    void setPhoneLink(bool connected);

signals:
    void stateChanged(const QVariantMap &state);

private slots:
    void refresh();
    void onModemAdded(const QDBusMessage &msg);
    void onModemRemoved(const QDBusMessage &msg);
    void onModemPropertyChanged(const QDBusMessage &msg);
    void onCallAdded(const QDBusMessage &msg);
    void onCallRemoved(const QDBusMessage &msg);
    void onCallPropertyChanged(const QDBusMessage &msg);
    void onVolumePropertyChanged(const QDBusMessage &msg);
    void onCardAdded(const QDBusMessage &msg);
    void onCardRemoved(const QDBusMessage &msg);
    void onCardPropertyChanged(const QDBusMessage &msg);
    void connectHandsfree();
    void onSettingChanged();

private:
    struct Call {
        QString state;
    };

    QDBusConnection mBus;
    QDBusServiceWatcher *mWatcher;
    MDConfItem *mEnabled, *mCallVolume;
    QString mModem, mCard;
    QStringList mModemInterfaces;
    QMap<QString, Call> mCalls;
    QStringList mCallOrder;
    bool mAudioAllowed = false, mAudioConnected = false;
    bool mMuted = false;
    // the call volume in percent (the dconf key), what the sink and oFono hold
    int mVolumePct = 80, mAppliedVolume = -1, mOfonoVolume = -1;
    bool mNrecDone = false;
    bool mPhoneLink = false;
    QTimer mConnectTimer;
    int mConnectDelay = 0, mConnectTries = 0;
    QVariantMap mLast;

    void setModem(const QString &path, const QVariantMap &properties);
    void clearModem();
    void watchModemInterfaces();
    void addCall(const QString &path, const QVariantMap &properties);
    void setCard(const QString &path, const QVariantMap &properties);
    QString mainCall() const;
    QString ringingCall() const;
    bool available() const;
    bool onWatch() const;
    bool enabled() const;
    int volumePercent() const;
    int volumeStep() const;
    void setAudioAllowed(bool allowed);
    void connectAudio();
    void applyVolume();
    void muteMicrophone(bool muted);
    void pactl(const QString &script);
    void changed();
    void scheduleConnect(bool fresh);
    void disconnectHandsfree();
    static bool rfcommAvailable();
};

#endif // CALLAUDIO_H
