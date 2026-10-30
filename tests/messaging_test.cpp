/*
 * Tests of the M5 messaging polish of the notification service on a private D-Bus with a
 * dconf-service (cmake -DBTSYNCD_TESTS=ON; dev-btsyncd.sh runs it as weather_test):
 *   dbus-run-session -- sh -c 'DBUS_SYSTEM_BUS_ADDRESS=$DBUS_SESSION_BUS_ADDRESS messaging_test'
 * A fake lipstick records what btsyncd posts, a fake mce says whether the charger is on.
 *   - capabilities: <me>1</me>
 *   - a conversation's own messages (<me>1</me> in an <m>): the x-asteroid-messages hint
 *     (JSON, own ones marked, text as it came: quotes, <, &, emoji), the body as before;
 *     an older phone (no <me>): none marked; no messages: no hint
 *   - quiet on the charger (Wear OS mutes alerting while power is connected): mce's
 *     charger at the start and its signals; no banner, no vibration, the card still
 *     posted; the find-my-watch alert still rings; a call: its card, no vibration;
 *     unplugged: alerting again; dconf quiet-on-charger false: alerting on the charger;
 *     QuietOnCharger on the session bus API
 *   - SetAppNotifications: mute / unmute feedback for the card's phone id, the card stays;
 *     refused (nothing sent) without a phone, for a card that does not offer mute (a call,
 *     the find-my-watch alert), for an unknown card
 * All names and texts are made up.
 */

#include "notificationservice.h"
#include "common.h"

#include <MDConfItem>
#include <QCoreApplication>
#include <QDBusConnection>
#include <QDBusMessage>
#include <QElapsedTimer>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMutex>
#include <QMutexLocker>
#include <QThread>
#include <QVariantMap>
#include <cstdio>
#include <functional>

static int failures = 0;

#define CHECK(cond) do { if (!(cond)) { fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); failures++; } } while (0)

static const char *CHR_UPDATE = "/org/asteroidos/btsyncd/service0/char0";
static const char *CHR_FEEDBACK = "/org/asteroidos/btsyncd/service0/char1";
static const char *CHR_CAPS = "/org/asteroidos/btsyncd/service0/char3";

struct Posted {
    QString appName, summary, body;
    uint replacesId = 0;
    uint id = 0;        // the lipstick id it got
    QStringList actions;
    QVariantMap hints;
};

class FakeLipstick : public QObject
{
    Q_OBJECT
    Q_CLASSINFO("D-Bus Interface", "org.freedesktop.Notifications")
public:
    QMutex lock;
    QList<Posted> posted;
    QList<uint> closed;
    uint nextId = 300;

public slots:
    uint Notify(const QString &appName, uint replacesId, const QString &, const QString &summary,
                const QString &body, const QStringList &actions, const QVariantMap &hints, int)
    {
        QMutexLocker l(&lock);
        Posted p;
        p.appName = appName;
        p.replacesId = replacesId;
        p.summary = summary;
        p.body = body;
        p.actions = actions;
        p.hints = hints;
        p.id = replacesId ? replacesId : nextId++;
        posted << p;
        return p.id;
    }
    void CloseNotification(uint id)
    {
        QMutexLocker l(&lock);
        closed << id;
    }
};

// mce's request interface, as BatteryStatus asks it
class FakeMce : public QObject
{
    Q_OBJECT
    Q_CLASSINFO("D-Bus Interface", "com.nokia.mce.request")
public:
    QMutex lock;
    QString charger = "off";

public slots:
    int get_battery_level() { return 64; }
    QString get_charger_state() { QMutexLocker l(&lock); return charger; }
    QString get_battery_state() { return "discharging"; }
    QString get_battery_status() { return "ok"; }
};

class Collector : public QObject
{
    Q_OBJECT
public:
    QByteArray feedback;
public slots:
    void propertiesChanged(const QString &, const QVariantMap &changed, const QStringList &)
    {
        if(changed.contains("Value"))
            feedback += changed.value("Value").toByteArray();
    }
};

static void spin(int ms)
{
    QElapsedTimer t;
    t.start();
    while(t.elapsed() < ms)
        QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
}

static bool waitFor(const std::function<bool()> &cond, int ms = 3000)
{
    QElapsedTimer t;
    t.start();
    while(!cond() && t.elapsed() < ms)
        QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
    return cond();
}

static QString btsyncdName;

static QDBusMessage gattCall(QDBusConnection &driver, const char *path, const char *method, const QList<QVariant> &args)
{
    QDBusMessage m = QDBusMessage::createMethodCall(btsyncdName, path, GATT_CHRC_IFACE, method);
    m.setArguments(args);
    return driver.call(m, QDBus::BlockWithGui, 5000);
}

static void write(QDBusConnection &driver, const QByteArray &value)
{
    QVariantMap options;
    options.insert("mtu", 185);
    CHECK(gattCall(driver, CHR_UPDATE, "WriteValue", {value, options}).type() == QDBusMessage::ReplyMessage);
}

static QVariant api(QDBusConnection &driver, const char *method, const QList<QVariant> &args = {})
{
    QDBusMessage m = QDBusMessage::createMethodCall(SERVICE_NAME, BTSYNCD_NOTIF_PATH, BTSYNCD_NOTIF_IFACE, method);
    m.setArguments(args);
    const QDBusMessage r = driver.call(m, QDBus::BlockWithGui, 5000);
    CHECK(r.type() == QDBusMessage::ReplyMessage);
    return r.arguments().value(0);
}

static int postedCount(FakeLipstick &fake)
{
    QMutexLocker l(&fake.lock);
    return fake.posted.size();
}

static Posted lastPosted(FakeLipstick &fake)
{
    QMutexLocker l(&fake.lock);
    return fake.posted.isEmpty() ? Posted() : fake.posted.last();
}

// Posts it and waits for the card
static Posted post(QDBusConnection &driver, FakeLipstick &fake, const QByteArray &insert)
{
    const int before = postedCount(fake);
    write(driver, insert);
    CHECK(waitFor([&] { return postedCount(fake) > before; }));
    return lastPosted(fake);
}

static bool alerts(const Posted &p)
{
    return p.hints.contains("x-nemo-preview-summary") && p.hints.contains("x-nemo-preview-body")
        && p.hints.value("x-nemo-feedback").toString() != "notif_silent"
        && !p.hints.value("x-nemo-feedback-suppressed").toBool();
}

static bool quiet(const Posted &p)
{
    return !p.hints.contains("x-nemo-preview-summary") && !p.hints.contains("x-nemo-preview-body")
        && p.hints.value("x-nemo-feedback").toString() == "notif_silent"
        && p.hints.value("x-nemo-feedback-suppressed").toBool();
}

static QDBusConnection *mceConn;

static void chargerSignal(FakeMce &mce, const QString &state)
{
    {
        QMutexLocker l(&mce.lock);
        mce.charger = state;
    }
    QDBusMessage m = QDBusMessage::createSignal("/com/nokia/mce/signal", "com.nokia.mce.signal", "charger_state_ind");
    m << state;
    CHECK(mceConn->send(m));
}

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    if(qEnvironmentVariableIsEmpty("DBUS_SESSION_BUS_ADDRESS") || qEnvironmentVariableIsEmpty("DBUS_SYSTEM_BUS_ADDRESS")) {
        fprintf(stderr, "needs a private bus as session and system bus (see the top of the source)\n");
        return 2;
    }
    MDConfItem(NotificationService::QuietOnChargerKey).unset();

    QThread fakeThread;
    fakeThread.start();
    FakeLipstick fake;
    fake.moveToThread(&fakeThread);
    QDBusConnection fakeConn = QDBusConnection::connectToBus(QDBusConnection::SessionBus, "fake-lipstick");
    CHECK(fakeConn.registerObject("/org/freedesktop/Notifications", &fake, QDBusConnection::ExportAllSlots));
    CHECK(fakeConn.registerService("org.freedesktop.Notifications"));

    // mce, before btsyncd asks it: the charger is off
    FakeMce mce;
    mce.moveToThread(&fakeThread);
    QDBusConnection mceBus = QDBusConnection::connectToBus(QDBusConnection::SystemBus, "fake-mce");
    mceConn = &mceBus;
    CHECK(mceBus.registerObject("/com/nokia/mce/request", &mce, QDBusConnection::ExportAllSlots));
    CHECK(mceBus.registerService("com.nokia.mce"));

    NotificationService service(0, QDBusConnection::systemBus());
    btsyncdName = QDBusConnection::systemBus().baseService();

    QDBusConnection driver = QDBusConnection::connectToBus(QDBusConnection::SessionBus, "driver");
    Collector collector;
    CHECK(driver.connect(btsyncdName, CHR_FEEDBACK, "org.freedesktop.DBus.Properties", "PropertiesChanged",
                         &collector, SLOT(propertiesChanged(QString, QVariantMap, QStringList))));
    spin(300);

    // Capabilities: the user's own messages
    {
        const QDBusMessage r = gattCall(driver, CHR_CAPS, "ReadValue", {QVariantMap()});
        CHECK(r.type() == QDBusMessage::ReplyMessage);
        const QByteArray caps = r.arguments().value(0).toByteArray();
        CHECK(caps.contains("<me>1</me>"));
        CHECK(caps.contains("<version>4</version>"));
        CHECK(caps.endsWith("</capabilities>"));
    }

    // A conversation with the user's own messages
    {
        const Posted p = post(driver, fake,
            "<insert><id>501</id><vb>normal</vb><pn>org.example.chat</pn><an>Example Chat</an>"
            "<su>Alex Example</su><ac>reply,open,mute</ac><th>00000000000000a1</th>"
            "<ms><m><s>Alex Example</s><t>Are you coming? \"8pm\" &lt;rooftop&gt; &amp; snacks</t></m>"
            "<m><s>You</s><t>On my way</t><me>1</me></m>"
            "<m><s>Alex Example</s><t>\xf0\x9f\x91\x8d great</t></m></ms></insert>");
        const QJsonArray list = QJsonDocument::fromJson(p.hints.value("x-asteroid-messages").toString().toUtf8()).array();
        CHECK(list.size() == 3);
        CHECK(list.at(0).toObject().value("s").toString() == "Alex Example");
        CHECK(list.at(0).toObject().value("t").toString() == "Are you coming? \"8pm\" <rooftop> & snacks");
        CHECK(!list.at(0).toObject().contains("me"));
        CHECK(list.at(1).toObject().value("t").toString() == "On my way");
        CHECK(list.at(1).toObject().value("me").toBool() == true);
        CHECK(list.at(2).toObject().value("t").toString() == QString::fromUtf8("\xf0\x9f\x91\x8d great"));
        CHECK(!list.at(2).toObject().contains("me"));
        // the body as before (older launchers, the stream's card)
        CHECK(p.body == QString::fromUtf8("Alex Example: Are you coming? \"8pm\" <rooftop> & snacks\nYou: On my way\nAlex Example: \xf0\x9f\x91\x8d great"));
        CHECK(alerts(p));
        fprintf(stderr, "messages hint: %s\n", qPrintable(p.hints.value("x-asteroid-messages").toString()));
    }

    // An older phone (no <me>): no message marked; a <me> that is not 1 is not one
    {
        const Posted p = post(driver, fake,
            "<insert><id>502</id><vb>normal</vb><an>Example Chat</an><su>Kim Example</su><ac>reply,open,mute</ac>"
            "<th>00000000000000a2</th><ms><m><s>Kim Example</s><t>Hello</t></m><m><s>You</s><t>Hi</t></m>"
            "<m><s>You</s><t>Again</t><me>0</me></m></ms></insert>");
        const QJsonArray list = QJsonDocument::fromJson(p.hints.value("x-asteroid-messages").toString().toUtf8()).array();
        CHECK(list.size() == 3);
        for(const QJsonValue &m : list)
            CHECK(!m.toObject().contains("me"));
    }

    // No messages: no hint
    {
        const Posted p = post(driver, fake,
            "<insert><id>503</id><vb>normal</vb><an>Example Mail</an><su>Weekly report</su><bo>Ready</bo><ac>open,mute</ac></insert>");
        CHECK(!p.hints.contains("x-asteroid-messages"));
    }

    // More than six messages: the newest six, their marks kept with them
    {
        QByteArray ms;
        for(int i = 0; i < 8; i++)
            ms += "<m><s>" + QByteArray(i % 2 ? "You" : "Alex Example") + "</s><t>m" + QByteArray::number(i) + "</t>"
                + (i % 2 ? "<me>1</me>" : "") + "</m>";
        const Posted p = post(driver, fake,
            "<insert><id>504</id><vb>normal</vb><an>Example Chat</an><su>Alex Example</su><ac>reply,mute</ac>"
            "<th>00000000000000a4</th><ms>" + ms + "</ms></insert>");
        const QJsonArray list = QJsonDocument::fromJson(p.hints.value("x-asteroid-messages").toString().toUtf8()).array();
        CHECK(list.size() == 6);
        CHECK(list.at(0).toObject().value("t").toString() == "m2");
        CHECK(!list.at(0).toObject().contains("me"));
        CHECK(list.at(1).toObject().value("t").toString() == "m3");
        CHECK(list.at(1).toObject().value("me").toBool());
        CHECK(list.at(5).toObject().value("t").toString() == "m7");
        CHECK(list.at(5).toObject().value("me").toBool());
    }

    // Quiet on the charger. Off at the start (mce's answer): alerting
    CHECK(waitFor([&] { return !api(driver, "QuietOnCharger").toBool(); }));
    CHECK(alerts(post(driver, fake, "<insert><id>601</id><vb>normal</vb><an>Example Chat</an><su>Alex Example</su><bo>One</bo><ac>reply,open,mute</ac></insert>")));

    // Plugged in: quiet, the card still comes
    chargerSignal(mce, "on");
    CHECK(waitFor([&] { return api(driver, "QuietOnCharger").toBool(); }));
    {
        const Posted p = post(driver, fake, "<insert><id>602</id><vb>strong</vb><an>Example Chat</an><su>Alex Example</su><bo>Two</bo><ac>reply,open,mute</ac></insert>");
        CHECK(quiet(p));
        CHECK(p.summary == "Alex Example" && p.body == "Two");
        CHECK(p.replacesId == 0);
    }
    // ... also an alarm on the phone and important ones
    CHECK(quiet(post(driver, fake, "<insert><id>603</id><vb>alarm</vb><an>Clock</an><su>Alarm</su><bo>07:00</bo><ac>open</ac><pr>5</pr><ca>alarm</ca></insert>")));
    // A ringing call: its card (the launcher's call screen), no vibration
    {
        const Posted p = post(driver, fake, "<insert><id>604</id><vb>ringtone</vb><an>Incoming call</an><su>Kim Example</su><bo>Example Talk call</bo><ac>answer,reject</ac></insert>");
        CHECK(quiet(p));
        CHECK(p.actions.contains("answer"));
    }
    // The find-my-watch alert still rings: the user asked for it
    {
        const Posted p = post(driver, fake, "<insert><id>-77</id><vb>ringtone</vb><an>Find my watch</an><su>Gadgetbridge</su><ac>found</ac></insert>");
        CHECK(alerts(p));
        CHECK(p.hints.value("x-nemo-feedback").toString() == "ringtone");
    }
    // A conversation's new message on the charger: quiet, updated in its card
    {
        const Posted first = post(driver, fake, "<insert><id>605</id><vb>normal</vb><an>Example Chat</an><su>Robin Example</su><ac>reply,mute</ac>"
                                                "<th>00000000000000b5</th><ms><m><s>Robin Example</s><t>First</t></m></ms></insert>");
        CHECK(quiet(first));
        const Posted next = post(driver, fake, "<insert><id>606</id><vb>normal</vb><an>Example Chat</an><su>Robin Example</su><ac>reply,mute</ac>"
                                               "<th>00000000000000b5</th><ms><m><s>Robin Example</s><t>First</t></m><m><s>Robin Example</s><t>Second</t></m></ms></insert>");
        CHECK(quiet(next));
        CHECK(next.body == "First\nSecond");
        // not the old timestamp: a new message (lipstick stamps it now)
        CHECK(!next.hints.contains("x-nemo-timestamp"));
    }

    // Turned off in dconf: alerting on the charger
    MDConfItem(NotificationService::QuietOnChargerKey).set(false);
    CHECK(waitFor([&] { return MDConfItem(NotificationService::QuietOnChargerKey).value(true).toBool() == false; }));
    CHECK(!api(driver, "QuietOnCharger").toBool());
    CHECK(alerts(post(driver, fake, "<insert><id>607</id><vb>normal</vb><an>Example Chat</an><su>Alex Example</su><bo>Three</bo><ac>reply,mute</ac></insert>")));
    MDConfItem(NotificationService::QuietOnChargerKey).unset();
    CHECK(waitFor([&] { return api(driver, "QuietOnCharger").toBool(); }));
    CHECK(quiet(post(driver, fake, "<insert><id>608</id><vb>normal</vb><an>Example Chat</an><su>Alex Example</su><bo>Four</bo><ac>reply,mute</ac></insert>")));

    // Unplugged: alerting again
    chargerSignal(mce, "off");
    CHECK(waitFor([&] { return !api(driver, "QuietOnCharger").toBool(); }));
    CHECK(alerts(post(driver, fake, "<insert><id>609</id><vb>normal</vb><an>Example Chat</an><su>Alex Example</su><bo>Five</bo><ac>reply,mute</ac></insert>")));
    // mce's "unknown" is not the charger
    chargerSignal(mce, "unknown");
    spin(300);
    CHECK(!api(driver, "QuietOnCharger").toBool());

    // The app's notifications off and on from the watch. The card of phone id 609:
    const uint card609 = lastPosted(fake).id;
    // no phone subscribed yet: refused, nothing sent
    collector.feedback.clear();
    CHECK(api(driver, "SetAppNotifications", {card609, false}).toBool() == false);
    spin(200);
    CHECK(collector.feedback.isEmpty());

    CHECK(gattCall(driver, CHR_FEEDBACK, "StartNotify", {}).type() == QDBusMessage::ReplyMessage);
    spin(200);
    CHECK(api(driver, "SetAppNotifications", {card609, false}).toBool() == true);
    CHECK(waitFor([&] { return collector.feedback.endsWith("</action>"); }));
    CHECK(collector.feedback == "<action><id>609</id><ty>mute</ty></action>");
    collector.feedback.clear();
    CHECK(api(driver, "SetAppNotifications", {card609, true}).toBool() == true);
    CHECK(waitFor([&] { return collector.feedback.endsWith("</action>"); }));
    CHECK(collector.feedback == "<action><id>609</id><ty>unmute</ty></action>");
    // the card stays (not closed, not dismissed)
    {
        QMutexLocker l(&fake.lock);
        CHECK(!fake.closed.contains(card609));
    }
    // a call, the find-my-watch alert and an unknown card do not offer it
    collector.feedback.clear();
    uint callCard = 0, findCard = 0;
    {
        QMutexLocker l(&fake.lock);
        for(const Posted &p : fake.posted) {
            if(p.hints.value("x-asteroid-phone-id").toString() == "604") callCard = p.id;
            if(p.hints.value("x-asteroid-phone-id").toString() == "-77") findCard = p.id;
        }
    }
    CHECK(callCard && findCard);
    CHECK(api(driver, "SetAppNotifications", {callCard, false}).toBool() == false);
    CHECK(api(driver, "SetAppNotifications", {findCard, false}).toBool() == false);
    CHECK(api(driver, "SetAppNotifications", {99999u, false}).toBool() == false);
    spin(200);
    CHECK(collector.feedback.isEmpty());

    MDConfItem(NotificationService::QuietOnChargerKey).unset();
    fakeThread.quit();
    fakeThread.wait();
    if(failures)
        fprintf(stderr, "%d failure(s)\n", failures);
    else
        fprintf(stderr, "all passed\n");
    return failures ? 1 : 0;
}

#include "messaging_test.moc"
