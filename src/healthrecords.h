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
 * The health history asteroid-sensorlogd keeps under <root>:
 *
 * - heartrate.db: every heart rate reading, an SQLite table (WAL; read
 *   while sensorlogd writes) heartrate (time INTEGER PRIMARY KEY, bpm,
 *   accuracy), time in epoch milliseconds, accuracy QHrmSensor::Status
 *   (1 unreliable .. 4 high).
 * - One file per sensor and (local) day, <root>/<sensor>/<yyyy-MM-dd>.log,
 *   one "epochSeconds:value" line per record, appended in time order:
 *   heartrateMonitor, one median reading per interval (1 or 10 min);
 *   stepCounter, the steps so far that day, a 23:59:59 record closing it.
 *
 * Sent as:
 * - hr: one record per minute, dated by the minute's start: the median of
 *   the minute's usable readings in heartrate.db. For a minute without
 *   readings there (history from before the database, a gap in it), the
 *   heartrateMonitor records, dated by their reading.
 * - hrr: every usable reading in heartrate.db; times in milliseconds.
 * - steps: the steps taken since the record before (deltas).
 * Usable: accuracy at least sensorlogd's heartrateSensor/minStatus, the
 * readings sensorlogd's own records use.
 */
namespace HealthRecords {

enum Type { Unknown, HeartRate, Steps, HeartRateReadings };

struct Record {
    qint64 time;    // epoch seconds (hrr: milliseconds)
    qint64 value;   // bpm, or steps since the record before
};

struct Page {
    QList<Record> records;
    // hr, hrr: the first record's time. steps: when the first record's
    // interval began (the record before it that day, or the day's local midnight)
    qint64 t0 = 0;
    bool more = false;  // records after these: ask again from the last one
};

// Most records in one answer (a client may ask for fewer)
const int MaxRecords = 1000;

// sensorlogd's default for heartrateSensor/minStatus: medium and up
const int DefaultMinAccuracy = 3;

Type typeFromName(const QString &name);
QString typeName(Type type);

// Records of the type after since (exclusive; hrr: milliseconds), oldest
// first, at most max; never splits records with the same time over two pages
Page query(const QString &root, Type type, qint64 since, int max = MaxRecords,
           int minAccuracy = DefaultMinAccuracy);

// <data><ty>..</ty><t0>..</t0><n>..</n><mo>..</mo><r>dt,v;dt,v</r></data>
QByteArray toXml(Type type, const Page &page);
QByteArray errorXml(const QString &type, const QString &error);

}

#endif // HEALTHRECORDS_H
