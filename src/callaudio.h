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

#ifndef CALLAUDIO_H
#define CALLAUDIO_H

#include <QDBusConnection>
#include <QDBusMessage>
#include <QObject>
#include <QStringList>
#include <QTimer>
#include <QVariantMap>

#include "common.h"

class CallAudio;
class MDConfItem;
class QDBusServiceWatcher;

/*
 * The call view's interface (session bus, BTSYNCD_CALLS_PATH).
 *
 * GetCall() and CallChanged() give the call of the phone as the watch's
 * hands-free link sees it:
 *   available     the hands-free link to the phone is up (calls can be
 *                 answered, ended and heard on the watch)
 *   state         "" (no call), "incoming", "waiting", "dialing",
 *                 "alerting", "active", "held"
 *   number, name  the caller as the phone gives it (often only the number)
 *   activeSince   when the call became active (ms since the epoch), or 0
 *   audioOnWatch  the call audio is on the watch (speaker and microphone)
 *   muted         the watch's microphone is muted (local, as stock)
 *   volume        call volume 0-100 (local, and sent to the phone)
 */
class CallsDBusApi : public QObject
{
    Q_OBJECT
    Q_CLASSINFO("D-Bus Interface", BTSYNCD_CALLS_IFACE)

public:
    explicit CallsDBusApi(CallAudio *calls);

public slots:
    QVariantMap GetCall();
    // Answer on the watch: the call audio comes to the watch (false: no
    // ringing call on the hands-free link; use the phone's notification)
    bool Answer();
    // Decline a ringing call or end the call
    bool Hangup();
    bool SetMuted(bool muted);
    bool SetVolume(int volume);
    // Bring the call audio to the watch, or leave it on the phone
    bool SetAudioOnWatch(bool onWatch);

signals:
    void CallChanged(const QVariantMap &call);

private:
    CallAudio *mCalls;
};

class CallAudio : public QObject
{
    Q_OBJECT

public:
    explicit CallAudio(QObject *parent = nullptr);

    QVariantMap call() const;
    bool answer();
    bool hangup();
    bool setMuted(bool muted);
    bool setVolume(int volume);
    bool setAudioOnWatch(bool onWatch);

    // The phone's Bluetooth LE link (BlueZManager): with it up, the watch
    // connects the hands-free link as stock keeps it connected
    void setPhoneLink(bool connected);

signals:
    void callChanged(const QVariantMap &call);

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

private:
    struct Call {
        QString state, number, name;
        qint64 activeSince = 0;
    };

    QDBusConnection mBus;
    QDBusServiceWatcher *mWatcher;
    MDConfItem *mEnabled;
    QString mModem, mCard;
    QStringList mModemInterfaces;
    QMap<QString, Call> mCalls;
    QStringList mCallOrder;
    bool mAudioAllowed = false, mAudioConnected = false;
    bool mMuted = false;
    int mVolume = -1, mAppliedVolume = -1;
    bool mNrecDone = false;
    bool mPhoneLink = false;
    QTimer mConnectTimer;
    int mConnectDelay = 0;
    QVariantMap mLast;

    void setModem(const QString &path, const QVariantMap &properties);
    void clearModem();
    void watchModemInterfaces();
    void addCall(const QString &path, const QVariantMap &properties);
    void setCard(const QString &path, const QVariantMap &properties);
    QString mainCall() const;
    void setAudioAllowed(bool allowed);
    void connectAudio();
    void applyVolume();
    void muteMicrophone(bool muted);
    void pactl(const QString &script);
    void callsChanged();
    void scheduleConnect(bool soon);
    bool available() const;
};

#endif // CALLAUDIO_H
