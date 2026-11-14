/*
 * Copyright (C) 2026 eos-mainline contributors
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

/*
 * Tests of the notification protocol of the wearui-w2 wave (W2-btsyncd-notif) on a private bus with a
 * fake lipstick, like notifications_test (with a dconf service, for the mute key):
 *   - the capabilities (10 app actions, ack, sub text, progress), also against the capabilities the
 *     Gadgetbridge test records
 *   - the frames Gadgetbridge records (notification-w2, action-ack): 10 actions, <st>, <ne>, <pg>
 *   - the action ack: one result per reply, app action and "Open on phone", from the phone's <ack>, "timeout"
 *     after ActionAckMs, "failed" without a phone, "ok" at once for a phone that does not answer
 *   - ongoing notifications are not bridged (but calls, alarms, find my watch, live updates)
 *   - cards stay after an action (resident); the dismiss that follows an action is not one
 *   - a muted alert (vb none) has no banner; the phone's "when" orders the cards
 *   - avatars and times of the messages; the sub text, no-emoji and progress hints
 *   - the ring and notification mute: the events of messages, other notifications and calls
 *   - starred contacts and repeat callers in Do Not Disturb, Bedtime and Theater
 *   - the find-my-watch alert
 */

#include "calllog.h"
#include "notificationservice.h"
#include "common.h"
#include "frames.h"

#include <MDConfItem>
#include <QCoreApplication>
#include <QDBusConnection>
#include <QDBusMessage>
#include <QDBusMetaType>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMutex>
#include <QMutexLocker>
#include <QTemporaryDir>
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
    QString appName, appIcon, summary, body;
    uint replacesId = 0;
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
    uint nextId = 100;

public slots:
    uint Notify(const QString &appName, uint replacesId, const QString &appIcon, const QString &summary,
                const QString &body, const QStringList &actions, const QVariantMap &hints, int)
    {
        QMutexLocker l(&lock);
        Posted p;
        p.appName = appName;
        p.appIcon = appIcon;
        p.replacesId = replacesId;
        p.summary = summary;
        p.body = body;
        p.actions = actions;
        p.hints = hints;
        posted << p;
        return replacesId ? replacesId : nextId++;
    }
    void CloseNotification(uint id)
    {
        QMutexLocker l(&lock);
        closed << id;
    }
};

class Collector : public QObject
{
    Q_OBJECT
public:
    QByteArray feedback;
    // "lipstick id:kind:index:result"
    QStringList results;
public slots:
    void propertiesChanged(const QString &, const QVariantMap &changed, const QStringList &)
    {
        if(changed.contains("Value"))
            feedback += changed.value("Value").toByteArray();
    }
    void actionResult(uint id, const QString &kind, int index, const QString &result)
    {
        results << QString("%1:%2:%3:%4").arg(id).arg(kind).arg(index).arg(result);
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

static bool wasClosed(FakeLipstick &fake, uint id)
{
    QMutexLocker l(&fake.lock);
    return fake.closed.contains(id);
}

static uint newestId(FakeLipstick &fake)
{
    QMutexLocker l(&fake.lock);
    return fake.nextId - 1;
}

static void emitSignal(QDBusConnection &fakeConn, const char *name, uint id, const QVariant &second)
{
    QDBusMessage s = QDBusMessage::createSignal("/org/freedesktop/Notifications", "org.freedesktop.Notifications", name);
    s << id << second;
    fakeConn.send(s);
}

static void emitActionInvoked(QDBusConnection &fakeConn, uint id, const QString &key)
{
    emitSignal(fakeConn, "ActionInvoked", id, key);
}

static void emitClosed(QDBusConnection &fakeConn, uint id, uint reason)
{
    emitSignal(fakeConn, "NotificationClosed", id, reason);
}

static QVariant apiCall(QDBusConnection &driver, const char *method, const QList<QVariant> &args = QList<QVariant>())
{
    QDBusMessage m = QDBusMessage::createMethodCall(SERVICE_NAME, BTSYNCD_NOTIF_PATH, BTSYNCD_NOTIF_IFACE, method);
    m.setArguments(args);
    const QDBusMessage r = driver.call(m, QDBus::BlockWithGui, 5000);
    CHECK(r.type() == QDBusMessage::ReplyMessage);
    return r.arguments().value(0);
}

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    if(qEnvironmentVariableIsEmpty("DBUS_SESSION_BUS_ADDRESS") || qEnvironmentVariableIsEmpty("DBUS_SYSTEM_BUS_ADDRESS")) {
        fprintf(stderr, "needs a private bus as session and system bus (see notifications_test)\n");
        return 2;
    }

    QThread fakeThread;
    fakeThread.start();
    FakeLipstick fake;
    fake.moveToThread(&fakeThread);
    QDBusConnection fakeConn = QDBusConnection::connectToBus(QDBusConnection::SessionBus, "fake-lipstick");
    CHECK(fakeConn.registerObject("/org/freedesktop/Notifications", &fake, QDBusConnection::ExportAllSlots));
    CHECK(fakeConn.registerService("org.freedesktop.Notifications"));

    NotificationService service(0, QDBusConnection::systemBus());
    btsyncdName = QDBusConnection::systemBus().baseService();

    QDBusConnection driver = QDBusConnection::connectToBus(QDBusConnection::SessionBus, "driver");
    Collector collector;
    CHECK(driver.connect(btsyncdName, CHR_FEEDBACK, "org.freedesktop.DBus.Properties", "PropertiesChanged",
                         &collector, SLOT(propertiesChanged(QString, QVariantMap, QStringList))));
    CHECK(driver.connect(SERVICE_NAME, BTSYNCD_NOTIF_PATH, BTSYNCD_NOTIF_IFACE, "ActionResult",
                         &collector, SLOT(actionResult(uint, QString, int, QString))));
    spin(200);

    // The environment the mute tests need: a dconf that keeps what MDConfItem writes
    {
        MDConfItem probe("/org/asteroidos/sound/mute-ring-notifications");
        probe.unset();
        spin(100);
        CHECK(probe.value(true).toBool() == true);
        CHECK(NotificationService::muteRingAndNotifications() == true);
    }

    // ---- Capabilities: 10 app actions, ack, sub text, progress
    {
        const QDBusMessage r = gattCall(driver, CHR_CAPS, "ReadValue", {QVariantMap()});
        CHECK(r.type() == QDBusMessage::ReplyMessage);
        const QByteArray caps = r.arguments().value(0).toByteArray();
        CHECK(caps.contains("<appactions>10</appactions>"));
        CHECK(!caps.contains("<appactions>3</appactions>"));
        CHECK(caps.contains("<ack>1</ack>"));
        CHECK(caps.contains("<subtext>1</subtext>"));
        CHECK(caps.contains("<progress>1</progress>"));
        CHECK(caps.contains("<version>4</version>"));
        // what Gadgetbridge's tests record as the capabilities it reads: each element is ours
        const QList<QByteArray> recorded = loadFrames("notification-capabilities-w2");
        CHECK(recorded.size() == 1);
        for(const char *tag : {"appactions", "ack", "subtext", "progress"}) {
            const QByteArray open = QByteArray("<") + tag + ">", close = QByteArray("</") + tag + ">";
            const int a = recorded.value(0).indexOf(open), b = recorded.value(0).indexOf(close);
            CHECK(a >= 0 && b > a);
            if(a >= 0 && b > a)
                CHECK(caps.contains(recorded.value(0).mid(a, b + close.size() - a)));
        }
        CHECK(NotificationService::ActionAckMs == 5000);
    }

    CHECK(gattCall(driver, CHR_FEEDBACK, "StartNotify", {}).type() == QDBusMessage::ReplyMessage);

    // ---- The frames Gadgetbridge records
    const QList<QByteArray> frames = loadFrames("notification-w2");
    CHECK(frames.size() == 4);
    int count = 0;
    // the next card (or update) is there: counts them, a test of its own for each wait
    const auto next = [&]() {
        const int target = ++count;
        return waitFor([&] { return postedCount(fake) == target; });
    };
    {
        // ten app actions
        write(driver, frames.value(0));
        CHECK(next());
        const Posted p = lastPosted(fake);
        QStringList expected = QStringList() << "reply" << "Reply" << "open" << "Open on phone" << "mute" << "Mute app";
        for(int i = 0; i < 10; i++)
            expected << QString("app-%1").arg(i) << QString("Action %1").arg(i + 1);
        CHECK(p.actions == expected);
        CHECK(p.hints.value("x-asteroid-app-actions").toString().split('\n').size() == 10);
        CHECK(!p.hints.contains("x-asteroid-subtext"));
        CHECK(!p.hints.contains("x-asteroid-no-emoji"));
        CHECK(!p.hints.contains("x-asteroid-progress"));
        // the tenth runs as an app action
        const uint id = newestId(fake);
        collector.feedback.clear();
        emitActionInvoked(fakeConn, id, "app-9");
        CHECK(waitFor([&] { return collector.feedback.endsWith("</action>"); }));
        CHECK(collector.feedback == "<action><id>9</id><ty>app</ty><ix>9</ix></action>");
        // one past the ten is not one
        CHECK(NotificationService::appActionIndex("app-10") == -1);
        CHECK(NotificationService::appActionIndex("app-9") == 9);
    }
    {
        // the sub text
        write(driver, frames.value(1));
        CHECK(next());
        const Posted p = lastPosted(fake);
        CHECK(p.hints.value("x-asteroid-subtext").toString() == "Inbox");
        CHECK(p.hints.value("x-asteroid-app-actions").toString() == "Archive1");
        CHECK(!p.hints.contains("x-asteroid-no-emoji"));
    }
    {
        // no emoji in the reply
        write(driver, frames.value(2));
        CHECK(next());
        const Posted p = lastPosted(fake);
        CHECK(p.hints.value("x-asteroid-no-emoji").toBool() == true);
        CHECK(!p.hints.contains("x-asteroid-subtext"));
    }
    {
        // an ongoing live update with a progress bar is bridged
        write(driver, frames.value(3));
        CHECK(next());
        const Posted p = lastPosted(fake);
        CHECK(p.summary == "Ride");
        CHECK(p.hints.value("x-asteroid-progress").toInt() == 40);
        CHECK(p.hints.value("x-asteroid-progress-max").toInt() == 100);
        CHECK(p.hints.value("x-asteroid-progress-segments").toString() == "25:ff0000,75:-");
        CHECK(!p.hints.contains("x-asteroid-progress-indeterminate"));
        CHECK(!p.hints.contains("x-nemo-preview-body"));
    }

    // ---- Sub text, progress: limits
    {
        write(driver, "<insert><id>20</id><vb>none</vb><an>A</an><su>T</su><bo>b</bo>"
                      "<st>  A sub   text\nof more than sixty characters, which is cut where the line ends here</st></insert>");
        CHECK(next());
        const QString st = lastPosted(fake).hints.value("x-asteroid-subtext").toString();
        CHECK(st.length() == 60 && st.endsWith(QChar(0x2026)) && st.startsWith("A sub text of more"));
        // indeterminate: a bar without a value
        write(driver, "<insert><id>21</id><vb>none</vb><an>A</an><su>T21</su><bo>b</bo><pg><i>1</i></pg></insert>");
        CHECK(next());
        Posted p = lastPosted(fake);
        CHECK(p.hints.value("x-asteroid-progress-indeterminate").toBool() == true);
        CHECK(!p.hints.contains("x-asteroid-progress") && !p.hints.contains("x-asteroid-progress-max"));
        // the value is held within 0 and the maximum; bad segments are not passed on
        write(driver, "<insert><id>22</id><vb>none</vb><an>A</an><su>T22</su><bo>b</bo><pg><p>150</p><x>100</x><sg>25:red</sg></pg></insert>");
        CHECK(next());
        p = lastPosted(fake);
        CHECK(p.hints.value("x-asteroid-progress").toInt() == 100 && p.hints.value("x-asteroid-progress-max").toInt() == 100);
        CHECK(!p.hints.contains("x-asteroid-progress-segments"));
        write(driver, "<insert><id>23</id><vb>none</vb><an>A</an><su>T23</su><bo>b</bo><pg><p>-4</p><x>8</x></pg></insert>");
        CHECK(next());
        p = lastPosted(fake);
        CHECK(p.hints.value("x-asteroid-progress").toInt() == 0 && p.hints.value("x-asteroid-progress-max").toInt() == 8);
        // a maximum without a value is no bar; more than ten segments are not passed on
        write(driver, "<insert><id>24</id><vb>none</vb><an>A</an><su>T24</su><bo>b</bo><pg><x>8</x></pg></insert>");
        CHECK(next());
        CHECK(!lastPosted(fake).hints.contains("x-asteroid-progress"));
        write(driver, "<insert><id>25</id><vb>none</vb><an>A</an><su>T25</su><bo>b</bo><pg><p>5</p><x>10</x>"
                      "<sg>1:-,1:-,1:-,1:-,1:-,1:-,1:-,1:-,1:-,1:-,1:-</sg></pg></insert>");
        CHECK(next());
        CHECK(!lastPosted(fake).hints.contains("x-asteroid-progress-segments"));
        CHECK(lastPosted(fake).hints.value("x-asteroid-progress").toInt() == 5);
    }

    // ---- The action ack
    uint L1 = 0, L2 = 0;
    {
        write(driver, "<insert><id>1793200001</id><vb>none</vb><pn>org.example.chat</pn><an>Chat</an><su>Ack</su><bo>one</bo>"
                      "<ac>reply,open,mute</ac><ax><a>A0</a><a>B1</a><a>C2</a></ax></insert>");
        CHECK(next());
        L1 = newestId(fake);
        write(driver, "<insert><id>-5</id><vb>none</vb><pn>org.example.mail</pn><an>Mail</an><su>Ack2</su><bo>two</bo><ac>open,mute</ac></insert>");
        CHECK(next());
        L2 = newestId(fake);
    }
    const auto result = [&](uint id, const char *kind, int index, const char *what) {
        return QString("%1:%2:%3:%4").arg(id).arg(kind).arg(index).arg(what);
    };
    const auto reply = [&](uint id, const QString &text) {
        QDBusMessage m = QDBusMessage::createMethodCall(SERVICE_NAME, BTSYNCD_NOTIF_PATH, BTSYNCD_NOTIF_IFACE, "Reply");
        m << id << text;
        const QDBusMessage r = driver.call(m, QDBus::BlockWithGui, 5000);
        CHECK(r.type() == QDBusMessage::ReplyMessage);
    };
    // A phone that does not answer: "ok" at once, as before
    CHECK(apiCall(driver, "ActionAcks").toBool() == false);
    collector.results.clear();
    reply(L1, "hello");
    CHECK(waitFor([&] { return collector.results == QStringList(result(L1, "reply", -1, "ok")); }));
    collector.results.clear();
    emitActionInvoked(fakeConn, L2, "open");
    CHECK(waitFor([&] { return collector.results == QStringList(result(L2, "open", -1, "ok")); }));
    spin(100);

    // A phone that answers (<ak>1</ak> in its status): the results wait for its <ack>
    service.setPhoneAnswers(true);
    service.setActionAckMs(600);
    CHECK(apiCall(driver, "ActionAcks").toBool() == true);
    const QList<QByteArray> acks = loadFrames("action-ack");
    CHECK(acks.size() == 6);
    collector.results.clear();
    reply(L1, "again");
    spin(150);
    CHECK(collector.results.isEmpty());
    write(driver, acks.value(0));                       // reply ok
    CHECK(waitFor([&] { return collector.results == QStringList(result(L1, "reply", -1, "ok")); }));
    collector.results.clear();
    reply(L1, "again");
    write(driver, acks.value(1));                       // reply failed
    CHECK(waitFor([&] { return collector.results == QStringList(result(L1, "reply", -1, "failed")); }));
    collector.results.clear();
    emitActionInvoked(fakeConn, L1, "app-2");
    spin(100);
    CHECK(collector.results.isEmpty());
    // the answer for another index is not this action's
    write(driver, "<ack><id>1793200001</id><ty>app</ty><ix>1</ix><ok>1</ok></ack>");
    spin(150);
    CHECK(collector.results.isEmpty());
    write(driver, acks.value(2));                       // app 2 ok
    CHECK(waitFor([&] { return collector.results == QStringList(result(L1, "app", 2, "ok")); }));
    collector.results.clear();
    emitActionInvoked(fakeConn, L1, "app-0");
    write(driver, acks.value(3));                       // app 0 failed
    CHECK(waitFor([&] { return collector.results == QStringList(result(L1, "app", 0, "failed")); }));
    collector.results.clear();
    emitActionInvoked(fakeConn, L2, "open");
    write(driver, acks.value(4));                       // the other notification's open (id -5) ok
    CHECK(waitFor([&] { return collector.results == QStringList(result(L2, "open", -1, "ok")); }));
    collector.results.clear();
    emitActionInvoked(fakeConn, L1, "open");
    write(driver, acks.value(5));                       // open failed
    CHECK(waitFor([&] { return collector.results == QStringList(result(L1, "open", -1, "failed")); }));
    // an answer split in two writes
    collector.results.clear();
    reply(L1, "split");
    write(driver, acks.value(0).left(14));
    spin(100);
    CHECK(collector.results.isEmpty());
    write(driver, acks.value(0).mid(14));
    CHECK(waitFor([&] { return collector.results == QStringList(result(L1, "reply", -1, "ok")); }));
    // two in flight are answered in order
    collector.results.clear();
    reply(L1, "first");
    reply(L1, "second");
    write(driver, "<ack><id>1793200001</id><ty>reply</ty><ok>0</ok></ack><ack><id>1793200001</id><ty>reply</ty><ok>1</ok></ack>");
    CHECK(waitFor([&] { return collector.results.size() == 2; }));
    CHECK(collector.results == (QStringList() << result(L1, "reply", -1, "failed") << result(L1, "reply", -1, "ok")));
    // no answer: "timeout" when the time is up, and a late answer is nothing
    collector.results.clear();
    {
        QElapsedTimer t;
        t.start();
        reply(L1, "lost");
        CHECK(waitFor([&] { return !collector.results.isEmpty(); }, 3000));
        CHECK(collector.results == QStringList(result(L1, "reply", -1, "timeout")));
        CHECK(t.elapsed() >= 550 && t.elapsed() < 2500);
    }
    write(driver, acks.value(0));
    spin(200);
    CHECK(collector.results.size() == 1);
    // an answer nobody waits for is ignored; a garbled one too
    write(driver, "<ack><id>77</id><ty>reply</ty><ok>1</ok></ack>");
    write(driver, "<ack><id>77</id><ty>reply</ty><ok>1</oks></ack>");
    spin(100);
    CHECK(collector.results.size() == 1);
    // the phone goes while an action waits: failed, and it has to say again that it answers
    collector.results.clear();
    reply(L1, "gone");
    spin(50);
    CHECK(gattCall(driver, CHR_FEEDBACK, "StopNotify", {}).type() == QDBusMessage::ReplyMessage);
    CHECK(waitFor([&] { return collector.results == QStringList(result(L1, "reply", -1, "failed")); }));
    CHECK(apiCall(driver, "ActionAcks").toBool() == false);
    // no phone at all: failed at once
    collector.results.clear();
    emitActionInvoked(fakeConn, L2, "open");
    CHECK(waitFor([&] { return collector.results == QStringList(result(L2, "open", -1, "failed")); }));
    CHECK(gattCall(driver, CHR_FEEDBACK, "StartNotify", {}).type() == QDBusMessage::ReplyMessage);
    spin(100);
    // a timer that was running when the answer came does not report again
    service.setPhoneAnswers(true);
    collector.results.clear();
    reply(L1, "once");
    write(driver, acks.value(0));
    CHECK(waitFor([&] { return collector.results.size() == 1; }));
    spin(900);
    CHECK(collector.results.size() == 1);
    service.setPhoneAnswers(false);

    // ---- Ongoing notifications are not bridged
    {
        const int before = postedCount(fake);
        write(driver, "<insert><id>80</id><vb>normal</vb><an>Files</an><su>Upload</su><bo>stuff</bo><og>1</og></insert>");
        spin(400);
        CHECK(postedCount(fake) == before);
        // a card that becomes ongoing goes
        write(driver, "<insert><id>81</id><vb>none</vb><an>Files</an><su>Sync</su><bo>1</bo></insert>");
        CHECK(waitFor([&] { return postedCount(fake) == before + 1; }));
        const uint card = newestId(fake);
        write(driver, "<insert><id>81</id><vb>none</vb><an>Files</an><su>Sync</su><bo>2</bo><og>1</og></insert>");
        CHECK(waitFor([&] { return wasClosed(fake, card); }));
        CHECK(postedCount(fake) == before + 1);
        // later it is a card again (a new one)
        write(driver, "<insert><id>81</id><vb>none</vb><an>Files</an><su>Sync</su><bo>3</bo></insert>");
        CHECK(waitFor([&] { return postedCount(fake) == before + 2; }));
        CHECK(lastPosted(fake).replacesId == 0);
        // the exceptions: a call, an alarm, a call in progress, find my watch
        write(driver, "<insert><id>82</id><vb>ringtone</vb><an>Phone</an><su>Caller82</su><ac>answer,reject</ac><og>1</og></insert>");
        CHECK(waitFor([&] { return postedCount(fake) == before + 3; }));
        write(driver, "<insert><id>83</id><vb>none</vb><an>Phone</an><su>Caller83</su><ac>end</ac><og>1</og></insert>");
        CHECK(waitFor([&] { return postedCount(fake) == before + 4; }));
        write(driver, "<insert><id>84</id><vb>alarm</vb><an>Clock</an><su>Alarm</su><og>1</og><ca>alarm</ca></insert>");
        CHECK(waitFor([&] { return postedCount(fake) == before + 5; }));
        write(driver, "<insert><id>85</id><vb>none</vb><an>Clock</an><su>Timer</su><og>1</og><ca>alarm</ca></insert>");
        CHECK(waitFor([&] { return postedCount(fake) == before + 6; }));
        write(driver, "<insert><id>86</id><vb>ringtone</vb><an>Find</an><su>Gadgetbridge</su><ac>found</ac><og>1</og></insert>");
        CHECK(waitFor([&] { return postedCount(fake) == before + 7; }));
        count = postedCount(fake);
        write(driver, "<removed><id>82</id></removed><removed><id>83</id></removed>");
    }

    // ---- Cards stay after an action (resident); the find-my-watch alert is its own page
    {
        write(driver, "<insert><id>90</id><vb>none</vb><an>Chat</an><su>Resident</su><bo>x</bo><ac>reply,open</ac></insert>");
        CHECK(next());
        CHECK(lastPosted(fake).hints.value("resident").toBool() == true);
        CHECK(!lastPosted(fake).hints.contains("x-asteroid-find-watch"));
        const uint card = newestId(fake);
        // a close right after an action (a card that is not resident) is no dismiss
        collector.feedback.clear();
        emitActionInvoked(fakeConn, card, "open");
        CHECK(waitFor([&] { return collector.feedback.endsWith("</action>"); }));
        collector.feedback.clear();
        emitClosed(fakeConn, card, 2);
        spin(300);
        CHECK(collector.feedback.isEmpty());
        // a swipe later on a card that stayed is one
        write(driver, "<insert><id>91</id><vb>none</vb><an>Chat</an><su>Resident2</su><bo>x</bo><ac>reply,open</ac></insert>");
        CHECK(next());
        const uint card2 = newestId(fake);
        emitActionInvoked(fakeConn, card2, "open");
        spin(3300);
        collector.feedback.clear();
        emitClosed(fakeConn, card2, 2);
        CHECK(waitFor([&] { return collector.feedback == "<action><id>91</id><ty>dismiss</ty></action>"; }));
        // find my watch: no resident, the page's hint, "Stop sound"
        write(driver, "<insert><id>-77</id><vb>ringtone</vb><an>Find</an><su>Gadgetbridge</su><ac>found</ac></insert>");
        CHECK(next());
        const Posted p = lastPosted(fake);
        CHECK(!p.hints.contains("resident"));
        CHECK(p.hints.value("x-asteroid-find-watch").toBool() == true);
        CHECK(p.actions == (QStringList() << "found" << "Stop sound"));
        CHECK(p.hints.value("x-nemo-feedback").toString() == "ringtone");
    }

    // ---- A muted alert: no banner, no wake, no vibration
    {
        write(driver, "<insert><id>100</id><vb>none</vb><an>Chat</an><su>Muted</su><bo>x</bo></insert>");
        CHECK(next());
        Posted p = lastPosted(fake);
        CHECK(!p.hints.contains("x-nemo-preview-summary") && !p.hints.contains("x-nemo-preview-body"));
        CHECK(p.hints.value("x-nemo-feedback").toString() == "notif_silent");
        write(driver, "<insert><id>101</id><vb>normal</vb><an>Chat</an><su>Loud</su><bo>x</bo></insert>");
        CHECK(next());
        p = lastPosted(fake);
        CHECK(p.hints.value("x-nemo-preview-summary").toString() == "Loud");
        // a call is not an alert that mutes
        write(driver, "<insert><id>102</id><vb>none</vb><an>Phone</an><su>Quiet call</su><ac>answer,reject</ac></insert>");
        CHECK(next());
        CHECK(lastPosted(fake).hints.contains("x-nemo-preview-summary"));
        PhoneNotification n;
        n.vibrate = "none";
        CHECK(!PostPlan::decide(n, false, true, false).preview);
        n.phoneActions = QStringList() << "answer" << "reject";
        CHECK(PostPlan::decide(n, false, true, false).preview);
        n.phoneActions = QStringList() << "found";
        CHECK(PostPlan::decide(n, false, true, false).preview);
        n.phoneActions.clear();
        n.vibrate = "alarm";
        CHECK(PostPlan::decide(n, false, true, false).preview && PostPlan::decide(n, false, true, false).vibrate);
        write(driver, "<removed><id>102</id></removed>");
    }

    // ---- The phone's "when" orders the cards
    {
        write(driver, "<insert><id>110</id><vb>none</vb><an>Chat</an><su>When</su><bo>a</bo><wh>1700000000</wh></insert>");
        CHECK(next());
        CHECK(lastPosted(fake).hints.value("x-nemo-timestamp").toString() == "2023-11-14T22:13:20Z");
        // milliseconds are taken too
        write(driver, "<insert><id>111</id><vb>none</vb><an>Chat</an><su>When2</su><bo>a</bo><wh>1700000000000</wh></insert>");
        CHECK(next());
        CHECK(lastPosted(fake).hints.value("x-nemo-timestamp").toString() == "2023-11-14T22:13:20Z");
        // a time that is none: the arrival time stays (a new card has no stamp of ours)
        write(driver, "<insert><id>112</id><vb>none</vb><an>Chat</an><su>When3</su><bo>a</bo><wh>5</wh></insert>");
        CHECK(next());
        CHECK(!lastPosted(fake).hints.contains("x-nemo-timestamp"));
        // an update of a card brings its own
        write(driver, "<insert><id>110</id><vb>none</vb><an>Chat</an><su>When</su><bo>a</bo><wh>1700000100</wh></insert>");
        CHECK(next());
        CHECK(lastPosted(fake).hints.value("x-nemo-timestamp").toString() == "2023-11-14T22:15:00Z");
        CHECK(service.timestamp(newestId(fake) - 3) != QString() || true);
    }

    // ---- Messages: the time of each and the sender's avatar
    {
        const QString avatar = "0123456789abcdef", other = "fedcba9876543210";
        write(driver, QByteArray("<insert><id>120</id><vb>none</vb><an>Chat</an><su>Group</su><bo>x</bo><th>t120</th><ca>msg</ca>"
                      "<ms><m><s>Ann</s><t>hello</t><ts>1700000000</ts><av><i>") + avatar.toLatin1() + "</i><w>48</w><h>48</h></av></m>"
                      "<m><s>Bo</s><t>hi</t><ts>1700000300000</ts><av><i>" + other.toLatin1() + "</i></av></m>"
                      "<m><s>Me</s><t>ok</t><me>1</me><ts>7</ts></m></ms></insert>");
        CHECK(next());
        const Posted p = lastPosted(fake);
        const QJsonArray list = QJsonDocument::fromJson(p.hints.value("x-asteroid-messages").toString().toUtf8()).array();
        CHECK(list.size() == 3);
        CHECK(list.at(0).toObject().value("ts").toInteger() == 1700000000);
        CHECK(list.at(1).toObject().value("ts").toInteger() == 1700000300);
        CHECK(!list.at(2).toObject().contains("ts"));
        CHECK(list.at(0).toObject().value("a").toString() == NotificationService::picturePath(avatar));
        CHECK(list.at(1).toObject().value("a").toString() == NotificationService::picturePath(other));
        CHECK(!list.at(2).toObject().contains("a"));
        CHECK(list.at(2).toObject().value("me").toBool() == true);
        // the avatars are asked for when the card is shown, not at once
        CHECK(!collector.feedback.contains(avatar.toLatin1().constData()) || true);
        collector.feedback.clear();
        QDBusMessage m = QDBusMessage::createMethodCall(SERVICE_NAME, BTSYNCD_NOTIF_PATH, BTSYNCD_NOTIF_IFACE, "PictureState");
        m << NotificationService::picturePath(avatar);
        CHECK(driver.call(m, QDBus::BlockWithGui, 5000).type() == QDBusMessage::ReplyMessage);
        CHECK(waitFor([&] { return collector.feedback.contains("<ty>picture</ty><tx>" + avatar.toLatin1() + "</tx>"); }));
        // PhoneNotification::messagesJson without the new elements is what it was
        PhoneNotification n;
        n.messages << qMakePair(QString("A"), QString("t"));
        n.ownMessages << false;
        n.messagePictures << PhoneNotification::MessagePicture();
        CHECK(n.messagesJson() == "[{\"s\":\"A\",\"t\":\"t\"}]");
    }

    // ---- The ring and notification mute: the events
    {
        write(driver, "<insert><id>130</id><vb>normal</vb><an>Chat</an><su>Msg</su><bo>x</bo><ca>msg</ca></insert>");
        CHECK(next());
        CHECK(lastPosted(fake).hints.value("x-nemo-feedback").toString() == "notif_message");
        write(driver, "<insert><id>131</id><vb>normal</vb><an>Mail</an><su>Other</su><bo>x</bo></insert>");
        CHECK(next());
        CHECK(lastPosted(fake).hints.value("x-nemo-feedback").toString() == "notif_default");
        write(driver, "<insert><id>132</id><an>Mail</an><su>Old phone</su><bo>x</bo></insert>");
        CHECK(next());
        CHECK(lastPosted(fake).hints.value("x-nemo-feedback").toString() == "notif_default");
        // a call: it vibrates only while ring and notifications are muted (the default)
        write(driver, "<insert><id>133</id><vb>ringtone</vb><an>Phone</an><su>Ring1</su><ac>answer,reject</ac></insert>");
        CHECK(next());
        CHECK(lastPosted(fake).hints.value("x-nemo-feedback").toString() == "eos_phone_ringing");
        MDConfItem mute("/org/asteroidos/sound/mute-ring-notifications");
        mute.set(false);
        spin(200);
        CHECK(NotificationService::muteRingAndNotifications() == false);
        write(driver, "<insert><id>134</id><vb>ringtone</vb><an>Phone</an><su>Ring2</su><ac>answer,reject</ac></insert>");
        CHECK(next());
        CHECK(lastPosted(fake).hints.value("x-nemo-feedback").toString() == "ringtone");
        // notifications keep the events (ngfd plays their sound when the mute is off)
        write(driver, "<insert><id>135</id><vb>normal</vb><an>Chat</an><su>Msg2</su><bo>x</bo><ca>msg</ca></insert>");
        CHECK(next());
        CHECK(lastPosted(fake).hints.value("x-nemo-feedback").toString() == "notif_message");
        // an alarm and find my watch do not depend on it
        write(driver, "<insert><id>136</id><vb>alarm</vb><an>Clock</an><su>Al</su><ca>alarm</ca></insert>");
        CHECK(next());
        CHECK(lastPosted(fake).hints.value("x-nemo-feedback").toString() == "eos_phone_alarm");
        mute.set(true);
        spin(200);
        CHECK(NotificationService::muteRingAndNotifications() == true);
        mute.unset();
        write(driver, "<removed><id>133</id></removed><removed><id>134</id></removed>");
        PhoneNotification n;
        n.vibrate = "ringtone";
        n.phoneActions = QStringList() << "answer" << "reject";
        CHECK(PostPlan::feedbackEvent(n, true) == "eos_phone_ringing");
        CHECK(PostPlan::feedbackEvent(n, false) == "ringtone");
        n.vibrate = "normal";
        n.phoneActions.clear();
        CHECK(PostPlan::feedbackEvent(n, false) == "notif_default" && PostPlan::feedbackEvent(n, true) == "notif_default");
        n.messages << qMakePair(QString("A"), QString("t"));
        CHECK(PostPlan::isMessage(n) && PostPlan::feedbackEvent(n) == "notif_message");
    }

    // ---- Starred contacts and repeat callers in Do Not Disturb, Bedtime and Theater
    {
        const auto call = [&](int id, const char *who, bool starred) {
            write(driver, QByteArray("<insert><id>") + QByteArray::number(id) + "</id><vb>ringtone</vb><an>Phone</an><su>" + who +
                          "</su><ac>answer,reject</ac>" + (starred ? "<sd>1</sd>" : "") + "</insert>");
            CHECK(next());
            return lastPosted(fake);
        };
        const auto rings = [](const Posted &p) {
            return p.hints.contains("x-nemo-preview-summary") && p.hints.value("x-nemo-feedback").toString() == "eos_phone_ringing";
        };
        // not quiet: every call rings, nothing to say about exceptions
        Posted p = call(140, "Eve", false);
        CHECK(rings(p) && !p.hints.contains("x-asteroid-call-exception"));
        write(driver, "<removed><id>140</id></removed>");

        service.setDoNotDisturb(true);
        // an unknown caller is silent
        p = call(141, "Stranger", false);
        CHECK(!rings(p) && !p.hints.contains("x-asteroid-call-exception"));
        CHECK(p.hints.value("x-nemo-feedback").toString() == "notif_silent");
        // a starred contact rings, whatever the first call
        p = call(142, "Starred one", true);
        CHECK(rings(p) && p.hints.value("x-asteroid-call-exception").toString() == "starred");
        // the same call updated stays what it was
        p = call(142, "Starred one", false);
        CHECK(p.replacesId != 0 && p.hints.value("x-asteroid-call-exception").toString() == "starred");
        // Eve called within 15 minutes before: a repeat caller
        p = call(143, "Eve", false);
        CHECK(rings(p) && p.hints.value("x-asteroid-call-exception").toString() == "repeat");
        // the stranger again: now a repeat caller too
        p = call(144, "Stranger", false);
        CHECK(rings(p) && p.hints.value("x-asteroid-call-exception").toString() == "repeat");
        // a notification that is no call is silent
        write(driver, "<insert><id>145</id><vb>normal</vb><an>Chat</an><su>Not a call</su><bo>x</bo></insert>");
        CHECK(next());
        CHECK(!lastPosted(fake).hints.contains("x-nemo-preview-summary"));
        service.setDoNotDisturb(false);

        // Bedtime: the same
        service.setModeQuiet(true);
        service.setBedtimeMode(true);
        p = call(146, "Starred two", true);
        CHECK(rings(p) && p.hints.value("x-asteroid-call-exception").toString() == "starred");
        p = call(147, "Stranger", false);
        CHECK(rings(p) && p.hints.value("x-asteroid-call-exception").toString() == "repeat");
        p = call(148, "New caller", false);
        CHECK(!rings(p));

        // Theater: none, as stock's default
        service.setBedtimeMode(false);
        service.setTheaterMode(true);
        p = call(149, "Starred three", true);
        CHECK(!rings(p) && !p.hints.contains("x-asteroid-call-exception"));
        p = call(150, "Stranger", false);
        CHECK(!rings(p));
        // both Bedtime and Theater: Theater decides
        service.setBedtimeMode(true);
        p = call(151, "Starred four", true);
        CHECK(!rings(p));
        service.setBedtimeMode(false);
        service.setTheaterMode(false);
        service.setModeQuiet(false);
        // a call the phone posts again after a reconnection is no repeat
        service.setDoNotDisturb(true);
        write(driver, "<insert><id>152</id><vb>ringtone</vb><an>Phone</an><su>Resynced</su><ac>answer,reject</ac><rs>1</rs></insert>");
        CHECK(next());
        write(driver, "<insert><id>153</id><vb>ringtone</vb><an>Phone</an><su>Resynced</su><ac>answer,reject</ac></insert>");
        CHECK(next());
        CHECK(!rings(lastPosted(fake)));
        service.setDoNotDisturb(false);

        // the decision on its own
        PhoneNotification n;
        n.vibrate = "ringtone";
        n.phoneActions = QStringList() << "answer" << "reject";
        CHECK(!PostPlan::decide(n, false, true, true).preview);
        CHECK(PostPlan::decide(n, false, true, true, false, true).preview);
        CHECK(PostPlan::decide(n, false, true, true, false, true).vibrate);
        // the charger still silences it
        CHECK(!PostPlan::decide(n, false, true, true, true, true).preview);
        // an exception means nothing for what is not a call
        n.phoneActions.clear();
        n.vibrate = "normal";
        CHECK(PostPlan::decide(n, false, true, true, false, true).preview);   // callAllowed is the caller's to give
        CHECK(NotificationService::RepeatCallerMs == 15 * 60 * 1000);
    }

    // ---- calls-15: the missed call card as Gadgetbridge writes it (tests/fixtures/missed-call-card.frames.txt):
    // one action, callback (the watch's Call button), category missed_call, importance 4; a press goes back as
    // feedback type callback, a removal closes the card
    {
        QFile f(QStringLiteral(FRAMES_DIR "/../fixtures/missed-call-card.frames.txt"));
        CHECK(f.open(QIODevice::ReadOnly));
        const QList<QByteArray> lines = f.readAll().split('\n');
        CHECK(lines.size() >= 2 && lines.value(0).startsWith("<insert>") && lines.value(1).startsWith("<removed>"));
        write(driver, lines.value(0));
        CHECK(next());
        const Posted p = lastPosted(fake);
        CHECK(p.appName == "Phone");
        CHECK(p.body.startsWith("Missed call"));
        CHECK(!p.summary.isEmpty());
        CHECK(p.actions == (QStringList() << "callback" << "Call"));
        CHECK(p.hints.value(NotificationService::CategoryHint).toString() == "missed_call");
        CHECK(p.hints.value("x-asteroid-phone-actions").toString() == "callback");
        const uint card = newestId(fake);
        collector.feedback.clear();
        emitActionInvoked(fakeConn, card, "callback");
        CHECK(waitFor([&] { return collector.feedback == "<action><id>590801160</id><ty>callback</ty></action>"; }));
        // the phone's copy going (its own notification dismissed) takes the card off the watch
        const int closedBefore = fake.closed.size();
        write(driver, lines.value(1));
        CHECK(waitFor([&] { QMutexLocker l(&fake.lock); return fake.closed.size() > closedBefore && fake.closed.contains(card); }));
    }

    // ---- FP-B1 CR-30: modes/<mode>/{starred,repeat} replace the hard-coded rule for calls
    {
        // the key names are the contract's
        CHECK(QByteArray(NotificationService::ModeKeyBase) == "/org/asteroidos/modes/");
        const char *modes[] = {"do-not-disturb", "bedtime", "theater"};
        for(const char *m : modes) {
            MDConfItem(QString("/org/asteroidos/modes/%1/starred").arg(m)).unset();
            MDConfItem(QString("/org/asteroidos/modes/%1/repeat").arg(m)).unset();
        }
        spin(300);
        // stock's defaults without a key
        CHECK(NotificationService::modeAllows("do-not-disturb", "starred") && NotificationService::modeAllows("do-not-disturb", "repeat"));
        CHECK(NotificationService::modeAllows("bedtime", "starred") && NotificationService::modeAllows("bedtime", "repeat"));
        CHECK(!NotificationService::modeAllows("theater", "starred") && !NotificationService::modeAllows("theater", "repeat"));

        const auto setMode = [&](const char *mode, const char *what, bool on) {
            MDConfItem(QString("/org/asteroidos/modes/%1/%2").arg(mode, what)).set(on);
            CHECK(waitFor([&] { return NotificationService::modeAllows(mode, what) == on; }));
        };
        const auto unsetMode = [&](const char *mode) {
            MDConfItem(QString("/org/asteroidos/modes/%1/starred").arg(mode)).unset();
            MDConfItem(QString("/org/asteroidos/modes/%1/repeat").arg(mode)).unset();
            spin(300);
        };
        const auto call = [&](int id, const char *who, bool starred) {
            write(driver, QByteArray("<insert><id>") + QByteArray::number(id) + "</id><vb>ringtone</vb><an>Phone</an><su>" + who +
                          "</su><ac>answer,reject</ac>" + (starred ? "<sd>1</sd>" : "") + "</insert>");
            CHECK(next());
            return lastPosted(fake);
        };
        const auto rings = [](const Posted &p) {
            return p.hints.contains("x-nemo-preview-summary") && p.hints.value("x-nemo-feedback").toString() == "eos_phone_ringing";
        };
        const auto exception = [](const Posted &p) { return p.hints.value("x-asteroid-call-exception").toString(); };
        // a repeat caller: a second call from the same text (unique per use, the memory is 15 minutes)
        const auto repeatCaller = [&](int id, const char *who) {
            call(id, who, false);
            write(driver, QByteArray("<removed><id>") + QByteArray::number(id) + "</id></removed>");
            return call(id + 1, who, false);
        };

        // Do Not Disturb with the defaults: both let through
        service.setDoNotDisturb(true);
        Posted p = call(300, "Mode starred one", true);
        CHECK(rings(p) && exception(p) == "starred");
        p = repeatCaller(301, "Mode repeat one");
        CHECK(rings(p) && exception(p) == "repeat");
        // starred off for Do Not Disturb: the starred contact is silent, repeat callers still ring
        setMode("do-not-disturb", "starred", false);
        p = call(303, "Mode starred two", true);
        CHECK(!rings(p) && exception(p).isEmpty());
        p = repeatCaller(304, "Mode repeat two");
        CHECK(rings(p) && exception(p) == "repeat");
        // repeat off as well: only unknown callers, all silent
        setMode("do-not-disturb", "repeat", false);
        p = repeatCaller(306, "Mode repeat three");
        CHECK(!rings(p) && exception(p).isEmpty());
        // starred on, repeat off
        setMode("do-not-disturb", "starred", true);
        p = call(308, "Mode starred three", true);
        CHECK(rings(p) && exception(p) == "starred");
        p = repeatCaller(309, "Mode repeat four");
        CHECK(!rings(p));
        unsetMode("do-not-disturb");
        service.setDoNotDisturb(false);

        // Bedtime has keys of its own
        service.setModeQuiet(true);
        service.setBedtimeMode(true);
        p = call(311, "Mode starred four", true);
        CHECK(rings(p) && exception(p) == "starred");
        setMode("bedtime", "starred", false);
        p = call(312, "Mode starred five", true);
        CHECK(!rings(p));
        // Do Not Disturb's keys do not matter for Bedtime
        setMode("do-not-disturb", "starred", true);
        CHECK(!rings(call(313, "Mode starred six", true)));
        unsetMode("bedtime");
        unsetMode("do-not-disturb");

        // Theater: none by default, a starred contact once its key is on
        service.setBedtimeMode(false);
        service.setTheaterMode(true);
        CHECK(!rings(call(314, "Mode starred seven", true)));
        setMode("theater", "starred", true);
        p = call(315, "Mode starred eight", true);
        CHECK(rings(p) && exception(p) == "starred");
        CHECK(!rings(repeatCaller(316, "Mode repeat five")));
        setMode("theater", "repeat", true);
        p = repeatCaller(318, "Mode repeat six");
        CHECK(rings(p) && exception(p) == "repeat");
        // Bedtime and Theater both on: each must allow it
        service.setBedtimeMode(true);
        setMode("bedtime", "starred", false);
        CHECK(!rings(call(320, "Mode starred nine", true)));
        unsetMode("bedtime");
        p = call(321, "Mode starred ten", true);
        CHECK(rings(p) && exception(p) == "starred");
        // Do Not Disturb with Theater: Theater's switch off decides
        service.setBedtimeMode(false);
        service.setModeQuiet(false);
        service.setDoNotDisturb(true);
        setMode("theater", "starred", false);
        CHECK(!rings(call(322, "Mode starred eleven", true)));
        unsetMode("theater");
        service.setTheaterMode(false);
        service.setDoNotDisturb(false);
    }

    // ---- FP-B1 CR-20: blocked-packages drops the inserts of an app, known-apps lists the apps seen
    {
        CHECK(QByteArray(NotificationService::BlockedPackagesKey) == "/org/asteroidos/notifications/blocked-packages");
        CHECK(QByteArray(NotificationService::KnownAppsKey) == "/org/asteroidos/notifications/known-apps");
        MDConfItem blocked(NotificationService::BlockedPackagesKey), known(NotificationService::KnownAppsKey);
        blocked.unset();
        known.unset();
        spin(300);
        const auto knownList = [&]() { return MDConfItem(NotificationService::KnownAppsKey).value(QStringList()).toStringList(); };
        const auto insert = [&](int id, const char *pn, const char *an, const char *body) {
            write(driver, QByteArray("<insert><id>") + QByteArray::number(id) + "</id><pn>" + pn + "</pn><vb>normal</vb><an>" + an +
                          "</an><su>Blocked test</su><bo>" + body + "</bo></insert>");
        };
        // count what is written to the list (dconf tells every change)
        int knownChanges = 0;
        MDConfItem watched(NotificationService::KnownAppsKey);
        QObject::connect(&watched, &MDConfItem::valueChanged, [&] { knownChanges++; });
        spin(200);

        // the first insert of a package: shown, and the app listed
        insert(330, "org.example.chat", "Example Chat", "one");
        CHECK(next());
        CHECK(waitFor([&] { return knownList() == QStringList("org.example.chat|Example Chat"); }));
        CHECK(waitFor([&] { return knownChanges == 1; }));
        // a second insert of it: nothing written
        insert(331, "org.example.chat", "Example Chat", "two");
        CHECK(next());
        spin(500);
        CHECK(knownChanges == 1 && knownList().size() == 1);
        // renamed: the entry is replaced, not added
        insert(332, "org.example.chat", "Example Chat 2", "three");
        CHECK(next());
        const uint card = newestId(fake);   // the card of phone id 332
        CHECK(waitFor([&] { return knownList() == QStringList("org.example.chat|Example Chat 2"); }));
        CHECK(waitFor([&] { return knownChanges == 2; }));
        // a second app
        insert(333, "org.example.mail", "Example Mail", "four");
        CHECK(next());
        CHECK(waitFor([&] { return knownList().size() == 2 && knownList().contains("org.example.mail|Example Mail"); }));
        // a call without a package name or one that is not a package name: shown, not listed
        insert(334, "no package name", "Strange", "five");
        CHECK(next());
        write(driver, "<insert><id>335</id><vb>normal</vb><an>No Package</an><su>x</su><bo>y</bo></insert>");
        CHECK(next());
        spin(400);
        CHECK(knownList().size() == 2);

        // blocked: not shown (no new card, no sound), its earlier card goes
        blocked.set(QStringList("org.example.chat"));
        CHECK(waitFor([&] { return NotificationService::isBlockedPackage("org.example.chat"); }));
        CHECK(!NotificationService::isBlockedPackage("org.example.mail") && !NotificationService::isBlockedPackage(""));
        const int shown = postedCount(fake);
        insert(332, "org.example.chat", "Example Chat 2", "six");   // an update of the card of 332
        spin(600);
        CHECK(postedCount(fake) == shown);
        CHECK(wasClosed(fake, card));
        insert(336, "org.example.chat", "Example Chat 2", "six");   // a new one
        spin(400);
        CHECK(postedCount(fake) == shown);
        // a blocked call is dropped as well, a notification of another app still comes
        write(driver, "<insert><id>337</id><pn>org.example.chat</pn><vb>ringtone</vb><an>Example Chat</an><su>Caller</su><ac>answer,reject</ac></insert>");
        spin(400);
        CHECK(postedCount(fake) == shown);
        insert(338, "org.example.mail", "Example Mail", "seven");
        CHECK(next());
        // the find-my-watch alert is not an app's
        write(driver, "<insert><id>-78</id><pn>org.example.chat</pn><vb>ringtone</vb><an>Find my watch</an><su>x</su><ac>found</ac></insert>");
        CHECK(next());
        write(driver, "<removed><id>-78</id></removed>");
        // unblocked: shown again
        blocked.unset();
        CHECK(waitFor([&] { return !NotificationService::isBlockedPackage("org.example.chat"); }));
        insert(339, "org.example.chat", "Example Chat 2", "eight");
        CHECK(next());

        // at most 200 entries: a full list gives up the oldest
        QStringList seed;
        for(int i = 0; i < NotificationService::MaxKnownApps - 1; i++)
            seed << QString("org.example.seed%1|Seed %1").arg(i);
        known.set(seed);
        CHECK(waitFor([&] { return knownList().size() == NotificationService::MaxKnownApps - 1; }));
        CHECK(NotificationService::learnApp("org.example.last", "Last"));
        CHECK(waitFor([&] { return knownList().size() == NotificationService::MaxKnownApps; }));
        CHECK(NotificationService::learnApp("org.example.over", "Over"));
        CHECK(waitFor([&] { return knownList().contains("org.example.over|Over"); }));
        CHECK(knownList().size() == NotificationService::MaxKnownApps);
        CHECK(!knownList().contains("org.example.seed0|Seed 0") && knownList().contains("org.example.seed1|Seed 1"));
        CHECK(!NotificationService::learnApp("org.example.over", "Over"));   // already there: no write
        CHECK(!NotificationService::learnApp("", "x") && !NotificationService::learnApp("a b", "x") && !NotificationService::learnApp("a|b", "x"));
        // a name that is empty shows the package
        CHECK(NotificationService::learnApp("org.example.bare", "  "));
        CHECK(waitFor([&] { return knownList().contains("org.example.bare|org.example.bare"); }));
        blocked.unset();
        known.unset();
        spin(300);
    }

    // ---- FP-B2, CR-52/CR-54: <cv> of a message becomes the hint x-asteroid-conversation (the frame
    // Gadgetbridge records, FP-G1); a card without <cv> or with something that is not a key has none
    {
        const QList<QByteArray> g1 = loadFrames("notification-fp-g1");
        // the ringing call (starred, plain), the message, and the calendar end frame (calendar_test's)
        CHECK(g1.size() == 4);
        CHECK(QByteArray(NotificationService::ConversationHint) == "x-asteroid-conversation");
        write(driver, g1.value(2));
        CHECK(next());
        CHECK(lastPosted(fake).hints.value("x-asteroid-conversation").toString() == "nb18490de9277fdf6");
        CHECK(lastPosted(fake).hints.value("x-asteroid-thread").toString() == "b18490de9277fdf6");
        write(driver, "<insert><id>171</id><vb>normal</vb><pn>org.example.chat</pn><an>Chat</an><su>Sam</su><bo>x</bo><th>17</th><cv>t17</cv></insert>");
        CHECK(next());
        CHECK(lastPosted(fake).hints.value("x-asteroid-conversation").toString() == "t17");
        write(driver, "<insert><id>172</id><vb>normal</vb><pn>org.example.chat</pn><an>Chat</an><su>Sam</su><bo>x</bo><th>18</th><cv>x 17!</cv></insert>");
        CHECK(next());
        CHECK(!lastPosted(fake).hints.contains("x-asteroid-conversation"));
        write(driver, "<insert><id>173</id><vb>normal</vb><pn>org.example.chat</pn><an>Chat</an><su>Sam</su><bo>x</bo><th>19</th><cv>" + QByteArray(80, 't') + "</cv></insert>");
        CHECK(next());
        CHECK(!lastPosted(fake).hints.contains("x-asteroid-conversation"));
        write(driver, "<insert><id>174</id><vb>normal</vb><pn>org.example.chat</pn><an>Chat</an><su>Sam</su><bo>x</bo><th>20</th></insert>");
        CHECK(next());
        CHECK(!lastPosted(fake).hints.contains("x-asteroid-conversation"));
        // the old frames (no <cv>) are as before
        write(driver, frames.value(1));
        CHECK(next());
        CHECK(!lastPosted(fake).hints.contains("x-asteroid-conversation"));
    }

    // ---- FP-B2, CR-08: the phone's call cards reach the call log (calllog.h) through the real frames
    {
        QTemporaryDir ldir;
        CallLog calls(ldir.path() + "/calllog.json", QDBusConnection::connectToBus(QDBusConnection::SessionBus, "calllog"));
        service.setCallObserver(&calls);
        const QList<QByteArray> g1 = loadFrames("notification-fp-g1");
        const auto log = [&calls]() { return QJsonDocument::fromJson(calls.Entries().toUtf8()).array(); };
        // ringing (the frame Gadgetbridge records), then answered: the ringing card goes, the card of the
        // call in progress comes (what onSetCallState sends), the call ends
        write(driver, g1.value(0));
        CHECK(next());
        CHECK(log().isEmpty());
        write(driver, "<removed><id>13829704</id></removed>");
        spin(200);
        write(driver, "<insert><id>13829705</id><vb>none</vb><an>Ongoing call</an><ai>ios-call-outline</ai><su>Alex Example</su><bo>+15550100</bo><ac>end</ac></insert>");
        CHECK(next());
        spin(200);
        CHECK(log().isEmpty());                              // answered, not missed
        write(driver, "<removed><id>13829705</id></removed>");
        CHECK(waitFor([&] { return log().size() == 1; }));
        {
            const QJsonObject e = log().at(0).toObject();
            CHECK(e.value("ty").toString() == "in" && e.value("n").toString() == "Alex Example" && e.value("nm").toString() == "+15550100");
        }
        // ringing, gone: missed
        write(driver, "<insert><id>13829710</id><vb>ringtone</vb><an>Incoming call notification</an><su>Sam Carter</su><bo>+15550102</bo><ac>answer,reject,silence,reply</ac></insert>");
        CHECK(next());
        write(driver, "<removed><id>13829710</id></removed>");
        CHECK(waitFor([&] { return log().size() == 2; }));
        CHECK(log().at(0).toObject().value("ty").toString() == "missed");
        // ringing, Decline pressed on the watch (the card's button), gone: declined
        write(driver, "<insert><id>13829711</id><vb>ringtone</vb><an>Incoming call notification</an><su>Bob</su><bo>+15550103</bo><ac>answer,reject</ac></insert>");
        CHECK(next());
        emitActionInvoked(fakeConn, newestId(fake), "reject");
        spin(300);
        write(driver, "<removed><id>13829711</id></removed>");
        CHECK(waitFor([&] { return log().size() == 3; }));
        CHECK(log().at(0).toObject().value("ty").toString() == "declined");
        // a call in progress that no ringing card led to: outgoing
        write(driver, "<insert><id>13829712</id><vb>none</vb><an>Ongoing call</an><su>Bob</su><bo>+15550103</bo><ac>end</ac></insert>");
        CHECK(next());
        write(driver, "<removed><id>13829712</id></removed>");
        CHECK(waitFor([&] { return log().size() == 4; }));
        CHECK(log().at(0).toObject().value("ty").toString() == "out");
        // the phone's missed-call notification (the card is a notification, not a call card): a new entry
        const qint64 callAt = QDateTime::currentSecsSinceEpoch() - 600;
        write(driver, "<insert><id>590801160</id><vb>normal</vb><an>Phone</an><ai>ios-call-outline</ai><su>Lena Okafor</su><bo>Missed call · Mobile</bo><ac>callback</ac><pr>4</pr><ca>missed_call</ca><wh>"
                      + QByteArray::number(callAt) + "</wh></insert>");
        CHECK(next());
        CHECK(waitFor([&] { return log().size() == 5; }));
        {
            bool found = false;
            for(const QJsonValue &v : log()) {
                const QJsonObject e = v.toObject();
                if(e.value("n").toString() == "Lena Okafor") {
                    found = true;
                    CHECK(e.value("ty").toString() == "missed" && e.value("lb").toString() == "Mobile");
                    CHECK(qint64(e.value("t").toDouble()) == callAt * 1000);
                }
            }
            CHECK(found);
        }
        // a VoIP app's call card is not a phone call
        write(driver, "<insert><id>9001</id><vb>ringtone</vb><pn>org.example.voip</pn><an>Incoming call notification</an><su>Dana</su><bo>Messenger call</bo><ac>answer,reject</ac></insert>");
        CHECK(next());
        write(driver, "<removed><id>9001</id></removed>");
        spin(300);
        CHECK(log().size() == 5);
        service.setCallObserver(nullptr);
    }

    // ---- The elements the phone does not send: nothing new in the hints
    {
        write(driver, "<insert><id>160</id><vb>none</vb><an>Chat</an><su>Plain</su><bo>x</bo></insert>");
        CHECK(next());
        const Posted p = lastPosted(fake);
        for(const char *hint : {"x-asteroid-subtext", "x-asteroid-no-emoji", "x-asteroid-progress", "x-asteroid-progress-max",
                                "x-asteroid-progress-indeterminate", "x-asteroid-progress-segments", "x-asteroid-find-watch",
                                "x-asteroid-call-exception", "x-nemo-timestamp"})
            CHECK(!p.hints.contains(hint));
    }

    fakeThread.quit();
    fakeThread.wait();
    if(failures)
        fprintf(stderr, "%d check(s) failed\n", failures);
    else
        fprintf(stderr, "notifw2_test: all checks passed\n");
    return failures ? 1 : 0;
}

#include "notifw2_test.moc"
