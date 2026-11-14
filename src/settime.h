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
 * time is applied only while AutoTimeKey is true, its zone (a source of one does
 * not exist yet: neither the time characteristic nor the current time service
 * carries it) only while AutoTimeZoneKey is true.
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
#endif // SETTIME_H
