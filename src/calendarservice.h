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
#include <QList>
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
 *   Capabilities version 2 (Gadgetbridge since W2-gb-data) adds, all optional
 *   and each left out when unknown, to <ev>:
 *     <co>AARRGGBB</co>  the event's colour, else its calendar's (6 digits: opaque)
 *     <rs>N</rs>         the user's response: 0 needs action, 1 accepted,
 *                        2 tentative, 3 declined
 *     <or>ORGANIZER</or> who organises it
 *     <rm>10,30</rm>     reminders, minutes before the start (at most five)
 *     <ei>N</ei>         the phone's own event id when it is not <id>
 *   They are kept on the mkcal event as custom properties X-EOS-COLOR (the
 *   8 hex digits, lower case), X-EOS-RSVP (0..3), X-EOS-ORGANIZER,
 *   X-EOS-CALENDAR (<cn>), X-EOS-EVENTID (<ei>, else <id>: what the watch
 *   sends back in the rsvp and open actions), and the reminders as alarms of
 *   type display with the start offset, stored disabled (nothing on the
 *   watch schedules them, so they add no notification and no wake). A
 *   property the phone no longer sends is removed from the event.
 *   <del><id>ID</id></del>      removes event ID
 *   <sync>begin</sync> ... <sync>end</sync>
 *     a full sync: at the end, the phone's events not sent since the begin
 *     are removed
 *   <clear/>                    removes all the phone's events
 *   <un>EPOCH</un>              right after <sync>end</sync> (CR-55, Gadgetbridge since FP-G1): the
 *                               end of the window the phone synced, seconds since the epoch (ms are
 *                               taken too). It goes to dconf /org/asteroidos/calendar/synced-until
 *                               (x, 0 = unknown), which the Calendar app reads. An older btsyncd
 *                               ignores the element, an older phone does not send it
 * Capabilities characteristic (read): <capabilities><version>2</version></capabilities>
 */
struct CalendarEventData {
    QString id, title, location, description, calendar;
    QDateTime start, end;
    bool allDay = false;
    // version 2
    QString color;              // "aarrggbb", empty: not sent
    int response = -1;          // 0..3, -1: not sent
    QString organizer;
    QList<int> reminders;       // minutes before the start, at most five
    QString eventId;            // <ei>, empty: the same as id
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
    // "ff3fa9f5", "FF3FA9F5" or "3fa9f5" -> "ff3fa9f5"; empty when it is not a colour
    static QString normalizeColor(const QString &text);
    // "10,30,x,-5,10" -> (10, 30): whole minutes 0..40320, no repeats, at most five
    static QList<int> parseReminders(const QString &text);

    // The custom properties on an event (the calendar app reads these)
    static constexpr const char *PropColor = "X-EOS-COLOR";
    static constexpr const char *PropResponse = "X-EOS-RSVP";
    static constexpr const char *PropOrganizer = "X-EOS-ORGANIZER";
    static constexpr const char *PropCalendar = "X-EOS-CALENDAR";
    static constexpr const char *PropEventId = "X-EOS-EVENTID";
    static const int MaxReminders = 5;
    static const int MaxReminderMinutes = 40320;

private:
    void scheduleSave();

    CalendarStore *mStore = nullptr;
    QTimer *mSaveTimer = nullptr;
    bool mSyncing = false;
    // <sync>end</sync> was the last element: an <un> may follow (CR-55)
    bool mUntilNext = false;
    QSet<QString> mSynced;
};

#endif // CALENDARSERVICE_H
