/*
 * Copyright (C) 2026 eos-mainline contributors
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

/*
 * R5 E-19 and E-20: the health types of the phone sync added to version 3,
 *   temp  the skin temperature readings of health.db's skin_temp (by time,
 *         milliseconds, hundredths of a degree; as hrr)
 *   dly   a day's totals of health.db's daily (by change sequence; as rhr,
 *         with steps, energy, distance, floors, AZM, hours, resting heart
 *         rate and the skin temperature variation)
 * on fixtures/health-v3.sql with invented readings and a day added. The
 * requests Gadgetbridge writes and the answers it reads are
 * fixtures/health-extra-{requests,answers}.frames.txt (FIXTURES_WRITE=1 writes
 * them again); its AsteroidOSHealthSyncTest stores them.
 */
#include "fixtures.h"
#include "healthchanges.h"
#include "healthrecords.h"
#include "healthservice.h"
#include "mdconfitem.h"
#include "testmain.h"

#include <QFile>
#include <QTemporaryDir>
#include <QTest>

#include <sqlite3.h>

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

// the invented night's readings (one out of range) and a day with only its skin temperature variation
static const char EXTRA_SQL[] =
    "INSERT INTO skin_temp (t_ms, c) VALUES (1768089000000, 33.41), (1768089060000, 33.47), (1768089120000, 33.5),"
    " (1768089180000, 72.0), (1768089240000, 34.125);"
    "INSERT INTO daily (date, temp_dev_c) VALUES ('2026-01-11', -0.4) ON CONFLICT (date) DO UPDATE SET"
    " temp_dev_c = excluded.temp_dev_c WHERE temp_dev_c IS NOT excluded.temp_dev_c;";

static const char TEMP_ALL[] =
    "<data><ty>temp</ty><t0>1768089000000</t0><n>4</n><mo>0</mo><r>0,3341;60000,3347;60000,3350;120000,3413</r></data>";
static const char DLY_ALL[] =
    "<data><ty>dly</ty><hs>628</hs><sq>628</sq><n>4</n><mo>0</mo>"
    "<d sq=\"3\" dt=\"2026-01-08\" st=\"6020\" kc=\"1980.0\" dm=\"4510.0\" fl=\"3\" az=\"12\" hm=\"7\"/>"
    "<d sq=\"5\" dt=\"2026-01-09\" st=\"9310\" kc=\"2205.5\" dm=\"7020.0\" fl=\"5\" az=\"41\" hm=\"8\" rh=\"55\"/>"
    "<d sq=\"588\" dt=\"2026-01-10\" st=\"11234\" kc=\"2310.5\" dm=\"8650.0\" fl=\"7\" az=\"58\" hm=\"9\" rh=\"53\"/>"
    "<d sq=\"628\" dt=\"2026-01-11\" tv=\"-0.4\"/></data>";

class TestHealthTypes : public QObject
{
    Q_OBJECT

private:
    QTemporaryDir mDir;
    HealthService *mService = nullptr;
    Characteristic *mRequest = nullptr;
    HealthDataChrc *mData = nullptr;
    QByteArray mAnswer;

    QString root() const { return mDir.path() + QLatin1Char('/'); }

    // a request as the phone writes it, and the whole answer the notifications carry
    QByteArray ask(const QByteArray &request)
    {
        mAnswer.clear();
        mRequest->WriteValue(request, {{"mtu", 185}});
        if (!QTest::qWaitFor([this] { return mAnswer.endsWith("</data>"); }, 5000))
            return QByteArray();
        return mAnswer;
    }

private slots:
    void initTestCase()
    {
        QVERIFY(mDir.isValid());
        MDConfItem::reset();
        MDConfItem("/org/asteroidos/btsyncd/health-data-dir").set(mDir.path());
        QVERIFY(runSql(root() + "health.db", fixtureFile("health-v3.sql")));
        QVERIFY(runSql(root() + "health.db", EXTRA_SQL));
        mService = new HealthService(6, QDBusConnection::systemBus(), this);
        const QList<Characteristic *> chrcs = mService->getCharacteristics();
        mRequest = chrcs[0];
        mData = qobject_cast<HealthDataChrc *>(chrcs[1]);
        QVERIFY(mData);
        connect(mData, &HealthDataChrc::valueChanged, this, [this] { mAnswer += mData->property("Value").toByteArray(); });
    }

    void capabilities()
    {
        const QByteArray caps = mService->getCharacteristics()[2]->ReadValue({});
        QVERIFY2(caps.contains("<version>3</version><types>hr,steps,hrr,wo,wot,sl,rhr,temp,dly</types>"), caps);
        QCOMPARE(HealthRecords::typeFromName(QStringLiteral("temp")), HealthRecords::SkinTemperature);
        QCOMPARE(HealthRecords::typeName(HealthRecords::SkinTemperature), QStringLiteral("temp"));
        QCOMPARE(HealthChanges::kindFromName(QStringLiteral("dly")), HealthChanges::DailyTotals);
        QCOMPARE(HealthChanges::kindName(HealthChanges::DailyTotals), QByteArray("dly"));
    }

    void skinTemperature()
    {
        QCOMPARE(ask("<req><ty>temp</ty><si>0</si></req>"), QByteArray(TEMP_ALL));
        // in pages: never a reading twice, the out-of-range one never
        QCOMPARE(ask("<req><ty>temp</ty><si>0</si><mx>2</mx></req>"),
                 QByteArray("<data><ty>temp</ty><t0>1768089000000</t0><n>2</n><mo>1</mo><r>0,3341;60000,3347</r></data>"));
        QCOMPARE(ask("<req><ty>temp</ty><si>1768089060000</si><mx>2</mx></req>"),
                 QByteArray("<data><ty>temp</ty><t0>1768089120000</t0><n>2</n><mo>0</mo><r>0,3350;120000,3413</r></data>"));
        QCOMPARE(ask("<req><ty>temp</ty><si>1768089240000</si></req>"), QByteArray("<data><ty>temp</ty><n>0</n><mo>0</mo></data>"));
    }

    void dailyTotals()
    {
        QCOMPARE(ask("<req><ty>dly</ty><sq>0</sq></req>"), QByteArray(DLY_ALL));
        QCOMPARE(ask("<req><ty>dly</ty><sq>588</sq></req>"),
                 QByteArray("<data><ty>dly</ty><hs>628</hs><sq>628</sq><n>1</n><mo>0</mo><d sq=\"628\" dt=\"2026-01-11\" tv=\"-0.4\"/></data>"));
        // a day's row that changes later comes again with its new sq
        QVERIFY(runSql(root() + "health.db", "UPDATE daily SET steps = 412, kcal = 120.25 WHERE date = '2026-01-11';"));
        QCOMPARE(ask("<req><ty>dly</ty><sq>628</sq></req>"),
                 QByteArray("<data><ty>dly</ty><hs>629</hs><sq>629</sq><n>1</n><mo>0</mo>"
                            "<d sq=\"629\" dt=\"2026-01-11\" st=\"412\" kc=\"120.3\" tv=\"-0.4\"/></data>"));
        // rhr as before (an older phone)
        const QByteArray rhr = ask("<req><ty>rhr</ty><sq>588</sq></req>");
        QCOMPARE(rhr, QByteArray("<data><ty>rhr</ty><hs>629</hs><sq>629</sq><n>1</n><mo>0</mo><d sq=\"629\" dt=\"2026-01-11\"/></data>"));
        QVERIFY(runSql(root() + "health.db", "UPDATE daily SET steps = NULL, kcal = NULL WHERE date = '2026-01-11';"));
    }

    // Gadgetbridge's frames: the requests of its sync of temp and dly, and the answers of the database as
    // initTestCase made it (skinTemperature and dailyTotals asked for them)
    void gadgetbridgeFrames()
    {
        const QList<QByteArray> requests = {"<req><ty>temp</ty><si>0</si></req>", "<req><ty>dly</ty><sq>0</sq></req>"};
        const QList<QByteArray> answers = {TEMP_ALL, DLY_ALL};
        if (qEnvironmentVariableIntValue("FIXTURES_WRITE") == 1) {
            QFile q(QStringLiteral(FIXTURE_DIR "/health-extra-requests.frames.txt"));
            QFile a(QStringLiteral(FIXTURE_DIR "/health-extra-answers.frames.txt"));
            QVERIFY(q.open(QIODevice::WriteOnly | QIODevice::Truncate) && a.open(QIODevice::WriteOnly | QIODevice::Truncate));
            for (int i = 0; i < requests.size(); i++) {
                q.write(requests[i] + "\n");
                a.write(answers[i] + "\n");
            }
            QSKIP("fixtures written");
        }
        QCOMPARE(fixtureFrames("health-extra-requests.frames.txt"), requests);
        QCOMPARE(fixtureFrames("health-extra-answers.frames.txt"), answers);
    }
};

BTSYNCD_TEST_MAIN(TestHealthTypes)
#include "tst_healthtypes.moc"
