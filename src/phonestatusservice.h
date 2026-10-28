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

#ifndef PHONESTATUSSERVICE_H
#define PHONESTATUSSERVICE_H

#include <QObject>

#include "characteristic.h"
#include "service.h"

class MDConfItem;
class NotificationService;
class PhoneStatusService;
class QTimer;

/*
 * The phone's state on the watch, and Do Not Disturb both ways (service
 * 0000C071):
 *   status   0000C001 write, phone -> watch, any of
 *              <ps><v>PROTOCOL</v><bl>BATTERY %</bl><bc>0|1 charging</bc>
 *                  <dnd>off|priority|alarms|none</dnd></ps>
 *            (in as many writes as needed; the phone sends what changed)
 *   requests 0000C002 notify, watch -> phone:
 *              <req><dnd>on|off</dnd></req>   the user switched the watch's DND
 *   capabilities 0000C003 read:
 *              <capabilities><version>1</version><battery>1</battery><dnd>1</dnd></capabilities>
 * The watch's DND is the dconf key /org/asteroidos/notifications/do-not-disturb
 * (the launcher's quick panel switches it); the phone's DND sets it, and a
 * change on the watch goes to the phone. Changed while no phone listened: the
 * watch's choice goes to the phone when it is back.
 */
class PhoneRequestsChrc : public Characteristic
{
    Q_OBJECT
    Q_PROPERTY(QByteArray Value READ getValue NOTIFY valueChanged)

public:
    PhoneRequestsChrc(QDBusConnection bus, int index, Service *service);
    void send(const QByteArray &xml);
    bool notifying() const { return mNotifying; }

public slots:
    void StartNotify() override;
    void StopNotify() override;

signals:
    void valueChanged();
    void notifyingChanged(bool notifying);

private slots:
    void emitPropertiesChanged();

private:
    QByteArray mValue;
    bool mNotifying = false;
    QByteArray getValue() { return mValue; }
};

/*
 * Session bus API for the launcher: org.asteroidos.btsyncd.Phone on
 * /org/asteroidos/btsyncd/phone.
 */
class PhoneDBusApi : public QObject
{
    Q_OBJECT
    Q_CLASSINFO("D-Bus Interface", "org.asteroidos.btsyncd.Phone")

public:
    explicit PhoneDBusApi(PhoneStatusService *service);

public slots:
    // A phone is connected (subscribed to the notification feedback)
    bool Connected();
    // The phone's battery in percent, -1 if it never said
    int BatteryLevel();
    bool Charging();
    bool DoNotDisturb();
    // Switches the watch's DND (and the phone's, when it listens); returns
    // whether the phone was told
    bool SetDoNotDisturb(bool on);

signals:
    void ConnectedChanged(bool connected);
    void BatteryChanged(int level, bool charging);
    void DoNotDisturbChanged(bool on);

private:
    PhoneStatusService *mService;
};

class PhoneStatusService : public Service
{
    Q_OBJECT
public:
    PhoneStatusService(int index, QDBusConnection bus, NotificationService *notifications, QObject *parent = 0);

    static constexpr const char *DndKey = "/org/asteroidos/notifications/do-not-disturb";
    // A phone gone for this long gets a quiet "Phone disconnected" card, if
    // CONNECTION_NOTICES_KEY is on (off: Wear OS shows only the state)
    static const int DisconnectNoticeMs = 60 * 1000;

    // The status the phone wrote (the XML of one <ps> element)
    void applyStatus(const QByteArray &xml);

    bool connected() const;
    int batteryLevel() const { return mBatteryLevel; }
    bool charging() const { return mCharging; }
    int phoneVersion() const { return mPhoneVersion; }
    bool doNotDisturb() const { return mDnd; }
    bool setDoNotDisturb(bool on);
    static QByteArray capabilities();
    // For the tests: a shorter wait for the disconnected notice, and the
    // notice on or off whatever CONNECTION_NOTICES_KEY says
    void setDisconnectNoticeMs(int ms);
    void setDisconnectNoticeEnabled(bool on) { mNoticeOverride = on ? 1 : 0; }

signals:
    void connectedChanged(bool connected);
    void batteryChanged(int level, bool charging);
    void doNotDisturbChanged(bool on);

private slots:
    void phoneConnectedChanged(bool connected);
    void dndConfChanged();
    void showDisconnectNotice();

private:
    void setDnd(bool on, bool fromPhone);
    bool sendDndRequest();
    void closeDisconnectNotice();
    void closeStaleNotices();
    bool disconnectNoticeEnabled() const;

    NotificationService *mNotifications;
    PhoneRequestsChrc *mRequests;
    MDConfItem *mDndConf;
    QTimer *mNoticeTimer;
    uint mNoticeId = 0;
    int mNoticeOverride = -1;
    bool mWasConnected = false;
    int mBatteryLevel = -1;
    bool mCharging = false;
    int mPhoneVersion = 0;
    bool mDnd = false;
    // The watch's DND changed while no phone listened, or was just sent to
    // the phone: the phone's own state must not undo it until the phone
    // follows (or this long has passed)
    bool mDndPending = false;
    qint64 mDndPendingSince = 0;
    static const int DndPendingMs = 10 * 1000;
};

#endif // PHONESTATUSSERVICE_H
