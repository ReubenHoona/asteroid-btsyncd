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

#ifndef HEALTHRECORDS_H
#define HEALTHRECORDS_H

#include <QByteArray>
#include <QList>
#include <QString>

/*
 * The health history asteroid-sensorlogd keeps: one file per sensor and
 * (local) day, <root>/<sensor>/<yyyy-MM-dd>.log, one "epochSeconds:value"
 * line per record, appended in time order.
 *
 * - hr (heartrateMonitor): beats per minute, dated by the minute it was
 *   measured in (continuous mode) or by the reading (interval mode).
 * - steps (stepCounter): steps so far that day; a 23:59:59 record closes
 *   the day. Sent as the steps taken since the record before (deltas).
 */
namespace HealthRecords {

enum Type { Unknown, HeartRate, Steps };

struct Record {
    qint64 time;    // epoch seconds
    qint64 value;   // bpm, or steps since the record before
};

struct Page {
    QList<Record> records;
    // hr: the first record's time. steps: when the first record's interval
    // began (the record before it that day, or the day's local midnight)
    qint64 t0 = 0;
    bool more = false;  // records after these: ask again from the last one
};

// Most records in one answer (a client may ask for fewer)
const int MaxRecords = 1000;

Type typeFromName(const QString &name);
QString typeName(Type type);

// Records of the type after since (exclusive), oldest first, at most max;
// never splits records with the same time over two pages
Page query(const QString &root, Type type, qint64 since, int max = MaxRecords);

// <data><ty>..</ty><t0>..</t0><n>..</n><mo>..</mo><r>dt,v;dt,v</r></data>
QByteArray toXml(Type type, const Page &page);
QByteArray errorXml(const QString &type, const QString &error);

}

#endif // HEALTHRECORDS_H
