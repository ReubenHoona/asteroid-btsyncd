/*
 * Copyright (C) 2026 eos-mainline contributors
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

/*
 * Tests of the Messages API (messageservice.h) over the assistant relay, on a
 * private bus like assistantlink_test:
 *   dbus-run-session -- sh -c 'DBUS_SYSTEM_BUS_ADDRESS=$DBUS_SESSION_BUS_ADDRESS messages_test'
 * A fake phone answers the acts the way Gadgetbridge does (the frames of
 * W2-gb-data in tests/fixtures): the requests on the wire are the fixture's
 * (as JSON, the order of the keys does not matter), the results reach the
 * caller as the reply of its D-Bus call.
 */

#include "assistantservice.h"
#include "common.h"
#include "contactnames.h"
#include "fixtures.h"
#include "messageservice.h"
#include "notificationservice.h"

#include <QCoreApplication>
#include <QDBusConnection>
#include <QDBusMessage>
#include <QDBusPendingCall>
#include <QDBusPendingReply>
#include <QElapsedTimer>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTemporaryDir>
#include <QVariantMap>
#include <QXmlStreamReader>
#include <cstdio>
#include <functional>

static int failures = 0;

#define CHECK(cond) do { if (!(cond)) { fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); failures++; } } while (0)

static const char *CHR_FEEDBACK = "/org/asteroidos/btsyncd/service0/char1";
static const char *CHR_REQUEST = "/org/asteroidos/btsyncd/service10/char0";
static const char *CHR_REPLY = "/org/asteroidos/btsyncd/service10/char1";
static const char *MESSAGES = "org.asteroidos.btsyncd.Messages";

static QStringList logLines;
static void logHandler(QtMsgType, const QMessageLogContext &, const QString &msg)
{
    logLines << msg;
}

class Collector : public QObject
{
    Q_OBJECT
public:
    QByteArray requests;
    QStringList changed;
    QList<uint> results;
public slots:
    void propertiesChanged(const QString &, const QVariantMap &c, const QStringList &)
    {
        if (c.contains("Value"))
            requests += c.value("Value").toByteArray();
    }
    void onChanged(const QString &key) { changed << key; }
    void onActResult(uint id, const QString &) { results << id; }
};

static bool waitFor(const std::function<bool()> &cond, int ms = 3000)
{
    QElapsedTimer t;
    t.start();
    while (!cond() && t.elapsed() < ms)
        QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
    return cond();
}

static void spin(int ms) { waitFor([] { return false; }, ms); }

static QString btsyncdName;

static QDBusMessage gattCall(QDBusConnection &driver, const char *path, const char *method, const QList<QVariant> &args)
{
    QDBusMessage m = QDBusMessage::createMethodCall(btsyncdName, path, GATT_CHRC_IFACE, method);
    m.setArguments(args);
    return driver.call(m, QDBus::BlockWithGui, 5000);
}

static void writeReply(QDBusConnection &driver, const QByteArray &xml)
{
    QVariantMap options;
    options.insert("mtu", 247);
    CHECK(gattCall(driver, CHR_REPLY, "WriteValue", {xml, options}).type() == QDBusMessage::ReplyMessage);
}

// A call that waits for the phone: the pending reply, to look at when the result is written
struct Call {
    explicit Call(const QDBusPendingCall &p) : pending(p) {}
    QDBusPendingCall pending;
    bool finished() const { return pending.isFinished(); }
    QString text() const { return QDBusPendingReply<QString>(pending).value(); }
    bool ok() const { return !pending.isError(); }
};

static Call startCall(QDBusConnection &driver, const char *method, const QList<QVariant> &args)
{
    QDBusMessage m = QDBusMessage::createMethodCall(SERVICE_NAME, BTSYNCD_MESSAGES_PATH, MESSAGES, method);
    m.setArguments(args);
    return Call(driver.asyncCall(m, 60000));
}

// The id and the JSON of the last act on the wire (and the wire cleared)
static bool takeAct(Collector &collector, uint *id, QJsonObject *act)
{
    if (!waitFor([&] { return collector.requests.endsWith("</act>"); }))
        return false;
    QXmlStreamReader x(collector.requests);
    QString js;
    while (!x.atEnd()) {
        x.readNext();
        if (x.isStartElement() && x.name() == QLatin1String("id"))
            *id = x.readElementText().toUInt();
        else if (x.isStartElement() && x.name() == QLatin1String("js"))
            js = x.readElementText();
    }
    collector.requests.clear();
    *act = QJsonDocument::fromJson(js.toUtf8()).object();
    return !act->isEmpty();
}

// The JSON a fixture frame holds (an <act> from the watch or an <acr> from the phone)
static QJsonObject fixtureJson(const QByteArray &frame)
{
    QXmlStreamReader x(frame);
    while (!x.atEnd()) {
        x.readNext();
        if (x.isStartElement() && x.name() == QLatin1String("js"))
            return QJsonDocument::fromJson(x.readElementText().toUtf8()).object();
    }
    return QJsonObject();
}

static QByteArray withId(QByteArray frame, uint from, uint to)
{
    return frame.replace("<id>" + QByteArray::number(from) + "</id>", "<id>" + QByteArray::number(to) + "</id>");
}

int main(int argc, char **argv)
{
    QTemporaryDir names;
    qputenv("BTSYNCD_STT_CONTEXT", (names.path() + QStringLiteral("/stt-context.txt")).toUtf8());
    QCoreApplication app(argc, argv);
    if (qEnvironmentVariableIsEmpty("DBUS_SESSION_BUS_ADDRESS") || qEnvironmentVariableIsEmpty("DBUS_SYSTEM_BUS_ADDRESS")) {
        fprintf(stderr, "needs a private bus as session and system bus (see the top of the source)\n");
        return 2;
    }
    qInstallMessageHandler(logHandler);

    // the request builders alone
    CHECK(MessageService::validKey("t9") && MessageService::validKey("n00000000deadbeef") && MessageService::validKey("a_b-c"));
    CHECK(!MessageService::validKey("") && !MessageService::validKey("t9;x") && !MessageService::validKey("a b")
          && !MessageService::validKey(QString(65, QLatin1Char('a'))) && MessageService::validKey(QString(64, QLatin1Char('a'))));
    CHECK(QJsonDocument::fromJson(MessageService::convsRequest(0, 500)).object().value("n").toInt() == 30);
    CHECK(QJsonDocument::fromJson(MessageService::convsRequest(0, 0)).object().value("n").toInt() == 1);
    CHECK(QJsonDocument::fromJson(MessageService::convsRequest(15, 15)).object().value("off").toInt() == 15);
    CHECK(QJsonDocument::fromJson(MessageService::msgsRequest("t9", -5, 500)).object().value("before").toInt() == 0);
    CHECK(QJsonDocument::fromJson(MessageService::msgsRequest("t9", 1793200000, 500)).object().value("n").toInt() == 50);
    CHECK(MessageService::msgsRequest("t9 x", 0, 20).isEmpty());
    CHECK(MessageService::msendRequest("t9", "   ", false).isEmpty());
    CHECK(MessageService::msendRequest("t9", QString(MessageService::MaxTextBytes, QLatin1Char('x')), false).size() > 0);
    CHECK(MessageService::msendRequest("t9", QString(MessageService::MaxTextBytes + 1, QLatin1Char('x')), false).isEmpty());
    CHECK(MessageService::mreadRequest("bad key").isEmpty());

    NotificationService notifications(0, QDBusConnection::systemBus());
    AssistantService assistant(10, QDBusConnection::systemBus(), &notifications);
    MessageService messages(&assistant);
    btsyncdName = QDBusConnection::systemBus().baseService();

    QDBusConnection driver = QDBusConnection::connectToBus(QDBusConnection::SessionBus, "driver");
    Collector collector;
    CHECK(driver.connect(btsyncdName, CHR_REQUEST, "org.freedesktop.DBus.Properties", "PropertiesChanged",
                         &collector, SLOT(propertiesChanged(QString, QVariantMap, QStringList))));
    CHECK(driver.connect(SERVICE_NAME, BTSYNCD_MESSAGES_PATH, MESSAGES, "Changed", &collector, SLOT(onChanged(QString))));
    CHECK(driver.connect(SERVICE_NAME, BTSYNCD_ASSISTANT_PATH, "org.asteroidos.btsyncd.Assistant", "ActResult",
                         &collector, SLOT(onActResult(uint, QString))));
    spin(200);

    // No phone: nothing is sent and the answer is immediate
    {
        Call c = startCall(driver, "Conversations", {0u, 15u});
        CHECK(waitFor([&] { return c.finished(); }));
        CHECK(c.ok() && c.text() == "{\"ok\":false,\"er\":\"disconnected\"}");
        QDBusMessage m = QDBusMessage::createMethodCall(SERVICE_NAME, BTSYNCD_MESSAGES_PATH, MESSAGES, "Available");
        CHECK(driver.call(m, QDBus::BlockWithGui, 5000).arguments().value(0).toBool() == false);
    }

    // The phone connects and says what it does (a Gadgetbridge with messages)
    notifications.setLinkConnected(true);
    CHECK(gattCall(driver, CHR_FEEDBACK, "StartNotify", {}).type() == QDBusMessage::ReplyMessage);
    CHECK(gattCall(driver, CHR_REQUEST, "StartNotify", {}).type() == QDBusMessage::ReplyMessage);
    writeReply(driver, "<asr><st>unavailable</st><er>no-url</er><ac>nav,contacts,sms,call,hangup,convs,msgs,msend,mread,rsvp,open,wifipw,cam</ac></asr>");
    {
        QDBusMessage m = QDBusMessage::createMethodCall(SERVICE_NAME, BTSYNCD_MESSAGES_PATH, MESSAGES, "Available");
        CHECK(driver.call(m, QDBus::BlockWithGui, 5000).arguments().value(0).toBool() == true);
    }

    const QList<QByteArray> requests = fixtureFrames("messages-requests.frames.txt");
    const QList<QByteArray> results = fixtureFrames("messages-results.frames.txt");
    CHECK(requests.size() == 4 && results.size() == 2);
    uint id = 0;
    QJsonObject act;

    // Conversations(0, 15): the fixture's request, the fixture's result
    {
        collector.requests.clear();
        Call c = startCall(driver, "Conversations", {0u, 15u});
        CHECK(takeAct(collector, &id, &act));
        CHECK(id >= AssistantService::InternalIdBase);
        CHECK(act == fixtureJson(requests.value(0)));
        CHECK(!c.finished());
        writeReply(driver, withId(results.value(0), 5, id));
        CHECK(waitFor([&] { return c.finished(); }));
        CHECK(c.ok() && QJsonDocument::fromJson(c.text().toUtf8()).object() == fixtureJson(results.value(0)));
        const QJsonObject r = QJsonDocument::fromJson(c.text().toUtf8()).object();
        CHECK(r.value("ok").toBool() && r.value("c").toArray().size() == 2 && !r.value("more").toBool());
    }
    // Messages("t9", 0, 20)
    {
        Call c = startCall(driver, "Messages", {QStringLiteral("t9"), qlonglong(0), 20u});
        CHECK(takeAct(collector, &id, &act));
        CHECK(act == fixtureJson(requests.value(1)));
        writeReply(driver, withId(results.value(1), 6, id));
        CHECK(waitFor([&] { return c.finished(); }));
        const QJsonObject r = QJsonDocument::fromJson(c.text().toUtf8()).object();
        CHECK(c.ok() && r == fixtureJson(results.value(1)) && r.value("m").toArray().size() == 3);
    }
    // Send: the real one, then the dry one
    {
        Call c = startCall(driver, "Send", {QStringLiteral("t9"), QStringLiteral("On my way")});
        CHECK(takeAct(collector, &id, &act));
        CHECK(act == fixtureJson(requests.value(2)));
        writeReply(driver, "<acr><id>" + QByteArray::number(id) + "</id><js>{&quot;ok&quot;:true,&quot;pending&quot;:true}</js></acr>");
        CHECK(waitFor([&] { return c.finished(); }));
        CHECK(c.ok() && c.text() == "{\"ok\":true,\"pending\":true}");
        Call d = startCall(driver, "SendDry", {QStringLiteral("t9"), QStringLiteral("On my way")});
        CHECK(takeAct(collector, &id, &act));
        CHECK(act.value("dry").toBool() && act.value("tx").toString() == "On my way" && act.value("k").toString() == "t9");
        writeReply(driver, "<acr><id>" + QByteArray::number(id) + "</id><js>{&quot;ok&quot;:true,&quot;dry&quot;:true}</js></acr>");
        CHECK(waitFor([&] { return d.finished(); }));
        CHECK(d.text() == "{\"ok\":true,\"dry\":true}");
    }
    // MarkRead, and the phone's own errors reach the caller as they are
    {
        Call c = startCall(driver, "MarkRead", {QStringLiteral("t9")});
        CHECK(takeAct(collector, &id, &act));
        CHECK(act == fixtureJson(requests.value(3)));
        writeReply(driver, "<acr><id>" + QByteArray::number(id) + "</id><js>{&quot;ok&quot;:false,&quot;er&quot;:&quot;no-permission&quot;,&quot;perm&quot;:&quot;sms&quot;}</js></acr>");
        CHECK(waitFor([&] { return c.finished(); }));
        CHECK(c.text() == "{\"ok\":false,\"er\":\"no-permission\",\"perm\":\"sms\"}");
    }
    // Arguments that are not acceptable are answered at once and nothing goes to the phone
    collector.requests.clear();
    for (const QPair<const char *, QList<QVariant>> &bad : QList<QPair<const char *, QList<QVariant>>>{
             {"Messages", {QStringLiteral("t9;x"), qlonglong(0), 20u}},
             {"Send", {QStringLiteral("t9"), QStringLiteral("  ")}},
             {"Send", {QStringLiteral("t9"), QString(MessageService::MaxTextBytes + 1, QLatin1Char('x'))}},
             {"Send", {QStringLiteral(""), QStringLiteral("hi")}},
             {"MarkRead", {QStringLiteral("a b")}}}) {
        Call c = startCall(driver, bad.first, bad.second);
        CHECK(waitFor([&] { return c.finished(); }));
        CHECK(c.ok() && c.text() == "{\"ok\":false,\"er\":\"bad-request\"}");
    }
    spin(200);
    CHECK(collector.requests.isEmpty());

    // Too many waiting at once: busy (MaxPending), the others still get their results
    {
        QList<Call> calls;
        collector.requests.clear();
        for (int i = 0; i < MessageService::MaxPending; i++)
            calls << startCall(driver, "Conversations", {uint(i), 15u});
        CHECK(waitFor([&] { return collector.requests.count("</act>") == MessageService::MaxPending; }));
        Call extra = startCall(driver, "Conversations", {99u, 15u});
        CHECK(waitFor([&] { return extra.finished(); }));
        CHECK(extra.text() == "{\"ok\":false,\"er\":\"busy\"}");
        // each is answered by its own id, in any order
        QList<uint> ids;
        // several <act> elements one after another: not one XML document, so by hand
        QByteArray rest = collector.requests;
        while (true) {
            const int a = rest.indexOf("<act><id>");
            if (a < 0)
                break;
            const int e = rest.indexOf("</id>", a);
            ids << rest.mid(a + 9, e - a - 9).toUInt();
            rest = rest.mid(e);
        }
        CHECK(ids.size() == MessageService::MaxPending);
        for (int i = ids.size() - 1; i >= 0; i--)
            writeReply(driver, "<acr><id>" + QByteArray::number(ids.at(i)) + "</id><js>{&quot;ok&quot;:true,&quot;n&quot;:" + QByteArray::number(i) + "}</js></acr>");
        for (int i = 0; i < calls.size(); i++) {
            CHECK(waitFor([&] { return calls[i].finished(); }));
            CHECK(calls[i].text() == QStringLiteral("{\"ok\":true,\"n\":%1}").arg(i));
        }
        collector.requests.clear();
    }

    // A phone that does not do messages (an older Gadgetbridge): unsupported, nothing sent
    writeReply(driver, "<asr><st>ready</st><ac>nav,contacts,sms</ac></asr>");
    {
        Call c = startCall(driver, "Conversations", {0u, 15u});
        CHECK(waitFor([&] { return c.finished(); }));
        CHECK(c.text() == "{\"ok\":false,\"er\":\"unsupported\"}");
        QDBusMessage m = QDBusMessage::createMethodCall(SERVICE_NAME, BTSYNCD_MESSAGES_PATH, MESSAGES, "Available");
        CHECK(driver.call(m, QDBus::BlockWithGui, 5000).arguments().value(0).toBool() == false);
    }
    spin(100);
    CHECK(collector.requests.isEmpty());

    // The phone goes while a call waits: "disconnected" at once
    writeReply(driver, "<asr><st>ready</st><ac>convs,msgs,msend,mread</ac></asr>");
    {
        Call c = startCall(driver, "Messages", {QStringLiteral("t9"), qlonglong(0), 20u});
        CHECK(takeAct(collector, &id, &act));
        notifications.setLinkConnected(false);
        CHECK(waitFor([&] { return c.finished(); }));
        CHECK(c.text() == "{\"ok\":false,\"er\":\"disconnected\"}");
    }

    // Changed(key): for whoever sees a message come in
    messages.changed(QStringLiteral("t9"));
    CHECK(waitFor([&] { return collector.changed == QStringList({"t9"}); }));

    // None of these is a session bus ActResult (the launcher's actions only)
    CHECK(collector.results.isEmpty());

    // The log has kinds and sizes: never a key, a text, a name
    const QString log = logLines.join('\n');
    CHECK(log.contains("Assistant: action"));
    CHECK(!log.contains("On my way") && !log.contains("t9") && !log.contains("Sam") && !log.contains("deadbeef"));

    if (failures)
        fprintf(stderr, "%d check(s) failed\n", failures);
    else
        fprintf(stderr, "messages_test: all checks passed\n");
    return failures ? 1 : 0;
}

#include "messages_test.moc"
