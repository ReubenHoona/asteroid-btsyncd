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

#include "healthrecords.h"

#include <QDate>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QSet>
#include <QStringList>
#include <QTimeZone>
#include <QVector>

#include <sqlite3.h>

#include <algorithm>
#include <climits>

namespace HealthRecords {

namespace {

struct Entry {
    qint64 time;
    qint64 value;
    qint64 start;   // steps: when the interval the value covers began
};

QString sensorDir(Type type)
{
    return type == HeartRate ? QStringLiteral("heartrateMonitor") : QStringLiteral("stepCounter");
}

/*
 * sensorlogd's heartrate.db, for reading. Opened read-write, though only
 * read: a WAL database is read through its -shm file, which a read-only
 * connection cannot make when sensorlogd has none open. Not there (a watch
 * from before it, or sensorlogd has not run yet): no readings.
 */
class ReadingsDb
{
public:
    explicit ReadingsDb(const QString &root)
    {
        const QString path = root + QStringLiteral("heartrate.db");
        if (!QFile::exists(path))
            return;
        if (sqlite3_open_v2(QFile::encodeName(path).constData(), &m_db, SQLITE_OPEN_READWRITE, nullptr) != SQLITE_OK) {
            sqlite3_close(m_db);
            m_db = nullptr;
            return;
        }
        sqlite3_busy_timeout(m_db, 2000); // a checkpoint of sensorlogd's
    }
    ~ReadingsDb()
    {
        sqlite3_finalize(m_stmt);
        sqlite3_close(m_db);
    }
    ReadingsDb(const ReadingsDb &) = delete;
    ReadingsDb &operator=(const ReadingsDb &) = delete;

    // a statement to step through; nullptr without a database (or its table)
    sqlite3_stmt *prepare(const char *sql)
    {
        sqlite3_finalize(m_stmt);
        m_stmt = nullptr;
        if (m_db && sqlite3_prepare_v2(m_db, sql, -1, &m_stmt, nullptr) != SQLITE_OK) {
            sqlite3_finalize(m_stmt);
            m_stmt = nullptr;
        }
        return m_stmt;
    }

private:
    sqlite3 *m_db = nullptr;
    sqlite3_stmt *m_stmt = nullptr;
};

bool plausibleBpm(qint64 bpm)
{
    return bpm > 0 && bpm < 300;
}

/*
 * The minutes of the database's readings from since's minute on, before
 * until (epoch seconds; LLONG_MAX: no end): the median usable reading of
 * each minute after since, dated by the minute's start, and every minute
 * that has a reading at all (covered: the heartrateMonitor records of
 * those minutes are not sent). Stops once more than max minutes have a
 * median; end is then the first minute not read, all before it were.
 */
struct DbMinutes {
    bool found = false;     // there is a database to read
    QList<Entry> medians;
    QSet<qint64> covered;   // epoch minutes (seconds / 60)
    qint64 end = LLONG_MAX; // epoch seconds
};

DbMinutes readMinutes(const QString &root, qint64 since, qint64 until, int max, int minAccuracy)
{
    DbMinutes out;
    ReadingsDb db(root);
    sqlite3_stmt *st = db.prepare("SELECT time, bpm, accuracy FROM heartrate WHERE time >= ? AND time < ? ORDER BY time");
    if (!st)
        return out;
    out.found = true;
    const qint64 from = qMax<qint64>(since, 0) / 60 * 60;
    sqlite3_bind_int64(st, 1, from * 1000);
    sqlite3_bind_int64(st, 2, until >= LLONG_MAX / 1000 ? LLONG_MAX : until * 1000);

    qint64 minute = -1;
    QVector<int> bpms;
    auto finish = [&]() {
        if (!bpms.isEmpty() && minute * 60 > since) {
            // the median (of an even count, the upper one), as sensorlogd's records
            std::sort(bpms.begin(), bpms.end());
            out.medians.append({minute * 60, bpms.at(bpms.size() / 2), minute * 60});
        }
        bpms.clear();
    };
    bool stopped = false;
    while (sqlite3_step(st) == SQLITE_ROW) {
        const qint64 m = sqlite3_column_int64(st, 0) / 60000;
        if (m != minute) {
            finish();
            if (out.medians.size() > max) {
                out.end = m * 60;
                stopped = true;
                break;
            }
            minute = m;
            out.covered.insert(m);
        }
        const qint64 bpm = sqlite3_column_int64(st, 1);
        if (sqlite3_column_int(st, 2) >= minAccuracy && plausibleBpm(bpm))
            bpms.append((int)bpm);
    }
    if (!stopped)
        finish();
    return out;
}

// hrr: the usable readings after since (epoch ms), at most max; more if there are others
Page queryReadings(const QString &root, qint64 since, int max, int minAccuracy)
{
    Page page;
    ReadingsDb db(root);
    sqlite3_stmt *st = db.prepare("SELECT time, bpm FROM heartrate WHERE time > ? AND accuracy >= ? "
                                  "AND bpm > 0 AND bpm < 300 ORDER BY time LIMIT ?");
    if (!st)
        return page;
    sqlite3_bind_int64(st, 1, since);
    sqlite3_bind_int(st, 2, minAccuracy);
    sqlite3_bind_int(st, 3, max + 1);
    while (sqlite3_step(st) == SQLITE_ROW) {
        if (page.records.size() == max) {
            page.more = true; // the times are the table's key: no two readings share one
            break;
        }
        page.records.append({sqlite3_column_int64(st, 0), sqlite3_column_int64(st, 1)});
    }
    if (!page.records.isEmpty())
        page.t0 = page.records.first().time;
    return page;
}

/*
 * One day's file. Steps become the steps taken since the record before
 * that day (the first record: since midnight). A value below the one
 * before means the counter started again from zero.
 */
void readDay(const QString &path, const QDate &date, Type type, QList<Entry> &out)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly))
        return;

    qint64 prevTime = date.startOfDay().toSecsSinceEpoch();
    qint64 prevValue = 0;
    while (!file.atEnd()) {
        const QByteArray line = file.readLine().trimmed();
        const int colon = line.indexOf(':');
        if (colon <= 0)
            continue;
        bool timeOk = false, valueOk = false;
        const qint64 time = line.left(colon).toLongLong(&timeOk);
        const qint64 value = line.mid(colon + 1).toLongLong(&valueOk);
        if (!timeOk || !valueOk || time <= 0)
            continue;

        if (type == HeartRate) {
            if (value > 0 && value < 300)
                out.append({time, value, time});
        } else {
            qint64 delta = value - prevValue;
            if (delta < 0)
                delta = qMax<qint64>(value, 0);
            out.append({time, delta, qMin(prevTime, time)});
            prevValue = value;
            prevTime = time;
        }
    }
}

}

Type typeFromName(const QString &name)
{
    if (name == QLatin1String("hr"))
        return HeartRate;
    if (name == QLatin1String("steps"))
        return Steps;
    if (name == QLatin1String("hrr"))
        return HeartRateReadings;
    return Unknown;
}

QString typeName(Type type)
{
    switch (type) {
    case HeartRate: return QStringLiteral("hr");
    case Steps:     return QStringLiteral("steps");
    case HeartRateReadings: return QStringLiteral("hrr");
    default:        return QString();
    }
}

Page query(const QString &root, Type type, qint64 since, int max, int minAccuracy)
{
    Page page;
    if (type == Unknown)
        return page;
    if (max <= 0 || max > MaxRecords)
        max = MaxRecords;
    if (type == HeartRateReadings)
        return queryReadings(root, since, max, minAccuracy);

    // The day before since's too: its 23:59:59 record, or a time zone change
    const QDate first = QDateTime::fromSecsSinceEpoch(qMax<qint64>(since, 0)).date().addDays(-1);
    const QDir dir(root + sensorDir(type));
    QList<Entry> entries;
    const QStringList names = dir.entryList({QStringLiteral("*.log")}, QDir::Files, QDir::Name);
    // Read only as many days as the page needs: a phone syncing a long
    // history asks page after page, and reading every later day for each
    // page made a full sync quadratic in the history. A day file holds the
    // records of a local date, so its times lie within -14 h / +12 h of that
    // date in UTC: once more than max records after since are read up to
    // date D, the files of D + 3 and later only hold later times.
    QDate lastDate;
    bool skipped = false;
    qsizetype after = 0;
    for (const QString &name : names) {
        const QDate date = QDate::fromString(name.chopped(4), QStringLiteral("yyyy-MM-dd"));
        if (!date.isValid() || date < first)
            continue;
        if (lastDate.isValid() && date > lastDate) {
            skipped = true;
            break;
        }
        const qsizetype before = entries.size();
        readDay(dir.filePath(name), date, type, entries);
        for (qsizetype i = before; i < entries.size(); i++) {
            if (entries[i].time > since)
                after++;
        }
        if (!lastDate.isValid() && after > max)
            lastDate = date.addDays(2);
    }

    if (type == HeartRate) {
        // Every minute with readings in the database is sent as their
        // median; a heartrateMonitor record only for a minute without any
        // (history from before the database, a gap in it). Times before
        // end are complete: the database stopped there, or the day files
        // after lastDate (times from the UTC start of lastDate + 1, - 14 h,
        // on) were not read.
        const qint64 horizon = skipped
                ? lastDate.addDays(1).startOfDay(QTimeZone::UTC).toSecsSinceEpoch() - 14 * 3600 : LLONG_MAX;
        const DbMinutes minutes = readMinutes(root, since, horizon, max, minAccuracy);
        if (minutes.found) {
            const qint64 end = qMin(horizon, minutes.end);
            entries.removeIf([&minutes, end](const Entry &e) {
                return e.time >= end || minutes.covered.contains(e.time / 60);
            });
            entries += minutes.medians;
            if (end < LLONG_MAX)
                skipped = true;
        }
    }

    entries.removeIf([since](const Entry &e) { return e.time <= since; });
    std::stable_sort(entries.begin(), entries.end(),
                     [](const Entry &a, const Entry &b) { return a.time < b.time; });

    // The next page starts after the last time sent: keep a time's records
    // together
    qsizetype n = 0;
    while (n < entries.size() && (n < max || entries[n].time == entries[n - 1].time))
        n++;

    // (never more after none: the phone takes that for a broken answer)
    page.more = n > 0 && (n < entries.size() || skipped);
    if (n > 0)
        page.t0 = type == Steps ? entries[0].start : entries[0].time;
    for (qsizetype i = 0; i < n; i++)
        page.records.append({entries[i].time, entries[i].value});
    return page;
}

QByteArray toXml(Type type, const Page &page)
{
    QByteArray xml = "<data><ty>" + typeName(type).toLatin1() + "</ty>";
    if (!page.records.isEmpty())
        xml += "<t0>" + QByteArray::number(page.t0) + "</t0>";
    xml += "<n>" + QByteArray::number(page.records.size()) + "</n>";
    xml += page.more ? "<mo>1</mo>" : "<mo>0</mo>";
    if (!page.records.isEmpty()) {
        xml += "<r>";
        qint64 prev = page.t0;
        for (qsizetype i = 0; i < page.records.size(); i++) {
            const Record &r = page.records[i];
            if (i)
                xml += ';';
            xml += QByteArray::number(r.time - prev) + ',' + QByteArray::number(r.value);
            prev = r.time;
        }
        xml += "</r>";
    }
    xml += "</data>";
    return xml;
}

QByteArray errorXml(const QString &type, const QString &error)
{
    return "<data><ty>" + type.toHtmlEscaped().toUtf8() + "</ty><er>"
            + error.toHtmlEscaped().toUtf8() + "</er></data>";
}

}
