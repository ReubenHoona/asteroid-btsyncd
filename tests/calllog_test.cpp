/*
 * Copyright (C) 2026 eos-mainline contributors
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

// FP-B2, CR-08: the call log object (calllog.h) on a private session bus, with the clock of the
// test:
//   - the interface: Entries() and the signal Changed() are in the introspection (what Nemo.DBus
//     of the Phone app needs: only introspected signals are connected), the D-Bus signal arrives
//   - a call that rings and is answered is one "in" entry, written when it ends (the ringing
//     card goes, the card of the call in progress comes), with its duration
//   - missed, declined (the user's Decline on the watch), outgoing, a VoIP call is not recorded
//   - the phone's own missed-call notification adds an entry only when there is none yet
//   - at most 200 entries, newest first; the file is written when an entry is added only, 0600,
//     and read again by the next start
// dbus-run-session -- calllog_test

#include "calllog.h"

#include <QCoreApplication>
#include <QDBusConnection>
#include <QDBusMessage>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTemporaryDir>
#include <cstdio>

static int failures = 0;

#define CHECK(cond) do { if (!(cond)) { fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); failures++; } } while (0)

class Counter : public QObject
{
    Q_OBJECT
public:
    int count = 0;
public slots:
    void changed() { count++; }
};

static void spin(int ms)
{
    QElapsedTimer t;
    t.start();
    while (t.elapsed() < ms)
        QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
}

static const QStringList Ringing = {"answer", "reject", "silence", "reply"};
static const QStringList InCall = {"end"};

static QJsonArray entries(CallLog &log)
{
    return QJsonDocument::fromJson(log.Entries().toUtf8()).array();
}

static QJsonObject entry(CallLog &log, int i)
{
    return entries(log).at(i).toObject();
}

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    if (qEnvironmentVariableIsEmpty("DBUS_SESSION_BUS_ADDRESS")) {
        fprintf(stderr, "needs a private session bus (dbus-run-session -- calllog_test)\n");
        return 2;
    }
    QTemporaryDir dir;
    const QString file = dir.path() + "/sub/calllog.json";

    qint64 clock = 1793200000000LL;
    CallLog log(file);
    log.setClock([&clock] { return clock; });
    Counter changed;
    QObject::connect(&log, &CallLog::Changed, &changed, &Counter::changed);

    // ---- the interface, as another process sees it
    QDBusConnection driver = QDBusConnection::connectToBus(QDBusConnection::SessionBus, "driver");
    const QString owner = QDBusConnection::sessionBus().baseService();
    Counter busSignal;
    CHECK(driver.connect(owner, BTSYNCD_CALLLOG_PATH, BTSYNCD_CALLLOG_IFACE, "Changed", &busSignal, SLOT(changed())));
    {
        QDBusMessage m = QDBusMessage::createMethodCall(owner, BTSYNCD_CALLLOG_PATH, "org.freedesktop.DBus.Introspectable", "Introspect");
        const QDBusMessage r = driver.call(m, QDBus::BlockWithGui, 5000);
        CHECK(r.type() == QDBusMessage::ReplyMessage);
        const QString xml = r.arguments().value(0).toString();
        CHECK(xml.contains("<interface name=\"org.asteroidos.btsyncd.CallLog\">"));
        CHECK(xml.contains("<method name=\"Entries\">"));
        CHECK(xml.contains("<signal name=\"Changed\">"));
        CHECK(QString(BTSYNCD_CALLLOG_PATH) == "/org/asteroidos/btsyncd/calllog");
        QDBusMessage e = QDBusMessage::createMethodCall(owner, BTSYNCD_CALLLOG_PATH, BTSYNCD_CALLLOG_IFACE, "Entries");
        const QDBusMessage er = driver.call(e, QDBus::BlockWithGui, 5000);
        CHECK(er.type() == QDBusMessage::ReplyMessage && er.arguments().value(0).toString() == "[]");
    }
    CHECK(log.Entries() == "[]");
    CHECK(!QFile::exists(file));

    // ---- parsing the caller as the call card words it
    {
        QString n, nm;
        CallLog::parseCaller("Alex Example", "+15550100", &n, &nm);
        CHECK(n == "Alex Example" && nm == "+15550100");
        CallLog::parseCaller("+1 (555) 010-0100", "+1 (555) 010-0100", &n, &nm);
        CHECK(n.isEmpty() && nm == "+15550100100");
        CallLog::parseCaller("0800 555 0100", "", &n, &nm);
        CHECK(n.isEmpty() && nm == "08005550100");
        CallLog::parseCaller("Pizza", "", &n, &nm);
        CHECK(n == "Pizza" && nm.isEmpty());
        CallLog::parseCaller("Sam", "Mobile", &n, &nm);
        CHECK(n == "Sam" && nm.isEmpty());
        CHECK(CallLog::normalizedNumber("12").isEmpty() && CallLog::normalizedNumber("12ab34").isEmpty());
        CHECK(CallLog::normalizedNumber("*100#") == "*100#" && CallLog::normalizedNumber("*#06#").isEmpty());
    }

    // ---- ringing, then answered: one "in" entry when the call ends
    clock += 1000;
    const qint64 ringAt = clock;
    log.callCard(7, "Alex Example", "+15550100", Ringing, "");
    CHECK(log.Entries() == "[]" && !QFile::exists(file));        // nothing until something ended
    clock += 2000;
    log.callGone(7);                                               // the ringing card goes ...
    CHECK(entries(log).size() == 1 && entry(log, 0).value("ty").toString() == "missed");
    CHECK(QFile::exists(file));
    clock += 1000;
    log.callCard(8, "Alex Example", "+15550100", InCall, "");     // ... and the card of the call in progress comes
    CHECK(log.Entries() == "[]");                                  // it was answered: not missed
    clock += 65000;
    log.callGone(8);
    {
        CHECK(entries(log).size() == 1);
        const QJsonObject e = entry(log, 0);
        CHECK(e.value("ty").toString() == "in");
        CHECK(qint64(e.value("t").toDouble()) == ringAt);
        CHECK(e.value("n").toString() == "Alex Example" && e.value("nm").toString() == "+15550100");
        CHECK(e.value("lb").toString().isEmpty());
        CHECK(e.value("d").toInt() == 65);
        // the keys the app reads, nothing else
        CHECK(e.keys().size() == 6);
        for (const char *k : {"t", "ty", "n", "nm", "lb", "d"})
            CHECK(e.contains(k));
    }
    // the same card from ringing to in progress (the phone updates it in place)
    clock += 5000;
    log.callCard(9, "Sam Carter", "+15550102", Ringing, "");
    clock += 3000;
    log.callCard(9, "Sam Carter", "+15550102", InCall, "");
    clock += 10000;
    log.callGone(9);
    CHECK(entries(log).size() == 2 && entry(log, 0).value("ty").toString() == "in" && entry(log, 0).value("d").toInt() == 10);

    // ---- missed: ringing, gone, no call in progress after it
    clock += 60000;
    log.callCard(10, "Sam Carter", "+15550102", Ringing, "");
    clock += 20000;
    log.callGone(10);
    {
        CHECK(entries(log).size() == 3);
        const QJsonObject e = entry(log, 0);
        CHECK(e.value("ty").toString() == "missed" && e.value("d").toInt() == 0 && e.value("n").toString() == "Sam Carter");
    }
    // a call in progress after the window is an outgoing one, not the answer to that ring
    clock += 40000;
    log.callCard(11, "Sam Carter", "+15550102", InCall, "");
    clock += 30000;
    log.callGone(11);
    CHECK(entries(log).size() == 4 && entry(log, 0).value("ty").toString() == "out" && entry(log, 0).value("d").toInt() == 30);
    CHECK(entry(log, 1).value("ty").toString() == "missed");

    // ---- declined on the watch
    clock += 10000;
    log.callCard(12, "Bob", "+15550103", Ringing, "");
    log.callAction(12, "reject");
    clock += 1500;
    log.callGone(12);
    CHECK(entries(log).size() == 5 && entry(log, 0).value("ty").toString() == "declined" && entry(log, 0).value("d").toInt() == 0);
    // "answer" or "silence" is not a decline
    log.callCard(13, "Bob", "+15550103", Ringing, "");
    log.callAction(13, "silence");
    log.callGone(13);
    CHECK(entry(log, 0).value("ty").toString() == "missed");
    // a call from a VoIP app is not a phone call
    const int before = entries(log).size();
    log.callCard(14, "Dana", "Messenger call", Ringing, "org.example.chat");
    log.callGone(14);
    CHECK(entries(log).size() == before);
    // a card that is neither ringing nor in progress is not a call
    log.callCard(15, "Dana", "x", QStringList({"open", "mute"}), "");
    log.callGone(15);
    CHECK(entries(log).size() == before);
    // a removal nobody knows
    log.callGone(999);
    CHECK(entries(log).size() == before);

    // ---- the phone's missed-call notification
    {
        const int n0 = entries(log).size();
        clock += 100000;
        log.callCard(20, "Lena Okafor", "+15550104", Ringing, "");
        clock += 15000;
        log.callGone(20);
        CHECK(entries(log).size() == n0 + 1);
        // the phone's notification for the same call: already in the log
        log.missedCallCard(500, "Lena Okafor", "Missed call \xC2\xB7 Mobile", clock / 1000);
        CHECK(entries(log).size() == n0 + 1);
        // for a call the watch did not see (it was away): a new entry with the label, at its time
        const qint64 callAt = clock / 1000 - 3600;
        log.missedCallCard(501, "+1 555 0105", QString::fromUtf8("Missed call \xC2\xB7 Work"), callAt);
        CHECK(entries(log).size() == n0 + 2);
        bool found = false;
        for (const QJsonValue &v : entries(log)) {
            const QJsonObject e = v.toObject();
            if (qint64(e.value("t").toDouble()) == callAt * 1000) {
                found = true;
                CHECK(e.value("ty").toString() == "missed" && e.value("nm").toString() == "+15550105" && e.value("lb").toString() == "Work");
            }
        }
        CHECK(found);
        // newest first
        qint64 last = 1LL << 62;
        for (const QJsonValue &v : entries(log)) {
            const qint64 t = qint64(v.toObject().value("t").toDouble());
            CHECK(t <= last);
            last = t;
        }
        // the same notification again (a resync): nothing; an old one: nothing
        log.missedCallCard(501, "+1 555 0105", "Missed call", callAt);
        log.missedCallCard(502, "+1 555 0106", "Missed call", clock / 1000 - 8 * 24 * 3600);
        CHECK(entries(log).size() == n0 + 2);
    }

    // ---- Changed: once per added entry (and for the call that was taken back), on the bus too
    spin(200);
    CHECK(changed.count > 0);
    CHECK(busSignal.count == changed.count);
    {
        const int c = changed.count;
        log.callCard(30, "Quiet", "+15550107", Ringing, "");
        spin(100);
        CHECK(changed.count == c);                                  // a ringing card changes nothing
        log.callGone(30);
        spin(200);
        CHECK(changed.count == c + 1 && busSignal.count == c + 1);
    }

    // ---- the file: 0600, what Entries() says, read again by the next start
    {
        QFile f(file);
        CHECK(f.open(QIODevice::ReadOnly));
        CHECK(QString::fromUtf8(f.readAll()) == log.Entries());
        CHECK((QFileInfo(file).permissions() & (QFileDevice::ReadGroup | QFileDevice::WriteGroup | QFileDevice::ReadOther | QFileDevice::WriteOther)) == 0);
        const qint64 mtimeBefore = QFileInfo(file).lastModified().toMSecsSinceEpoch();
        log.callCard(31, "Nobody", "+15550108", Ringing, "");
        spin(1200);
        CHECK(QFileInfo(file).lastModified().toMSecsSinceEpoch() == mtimeBefore);   // nothing written on a ringing card
        log.callGone(31);
        CallLog again(file, QDBusConnection::connectToBus(QDBusConnection::SessionBus, "second"));
        CHECK(again.Entries() == log.Entries());
    }
    // a damaged file is ignored, entries with a bad type or time are dropped
    {
        QFile f(dir.path() + "/bad.json");
        CHECK(f.open(QIODevice::WriteOnly));
        f.write("[{\"t\":1793200000000,\"ty\":\"in\",\"n\":\"A\",\"nm\":\"\",\"lb\":\"\",\"d\":5},{\"t\":0,\"ty\":\"in\"},{\"t\":5,\"ty\":\"weird\"},7]");
        f.close();
        CallLog bad(dir.path() + "/bad.json", QDBusConnection::connectToBus(QDBusConnection::SessionBus, "third"));
        CHECK(bad.entries().size() == 1 && bad.entries().first().seconds == 5);
        QFile g(dir.path() + "/junk.json");
        CHECK(g.open(QIODevice::WriteOnly));
        g.write("not json");
        g.close();
        CallLog junk(dir.path() + "/junk.json", QDBusConnection::connectToBus(QDBusConnection::SessionBus, "fourth"));
        CHECK(junk.entries().isEmpty());
    }

    // ---- at most 200 entries, the oldest goes
    {
        CallLog big(dir.path() + "/big.json", QDBusConnection::connectToBus(QDBusConnection::SessionBus, "fifth"));
        qint64 t = 1793300000000LL;
        big.setClock([&t] { return t; });
        for (int i = 0; i < 205; i++) {
            t += 1000;
            big.callCard(1000 + i, QString("Caller %1").arg(i), "", InCall, "");
            t += 4000;
            big.callGone(1000 + i);
        }
        CHECK(big.entries().size() == CallLog::MaxEntries && CallLog::MaxEntries == 200);
        const QJsonArray a = QJsonDocument::fromJson(big.Entries().toUtf8()).array();
        CHECK(a.size() == 200);
        CHECK(a.first().toObject().value("n").toString() == "Caller 204");
        CHECK(a.last().toObject().value("n").toString() == "Caller 5");
        CHECK(a.first().toObject().value("d").toInt() == 4);
        QFile f(dir.path() + "/big.json");
        CHECK(f.open(QIODevice::ReadOnly) && QJsonDocument::fromJson(f.readAll()).array().size() == 200);
    }
    // a tracked call whose removal never comes does not grow without bound
    {
        CallLog many(dir.path() + "/many.json", QDBusConnection::connectToBus(QDBusConnection::SessionBus, "sixth"));
        for (int i = 0; i < 40; i++)
            many.callCard(i, "X", "+15550199", Ringing, "");
        for (int i = 0; i < 40; i++)
            many.callGone(i);
        CHECK(many.entries().size() <= CallLog::MaxActive);
    }

    if (failures)
        fprintf(stderr, "%d check(s) failed\n", failures);
    else
        fprintf(stderr, "calllog_test: all checks passed\n");
    return failures ? 1 : 0;
}

#include "calllog_test.moc"
