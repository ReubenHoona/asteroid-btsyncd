/*
 * Copyright (C) 2026 Reuben Horner
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

#ifndef CALENDARSERVICE_H
#define CALENDARSERVICE_H

#include <QByteArray>
#include <QDateTime>
#include <QSet>
#include <QString>

#include "service.h"

class QTimer;
class CalendarStore;

/*
 * The phone's calendar on the watch: events the phone sends go to an mkcal
 * notebook "Phone" (the calendar app and the agenda show it).
 *
 * Events characteristic (phone writes, XML; an element may span writes):
 *   <ev><id>ID</id><ti>TITLE</ti><st>START</st><en>END</en><ad>1</ad>
 *       <lo>LOCATION</lo><de>DESCRIPTION</de><cn>CALENDAR</cn></ev>
 *     adds or replaces event ID (START, END: seconds since the epoch, UTC;
 *     all-day: START is midnight UTC of the first day, END of the day after
 *     the last)
 *   <del><id>ID</id></del>      removes event ID
 *   <sync>begin</sync> ... <sync>end</sync>
 *     a full sync: at the end, the phone's events not sent since the begin
 *     are removed
 *   <clear/>                    removes all the phone's events
 * Capabilities characteristic (read): <capabilities><version>1</version></capabilities>
 */
struct CalendarEventData {
    QString id, title, location, description, calendar;
    QDateTime start, end;
    bool allDay = false;
};

class CalendarService : public Service
{
    Q_OBJECT
public:
    explicit CalendarService(int index, QDBusConnection bus, QObject *parent = 0);
    ~CalendarService();

    // One complete element from the phone
    void handleElement(const QByteArray &xml);

    // Parses <ev>...</ev>; false if it has no id or no valid start
    static bool parseEvent(const QByteArray &xml, CalendarEventData *event);

private:
    void scheduleSave();

    CalendarStore *mStore = nullptr;
    QTimer *mSaveTimer = nullptr;
    bool mSyncing = false;
    QSet<QString> mSynced;
};

#endif // CALENDARSERVICE_H
