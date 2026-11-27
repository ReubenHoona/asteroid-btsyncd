/*
 * Copyright (C) 2026 - eos
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

#ifndef HEALTHSERVICE_H
#define HEALTHSERVICE_H

#include <QByteArray>
#include <QObject>
#include <QString>
#include <QTimer>

#include "healthchanges.h"
#include "service.h"

/*
 * Watch to phone: the answer to a request, as XML in notifications of at
 * most MTU - 3 bytes; the phone joins them until </data>.
 */
class HealthDataChrc : public Characteristic
{
    Q_OBJECT
    Q_PROPERTY(QByteArray Value READ getValue NOTIFY valueChanged)

public:
    HealthDataChrc(QDBusConnection bus, int index, Service *service);
    void send(const QByteArray &xml, int mtu);

public slots:
    void StartNotify() override {}
    void StopNotify() override {}

signals:
    void valueChanged();

private slots:
    void emitPropertiesChanged();

private:
    QByteArray mValue;
    QByteArray getValue() { return mValue; }
};

/*
 * Heart rate and step history from asteroid-sensorlogd (see
 * PROTOCOL-health-sync.md). The phone writes
 *   <req><ty>hr|steps|hrr</ty><si>SINCE</si><mx>MAX</mx></req>
 * and gets the records after SINCE on the data characteristic. Version 3
 * (healthchanges.h): workouts, sleep sessions and resting heart rates by
 * health.db's change sequence, <req><ty>wo|sl|rhr</ty><sq>SEQ</sq></req>,
 * and a workout's track, <req><ty>wot</ty><id>ID</id><si>MS</si></req>.
 * The phone also writes the user's profile and goals there,
 * <prof>...</prof> (healthprofile.h), which has no answer.
 *
 * Syncs the watch starts (R5 E-08; stock's sync triggers, SyncRequest
 * EventType WEAR_EXERCISE_UPLOAD_WORKOUT_DATA 7, WEAR_DAILY_SLEEP 11,
 * WEAR_TODAY_APP_OPEN 17, WEAR_TODAY_SYNC_BUTTON 5): the phone is asked over
 * the relay's act {"a":"hsync","ev":"wo|sl|today|button"} (assistantprotocol.h;
 * only a phone that lists hsync gets it) and syncs as at a connection; at the
 * end of every sync, whoever started it, the phone writes
 *   <synced><ok>1|0</ok><n>RECORDS</n></synced>
 * here (only to a watch whose capabilities have <takes>synced</takes>). The
 * events: wo, a workout ended; sl, a main night was added (sensorlogd's
 * HealthChanged signal, then two short reads of health.db); today, the Health
 * app's Today list opened, and button, its Sync button (SyncRequestKey, a
 * text the app writes, "today@MS" / "button@MS"). Nothing is asked while a
 * watch-started sync waits for its end, and wo, sl and today not within a
 * minute of the last sync's end. For the Today list's last row (stock's
 * "Syncing", "Can't Sync Data", "Last synced", Sync / Retry): SyncStateKey
 * is "syncing" from the ask to <synced>, "failed" when the phone refused,
 * did not end it within SyncTimeoutMs (a timer that does not wake the system)
 * or went, and when the button found no phone, else "idle"; SyncLastKey the
 * watch's time of the last <synced> with ok 1 (epoch ms); SyncSupportedKey true
 * once a phone sent <synced> (the row shows only then). Nothing here wakes the
 * watch: every event is one it is awake for.
 */
class HealthRequestChrc;
class MDConfItem;
class PhoneRelay;

class HealthService : public Service
{
    Q_OBJECT
public:
    explicit HealthService(int index, QDBusConnection bus, QObject *parent = 0);

    static constexpr const char *SyncRequestKey = "/org/asteroidos/health/sync/request";
    static constexpr const char *SyncStateKey = "/org/asteroidos/health/sync/state";
    static constexpr const char *SyncLastKey = "/org/asteroidos/health/sync/last-ms";
    static constexpr const char *SyncSupportedKey = "/org/asteroidos/health/sync/supported";
    // a watch-started sync that the phone has not ended by then failed
    static const int SyncTimeoutMs = 180000;
    // wo, sl and today wait this long after a sync's end
    static const qint64 SyncQuietMs = 60000;

    // The phone's relay (AssistantService), for the hsync act
    void setPhoneRelay(PhoneRelay *relay) { mRelay = relay; }

    // Asks the phone for a sync (wo, sl, today, button); false when nothing was asked
    bool requestSync(const QString &event);

    // The phone's <synced> message; false if it is not one
    bool synced(const QByteArray &xml);

    // Answers after the write returns, so the phone sees its write
    // acknowledged before the data. seq: the sq of wo, sl, rhr; id: the
    // workout of wot (-1: none given)
    void request(const QString &type, qint64 since, int max, int mtu, qint64 seq = 0, qint64 id = -1);

    // The phone's <prof> message: the profile and goals it changed go to
    // dconf (HealthProfile::apply); returns the groups written, -1 if the
    // message is no profile. Sends nothing.
    int profile(const QByteArray &xml);

public slots:
    // The phone's link (NotificationService::phoneConnectedChanged)
    void phoneConnectedChanged(bool connected);
    // asteroid-sensorlogd's HealthChanged(x seq): health.db changed
    void healthChanged(qlonglong seq);

private slots:
    void sendPending();
    void onSyncRequest();
    void onSyncTimeout();

private:
    void setSyncState(const char *state);

    HealthDataChrc *mData;
    HealthRequestChrc *mRequest;
    QByteArray mPending;
    int mPendingMtu = 23;
    PhoneRelay *mRelay = nullptr;
    MDConfItem *mSyncRequest;
    MDConfItem *mSyncState;
    MDConfItem *mSyncLast;
    MDConfItem *mSyncSupported;
    QTimer mSyncTimeout;
    bool mSyncing = false;
    HealthChanges::SyncMarks mMarks;
};

#endif // HEALTHSERVICE_H
