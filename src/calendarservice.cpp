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

#include "calendarservice.h"
#include "characteristic.h"
#include "common.h"
#include "wakehold.h"

#include <QDebug>
#include <QTimer>
#include <QTimeZone>
#include <QXmlStreamReader>

#include <MDConfItem>

#include <extendedcalendar.h>
#include <extendedstorage.h>
#include <notebook.h>
#include <KCalendarCore/Alarm>
#include <KCalendarCore/Event>
#include <QRegularExpression>

static const char CAPABILITIES[] = "<capabilities><version>2</version></capabilities>";

// The notebook the phone's events go to, and the prefix of their uids
static const char NotebookUid[] = "8f1c2e4a-5b7d-4e6f-9a0b-a57e401d05ca";
static const char UidPrefix[] = "phone-";
static const char UidSuffix[] = "@asteroid-btsyncd";

// An element larger than this never ends: dropped
static const int MaxPending = 16 * 1024;

/* The mkcal storage, opened at the first event */
class CalendarStore
{
public:
    ~CalendarStore()
    {
        if(mStorage)
            mStorage->close();
    }

    bool open()
    {
        if(mStorage)
            return true;
        mCal = mKCal::ExtendedCalendar::Ptr(new mKCal::ExtendedCalendar(QTimeZone::systemTimeZone()));
        mKCal::ExtendedStorage::Ptr storage = mKCal::ExtendedCalendar::defaultStorage(mCal);
        if(!storage || !storage->open()) {
            qWarning() << "Calendar: cannot open the mkcal storage";
            mCal.clear();
            return false;
        }
        mKCal::Notebook::Ptr nb = storage->notebook(NotebookUid);
        if(!nb) {
            // Visible, not shared, not the default: new events made on the
            // watch do not go here
            nb = mKCal::Notebook::Ptr(new mKCal::Notebook(NotebookUid, QObject::tr("Phone"),
                QStringLiteral("Events from the phone's calendar (asteroid-btsyncd)"),
                QStringLiteral("#3fa9f5"), false, false, false, false, true));
            if(!storage->addNotebook(nb)) {
                qWarning() << "Calendar: cannot add the Phone notebook";
                storage->close();
                mCal.clear();
                return false;
            }
            qInfo() << "Calendar: added the Phone notebook";
        }
        if(!storage->loadNotebookIncidences(NotebookUid))
            qWarning() << "Calendar: cannot load the Phone notebook";
        mStorage = storage;
        return true;
    }

    static void setOrRemove(const KCalendarCore::Event::Ptr &ev, const char *name, const QString &value)
    {
        if(value.isEmpty())
            ev->removeNonKDECustomProperty(name);
        else
            ev->setNonKDECustomProperty(name, value);
    }

    static QString uidFor(const QString &id)
    {
        return QLatin1String(UidPrefix) + id + QLatin1String(UidSuffix);
    }

    static QString idOf(const QString &uid)
    {
        if(!uid.startsWith(QLatin1String(UidPrefix)) || !uid.endsWith(QLatin1String(UidSuffix)))
            return QString();
        return uid.mid(int(sizeof(UidPrefix)) - 1, uid.length() - int(sizeof(UidPrefix)) - int(sizeof(UidSuffix)) + 2);
    }

    void upsert(const CalendarEventData &e)
    {
        if(!open())
            return;
        const QString uid = uidFor(e.id);
        KCalendarCore::Event::Ptr ev = mCal->event(uid);
        const bool isNew = !ev;
        if(isNew) {
            ev = KCalendarCore::Event::Ptr(new KCalendarCore::Event);
            ev->setUid(uid);
        } else {
            ev->startUpdates();
        }
        ev->setSummary(e.title);
        ev->setLocation(e.location);
        ev->setDescription(e.description);
        ev->setReadOnly(false);
        setOrRemove(ev, CalendarService::PropColor, e.color);
        setOrRemove(ev, CalendarService::PropResponse, e.response >= 0 ? QString::number(e.response) : QString());
        setOrRemove(ev, CalendarService::PropOrganizer, e.organizer);
        setOrRemove(ev, CalendarService::PropCalendar, e.calendar);
        setOrRemove(ev, CalendarService::PropEventId, e.eventId.isEmpty() ? e.id : e.eventId);
        // The reminders: display alarms before the start, never enabled
        ev->clearAlarms();
        for(int minutes : e.reminders) {
            KCalendarCore::Alarm::Ptr alarm = ev->newAlarm();
            alarm->setType(KCalendarCore::Alarm::Display);
            alarm->setStartOffset(KCalendarCore::Duration(-minutes * 60, KCalendarCore::Duration::Seconds));
            alarm->setEnabled(false);
        }
        const QTimeZone tz = QTimeZone::systemTimeZone();
        if(e.allDay) {
            // The phone's all-day times are UTC midnights; the end is exclusive
            // there and inclusive (the last day) here
            const QDate first = e.start.toUTC().date();
            QDate last = e.end.isValid() ? e.end.toUTC().date().addDays(-1) : first;
            if(last < first)
                last = first;
            ev->setDtStart(QDateTime(first, QTime(0, 0), tz));
            ev->setDtEnd(QDateTime(last, QTime(0, 0), tz));
            ev->setAllDay(true);
        } else {
            const QDateTime start = e.start.toTimeZone(tz);
            const QDateTime end = e.end.isValid() && e.end > e.start ? e.end.toTimeZone(tz) : start;
            ev->setAllDay(false);
            ev->setDtStart(start);
            ev->setDtEnd(end);
        }
        if(isNew) {
            if(!mCal->addEvent(ev, NotebookUid))
                qWarning() << "Calendar: cannot add event" << e.id;
        } else {
            ev->endUpdates();
        }
    }

    // false if there is no such event
    bool remove(const QString &id)
    {
        if(!open())
            return false;
        KCalendarCore::Event::Ptr ev = mCal->event(uidFor(id));
        return ev && mCal->deleteEvent(ev);
    }

    // The ids of the phone's events on the watch
    QSet<QString> ids()
    {
        QSet<QString> set;
        if(!open())
            return set;
        for(const KCalendarCore::Event::Ptr &ev : mCal->events()) {
            if(mCal->notebook(ev) != QLatin1String(NotebookUid))
                continue;
            const QString id = idOf(ev->uid());
            if(!id.isEmpty())
                set.insert(id);
        }
        return set;
    }

    void clear()
    {
        for(const QString &id : ids())
            remove(id);
    }

    bool save()
    {
        if(!mStorage)
            return false;
        const bool ok = mStorage->save(mKCal::ExtendedStorage::PurgeDeleted);
        if(!ok)
            qWarning() << "Calendar: saving failed";
        return ok;
    }

private:
    mKCal::ExtendedCalendar::Ptr mCal;
    mKCal::ExtendedStorage::Ptr mStorage;
};

class CalendarEventsChrc : public Characteristic
{
public:
    CalendarEventsChrc(QDBusConnection bus, unsigned int index, CalendarService *service)
      : Characteristic(bus, index, CAL_EVENTS_UUID, {"encrypt-authenticated-write"}, service, service), mService(service) {}

public slots:
    void WriteValue(QByteArray value, QVariantMap) override
    {
        // Awake until the events are stored (the save follows 500 ms after
        // the last write)
        WakeHold::hold();
        handle(value);
        WakeHold::release(WakeHold::GRACE_NOTIFICATION_MS);
    }

private:
    void handle(const QByteArray &value)
    {
        mPending += value;
        // Every complete element: <name>...</name> or <name/>
        for(;;) {
            int start = mPending.indexOf('<');
            if(start < 0) {
                mPending.clear();
                return;
            }
            if(start > 0)
                mPending.remove(0, start);
            int nameEnd = 1;
            while(nameEnd < mPending.size() && mPending[nameEnd] != '>' && mPending[nameEnd] != '/'
                  && mPending[nameEnd] != ' ')
                nameEnd++;
            const int tagEnd = mPending.indexOf('>');
            if(tagEnd < 0 || nameEnd >= mPending.size())
                break;
            const QByteArray name = mPending.mid(1, nameEnd - 1);
            int end;
            if(mPending[tagEnd - 1] == '/') {
                end = tagEnd + 1;
            } else {
                const QByteArray close = "</" + name + ">";
                const int closeAt = mPending.indexOf(close, tagEnd);
                if(closeAt < 0)
                    break;
                end = closeAt + close.size();
            }
            const QByteArray element = mPending.left(end);
            mPending.remove(0, end);
            mService->handleElement(element);
        }
        if(mPending.size() > MaxPending) {
            qWarning() << "Calendar: an element without an end, dropped";
            mPending.clear();
        }
    }

    CalendarService *mService;
    QByteArray mPending;
};

class CalendarCapabilitiesChrc : public Characteristic
{
public:
    CalendarCapabilitiesChrc(QDBusConnection bus, unsigned int index, Service *service)
      : Characteristic(bus, index, CAL_CAP_UUID, {"encrypt-authenticated-read"}, service, service) {}

public slots:
    QByteArray ReadValue(QVariantMap options) override
    {
        QByteArray value(CAPABILITIES);
        const int offset = options.value("offset", 0).toInt();
        return offset > 0 ? value.mid(offset) : value;
    }
};

CalendarService::CalendarService(int index, QDBusConnection bus, QObject *parent)
    : Service(bus, index, CALENDAR_UUID, parent)
{
    addCharacteristic(new CalendarEventsChrc(bus, 0, this));
    addCharacteristic(new CalendarCapabilitiesChrc(bus, 1, this));
    mStore = new CalendarStore;
    // A sync writes many events: saved together
    mSaveTimer = new QTimer(this);
    mSaveTimer->setSingleShot(true);
    mSaveTimer->setInterval(500);
    connect(mSaveTimer, &QTimer::timeout, this, [this] { mStore->save(); });
}

CalendarService::~CalendarService()
{
    if(mSaveTimer->isActive())
        mStore->save();
    delete mStore;
}

void CalendarService::scheduleSave()
{
    mSaveTimer->start();
}

static QDateTime fromEpoch(const QString &text)
{
    bool ok = false;
    const qint64 secs = text.trimmed().toLongLong(&ok);
    return ok ? QDateTime::fromSecsSinceEpoch(secs, QTimeZone::utc()) : QDateTime();
}

QString CalendarService::normalizeColor(const QString &text)
{
    static const QRegularExpression re(QStringLiteral("^(?:[0-9a-fA-F]{2})?[0-9a-fA-F]{6}$"));
    QString c = text.trimmed();
    if(c.startsWith(QLatin1Char('#')))
        c.remove(0, 1);
    if(!re.match(c).hasMatch())
        return QString();
    if(c.size() == 6)
        c.prepend(QStringLiteral("ff"));
    return c.toLower();
}

QList<int> CalendarService::parseReminders(const QString &text)
{
    QList<int> minutes;
    for(const QString &part : text.split(QLatin1Char(','), Qt::SkipEmptyParts)) {
        bool ok = false;
        const int m = part.trimmed().toInt(&ok);
        if(!ok || m < 0 || m > MaxReminderMinutes || minutes.contains(m))
            continue;
        minutes << m;
        if(minutes.size() == MaxReminders)
            break;
    }
    return minutes;
}

bool CalendarService::parseEvent(const QByteArray &xml, CalendarEventData *event)
{
    QXmlStreamReader reader(xml);
    if(!reader.readNextStartElement() || reader.name() != QLatin1String("ev"))
        return false;
    while(reader.readNextStartElement()) {
        const QStringView n = reader.name();
        if(n == QLatin1String("id")) event->id = reader.readElementText().trimmed();
        else if(n == QLatin1String("ti")) event->title = reader.readElementText();
        else if(n == QLatin1String("st")) event->start = fromEpoch(reader.readElementText());
        else if(n == QLatin1String("en")) event->end = fromEpoch(reader.readElementText());
        else if(n == QLatin1String("ad")) event->allDay = reader.readElementText().trimmed() == "1";
        else if(n == QLatin1String("lo")) event->location = reader.readElementText();
        else if(n == QLatin1String("de")) event->description = reader.readElementText();
        else if(n == QLatin1String("cn")) event->calendar = reader.readElementText();
        else if(n == QLatin1String("co")) event->color = normalizeColor(reader.readElementText());
        else if(n == QLatin1String("rs")) {
            bool ok = false;
            const int r = reader.readElementText().trimmed().toInt(&ok);
            event->response = ok && r >= 0 && r <= 3 ? r : -1;
        }
        else if(n == QLatin1String("or")) event->organizer = reader.readElementText().trimmed();
        else if(n == QLatin1String("rm")) event->reminders = parseReminders(reader.readElementText());
        else if(n == QLatin1String("ei")) event->eventId = reader.readElementText().trimmed();
        else reader.skipCurrentElement();
    }
    if(reader.hasError()) {
        qWarning() << "Calendar: bad event" << reader.errorString();
        return false;
    }
    return !event->id.isEmpty() && event->start.isValid();
}

void CalendarService::handleElement(const QByteArray &xml)
{
    // the <un> of an end frame follows its <sync>end</sync> directly; anything else in between
    // (or alone) is not the end of a sync
    const bool untilNext = mUntilNext;
    mUntilNext = false;
    if(xml.startsWith("<un>")) {
        if(!untilNext) {
            qWarning() << "Calendar: <un> without a sync end, ignored";
            return;
        }
        QXmlStreamReader reader(xml);
        bool ok = false;
        qint64 until = 0;
        if(reader.readNextStartElement())
            until = reader.readElementText().trimmed().toLongLong(&ok);
        // seconds since 1970 (milliseconds are taken too), a time in this century
        if(ok && until > 100000000000LL)
            until /= 1000;
        if(!ok || until < 1000000000LL || until > 4102444800LL) {
            qWarning() << "Calendar: the end of the synced window is not a time, ignored";
            return;
        }
        MDConfItem(QStringLiteral("/org/asteroidos/calendar/synced-until")).set(QVariant::fromValue<qlonglong>(until));
        qInfo() << "Calendar: the phone's events are synced until" << QDateTime::fromSecsSinceEpoch(until, QTimeZone::utc()).toString(Qt::ISODate);
        return;
    }
    if(xml.startsWith("<ev>") || xml.startsWith("<ev ")) {
        CalendarEventData event;
        if(!parseEvent(xml, &event)) {
            qWarning() << "Calendar: event without id or start, ignored";
            return;
        }
        mStore->upsert(event);
        if(mSyncing)
            mSynced.insert(event.id);
        scheduleSave();
    } else if(xml.startsWith("<del>")) {
        QXmlStreamReader reader(xml);
        QString id;
        if(reader.readNextStartElement()) {
            while(reader.readNextStartElement()) {
                if(reader.name() == QLatin1String("id"))
                    id = reader.readElementText().trimmed();
                else
                    reader.skipCurrentElement();
            }
        }
        if(!id.isEmpty()) {
            mStore->remove(id);
            mSynced.remove(id);
            scheduleSave();
        }
    } else if(xml.startsWith("<sync>")) {
        QXmlStreamReader reader(xml);
        QString state;
        if(reader.readNextStartElement())
            state = reader.readElementText().trimmed();
        if(state == "begin") {
            mSyncing = true;
            mSynced.clear();
        } else if(state == "end" && mSyncing) {
            int removed = 0;
            for(const QString &id : mStore->ids()) {
                if(!mSynced.contains(id) && mStore->remove(id))
                    removed++;
            }
            qInfo() << "Calendar: synced" << mSynced.size() << "events from the phone, removed" << removed;
            mSyncing = false;
            mSynced.clear();
            mSaveTimer->stop();
            mStore->save();
        }
        if(state == "end")
            mUntilNext = true;
    } else if(xml.startsWith("<clear")) {
        mStore->clear();
        mSynced.clear();
        scheduleSave();
    } else {
        qWarning() << "Calendar: unknown element" << xml.left(32);
    }
}
