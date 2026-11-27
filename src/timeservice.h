/*
 * Copyright (C) 2017 - Florent Revest <revestflo@gmail.com>
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

#ifndef TIMESERVICE_H
#define TIMESERVICE_H

#include <QDateTime>
#include <QObject>

#include "service.h"

/*
 * The phone sets the time (service 00005071, 00005001, write): 6 or 7 bytes,
 *   year-1900, month-1 (0..11), day, hour, minute, second [, 24-hour flag]
 * The 7th byte (Gadgetbridge since W2-gb-data) is 1 when the phone shows
 * 24-hour time, 0 when it shows 12-hour time. It is kept at dconf
 * TimeService::Phone24hKey, and while TimeService::FromPhoneKey is true
 * (Settings > Date & time > Automatic time format) the watch's
 * TimeService::Use12hKey follows it (true when the phone shows 12 hours),
 * also when the setting is switched on later. Any other value is ignored.
 *
 * The phone's time zone (R5 E-07; Gadgetbridge since then), after the flag:
 *   [7..10] the phone's offset from UTC at that time in seconds, a signed
 *           32-bit little-endian number (daylight saving included)
 *   [11]    n, the length of the phone's zone name (0: none)
 *   [12..]  the zone name, n ASCII bytes (an IANA name, "Etc/GMT-3")
 * With the offset the wall time is read as the instant it is at the phone, so
 * the watch's own zone does not matter; without it (6 or 7 bytes, an older
 * Gadgetbridge) in the watch's zone, as before. An older watch reads the first
 * six or seven bytes only. A frame longer than the MTU comes as a long write
 * (BlueZ's offset option): it is joined before it is read.
 */
class TimeSetChrc : public Characteristic
{
    Q_OBJECT
public:
    TimeSetChrc(QDBusConnection bus, int index, Service *service)
        : Characteristic(bus, index, TIME_SET_UUID, {"encrypt-authenticated-write"}, service) {}

    struct Frame {
        QDateTime time;
        int flag24 = -1;        // 1, 0, or -1 (none or invalid)
        bool hasOffset = false;
        int offsetSecs = 0;     // east of UTC
        QString zoneId;         // empty: none sent (or not a zone name)
    };

    // false if it is too short or not a date; flag24: 1, 0, or -1 (none or invalid)
    static bool decode(const QByteArray &value, QDateTime *time, int *flag24);
    static bool decode(const QByteArray &value, Frame *frame);
    // The bytes of the whole frame that begins with these (as many as there are when it has no zone part)
    static int frameSize(const QByteArray &start);

    // The longest frame: 12 bytes and a name of 255
    static const int MaxFrame = 12 + 255;

public slots:
    void WriteValue(QByteArray, QVariantMap);

signals:
    void screenshotTaken(QString);
    void phone24hFormat(bool is24h);
    // the frame had the phone's zone or offset
    void phoneTimeZone(const QString &zoneId, bool hasOffset, int offsetSecs);

private:
    QByteArray mPending;    // a long write's first parts
};

class MDConfItem;

/*
 * The phone's time zone (R5 E-07, stock TimeService SET_TIMEZONE and
 * TimeZoneMediator): kept at PhoneZoneKey (the zone this watch can set for it,
 * settime.h zoneForPhone) and set through timedate1 while Automatic time zone
 * is on. Switched on again while the phone is connected, the phone's last zone
 * is set at once (stock REEVALUATE_PHONE_TIME_ZONE when connected over
 * Bluetooth); else at the phone's next time write (every connection).
 */
class TimeService : public Service
{
    Q_OBJECT
public:
    explicit TimeService(int index, QDBusConnection bus, QObject *parent = 0);

    static constexpr const char *FromPhoneKey = "/org/asteroidos/settings/time-format-from-phone";
    static constexpr const char *Use12hKey = "/org/asteroidos/settings/use-12h-format";
    static constexpr const char *Phone24hKey = "/org/asteroidos/settings/phone-time-24h";
    static constexpr const char *PhoneZoneKey = "/org/asteroidos/settings/phone-time-zone";

public slots:
    // The phone's link (NotificationService::phoneConnectedChanged)
    void phoneConnectedChanged(bool connected);

private slots:
    void onPhoneFormat(bool is24h);
    void applyFormat();
    void onPhoneZone(const QString &zoneId, bool hasOffset, int offsetSecs);
    void onAutoZoneChanged();

private:
    MDConfItem *mFromPhone;
    MDConfItem *mPhone24h;
    MDConfItem *mUse12h;
    MDConfItem *mAutoZone;
    MDConfItem *mPhoneZone;
    bool mAutoZoneOn = true;
    bool mPhoneConnected = false;
};

#endif // TIMESERVICE_H
