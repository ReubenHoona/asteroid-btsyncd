/*
 * Tests of the calendar service with a temporary mkcal database
 * (cmake -DBTSYNCD_TESTS=ON). Run on a private bus, like notifications_test:
 *   dbus-run-session -- sh -c 'DBUS_SYSTEM_BUS_ADDRESS=$DBUS_SESSION_BUS_ADDRESS calendar_test'
 * The test writes to the events characteristic the way BlueZ does (also an
 * element split over writes) and reads the database back with mkcal:
 *   - timed and all-day events in the Phone notebook, with their texts
 *   - an update replaces an event, <del> removes one
 *   - a full sync (<sync>begin/end</sync>) removes what the phone did not send
 *   - bad elements are ignored and do not break what follows
 */

#include "calendarservice.h"
#include "common.h"
#include "fixtures.h"

#include <MDConfItem>
#include <QCoreApplication>
#include <QDBusConnection>
#include <QDBusMessage>
#include <QElapsedTimer>
#include <QTemporaryDir>
#include <QTimeZone>
#include <cstdio>

#include <extendedcalendar.h>
#include <extendedstorage.h>
#include <notebook.h>
#include <KCalendarCore/Alarm>
#include <KCalendarCore/Event>
#include <algorithm>

static int failures = 0;

#define CHECK(cond) do { if (!(cond)) { fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); failures++; } } while (0)

static const char *CHR_EVENTS = "/org/asteroidos/btsyncd/service0/char0";
static const char *CHR_CAPS = "/org/asteroidos/btsyncd/service0/char1";
static const QString Notebook = "8f1c2e4a-5b7d-4e6f-9a0b-a57e401d05ca";

static QString btsyncdName;

static void spin(int ms)
{
    QElapsedTimer t;
    t.start();
    while(t.elapsed() < ms)
        QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
}

static QDBusMessage gattCall(QDBusConnection &driver, const char *path, const char *method, const QList<QVariant> &args)
{
    QDBusMessage m = QDBusMessage::createMethodCall(btsyncdName, path, GATT_CHRC_IFACE, method);
    m.setArguments(args);
    return driver.call(m, QDBus::BlockWithGui, 5000);
}

static void write(QDBusConnection &driver, const QByteArray &value)
{
    const QDBusMessage r = gattCall(driver, CHR_EVENTS, "WriteValue", {value, QVariantMap()});
    CHECK(r.type() == QDBusMessage::ReplyMessage);
}

// What another process (the calendar app, the agenda) reads: a fresh storage
static QHash<QString, KCalendarCore::Event::Ptr> readBack(QString *notebookName = nullptr)
{
    QHash<QString, KCalendarCore::Event::Ptr> events;
    mKCal::ExtendedCalendar::Ptr cal(new mKCal::ExtendedCalendar(QTimeZone::systemTimeZone()));
    mKCal::ExtendedStorage::Ptr storage = mKCal::ExtendedCalendar::defaultStorage(cal);
    CHECK(storage->open());
    mKCal::Notebook::Ptr nb = storage->notebook(Notebook);
    CHECK(nb);
    if(nb && notebookName)
        *notebookName = nb->name();
    storage->loadNotebookIncidences(Notebook);
    for(const KCalendarCore::Event::Ptr &e : cal->events())
        events.insert(e->uid(), e);
    storage->close();
    return events;
}

static QString uid(const char *id)
{
    return QStringLiteral("phone-") + id + QStringLiteral("@asteroid-btsyncd");
}

int main(int argc, char **argv)
{
    QTemporaryDir dir;
    qputenv("MKCAL_STORAGEDB", (dir.path() + "/db").toUtf8());
    QCoreApplication app(argc, argv);
    if(qEnvironmentVariableIsEmpty("DBUS_SYSTEM_BUS_ADDRESS")) {
        fprintf(stderr, "needs a private bus as the system bus (see the top of the source)\n");
        return 2;
    }

    // Version 2 of the parser: colour, response, organizer, reminders, the phone's event id
    {
        CHECK(CalendarService::normalizeColor("ff3fa9f5") == "ff3fa9f5");
        CHECK(CalendarService::normalizeColor("FF3FA9F5") == "ff3fa9f5");
        CHECK(CalendarService::normalizeColor("3fa9f5") == "ff3fa9f5");
        CHECK(CalendarService::normalizeColor("#3FA9F5") == "ff3fa9f5");
        CHECK(CalendarService::normalizeColor("").isEmpty() && CalendarService::normalizeColor("red").isEmpty()
              && CalendarService::normalizeColor("ff3fa9f").isEmpty() && CalendarService::normalizeColor("ff3fa9f5a").isEmpty()
              && CalendarService::normalizeColor("gg3fa9f5").isEmpty());
        CHECK(CalendarService::parseReminders("10,30") == QList<int>({10, 30}));
        CHECK(CalendarService::parseReminders("0,1440,40320") == QList<int>({0, 1440, 40320}));
        CHECK(CalendarService::parseReminders("10,10,x,-5,40321,,15") == QList<int>({10, 15}));
        CHECK(CalendarService::parseReminders("1,2,3,4,5,6,7") == QList<int>({1, 2, 3, 4, 5}));
        CHECK(CalendarService::parseReminders("").isEmpty());
        CalendarEventData e;
        CHECK(CalendarService::parseEvent("<ev><id>5</id><st>1767225600</st><co>FF3FA9F5</co><rs>2</rs><or>a &amp; b</or><rm>5,10</rm><ei>99</ei></ev>", &e));
        CHECK(e.color == "ff3fa9f5" && e.response == 2 && e.organizer == "a & b" && e.reminders == QList<int>({5, 10}) && e.eventId == "99");
        CalendarEventData n;
        CHECK(CalendarService::parseEvent("<ev><id>6</id><st>1767225600</st><co>nope</co><rs>4</rs></ev>", &n));
        CHECK(n.color.isEmpty() && n.response == -1 && n.organizer.isEmpty() && n.reminders.isEmpty() && n.eventId.isEmpty());
        // the frames Gadgetbridge writes (W2-gb-data), all parse
        const QList<QByteArray> frames = fixtureFrames("calendar-events-v2.frames.txt");
        CHECK(frames.size() == 6);
        CalendarEventData f;
        CHECK(CalendarService::parseEvent(frames.value(1), &f));
        CHECK(f.id == "77" && f.title == "Stand-up" && f.start.toSecsSinceEpoch() == 1793200000 && f.end.toSecsSinceEpoch() == 1793203600
              && f.location == "Room 2" && f.description == "Notes" && f.calendar == "me@example.org/Work" && f.color == "ff3fa9f5"
              && f.response == 0 && f.organizer == "boss@example.org" && f.reminders == QList<int>({10, 30}) && f.eventId == "12");
    }

    // The parser alone
    {
        CalendarEventData e;
        CHECK(CalendarService::parseEvent("<ev><id>5</id><ti>A &amp; B</ti><st>1767225600</st><en>1767229200</en></ev>", &e));
        CHECK(e.id == "5" && e.title == "A & B" && !e.allDay);
        CHECK(e.start == QDateTime::fromSecsSinceEpoch(1767225600, QTimeZone::utc()));
        CalendarEventData bad;
        CHECK(!CalendarService::parseEvent("<ev><ti>no id</ti><st>1</st></ev>", &bad));
        CalendarEventData bad2;
        CHECK(!CalendarService::parseEvent("<ev><id>6</id><ti>no start</ti></ev>", &bad2));
    }

    CalendarService *service = new CalendarService(0, QDBusConnection::systemBus());
    btsyncdName = QDBusConnection::systemBus().baseService();
    QDBusConnection driver = QDBusConnection::connectToBus(QDBusConnection::SystemBus, "driver");
    spin(100);

    {
        const QDBusMessage r = gattCall(driver, CHR_CAPS, "ReadValue", {QVariantMap()});
        CHECK(r.arguments().value(0).toByteArray() == "<capabilities><version>2</version></capabilities>");
    }

    // A full sync: three events, one split over two writes
    const qint64 t0 = 1767258000; // 2026-01-01 09:00 UTC
    write(driver, "<sync>begin</sync>");
    write(driver, "<ev><id>101</id><ti>Standup</ti><st>" + QByteArray::number(t0) + "</st><en>"
                  + QByteArray::number(t0 + 900) + "</en><lo>Room &lt;2&gt;</lo><de>Daily</de><cn>Work</cn></ev>");
    write(driver, "<ev><id>102</id><ti>Holiday</ti><st>1767225600</st><en>1767398400</en><a");
    write(driver, "d>1</ad></ev><ev><id>103</id><ti>Lunch</ti><st>" + QByteArray::number(t0 + 3 * 3600) + "</st><en>"
                  + QByteArray::number(t0 + 4 * 3600) + "</en></ev>");
    write(driver, "<sync>end</sync>");
    spin(200);
    {
        QString name;
        const auto events = readBack(&name);
        CHECK(name == "Phone");
        CHECK(events.size() == 3);
        const KCalendarCore::Event::Ptr standup = events.value(uid("101"));
        CHECK(standup && standup->summary() == "Standup");
        CHECK(standup && standup->location() == "Room <2>");
        CHECK(standup && standup->description() == "Daily");
        CHECK(standup && standup->dtStart().toSecsSinceEpoch() == t0);
        CHECK(standup && standup->dtEnd().toSecsSinceEpoch() == t0 + 900);
        CHECK(standup && !standup->allDay());
        const KCalendarCore::Event::Ptr holiday = events.value(uid("102"));
        CHECK(holiday && holiday->allDay());
        // Two days: 1 and 2 January (the end is inclusive on the watch)
        CHECK(holiday && holiday->dtStart().date() == QDate(2026, 1, 1));
        CHECK(holiday && holiday->dtEnd().date() == QDate(2026, 1, 2));
        CHECK(events.contains(uid("103")));
    }

    // Version 2: the three events of the frames (the same event, as the phone sends it again
    // each time something about it changed); what is not sent again is gone from the event
    {
        const QList<QByteArray> frames = fixtureFrames("calendar-events-v2.frames.txt");
        auto prop = [](const KCalendarCore::Event::Ptr &e, const char *name) { return e ? e->nonKDECustomProperty(name) : QString(); };
        write(driver, frames.value(1));
        spin(1200);
        {
            const auto events = readBack();
            const KCalendarCore::Event::Ptr e = events.value(uid("77"));
            CHECK(e && e->summary() == "Stand-up" && e->location() == "Room 2" && e->description() == "Notes");
            CHECK(e && e->dtStart().toSecsSinceEpoch() == 1793200000 && e->dtEnd().toSecsSinceEpoch() == 1793203600);
            CHECK(prop(e, "X-EOS-COLOR") == "ff3fa9f5");
            CHECK(prop(e, "X-EOS-RSVP") == "0");
            CHECK(prop(e, "X-EOS-ORGANIZER") == "boss@example.org");
            CHECK(prop(e, "X-EOS-CALENDAR") == "me@example.org/Work");
            CHECK(prop(e, "X-EOS-EVENTID") == "12");
            // the reminders: display alarms 10 and 30 minutes before, never scheduled
            CHECK(e && e->alarms().size() == 2);
            QList<int> offsets;
            bool display = true;
            if(e) {
                for(const KCalendarCore::Alarm::Ptr &a : e->alarms()) {
                    offsets << int(a->startOffset().asSeconds());
                    display = display && a->type() == KCalendarCore::Alarm::Display;
                }
            }
            std::sort(offsets.begin(), offsets.end());
            CHECK(offsets == QList<int>({-1800, -600}) && display);
        }
        write(driver, frames.value(2));
        spin(1200);
        {
            const KCalendarCore::Event::Ptr e = readBack().value(uid("77"));
            CHECK(prop(e, "X-EOS-COLOR") == "ffd50000");
            CHECK(prop(e, "X-EOS-RSVP") == "1");
            CHECK(prop(e, "X-EOS-ORGANIZER").isEmpty());
            CHECK(prop(e, "X-EOS-EVENTID") == "12");
            CHECK(e && e->alarms().isEmpty());
        }
        write(driver, frames.value(3));
        spin(1200);
        {
            const KCalendarCore::Event::Ptr e = readBack().value(uid("77"));
            CHECK(prop(e, "X-EOS-COLOR").isEmpty() && prop(e, "X-EOS-RSVP").isEmpty());
            CHECK(prop(e, "X-EOS-CALENDAR") == "me@example.org/Work" && prop(e, "X-EOS-EVENTID") == "12");
        }
        // an event without <ei> is its own event id (what the rsvp and open actions send back)
        write(driver, "<ev><id>78</id><ti>Plain</ti><st>1793200000</st></ev>");
        spin(1200);
        CHECK(prop(readBack().value(uid("78")), "X-EOS-EVENTID") == "78");
        // the whole sequence as one sync: 78 and 101..104 were not sent, so they go; <del> of an
        // event the watch never had is harmless
        for(const QByteArray &f : frames)
            write(driver, f);
        spin(300);
        {
            const auto events = readBack();
            CHECK(events.size() == 1 && events.contains(uid("77")));
            CHECK(prop(events.value(uid("77")), "X-EOS-EVENTID") == "12");
        }
        write(driver, "<clear/>");
        spin(1200);
        CHECK(readBack().isEmpty());
        // the base for the rest of the test: the three events of the full sync above again
        write(driver, "<sync>begin</sync>");
        write(driver, "<ev><id>101</id><ti>Standup (moved)</ti><st>" + QByteArray::number(t0 + 1800) + "</st><en>"
                      + QByteArray::number(t0 + 2700) + "</en></ev>");
        write(driver, "<ev><id>102</id><ti>Holiday</ti><st>1767225600</st><en>1767398400</en><ad>1</ad></ev>");
        write(driver, "<ev><id>103</id><ti>Lunch</ti><st>" + QByteArray::number(t0 + 3 * 3600) + "</st></ev>");
        write(driver, "<sync>end</sync>");
        spin(300);
    }

    // An update, a deletion, a bad element, then a new event
    write(driver, "<ev><id>101</id><ti>Standup (moved)</ti><st>" + QByteArray::number(t0 + 1800) + "</st><en>"
                  + QByteArray::number(t0 + 2700) + "</en></ev>");
    write(driver, "<del><id>103</id></del>");
    write(driver, "<ev><ti>no id</ti><st>1</st></ev><bogus>x</bogus>");
    write(driver, "<ev><id>104</id><ti>Dentist</ti><st>" + QByteArray::number(t0 + 86400) + "</st></ev>");
    spin(1200);  // the save waits 500 ms
    {
        const auto events = readBack();
        CHECK(events.size() == 3);
        const KCalendarCore::Event::Ptr standup = events.value(uid("101"));
        CHECK(standup && standup->summary() == "Standup (moved)");
        CHECK(standup && standup->dtStart().toSecsSinceEpoch() == t0 + 1800);
        CHECK(!events.contains(uid("103")));
        const KCalendarCore::Event::Ptr dentist = events.value(uid("104"));
        // No end: as long as it starts
        CHECK(dentist && dentist->dtEnd().toSecsSinceEpoch() == t0 + 86400);
    }

    // A full sync with only 104 left on the phone
    write(driver, "<sync>begin</sync><ev><id>104</id><ti>Dentist</ti><st>" + QByteArray::number(t0 + 86400) + "</st></ev><sync>end</sync>");
    spin(200);
    {
        const auto events = readBack();
        CHECK(events.size() == 1);
        CHECK(events.contains(uid("104")));
    }

    // CR-55: the end frame Gadgetbridge writes, <sync>end</sync><un>EPOCH</un> (two elements in one
    // write), sets /org/asteroidos/calendar/synced-until; the old frame still works and leaves the key
    {
        MDConfItem until(QStringLiteral("/org/asteroidos/calendar/synced-until"));
        until.unset();
        spin(300);
        const auto untilIs = [&until](qint64 v) {
            QElapsedTimer t;
            t.start();
            while(until.value(0).toLongLong() != v && t.elapsed() < 3000)
                spin(50);
            return until.value(0).toLongLong() == v;
        };
        CHECK(untilIs(0));
        // the old frame: the sync ends as before, the key stays unset (0 = unknown)
        write(driver, "<sync>begin</sync><ev><id>104</id><ti>Dentist</ti><st>" + QByteArray::number(t0 + 86400) + "</st></ev>");
        write(driver, "<sync>end</sync>");
        spin(400);
        CHECK(untilIs(0));
        CHECK(readBack().contains(uid("104")));
        // the frame Gadgetbridge records (FP-G1): the last frame of the fixture
        const QList<QByteArray> frames = fixtureFrames("calendar-events-v2.frames.txt");
        CHECK(frames.value(5).contains("<sync>end</sync><un>1793800000</un>"));
        write(driver, "<sync>begin</sync>");
        write(driver, frames.value(5));
        CHECK(untilIs(1793800000));
        // milliseconds are taken as seconds
        write(driver, "<sync>begin</sync>");
        write(driver, "<sync>end</sync><un>1793900000000</un>");
        CHECK(untilIs(1793900000));
        // a value that is not a time, or an <un> that does not follow an end, changes nothing
        write(driver, "<sync>begin</sync>");
        write(driver, "<sync>end</sync><un>soon</un>");
        spin(300);
        CHECK(untilIs(1793900000));
        write(driver, "<un>1794000000</un>");
        spin(300);
        CHECK(untilIs(1793900000));
        write(driver, "<sync>begin</sync>");
        write(driver, "<sync>end</sync><clear/><un>1794000000</un>");
        spin(300);
        CHECK(untilIs(1793900000));
        // 0 is "not known" (the phone syncs no calendar any more): it is written, the stale end goes
        write(driver, "<sync>begin</sync>");
        write(driver, "<sync>end</sync><un>0</un>");
        CHECK(untilIs(0));
        // a negative value is still not a time
        write(driver, "<sync>begin</sync>");
        write(driver, "<sync>end</sync><un>1793900000</un>");
        CHECK(untilIs(1793900000));
        write(driver, "<sync>begin</sync>");
        write(driver, "<sync>end</sync><un>-5</un>");
        spin(300);
        CHECK(untilIs(1793900000));
        until.unset();
        spin(200);
    }

    // Clear
    write(driver, "<clear/>");
    spin(1200);
    CHECK(readBack().isEmpty());

    delete service;
    if(failures)
        fprintf(stderr, "%d check(s) failed\n", failures);
    else
        fprintf(stderr, "calendar_test: all checks passed\n");
    return failures ? 1 : 0;
}
