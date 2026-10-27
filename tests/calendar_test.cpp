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
#include <KCalendarCore/Event>

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
        CHECK(r.arguments().value(0).toByteArray() == "<capabilities><version>1</version></capabilities>");
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
