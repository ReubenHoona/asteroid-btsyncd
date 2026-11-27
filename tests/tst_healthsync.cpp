/*
 * Copyright (C) 2026 eos-mainline contributors
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

/*
 * R5 E-08: the syncs the watch starts (healthservice.h). A fake phone relay
 * takes the hsync acts; the end of a sync comes as Gadgetbridge writes it
 * (fixtures/health-synced.frames.txt, from its AsteroidOSHealthSyncTest);
 * health.db is fixtures/health-v3.sql with invented workouts and nights added
 * as sensorlogd writes them; sensorlogd's HealthChanged comes over the test
 * bus. Checked: the events (workout ended, a new main night, Today opened,
 * the Sync button), one at a time, the quiet minute, the states the Today
 * list shows (syncing, failed, idle, last synced) and when the row shows.
 */
#include "fixtures.h"
#include "healthservice.h"
#include "mdconfitem.h"
#include "phonerelay.h"
#include "testmain.h"

#include <QDBusConnection>
#include <QDBusMessage>
#include <QDateTime>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTemporaryDir>
#include <QTest>

#include <sqlite3.h>

#include <functional>

static QVariant key(const char *k)
{
    return MDConfItem(QString::fromLatin1(k)).value();
}

static void setKey(const char *k, const QVariant &v)
{
    MDConfItem(QString::fromLatin1(k)).set(v);
}

static bool runSql(const QString &path, const QByteArray &sql)
{
    sqlite3 *db = nullptr;
    if (sqlite3_open_v2(QFile::encodeName(path).constData(), &db, SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE,
                        nullptr) != SQLITE_OK) {
        sqlite3_close(db);
        return false;
    }
    char *err = nullptr;
    const bool ok = sqlite3_exec(db, sql.constData(), nullptr, nullptr, &err) == SQLITE_OK;
    if (!ok)
        qWarning() << "SQL error:" << err;
    sqlite3_free(err);
    sqlite3_close(db);
    return ok;
}

// The phone's relay: which acts went out; the phone answers when the test says
class FakeRelay : public PhoneRelay
{
public:
    bool has = true;
    QStringList acts;
    std::function<void(const QString &)> done;

    bool relayHas(const QString &act) const override { return has && act == QLatin1String("hsync"); }
    bool relayAct(const QByteArray &json, const std::function<void(const QString &)> &d) override
    {
        if (!has)
            return false;
        acts << QString::fromUtf8(json);
        done = d;
        return true;
    }
    QString lastEvent() const
    {
        return acts.isEmpty() ? QString()
                              : QJsonDocument::fromJson(acts.last().toUtf8()).object().value(QStringLiteral("ev")).toString();
    }
    void answer(const char *json)
    {
        if (done)
            done(QString::fromLatin1(json));
    }
};

class TestHealthSync : public QObject
{
    Q_OBJECT

private:
    QTemporaryDir mDir;
    HealthService *mService = nullptr;
    Characteristic *mRequest = nullptr;
    Characteristic *mCaps = nullptr;
    FakeRelay mRelay;
    QList<QByteArray> mSynced;

    QString db() const { return mDir.path() + QStringLiteral("/health.db"); }
    QString state() const { return key(HealthService::SyncStateKey).toString(); }

    void writeRequest(const QByteArray &value) { mRequest->WriteValue(value, {{"mtu", 100}}); }

    // a new service on the database as it is now (its marks are read when it is made)
    void remake()
    {
        delete mService;
        mService = new HealthService(6, QDBusConnection::systemBus(), this);
        mService->setPhoneRelay(&mRelay);
        const QList<Characteristic *> chrcs = mService->getCharacteristics();
        QCOMPARE(chrcs.size(), 3);
        mRequest = chrcs[0];
        mCaps = chrcs[2];
    }

    // the sync the watch asked for ends as the phone ends it
    void end(int i = 0)
    {
        writeRequest(mSynced.value(i));
    }

private slots:
    void initTestCase()
    {
        QVERIFY(mDir.isValid());
        mSynced = fixtureFrames("health-synced.frames.txt");
        QCOMPARE(mSynced.size(), 2);
    }

    void init()
    {
        MDConfItem::reset();
        MDConfItem("/org/asteroidos/btsyncd/health-data-dir").set(mDir.path());
        QFile::remove(db());
        QFile::remove(db() + "-wal");
        QFile::remove(db() + "-shm");
        QVERIFY(runSql(db(), fixtureFile("health-v3.sql")));
        mRelay = FakeRelay();
        remake();
    }

    void cleanupTestCase()
    {
        delete mService;
        mService = nullptr;
    }

    void keysAndCapabilities()
    {
        QCOMPARE(QByteArray(HealthService::SyncRequestKey), QByteArray("/org/asteroidos/health/sync/request"));
        QCOMPARE(QByteArray(HealthService::SyncStateKey), QByteArray("/org/asteroidos/health/sync/state"));
        QCOMPARE(QByteArray(HealthService::SyncLastKey), QByteArray("/org/asteroidos/health/sync/last-ms"));
        QCOMPARE(QByteArray(HealthService::SyncSupportedKey), QByteArray("/org/asteroidos/health/sync/supported"));
        const QByteArray caps = mCaps->ReadValue({});
        // the phone learns that it may write <synced>; the types as before
        QVERIFY(caps.contains("<takes>prof,synced</takes>"));
        QVERIFY(caps.contains("<version>3</version>"));
    }

    void theEndOfASync()
    {
        QVERIFY(!key(HealthService::SyncSupportedKey).isValid());
        const qint64 before = QDateTime::currentMSecsSinceEpoch();
        QVERIFY(mService->synced(mSynced[0]));
        QCOMPARE(key(HealthService::SyncSupportedKey).toBool(), true);
        QCOMPARE(state(), QStringLiteral("idle"));
        const qint64 last = key(HealthService::SyncLastKey).toLongLong();
        QVERIFY(last >= before && last <= QDateTime::currentMSecsSinceEpoch());
        // one that ended early: failed, the last time kept
        QVERIFY(mService->synced(mSynced[1]));
        QCOMPARE(state(), QStringLiteral("failed"));
        QCOMPARE(key(HealthService::SyncLastKey).toLongLong(), last);
        // not one
        QVERIFY(!mService->synced("<synced><n>3</n></synced>"));
        QVERIFY(!mService->synced("<prof><gd>other</gd></prof>"));
        QVERIFY(!mService->synced("<synced><ok>1</ok>"));
        // through the request characteristic, also in pieces, and no answer goes out
        HealthDataChrc *data = qobject_cast<HealthDataChrc *>(mService->getCharacteristics()[1]);
        QVERIFY(data);
        int notified = 0;
        connect(data, &HealthDataChrc::valueChanged, this, [&notified] { notified++; });
        const QByteArray frame = mSynced[0];
        mRequest->WriteValue(frame.left(9), {{"offset", 0}, {"mtu", 12}});
        QCOMPARE(state(), QStringLiteral("failed"));
        mRequest->WriteValue(frame.mid(9), {{"offset", 9}, {"mtu", 12}});
        QCOMPARE(state(), QStringLiteral("idle"));
        QTest::qWait(100);
        QCOMPARE(notified, 0);
    }

    void noPhoneThatSyncs()
    {
        delete mService;
        mService = new HealthService(6, QDBusConnection::systemBus(), this);   // no relay at all
        QVERIFY(!mService->requestSync(QStringLiteral("today")));
        QVERIFY(!key(HealthService::SyncStateKey).isValid());
        // the button says it could not
        QVERIFY(!mService->requestSync(QStringLiteral("button")));
        QCOMPARE(state(), QStringLiteral("failed"));
        // a phone without hsync (an older Gadgetbridge): the same, nothing sent
        remake();
        mRelay.has = false;
        setKey(HealthService::SyncStateKey, QStringLiteral("idle"));
        QVERIFY(!mService->requestSync(QStringLiteral("wo")));
        QCOMPARE(state(), QStringLiteral("idle"));
        QVERIFY(!mService->requestSync(QStringLiteral("button")));
        QCOMPARE(state(), QStringLiteral("failed"));
        QVERIFY(mRelay.acts.isEmpty());
    }

    void oneAtATimeAndTheQuietMinute()
    {
        QVERIFY(mService->requestSync(QStringLiteral("today")));
        QCOMPARE(mRelay.acts.size(), 1);
        QCOMPARE(mRelay.acts[0], QStringLiteral("{\"a\":\"hsync\",\"ev\":\"today\"}"));
        QCOMPARE(state(), QStringLiteral("syncing"));
        // the phone started it; until its end nothing more is asked
        mRelay.answer("{\"ok\":true}");
        QCOMPARE(state(), QStringLiteral("syncing"));
        QVERIFY(!mService->requestSync(QStringLiteral("button")));
        QVERIFY(!mService->requestSync(QStringLiteral("wo")));
        QCOMPARE(mRelay.acts.size(), 1);
        end();
        QCOMPARE(state(), QStringLiteral("idle"));
        // within a minute of the end: wo, sl and today wait, the button does not
        QVERIFY(!mService->requestSync(QStringLiteral("today")));
        QVERIFY(!mService->requestSync(QStringLiteral("sl")));
        QVERIFY(mService->requestSync(QStringLiteral("button")));
        QCOMPARE(mRelay.lastEvent(), QStringLiteral("button"));
        end();
        // a minute later: again
        setKey(HealthService::SyncLastKey, QDateTime::currentMSecsSinceEpoch() - HealthService::SyncQuietMs - 1);
        QVERIFY(mService->requestSync(QStringLiteral("today")));
        QCOMPARE(mRelay.acts.size(), 3);
    }

    void failures()
    {
        // the phone refuses
        QVERIFY(mService->requestSync(QStringLiteral("button")));
        mRelay.answer("{\"ok\":false,\"er\":\"failed\"}");
        QCOMPARE(state(), QStringLiteral("failed"));
        // the relay's own timeout or the phone gone before it answered
        QVERIFY(mService->requestSync(QStringLiteral("button")));
        mRelay.answer("{\"ok\":false,\"er\":\"timeout\"}");
        QCOMPARE(state(), QStringLiteral("failed"));
        // the link drops while it runs
        QVERIFY(mService->requestSync(QStringLiteral("button")));
        mRelay.answer("{\"ok\":true}");
        mService->phoneConnectedChanged(false);
        QCOMPARE(state(), QStringLiteral("failed"));
        // the phone never ends it
        QVERIFY(mService->requestSync(QStringLiteral("button")));
        QVERIFY(QMetaObject::invokeMethod(mService, "onSyncTimeout"));
        QCOMPARE(state(), QStringLiteral("failed"));
        // and the next one goes out (none waits any more)
        QVERIFY(mService->requestSync(QStringLiteral("button")));
        end();
        QCOMPARE(state(), QStringLiteral("idle"));
        // a late answer of a finished one changes nothing
        mRelay.answer("{\"ok\":false,\"er\":\"timeout\"}");
        QCOMPARE(state(), QStringLiteral("idle"));
    }

    void theHealthAppsKey()
    {
        setKey(HealthService::SyncRequestKey, QStringLiteral("today@1795600000000"));
        QTRY_COMPARE(mRelay.acts.size(), 1);
        QCOMPARE(mRelay.lastEvent(), QStringLiteral("today"));
        end();
        setKey(HealthService::SyncRequestKey, QStringLiteral("button@1795600001000"));
        QTRY_COMPARE(mRelay.acts.size(), 2);
        QCOMPARE(mRelay.lastEvent(), QStringLiteral("button"));
        end();
        // anything else asks for nothing
        setKey(HealthService::SyncRequestKey, QStringLiteral("wo@1795600002000"));
        setKey(HealthService::SyncRequestKey, QStringLiteral("sync"));
        QTest::qWait(200);
        QCOMPARE(mRelay.acts.size(), 2);
    }

    void workoutsAndNights()
    {
        // nothing new since the service was made
        mService->healthChanged(40);
        QVERIFY(mRelay.acts.isEmpty());
        // a workout begins: nothing; it ends: wo
        QVERIFY(runSql(db(), "INSERT INTO workout (start_ms, type, source, gps) VALUES (1768200000000, 'Run', 'manual', 0);"));
        mService->healthChanged(41);
        QVERIFY(mRelay.acts.isEmpty());
        QVERIFY(runSql(db(), "UPDATE workout SET end_ms = 1768201800000, active_s = 1800 WHERE start_ms = 1768200000000;"));
        mService->healthChanged(42);
        QCOMPARE(mRelay.acts.size(), 1);
        QCOMPARE(mRelay.lastEvent(), QStringLiteral("wo"));
        mRelay.answer("{\"ok\":true}");
        end();
        setKey(HealthService::SyncLastKey, 0);
        // the same workout changed again (its laps, a deletion): no new event
        QVERIFY(runSql(db(), "UPDATE workout SET kcal = 210.0 WHERE start_ms = 1768200000000;"));
        mService->healthChanged(43);
        QVERIFY(runSql(db(), "UPDATE workout SET deleted = 1 WHERE start_ms = 1768200000000;"));
        mService->healthChanged(44);
        QCOMPARE(mRelay.acts.size(), 1);
        // the night estimated again: no event; a new main night: sl; a nap: none
        QVERIFY(runSql(db(), "UPDATE sleep_session SET score = 77 WHERE deleted = 0 AND main = 1;"));
        mService->healthChanged(45);
        QCOMPARE(mRelay.acts.size(), 1);
        QVERIFY(runSql(db(), "INSERT INTO sleep_session (wake_date, start_ms, end_ms, main, asleep_min) "
                             "VALUES ('2026-01-13', 1768250000000, 1768275000000, 0, 60);"));
        mService->healthChanged(46);
        QCOMPARE(mRelay.acts.size(), 1);
        QVERIFY(runSql(db(), "INSERT INTO sleep_session (wake_date, start_ms, end_ms, main, asleep_min) "
                             "VALUES ('2026-01-14', 1768340000000, 1768368000000, 1, 430);"));
        mService->healthChanged(47);
        QCOMPARE(mRelay.acts.size(), 2);
        QCOMPARE(mRelay.lastEvent(), QStringLiteral("sl"));
    }

    void noDatabaseFirst()
    {
        // health.db made after btsyncd started: what it has first is no event, what comes after is
        QFile::remove(db());
        QFile::remove(db() + "-wal");
        QFile::remove(db() + "-shm");
        remake();
        mService->healthChanged(1);
        QVERIFY(mRelay.acts.isEmpty());
        QVERIFY(runSql(db(), fixtureFile("health-v3.sql")));
        mService->healthChanged(40);
        QVERIFY(mRelay.acts.isEmpty());
        QVERIFY(runSql(db(), "INSERT INTO workout (start_ms, end_ms, type) VALUES (1768300000000, 1768301000000, 'Bike');"));
        mService->healthChanged(41);
        QCOMPARE(mRelay.lastEvent(), QStringLiteral("wo"));
    }

    void sensorlogdsSignal()
    {
        // HealthChanged from the well-known name, as sensorlogd sends it
        QDBusConnection peer = QDBusConnection::connectToBus(QDBusConnection::SessionBus, QStringLiteral("fake-sensorlogd"));
        QVERIFY(peer.registerService(QStringLiteral("org.asteroid.sensorlogd.logger")));
        QVERIFY(runSql(db(), "INSERT INTO workout (start_ms, end_ms, type) VALUES (1768400000000, 1768401000000, 'Walk');"));
        QDBusMessage signal = QDBusMessage::createSignal(QStringLiteral("/org/asteroid/sensorlogd/logger"),
                                                         QStringLiteral("org.asteroid.sensorlogd.logger"),
                                                         QStringLiteral("HealthChanged"));
        signal << qlonglong(50);
        QVERIFY(peer.send(signal));
        QTRY_COMPARE(mRelay.acts.size(), 1);
        QCOMPARE(mRelay.lastEvent(), QStringLiteral("wo"));
        peer.unregisterService(QStringLiteral("org.asteroid.sensorlogd.logger"));
        QDBusConnection::disconnectFromBus(QStringLiteral("fake-sensorlogd"));
    }

    void restartedWhileSyncing()
    {
        // btsyncd stopped while it waited: the row does not stay at Syncing
        setKey(HealthService::SyncStateKey, QStringLiteral("syncing"));
        remake();
        QCOMPARE(state(), QStringLiteral("idle"));
    }
};

BTSYNCD_TEST_MAIN(TestHealthSync)
#include "tst_healthsync.moc"
