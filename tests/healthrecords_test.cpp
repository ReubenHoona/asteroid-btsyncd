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
 * Tests of the health records (sensorlogd files to protocol pages), without
 * D-Bus. Run with TZ=UTC: healthrecords_test (exit status = failures).
 */

#include <QDate>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QTemporaryDir>
#include <QTime>

#include <cstdio>

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

    printf("%d failure(s)\n", failures);
    return failures;
}
