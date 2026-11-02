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
 * The phone's state on the watch, and Do Not Disturb and Bedtime both ways
 * (service 0000C071):
 *   status   0000C001 write, phone -> watch, any of
 *              <ps><v>PROTOCOL</v><bl>BATTERY %</bl><bc>0|1 charging</bc>
 *                  <dnd>off|priority|alarms|none</dnd><bed>on|off</bed></ps>
 *            (in as many writes as needed; the phone sends what changed;
 *            <bed>, the phone's Bedtime mode, from capabilities version 2,
 *            for a phone that can tell: Android lets no app read it, so
 *            Gadgetbridge sends none and the phone's Bedtime arrives as its
 *            Do Not Disturb)
 *   requests 0000C002 notify, watch -> phone:
 *              <req><dnd>on|off</dnd></req>   the watch went quiet or not
 *   capabilities 0000C003 read:
 *              <capabilities><version>2</version><battery>1</battery><dnd>1</dnd>
 *                  <bedtime>1</bedtime></capabilities>
 * As the stock Pixel Watch 2's "Do Not Disturb & Bedtime: sync across watch
 * and phone" (STOCK-MODES.md): the watch is quiet in Do Not Disturb
 * (/org/asteroidos/notifications/do-not-disturb) or in Bedtime mode
 * (/org/asteroidos/modes/bedtime; the launcher's modes engine keeps the two
 * apart, the latest wins), and the phone's Do Not Disturb follows that:
 *   - the watch going quiet or not goes to the phone as <dnd>on|off</dnd>
 *     (Gadgetbridge 44 and older already set the phone's DND from it; the
 *     switch between DND and Bedtime on the watch sends nothing);
 *   - the phone's DND coming on makes a watch that is not quiet DND; a
 *     watch in Bedtime stays in Bedtime;
 *   - the phone's DND going off ends the watch's DND or Bedtime.
 * Changed while no phone listened: the watch's choice goes to the phone
 * when it is back. Theater mode (/org/asteroidos/modes/theater) silences
 * the phone's notifications on the watch too, without syncing (stock).
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
    // Bedtime mode on the watch (dconf /org/asteroidos/modes/bedtime)
    bool Bedtime();
    // Switches the watch's DND (and the phone's, when it listens); returns
    // whether the phone was told
    bool SetDoNotDisturb(bool on);

signals:
    void ConnectedChanged(bool connected);
    void BatteryChanged(int level, bool charging);
    void DoNotDisturbChanged(bool on);
    void BedtimeChanged(bool on);

private:
    PhoneStatusService *mService;
};

class PhoneStatusService : public Service
{
    Q_OBJECT
public:
    PhoneStatusService(int index, QDBusConnection bus, NotificationService *notifications, QObject *parent = 0);

    static constexpr const char *DndKey = "/org/asteroidos/notifications/do-not-disturb";
    static constexpr const char *BedtimeKey = "/org/asteroidos/modes/bedtime";
    static constexpr const char *TheaterKey = "/org/asteroidos/modes/theater";
    // the watch's changes are sent once they settle (the switch between
    // DND and Bedtime is two dconf writes)
    static const int QuietSettleMs = 250;
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
    bool bedtime() const { return mBedtime; }
    // Do Not Disturb or Bedtime: what the phone's DND follows
    bool quiet() const { return mDnd || mBedtime; }
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
    void bedtimeChanged(bool on);

private slots:
    void phoneConnectedChanged(bool connected);
    void dndConfChanged();
    void modeConfChanged();
    void showDisconnectNotice();

private:
    void setDnd(bool on, bool fromPhone);
    void setBedtime(bool on, bool fromPhone);
    void quietChanged(bool wasQuiet, bool fromPhone);
    void updateSilence();
    // the phone's state may set the watch's: not while the watch's own
    // change is on its way (or waits for the phone)
    bool phoneMayChange(bool phoneQuiet);
    bool sendDndRequest();
    void closeDisconnectNotice();
    void closeStaleNotices();
    bool disconnectNoticeEnabled() const;

    NotificationService *mNotifications;
    PhoneRequestsChrc *mRequests;
    MDConfItem *mDndConf;
    MDConfItem *mBedtimeConf;
    MDConfItem *mTheaterConf;
    QTimer *mSettleTimer;
    QTimer *mNoticeTimer;
    uint mNoticeId = 0;
    int mNoticeOverride = -1;
    bool mWasConnected = false;
    int mBatteryLevel = -1;
    bool mCharging = false;
    int mPhoneVersion = 0;
    bool mDnd = false;
    bool mBedtime = false;
    // The watch's quiet changed while no phone listened, or was just sent
    // to the phone: the phone's own state must not undo it until the phone
    // follows (or this long has passed)
    bool mDndPending = false;
    qint64 mDndPendingSince = 0;
    // The phone's DND as it last said or was told (-1: not known): a watch
    // change that ends where the phone is already needs no request
    int mPhoneQuiet = -1;
    static const int DndPendingMs = 10 * 1000;
};

#endif // PHONESTATUSSERVICE_H
