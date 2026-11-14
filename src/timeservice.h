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
 */
class TimeSetChrc : public Characteristic
{
    Q_OBJECT
public:
    TimeSetChrc(QDBusConnection bus, int index, Service *service)
        : Characteristic(bus, index, TIME_SET_UUID, {"encrypt-authenticated-write"}, service) {}

    // false if it is too short or not a date; flag24: 1, 0, or -1 (none or invalid)
    static bool decode(const QByteArray &value, QDateTime *time, int *flag24);

public slots:
    void WriteValue(QByteArray, QVariantMap);

signals:
    void screenshotTaken(QString);
    void phone24hFormat(bool is24h);
};

class MDConfItem;

class TimeService : public Service
{
    Q_OBJECT
public:
    explicit TimeService(int index, QDBusConnection bus, QObject *parent = 0);

    static constexpr const char *FromPhoneKey = "/org/asteroidos/settings/time-format-from-phone";
    static constexpr const char *Use12hKey = "/org/asteroidos/settings/use-12h-format";
    static constexpr const char *Phone24hKey = "/org/asteroidos/settings/phone-time-24h";

private slots:
    void onPhoneFormat(bool is24h);
    void applyFormat();

private:
    MDConfItem *mFromPhone;
    MDConfItem *mPhone24h;
    MDConfItem *mUse12h;
};

#endif // TIMESERVICE_H
