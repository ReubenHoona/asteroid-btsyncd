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

// Voice replies recognised on the watch (VoiceService with a fake
// asteroid-assistant), on a private bus like notifications_test:
// dbus-run-session -- sh -c 'DBUS_SYSTEM_BUS_ADDRESS=$DBUS_SESSION_BUS_ADDRESS voicelocal_test'

#include "voiceservice.h"
#include "common.h"

#include <QAtomicInt>
#include <QCoreApplication>
#include <QDBusConnection>
#include <QDBusMessage>
#include <QElapsedTimer>
#include <QMutex>
#include <QMutexLocker>
#include <QThread>
#include <QTimer>
#include <cstdio>
#include <functional>

static int failures = 0;

#define CHECK(cond) do { if (!(cond)) { fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); failures++; } } while (0)

static const char *A_SERVICE = "org.asteroidos.assistant";
static const char *A_PATH = "/org/asteroidos/assistant";
static const char *A_IFACE = "org.asteroidos.assistant";

/* What Dictate() does: signals before its reply, the session it returns, and
 * signals after it (ms, session, Partial text or Result JSON) */
struct Plan
{
    QList<QPair<uint, QString>> partialsBefore, resultsBefore;
    uint session = 0;
    int blockMs = 0;
    QList<std::tuple<int, uint, QString, bool>> after;   // ms, session, text, isResult
};

class FakeAssistant : public QObject
{
    Q_OBJECT
    Q_CLASSINFO("D-Bus Interface", "org.asteroidos.assistant")
public:
    QDBusConnection *conn = nullptr;
    QMutex lock;
    Plan plan;
    QAtomicInt cancels = 0, stops = 0, dictates = 0;

    void signal(uint session, const QString &text, bool result)
    {
        QDBusMessage s = QDBusMessage::createSignal(A_PATH, A_IFACE, result ? "Result" : "Partial");
        s << session << text;
        conn->send(s);
    }

public slots:
    uint Dictate(const QString &)
    {
        dictates.ref();
        Plan p;
        {
            QMutexLocker l(&lock);
            p = plan;
        }
        if (p.blockMs)
            QThread::msleep(p.blockMs);
        for (const auto &x : p.partialsBefore)
            signal(x.first, x.second, false);
        for (const auto &x : p.resultsBefore)
            signal(x.first, x.second, true);
        for (const auto &a : p.after) {
            const uint s = std::get<1>(a);
            const QString text = std::get<2>(a);
            const bool result = std::get<3>(a);
            QTimer::singleShot(std::get<0>(a), this, [this, s, text, result] { signal(s, text, result); });
        }
        return p.session;
    }
    bool Stop() { stops.ref(); return true; }
    bool Cancel() { cancels.ref(); return true; }
};

class Collector : public QObject
{
    Q_OBJECT
public:
    QStringList partials, finals, errors;
public slots:
    void partial(const QString &t) { partials << t; }
    void final(const QString &t) { finals << t; }
    void error(const QString &c) { errors << c; }
};

static bool waitFor(const std::function<bool()> &cond, int ms = 3000)
{
    QElapsedTimer t;
    t.start();
    while (!cond() && t.elapsed() < ms)
        QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
    return cond();
}

static void spin(int ms)
{
    waitFor([] { return false; }, ms);
}

static QString btsyncdName;

static QVariant api(QDBusConnection &driver, const char *method, const QList<QVariant> &args = {})
{
    QDBusMessage m = QDBusMessage::createMethodCall(btsyncdName, BTSYNCD_VOICE_PATH, BTSYNCD_VOICE_IFACE, method);
    m.setArguments(args);
    const QDBusMessage r = driver.call(m, QDBus::BlockWithGui, 5000);
    CHECK(r.type() == QDBusMessage::ReplyMessage);
    return r.arguments().value(0);
}

static QString dictation(const QString &text)
{
    return QStringLiteral("{\"type\":\"dictation\",\"text\":\"%1\"}").arg(text);
}

int main(int argc, char **argv)
{
    qputenv("BTSYNCD_ASSISTANT_INSTALLED", "1");
    qputenv("BTSYNCD_VOICE_RECOGNITION", "watch");
    qputenv("BTSYNCD_VOICE_LOCAL_TIMEOUT_MS", "1500");
    QCoreApplication app(argc, argv);
    if (qEnvironmentVariableIsEmpty("DBUS_SESSION_BUS_ADDRESS") || qEnvironmentVariableIsEmpty("DBUS_SYSTEM_BUS_ADDRESS")) {
        fprintf(stderr, "needs a private bus as session and system bus (see the top of the source)\n");
        return 2;
    }

    QThread fakeThread;
    fakeThread.start();
    FakeAssistant fake;
    fake.moveToThread(&fakeThread);
    QDBusConnection fakeConn = QDBusConnection::connectToBus(QDBusConnection::SessionBus, "fake-assistant");
    fake.conn = &fakeConn;
    CHECK(fakeConn.registerObject(A_PATH, &fake, QDBusConnection::ExportAllSlots));
    CHECK(fakeConn.registerService(A_SERVICE));

    VoiceService voice(7, QDBusConnection::systemBus(), nullptr);
    btsyncdName = QDBusConnection::sessionBus().baseService();

    QDBusConnection driver = QDBusConnection::connectToBus(QDBusConnection::SessionBus, "driver");
    Collector c;
    CHECK(driver.connect(btsyncdName, BTSYNCD_VOICE_PATH, BTSYNCD_VOICE_IFACE, "Partial", &c, SLOT(partial(QString))));
    CHECK(driver.connect(btsyncdName, BTSYNCD_VOICE_PATH, BTSYNCD_VOICE_IFACE, "Final", &c, SLOT(final(QString))));
    CHECK(driver.connect(btsyncdName, BTSYNCD_VOICE_PATH, BTSYNCD_VOICE_IFACE, "Error", &c, SLOT(error(QString))));
    spin(200);
    auto reset = [&] { c.partials.clear(); c.finals.clear(); c.errors.clear(); };
    auto setPlan = [&](const Plan &p) { QMutexLocker l(&fake.lock); fake.plan = p; };

    // 1. A Partial of the session Dictate() replaced, still in flight, comes
    // before the reply: it is not this session's, and does not hide it
    {
        Plan p;
        p.partialsBefore << qMakePair(41u, QStringLiteral("stale words"));
        p.session = 42;
        p.after << std::make_tuple(150, 42u, QStringLiteral("hello"), false)
                << std::make_tuple(300, 42u, dictation("hello there"), true);
        setPlan(p);
        reset();
        CHECK(api(driver, "Start", {0u, QString()}).toUInt() != 0);
        CHECK(waitFor([&] { return !c.finals.isEmpty() || !c.errors.isEmpty(); }));
        CHECK(c.finals == QStringList() << "hello there");
        CHECK(c.partials == QStringList() << "hello");
        CHECK(c.errors.isEmpty());
        CHECK(api(driver, "State").toString() == "done");
    }

    // 2. The session failed at once: its Result comes before the reply
    {
        Plan p;
        p.resultsBefore << qMakePair(43u, QStringLiteral("{\"type\":\"error\",\"code\":\"mic\"}"));
        p.session = 43;
        setPlan(p);
        reset();
        CHECK(api(driver, "Start", {0u, QString()}).toUInt() != 0);
        CHECK(waitFor([&] { return !c.errors.isEmpty(); }));
        CHECK(c.errors == QStringList() << "mic");
        CHECK(api(driver, "State").toString() == "idle");
    }

    // 3. Another session's Result (the assistant overlay's) before the reply
    {
        Plan p;
        p.resultsBefore << qMakePair(44u, QStringLiteral("{\"type\":\"intent\",\"intent\":\"timer.set\"}"));
        p.session = 45;
        p.after << std::make_tuple(200, 45u, dictation("second"), true);
        setPlan(p);
        reset();
        CHECK(api(driver, "Start", {0u, QString()}).toUInt() != 0);
        CHECK(waitFor([&] { return !c.finals.isEmpty() || !c.errors.isEmpty(); }));
        CHECK(c.finals == QStringList() << "second");
        CHECK(c.errors.isEmpty());
    }

    // 4. asteroid-assistant goes away in the middle (a crash): Error, idle
    {
        Plan p;
        p.session = 46;
        setPlan(p);
        reset();
        CHECK(api(driver, "Start", {0u, QString()}).toUInt() != 0);
        spin(300);
        CHECK(api(driver, "State").toString() == "listening");
        CHECK(fakeConn.unregisterService(A_SERVICE));
        CHECK(waitFor([&] { return !c.errors.isEmpty(); }));
        CHECK(c.errors == QStringList() << "engine");
        CHECK(api(driver, "State").toString() == "idle");
        CHECK(fakeConn.registerService(A_SERVICE));
        spin(200);
    }

    // 5. No text after Stop: the session ends (and is cancelled there)
    {
        Plan p;
        p.session = 47;
        p.after << std::make_tuple(100, 47u, QStringLiteral("almost"), false);
        setPlan(p);
        reset();
        const int cancels = fake.cancels.loadRelaxed();
        CHECK(api(driver, "Start", {0u, QString()}).toUInt() != 0);
        CHECK(waitFor([&] { return !c.partials.isEmpty(); }));
        CHECK(api(driver, "Stop").toBool() == true);
        CHECK(waitFor([&] { return !c.finals.isEmpty() || !c.errors.isEmpty(); }, 4000));
        // the last partial is the text, as with the phone
        CHECK(c.finals == QStringList() << "almost");
        CHECK(waitFor([&] { return fake.cancels.loadRelaxed() == cancels + 1; }));
    }

    // 6. Dictate() does not answer within its 5 s: dropped there too
    {
        Plan p;
        p.session = 48;
        p.blockMs = 6000;
        setPlan(p);
        reset();
        CHECK(api(driver, "Start", {0u, QString()}).toUInt() != 0);
        const int cancels = fake.cancels.loadRelaxed();
        CHECK(waitFor([&] { return !c.errors.isEmpty(); }, 8000));
        CHECK(c.errors == QStringList() << "engine");
        CHECK(waitFor([&] { return fake.cancels.loadRelaxed() == cancels + 1; }, 4000));
        CHECK(api(driver, "State").toString() == "idle");
    }

    fakeThread.quit();
    fakeThread.wait();
    if (failures)
        fprintf(stderr, "%d check(s) failed\n", failures);
    else
        fprintf(stderr, "voicelocal_test: all checks passed\n");
    return failures ? 1 : 0;
}

#include "voicelocal_test.moc"
