/*
 * Copyright (C) 2026 - Ed Beroset <beroset@ieee.org>
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

#ifndef SETTIME_H
#define SETTIME_H

#include <QDateTime>

bool setSystemTime(const QDateTime& dateTime);

/*
 * Settings > System > Date & time (CR-27, CR-28): "Automatic date & time" and
 * "Automatic time zone" are the dconf keys below (both default true). The phone's
 * time is applied only while AutoTimeKey is true, its zone only while
 * AutoTimeZoneKey is true (stock TimeService SET_TIMEZONE: the phone's zone is set
 * unless clockwork_auto_time_zone is off). The zone comes with the time
 * characteristic from Gadgetbridge (timeservice.h, R5 E-07); the current time
 * service (iPhones) carries none. A zone the user chose stays: Settings lets the
 * user choose one only while AutoTimeZoneKey is false.
 */
static constexpr const char *AutoTimeKey = "/org/asteroidos/settings/auto-time";
static constexpr const char *AutoTimeZoneKey = "/org/asteroidos/settings/auto-time-zone";
// The first-run flow (launcher) reads this: true once the phone set the watch's clock
static constexpr const char *PhoneSetKey = "/org/asteroidos/time/phone-set";
bool autoTimeEnabled();
bool autoTimeZoneEnabled();

/*
 * The time the phone gave: set while auto time is on (else the watch's clock is left as
 * the user set it); after the clock was set, PhoneSetKey is written true unless it is
 * already (never false). Returns whether the clock was set.
 */
bool setSystemTimeFromPhone(const QDateTime& dateTime);

/*
 * The system's time zone through timedate1 (its Timezone property, SetTimezone),
 * the path Settings > Date & time takes. systemTimeZoneName() is empty when
 * timedate1 does not answer.
 */
QString systemTimeZoneName();
bool setSystemTimeZone(const QString &zone);

/*
 * The zone to set for the phone's: its name if this watch's zone database has
 * it, else, for a whole-hour offset (hasOffset), the Etc zone of that offset
 * ("Etc/GMT-3" is three hours ahead of UTC); empty when neither.
 */
QString zoneForPhone(const QString &zoneId, bool hasOffset, int offsetSecs);

/*
 * The phone's zone: set while auto time zone is on and the system's differs
 * (else left as it is). Returns whether the system's zone is that one now.
 */
bool setSystemTimeZoneFromPhone(const QString &zone);
#endif // SETTIME_H
