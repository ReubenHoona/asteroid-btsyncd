/*
 * Copyright (C) 2016 - Florent Revest <revestflo@gmail.com>
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

#ifndef WEATHERSERVICE_H
#define WEATHERSERVICE_H

#include <QObject>

#include "service.h"

/*
 * The phone's forecast (service 00008071). The old characteristics (city,
 * ids, min and max temperatures of five days, location) are unchanged; a
 * phone that also writes the full forecast does it to
 *   00008006 write (encrypt-authenticated): <wx>JSON</wx>, XML-escaped
 *            (& < > only), in as many writes as needed, at most 8 KiB of JSON
 *   00008007 read: <capabilities><version>1</version><extended>1</extended></capabilities>
 *            (the phone writes 00008006 only when it reads extended 1)
 * The JSON (version 1) is stored as it came, once it parses and "v" is 1,
 * at dconf WeatherService::ExtendedKey; timestamp-day0 moves with it. Fields
 * are absent, never 0, when unknown (eos-weather writes the same key when
 * the phone is away).
 */
class WeatherService : public Service
{
    Q_OBJECT
public:
    explicit WeatherService(int index, QDBusConnection bus, QObject *parent = 0);

    static constexpr const char *ExtendedKey = "/org/asteroidos/weather/extended";
    static const int MaxJsonBytes = 8192;

    // One <wx>...</wx> from the phone: the JSON text if it is a forecast
    // (an object, "v" 1, "now" an object when there, "hours"/"days" arrays of
    // at most 24/10 when there), else false
    static bool parseExtended(const QByteArray &xml, QString *json);
    static QByteArray capabilities();
};

#endif // WEATHERSERVICE_H
