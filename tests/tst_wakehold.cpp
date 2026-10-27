/*
 * WakeHold: the mce cpu-keepalive session around a write from the phone
 * (libkeepalive replaced by a stub that records run() / stop()).
 */
#include "wakehold.h"
#include "backgroundactivity.h"

#include <QCoreApplication>
#include <QElapsedTimer>
#include <QTest>

class TestWakeHold : public QObject
{
    Q_OBJECT

private:
    static bool running() { return BackgroundActivity::lastRunning(); }

    // Ends any session a test left behind, then zeroes the counters
    void settle()
    {
        WakeHold::release(0);
        BackgroundActivity::runs() = 0;
        BackgroundActivity::stops() = 0;
    }

private slots:
    void init() { settle(); }
    void cleanup() { settle(); }

    void releaseWhileIdleDoesNothing()
    {
        WakeHold::release(WakeHold::GRACE_NOTIFICATION_MS);
        QCOMPARE(BackgroundActivity::runs(), 0);
        QCOMPARE(BackgroundActivity::stops(), 0);
        QVERIFY(!running());
    }

    void holdStartsOneSession()
    {
        WakeHold::hold();
        QVERIFY(running());
        WakeHold::hold();   // a second chunk of the same notification
        WakeHold::hold();
        QCOMPARE(BackgroundActivity::runs(), 1);
    }

    void releaseWithoutGraceStopsAtOnce()
    {
        WakeHold::hold();
        WakeHold::release(0);
        QVERIFY(!running());
        QCOMPARE(BackgroundActivity::stops(), 1);
    }

    void negativeGraceStopsAtOnce()
    {
        WakeHold::hold();
        WakeHold::release(-50);
        QVERIFY(!running());
    }

    void graceKeepsTheSessionABit()
    {
        WakeHold::hold();
        QElapsedTimer clock;
        clock.start();
        WakeHold::release(WakeHold::GRACE_SHORT_MS);
        QVERIFY(running());
        QTRY_VERIFY_WITH_TIMEOUT(!running(), 3000);
        QVERIFY2(clock.elapsed() >= WakeHold::GRACE_SHORT_MS - 50, qPrintable(QString::number(clock.elapsed())));
        QCOMPARE(BackgroundActivity::stops(), 1);
    }

    void holdDuringGraceCancelsIt()
    {
        WakeHold::hold();
        WakeHold::release(WakeHold::GRACE_SHORT_MS);
        WakeHold::hold();   // the next notification arrives during the grace
        QTest::qWait(WakeHold::GRACE_SHORT_MS + 300);
        QVERIFY(running());
        QCOMPARE(BackgroundActivity::runs(), 1);
        QCOMPARE(BackgroundActivity::stops(), 0);
    }

    void longerGraceIsKept()
    {
        WakeHold::hold();
        QElapsedTimer clock;
        clock.start();
        WakeHold::release(WakeHold::GRACE_NOTIFICATION_MS);
        WakeHold::release(WakeHold::GRACE_SHORT_MS);    // does not shorten it
        QTest::qWait(WakeHold::GRACE_SHORT_MS + 200);
        QVERIFY(running());
        QTRY_VERIFY_WITH_TIMEOUT(!running(), 3000);
        QVERIFY(clock.elapsed() >= WakeHold::GRACE_NOTIFICATION_MS - 50);
    }

    void shorterGraceIsExtended()
    {
        WakeHold::hold();
        QElapsedTimer clock;
        clock.start();
        WakeHold::release(WakeHold::GRACE_SHORT_MS);
        WakeHold::release(WakeHold::GRACE_NOTIFICATION_MS);
        QTest::qWait(WakeHold::GRACE_SHORT_MS + 200);
        QVERIFY(running());
        QTRY_VERIFY_WITH_TIMEOUT(!running(), 3000);
        QVERIFY(clock.elapsed() >= WakeHold::GRACE_NOTIFICATION_MS - 50);
    }

    void graceNeverPassesTheLimit()
    {
        WakeHold::hold();
        QElapsedTimer clock;
        clock.start();
        WakeHold::release(60000);
        QTRY_VERIFY_WITH_TIMEOUT(!running(), WakeHold::HOLD_LIMIT_MS + 2000);
        QVERIFY2(clock.elapsed() < WakeHold::HOLD_LIMIT_MS + 1000, qPrintable(QString::number(clock.elapsed())));
    }

    void lostLastChunkEndsAtTheLimit()
    {
        // hold() and never release(): the final chunk never came
        WakeHold::hold();
        QElapsedTimer clock;
        clock.start();
        QTest::qWait(1000);
        QVERIFY(running());
        QTRY_VERIFY_WITH_TIMEOUT(!running(), WakeHold::HOLD_LIMIT_MS + 2000);
        QVERIFY(clock.elapsed() >= WakeHold::HOLD_LIMIT_MS - 100);
        QCOMPARE(BackgroundActivity::stops(), 1);
    }

    void scopeHoldsUntilItEnds()
    {
        {
            WakeHoldScope wake(0);
            QVERIFY(running());
        }
        QVERIFY(!running());
        {
            WakeHoldScope wake;     // GRACE_SHORT_MS after the scope
            QVERIFY(running());
        }
        QVERIFY(running());
        QTRY_VERIFY_WITH_TIMEOUT(!running(), 3000);
        QCOMPARE(BackgroundActivity::runs(), 2);
    }
};

QTEST_GUILESS_MAIN(TestWakeHold)
#include "tst_wakehold.moc"
