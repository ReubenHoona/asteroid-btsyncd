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
#include <QStringList>

#include <algorithm>

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
    return Unknown;
}

QString typeName(Type type)
{
    switch (type) {
    case HeartRate: return QStringLiteral("hr");
    case Steps:     return QStringLiteral("steps");
    default:        return QString();
    }
}

Page query(const QString &root, Type type, qint64 since, int max)
{
    Page page;
    if (type == Unknown)
        return page;
    if (max <= 0 || max > MaxRecords)
        max = MaxRecords;

    // The day before since's too: its 23:59:59 record, or a time zone change
    const QDate first = QDateTime::fromSecsSinceEpoch(qMax<qint64>(since, 0)).date().addDays(-1);
    const QDir dir(root + sensorDir(type));
    QList<Entry> entries;
    const QStringList names = dir.entryList({QStringLiteral("*.log")}, QDir::Files, QDir::Name);
    for (const QString &name : names) {
        const QDate date = QDate::fromString(name.chopped(4), QStringLiteral("yyyy-MM-dd"));
        if (!date.isValid() || date < first)
            continue;
        readDay(dir.filePath(name), date, type, entries);
    }

    entries.removeIf([since](const Entry &e) { return e.time <= since; });
    std::stable_sort(entries.begin(), entries.end(),
                     [](const Entry &a, const Entry &b) { return a.time < b.time; });

    // The next page starts after the last time sent: keep a time's records
    // together
    qsizetype n = 0;
    while (n < entries.size() && (n < max || entries[n].time == entries[n - 1].time))
        n++;

    page.more = n < entries.size();
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
