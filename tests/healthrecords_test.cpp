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

/*
 * Tests of the health records (sensorlogd files and heart rate database to
 * protocol pages), without D-Bus, on synthetic readings. Run with TZ=UTC:
 * healthrecords_test (exit status = failures).
 */

#include <QDate>
#include <QDateTime>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QSet>
#include <QTemporaryDir>
#include <QTime>

#include <cstdio>

#include <sqlite3.h>

#include "healthrecords.h"

using namespace HealthRecords;

static int failures = 0;

static void check(const char *name, const QByteArray &expected, const QByteArray &actual)
{
    if (expected == actual) {
        printf("PASS %s\n", name);
    } else {
        printf("FAIL %s\n  expected %s\n  got      %s\n", name, expected.constData(), actual.constData());
        failures++;
    }
}

static qint64 at(const QDate &date, int h, int m, int s = 0)
{
    return QDateTime(date, QTime(h, m, s)).toSecsSinceEpoch();
}

static void writeFile(const QString &root, const QString &sensor, const QDate &date, const QByteArray &content)
{
    QDir().mkpath(root + sensor);
    QFile f(root + sensor + "/" + date.toString("yyyy-MM-dd") + ".log");
    if (!f.open(QIODevice::WriteOnly))
        return;
    f.write(content);
}

static QByteArray line(qint64 t, qint64 v)
{
    return QByteArray::number(t) + ":" + QByteArray::number(v) + "\n";
}

static QByteArray n(qint64 v) { return QByteArray::number(v); }

struct Reading {
    qint64 ms;
    int bpm;
    int accuracy;
};

// sensorlogd's heartrate.db (as its heartrateDb.cpp makes it) with these readings added
static sqlite3 *openDb(const QString &root)
{
    sqlite3 *db = nullptr;
    sqlite3_open_v2(QFile::encodeName(root + "heartrate.db").constData(), &db,
                    SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE, nullptr);
    sqlite3_exec(db, "PRAGMA journal_mode=WAL; PRAGMA synchronous=NORMAL; CREATE TABLE IF NOT EXISTS heartrate "
                     "(time INTEGER PRIMARY KEY, bpm INTEGER NOT NULL, accuracy INTEGER NOT NULL)",
                 nullptr, nullptr, nullptr);
    return db;
}

static void insert(sqlite3 *db, const QList<Reading> &readings)
{
    sqlite3_stmt *st = nullptr;
    sqlite3_prepare_v2(db, "INSERT OR IGNORE INTO heartrate VALUES (?, ?, ?)", -1, &st, nullptr);
    for (const Reading &r : readings) {
        sqlite3_bind_int64(st, 1, r.ms);
        sqlite3_bind_int(st, 2, r.bpm);
        sqlite3_bind_int(st, 3, r.accuracy);
        sqlite3_step(st);
        sqlite3_reset(st);
    }
    sqlite3_finalize(st);
}

static void addReadings(const QString &root, const QList<Reading> &readings)
{
    sqlite3 *db = openDb(root);
    sqlite3_exec(db, "BEGIN", nullptr, nullptr, nullptr);
    insert(db, readings);
    sqlite3_exec(db, "COMMIT", nullptr, nullptr, nullptr);
    sqlite3_close(db);
}

// one reading a second from..to (epoch seconds, to exclusive), bpm from f(second), accuracy high
template<typename F>
static QList<Reading> seconds(qint64 from, qint64 to, F bpm, int accuracy = 4)
{
    QList<Reading> out;
    for (qint64 t = from; t < to; t++)
        out.append({t * 1000 + 250, bpm(t), accuracy});
    return out;
}

// every page of a type from since on, asking again from each page's last record as the phone does;
// "time,value" per record, and the pages' more flags
static QList<QByteArray> sync(const QString &root, Type type, qint64 since, int max, QByteArray *mores = nullptr)
{
    QList<QByteArray> out;
    for (int pages = 0; pages < 10000; pages++) {
        const Page p = query(root, type, since, max);
        for (const Record &r : p.records)
            out << n(r.time) + "," + n(r.value);
        if (mores)
            *mores += p.more ? '1' : '0';
        if (!p.more)
            break;
        since = p.records.last().time;
    }
    return out;
}

int main()
{
    printf("UTC offset in January: %d s\n", QDateTime(QDate(2026, 1, 10), QTime(12, 0)).offsetFromUtc());
    QTemporaryDir tmp;
    const QString root = tmp.path() + "/";
    const QDate d1(2026, 1, 10), d2(2026, 1, 11);

    check("empty dir", "<data><ty>steps</ty><n>0</n><mo>0</mo></data>",
          toXml(Steps, query(root, Steps, 0)));

    // Heart rate: junk lines and impossible values are skipped
    const qint64 h = at(d1, 9, 0);
    writeFile(root, "heartrateMonitor", d1,
              line(h, 70) + line(h + 60, 72) + "junk\n:5\n12x:7\n" + line(h + 120, 0)
              + line(h + 180, 75) + line(h + 240, 400) + "\n");
    check("hr all", "<data><ty>hr</ty><t0>" + n(h) + "</t0><n>3</n><mo>0</mo><r>0,70;60,72;120,75</r></data>",
          toXml(HeartRate, query(root, HeartRate, 0)));
    check("hr since", "<data><ty>hr</ty><t0>" + n(h + 180) + "</t0><n>1</n><mo>0</mo><r>0,75</r></data>",
          toXml(HeartRate, query(root, HeartRate, h + 60)));
    check("hr since last", "<data><ty>hr</ty><n>0</n><mo>0</mo></data>",
          toXml(HeartRate, query(root, HeartRate, h + 180)));

    // Pages
    Page p = query(root, HeartRate, 0, 2);
    check("hr page 1", "<data><ty>hr</ty><t0>" + n(h) + "</t0><n>2</n><mo>1</mo><r>0,70;60,72</r></data>",
          toXml(HeartRate, p));
    p = query(root, HeartRate, p.records.last().time, 2);
    check("hr page 2", "<data><ty>hr</ty><t0>" + n(h + 180) + "</t0><n>1</n><mo>0</mo><r>0,75</r></data>",
          toXml(HeartRate, p));

    // Records of one time stay on one page; the next day's file is read
    // and records come out in time order
    writeFile(root, "heartrateMonitor", d2, line(at(d2, 1, 1), 61) + line(at(d2, 1, 0), 60) + line(at(d2, 1, 1), 62));
    p = query(root, HeartRate, h + 180, 2);
    check("hr ties", "<data><ty>hr</ty><t0>" + n(at(d2, 1, 0)) + "</t0><n>3</n><mo>0</mo><r>0,60;60,61;0,62</r></data>",
          toXml(HeartRate, p));
    p = query(root, HeartRate, 0, 4);
    check("hr ties at the page end", "4 1", n(p.records.size()) + " " + n(p.more));
    p = query(root, HeartRate, 0, 5);
    check("hr tie group kept", "6 0", n(p.records.size()) + " " + n(p.more));

    // Steps: per-day cumulative to deltas; the day closes at 23:59:59; a
    // smaller value starts the count again
    writeFile(root, "stepCounter", d1,
              line(at(d1, 8, 0), 100) + line(at(d1, 8, 10), 250) + line(at(d1, 8, 20), 250)
              + line(at(d1, 23, 59, 59), 400));
    writeFile(root, "stepCounter", d2,
              line(at(d2, 0, 10), 30) + line(at(d2, 0, 20), 20) + line(at(d2, 0, 30), 50));
    check("steps all", "<data><ty>steps</ty><t0>" + n(at(d1, 0, 0)) + "</t0><n>7</n><mo>0</mo>"
          "<r>28800,100;600,150;600,0;56399,150;601,30;600,20;600,30</r></data>",
          toXml(Steps, query(root, Steps, 0)));
    check("steps since", "<data><ty>steps</ty><t0>" + n(at(d1, 8, 10)) + "</t0><n>1</n><mo>1</mo><r>600,0</r></data>",
          toXml(Steps, query(root, Steps, at(d1, 8, 10), 1)));
    check("steps next day", "<data><ty>steps</ty><t0>" + n(at(d2, 0, 0)) + "</t0><n>3</n><mo>0</mo><r>600,30;600,20;600,30</r></data>",
          toXml(Steps, query(root, Steps, at(d1, 23, 59, 59))));

    // A day that starts below zero (sensorlogd's offset from before a
    // reboot) counts from its first record
    const QDate d3(2026, 1, 12);
    writeFile(root, "stepCounter", d3, line(at(d3, 7, 0), -4000) + line(at(d3, 7, 10), -3900));
    check("steps negative", "<data><ty>steps</ty><t0>" + n(at(d3, 0, 0)) + "</t0><n>2</n><mo>0</mo><r>25200,0;600,100</r></data>",
          toXml(Steps, query(root, Steps, at(d2, 0, 30))));

    // Old files are not read; files that are not days are ignored
    writeFile(root, "stepCounter", QDate(2025, 12, 1), line(at(d3, 9, 0), 5));
    QFile junk(root + "stepCounter/notes.log");
    if (junk.open(QIODevice::WriteOnly))
        junk.write(line(at(d3, 9, 0), 5));
    junk.close();
    check("steps old file", "0", n(query(root, Steps, at(d3, 7, 10)).records.size()));

    check("unknown type", "0", n(query(root, Unknown, 0).records.size()));
    check("type names", "hr steps 1 2 0", (typeName(HeartRate) + " " + typeName(Steps)).toLatin1() + " "
          + n(typeFromName("hr")) + " " + n(typeFromName("steps")) + " " + n(typeFromName("baro")));
    check("error", "<data><ty>a&lt;b</ty><er>type</er></data>", errorXml("a<b", "type"));
    // (the 2025-12-01 file counts when asking from the start)
    check("no max", "10", n(query(root, Steps, 0, 0).records.size()));
    check("hrr name", "hrr 3", typeName(HeartRateReadings).toLatin1() + " " + n(typeFromName("hrr")));
    check("hrr without a database", "<data><ty>hrr</ty><n>0</n><mo>0</mo></data>",
          toXml(HeartRateReadings, query(root, HeartRateReadings, 0)));

    // ---- sensorlogd's heart rate database: every reading
    const QDate e0(2026, 2, 9), e1(2026, 2, 10), e2(2026, 2, 11);
    QTemporaryDir tmpDb;
    const QString db = tmpDb.path() + "/";
    const qint64 m0 = at(e1, 9, 0);
    addReadings(db, {{m0 * 1000 + 100, 70, 4}, {m0 * 1000 + 1100, 72, 3}, {m0 * 1000 + 2100, 71, 4},
                     {m0 * 1000 + 3100, 90, 2} /* low: not used */, {m0 * 1000 + 4100, 73, 4},
                     {(m0 + 60) * 1000 + 500, 60, 3},
                     {(m0 + 120) * 1000 + 500, 99, 1} /* a minute of an unreliable reading only */});

    // hr: the median of each minute's usable readings (of an even count the upper one), dated by the minute
    check("hr minutes", "<data><ty>hr</ty><t0>" + n(m0) + "</t0><n>2</n><mo>0</mo><r>0,72;60,60</r></data>",
          toXml(HeartRate, query(db, HeartRate, 0)));
    check("hr minutes, any accuracy", "<data><ty>hr</ty><t0>" + n(m0) + "</t0><n>3</n><mo>0</mo><r>0,72;60,60;60,99</r></data>",
          toXml(HeartRate, query(db, HeartRate, 0, MaxRecords, 1)));
    check("hr minutes since", "<data><ty>hr</ty><t0>" + n(m0 + 60) + "</t0><n>1</n><mo>0</mo><r>0,60</r></data>",
          toXml(HeartRate, query(db, HeartRate, m0)));
    // (a minute starting at since was sent with it)
    check("hr minutes since in a minute", "1", n(query(db, HeartRate, m0 + 30).records.size()));

    // hrr: every usable reading, times in milliseconds
    check("hrr all", "<data><ty>hrr</ty><t0>" + n(m0 * 1000 + 100) + "</t0><n>5</n><mo>0</mo>"
          "<r>0,70;1000,72;1000,71;2000,73;56400,60</r></data>",
          toXml(HeartRateReadings, query(db, HeartRateReadings, 0)));
    check("hrr since (exclusive)", "<data><ty>hrr</ty><t0>" + n(m0 * 1000 + 2100) + "</t0><n>3</n><mo>0</mo>"
          "<r>0,71;2000,73;56400,60</r></data>",
          toXml(HeartRateReadings, query(db, HeartRateReadings, m0 * 1000 + 1100)));
    check("hrr page", "<data><ty>hrr</ty><t0>" + n(m0 * 1000 + 100) + "</t0><n>2</n><mo>1</mo><r>0,70;1000,72</r></data>",
          toXml(HeartRateReadings, query(db, HeartRateReadings, 0, 2)));
    check("hrr any accuracy", "7", n(query(db, HeartRateReadings, 0, MaxRecords, 1).records.size()));

    // The day files' records where the database has no readings: the days before it, and a day's minutes
    // without any (here the morning before the update, 09:05); a minute with readings is their median
    writeFile(db, "heartrateMonitor", e0, line(at(e0, 12, 0), 80));
    writeFile(db, "heartrateMonitor", e1, line(at(e1, 8, 0), 65) + line(at(e1, 8, 10), 66)
              + line(at(e1, 9, 0, 2), 71) + line(at(e1, 9, 2, 30), 98) + line(at(e1, 9, 5), 67));
    check("hr fallback", "<data><ty>hr</ty><t0>" + n(at(e0, 12, 0)) + "</t0><n>6</n><mo>0</mo><r>0,80;"
          + n(at(e1, 8, 0) - at(e0, 12, 0)) + ",65;600,66;3000,72;60,60;240,67</r></data>",
          toXml(HeartRate, query(db, HeartRate, 0)));
    check("hr fallback since", "<data><ty>hr</ty><t0>" + n(at(e1, 8, 10)) + "</t0><n>4</n><mo>0</mo>"
          "<r>0,66;3000,72;60,60;240,67</r></data>",
          toXml(HeartRate, query(db, HeartRate, at(e1, 8, 0))));
    check("hr fallback page", "<data><ty>hr</ty><t0>" + n(at(e0, 12, 0)) + "</t0><n>4</n><mo>1</mo><r>0,80;"
          + n(at(e1, 8, 0) - at(e0, 12, 0)) + ",65;600,66;3000,72</r></data>",
          toXml(HeartRate, query(db, HeartRate, 0, 4)));
    check("hr fallback all pages", "6", n(sync(db, HeartRate, 0, 2).size()));

    // A late batch across midnight: the phone synced when the readings went up to 23:50:29; the hub's
    // batch from 23:50:30 to 00:01:29 came later; the next sync asks from its last record - 10 min
    {
        QTemporaryDir tmpLate;
        const QString late = tmpLate.path() + "/";
        auto bpm = [](qint64 t) { return t % 60 < 30 ? 60 : 80; };
        addReadings(late, seconds(at(e1, 23, 40), at(e1, 23, 50, 30), bpm));
        writeFile(late, "heartrateMonitor", e1, line(at(e1, 23, 45, 10), 60));
        Page p = query(late, HeartRate, 0);
        check("late: first sync", n(at(e1, 23, 40)) + " " + n(at(e1, 23, 50)) + " 11 60",
              n(p.records.first().time) + " " + n(p.records.last().time) + " " + n(p.records.size()) + " "
              + n(p.records.last().value));
        const qint64 last = p.records.last().time;
        addReadings(late, seconds(at(e1, 23, 50, 30), at(e2, 0, 1, 30), bpm));
        writeFile(late, "heartrateMonitor", e2, line(at(e2, 0, 0, 40), 80));
        const QList<QByteArray> again = sync(late, HeartRate, last - 600, MaxRecords);
        QByteArray times;
        for (const QByteArray &r : again)
            times += r.split(',').first() == n(at(e1, 23, 50)) ? r + " " : QByteArray();
        check("late: the minute before is whole now", n(at(e1, 23, 50)) + ",80 ", times);
        check("late: every minute to 00:01", "21 " + n(at(e1, 23, 41)) + ",80 " + n(at(e2, 0, 1)) + ",60",
              n(again.size()) + " " + again.first() + " " + again.last());
        check("late: midnight", "1 1", n(again.contains(n(at(e1, 23, 59)) + ",80")) + " "
              + n(again.contains(n(at(e2, 0, 0)) + ",80")));
        check("late: no day file record of a minute with readings", "0 0",
              n(again.contains(n(at(e1, 23, 45, 10)) + ",60")) + " " + n(again.contains(n(at(e2, 0, 0, 40)) + ",80")));
        QByteArray all;
        for (const QByteArray &r : sync(late, HeartRateReadings, 0, MaxRecords))
            all += r.split(',').first().toLongLong() / 1000 >= at(e2, 0, 0) ? "+" : "";
        check("late: hrr after midnight", n(90), n(all.size()));
    }

    // A long backlog (hrr: the first sync of a phone), in pages; and asking again from the middle (a sync
    // that stopped when the link dropped) gives the rest, once each
    {
        QTemporaryDir tmpLong;
        const QString lng = tmpLong.path() + "/";
        addReadings(lng, seconds(at(e1, 10, 0), at(e1, 10, 0) + 5000, [](qint64 t) { return 50 + int(t % 90); }));
        QByteArray mores;
        const QList<QByteArray> all = sync(lng, HeartRateReadings, 0, MaxRecords, &mores);
        bool ordered = true;
        for (int i = 1; i < all.size(); i++)
            ordered = ordered && all[i - 1].split(',').first().toLongLong() < all[i].split(',').first().toLongLong();
        check("backlog: hrr pages", "5000 11110 1", n(all.size()) + " " + mores + " " + n(ordered));
        const QList<QByteArray> rest = sync(lng, HeartRateReadings, all[2499].split(',').first().toLongLong(), MaxRecords);
        check("backlog: resumed", "2500 1", n(rest.size()) + " " + n(rest == all.mid(2500)));
        check("backlog: resumed with an overlap", "2600 1",
              n(sync(lng, HeartRateReadings, all[2399].split(',').first().toLongLong(), 300).size()) + " "
              + n(sync(lng, HeartRateReadings, all[2399].split(',').first().toLongLong(), 300) == all.mid(2400)));
    }

    // hr over many minutes in pages, with day file records (every 10 min, all in minutes with readings)
    {
        QTemporaryDir tmpMin;
        const QString mins = tmpMin.path() + "/";
        QList<Reading> sparse;
        const qint64 start = at(e1, 0, 0);
        for (qint64 t = start; t < start + 2500 * 60; t += 20)
            sparse.append({t * 1000 + 7, 55 + int((t / 60) % 40), 3});
        addReadings(mins, sparse);
        QByteArray day1, day2;
        for (qint64 t = start + 5; t < start + 2500 * 60; t += 600)
            (QDateTime::fromSecsSinceEpoch(t).date() == e1 ? day1 : day2) += line(t, 55 + int((t / 60) % 40));
        writeFile(mins, "heartrateMonitor", e1, day1);
        writeFile(mins, "heartrateMonitor", e2, day2);
        QByteArray mores;
        const QList<QByteArray> all = sync(mins, HeartRate, 0, MaxRecords, &mores);
        QSet<QByteArray> unique(all.begin(), all.end());
        bool minutes = true;
        for (int i = 0; i < all.size(); i++)
            minutes = minutes && all[i].split(',').first().toLongLong() == start + i * 60;
        check("hr pages of minutes", "2500 2500 110 1", n(all.size()) + " " + n(unique.size()) + " " + mores + " "
              + n(minutes));
    }

    // A read while sensorlogd writes a batch (WAL): the readings committed so far, at once
    {
        QTemporaryDir tmpWal;
        const QString wal = tmpWal.path() + "/";
        addReadings(wal, seconds(at(e1, 7, 0), at(e1, 7, 1), [](qint64) { return 64; }));
        sqlite3 *writer = openDb(wal);
        sqlite3_exec(writer, "BEGIN IMMEDIATE", nullptr, nullptr, nullptr);
        insert(writer, seconds(at(e1, 7, 1), at(e1, 7, 31), [](qint64) { return 66; }));
        QElapsedTimer took;
        took.start();
        const Page during = query(wal, HeartRateReadings, 0);
        const qint64 ms = took.elapsed();
        check("wal: read during a write", "60 1", n(during.records.size()) + " " + n(ms < 1000));
        sqlite3_exec(writer, "COMMIT", nullptr, nullptr, nullptr);
        check("wal: after it", "1000 1", n(query(wal, HeartRateReadings, 0).records.size()) + " "
              + n(query(wal, HeartRateReadings, 0).more));
        check("wal: hr after it", "31", n(query(wal, HeartRate, 0).records.size()));
        sqlite3_close(writer);
    }

    printf("%d failure(s)\n", failures);
    return failures;
}
