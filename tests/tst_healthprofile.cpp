/*
 * Copyright (C) 2026 - eos
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

/*
 * The profile and goals from the phone (M4 WO-11, healthprofile.h):
 *   - the <prof> message: every element, any order, unknown ones skipped,
 *     values out of range left out, not well-formed or not a profile
 *   - who wins: a group (profile, steps goal, sleep goal) is written when the
 *     phone's differs from what it sent last (or nothing was sent yet), so a
 *     value set on the watch after it stays until the phone's changes; a group
 *     not sent changes nothing; a wiped dconf (factory reset) takes it again
 *   - the health request characteristic: a long write in pieces, no answer, an
 *     unanswered request's answer untouched, a profile cut by a new request
 *   - Gadgetbridge's frames (fixtures/health-profile.frames.txt, from the gb
 *     repository's AsteroidOSProfileTest) with watch edits between them, and
 *     the capabilities it reads (fixtures/health-capabilities.frames.txt;
 *     FIXTURES_WRITE=1 writes that file again)
 * Every profile here is invented.
 */
#include "fixtures.h"
#include "healthprofile.h"
#include "healthservice.h"
#include "mdconfitem.h"
#include "testmain.h"

#include <QFile>
#include <QTemporaryDir>
#include <QTest>

using namespace HealthProfile;

static QVariant key(const char *k)
{
    return MDConfItem(QString::fromLatin1(k)).value();
}

static void setKey(const char *k, const QVariant &v)
{
    MDConfItem(QString::fromLatin1(k)).set(v);
}

class TestHealthProfile : public QObject
{
    Q_OBJECT

private:
    QTemporaryDir mDir;
    HealthService *mService = nullptr;
    Characteristic *mRequest = nullptr;
    HealthDataChrc *mData = nullptr;
    Characteristic *mCaps = nullptr;
    QList<QByteArray> mChunks;
    const QDate mToday = QDate(2026, 3, 10);

    static Values parsed(const QByteArray &xml, bool *ok = nullptr, const QDate &today = QDate(2026, 3, 10))
    {
        Values v;
        const bool r = parse(xml, &v, today);
        if (ok)
            *ok = r;
        return v;
    }

    // a long write as BlueZ hands it over: pieces with their offsets
    void writeInPieces(const QByteArray &value, int piece)
    {
        for (int pos = 0; pos < value.size(); pos += piece)
            mRequest->WriteValue(value.mid(pos, piece), {{"offset", pos}, {"mtu", piece + 3}});
    }

    void write(const QByteArray &value) { mRequest->WriteValue(value, {{"mtu", 100}}); }

    // the watch as its own Settings and Health app left it before any phone wrote
    static void watchOwnValues()
    {
        setKey(GenderKey, QStringLiteral("male"));
        setKey(HeightKey, 175);
        setKey(WeightKey, 75.0);
        setKey(StepsGoalKey, 10000);
    }

    static void expectWatch(const QString &gender, int heightCm, double weightKg, const QString &born, int steps, const QVariant &sleep)
    {
        QCOMPARE(key(GenderKey).toString(), gender);
        QCOMPARE(key(HeightKey).toInt(), heightCm);
        QCOMPARE(key(WeightKey).toDouble(), weightKg);
        QCOMPARE(key(BirthDateKey).toString(), born);
        QCOMPARE(key(StepsGoalKey).toInt(), steps);
        QCOMPARE(key(SleepGoalKey), sleep);
    }

private slots:
    void initTestCase()
    {
        QVERIFY(mDir.isValid());
        mService = new HealthService(6, QDBusConnection::systemBus(), this);
        const QList<Characteristic *> chrcs = mService->getCharacteristics();
        QCOMPARE(chrcs.size(), 3);
        mRequest = chrcs[0];
        mData = qobject_cast<HealthDataChrc *>(chrcs[1]);
        mCaps = chrcs[2];
        QVERIFY(mData);
        connect(mData, &HealthDataChrc::valueChanged, this, [this] {
            mChunks << mData->property("Value").toByteArray();
        });
    }

    void init()
    {
        mChunks.clear();
        MDConfItem::reset();
        MDConfItem("/org/asteroidos/btsyncd/health-data-dir").set(mDir.path());
    }

    /* the message */

    void fullProfile()
    {
        bool ok = false;
        const Values v = parsed("<prof><gd>female</gd><hc>168</hc><wk>61</wk><bd>1991-04-17</bd><sg>9000</sg>"
                                "<sl>450</sl></prof>", &ok);
        QVERIFY(ok);
        QCOMPARE(v.gender, QStringLiteral("female"));
        QCOMPARE(v.heightCm, 168);
        QCOMPARE(v.weightKg, 61.0);
        QCOMPARE(v.birthDate, QDate(1991, 4, 17));
        QCOMPARE(v.stepsGoal, 9000);
        QCOMPARE(v.sleepGoalMin, 450);
        QVERIFY(v.hasProfile());
        QCOMPARE(profileText(v), QByteArray("gd=female;hc=168;wk=61.0;bd=1991-04-17;"));
    }

    void anyOrderUnknownElementsAndSpaces()
    {
        bool ok = false;
        const Values v = parsed("<prof><x a=\"1\"><y>2</y></x><sl> 480 </sl><future>7</future><gd>male</gd>"
                                "<wk>80.26</wk><hc>175.4</hc></prof>", &ok);
        QVERIFY(ok);
        QCOMPARE(v.gender, QStringLiteral("male"));
        QCOMPARE(v.heightCm, 175);
        QCOMPARE(v.weightKg, 80.3);
        QVERIFY(!v.birthDate.isValid());
        QCOMPARE(v.stepsGoal, 0);
        QCOMPARE(v.sleepGoalMin, 480);
    }

    void outOfRange_data()
    {
        QTest::addColumn<QByteArray>("xml");
        QTest::newRow("gender case") << QByteArray("<prof><gd>Female</gd></prof>");
        QTest::newRow("gender other word") << QByteArray("<prof><gd>non-binary</gd></prof>");
        QTest::newRow("height low") << QByteArray("<prof><hc>29</hc></prof>");
        QTest::newRow("height high") << QByteArray("<prof><hc>273</hc></prof>");
        QTest::newRow("height text") << QByteArray("<prof><hc>tall</hc></prof>");
        QTest::newRow("height nan") << QByteArray("<prof><hc>nan</hc></prof>");
        QTest::newRow("weight zero") << QByteArray("<prof><wk>0</wk></prof>");
        QTest::newRow("weight high") << QByteArray("<prof><wk>907.1</wk></prof>");
        QTest::newRow("born today") << QByteArray("<prof><bd>2026-03-10</bd></prof>");
        QTest::newRow("born later") << QByteArray("<prof><bd>2999-01-01</bd></prof>");
        QTest::newRow("born 1899") << QByteArray("<prof><bd>1899-12-31</bd></prof>");
        QTest::newRow("no such day") << QByteArray("<prof><bd>1991-02-30</bd></prof>");
        QTest::newRow("date and time") << QByteArray("<prof><bd>1991-04-17T00:00:00</bd></prof>");
        QTest::newRow("steps zero") << QByteArray("<prof><sg>0</sg></prof>");
        QTest::newRow("steps half") << QByteArray("<prof><sg>9000.5</sg></prof>");
        QTest::newRow("steps high") << QByteArray("<prof><sg>100001</sg></prof>");
        QTest::newRow("sleep short") << QByteArray("<prof><sl>59</sl></prof>");
        QTest::newRow("sleep long") << QByteArray("<prof><sl>1441</sl></prof>");
        QTest::newRow("empty") << QByteArray("<prof><gd></gd><hc/></prof>");
    }

    void outOfRange()
    {
        QFETCH(QByteArray, xml);
        bool ok = false;
        const Values v = parsed(xml, &ok);
        QVERIFY(ok);
        QVERIFY(!v.hasProfile());
        QCOMPARE(v.stepsGoal, 0);
        QCOMPARE(v.sleepGoalMin, 0);
        QCOMPARE(profileText(v), QByteArray());
    }

    void edgesTaken()
    {
        const Values v = parsed("<prof><hc>30</hc><wk>907</wk><bd>1900-01-01</bd><sg>100000</sg><sl>1440</sl></prof>");
        QCOMPARE(v.heightCm, 30);
        QCOMPARE(v.weightKg, 907.0);
        QCOMPARE(v.birthDate, QDate(1900, 1, 1));
        QCOMPARE(v.stepsGoal, 100000);
        QCOMPARE(v.sleepGoalMin, 1440);
        QCOMPARE(parsed("<prof><hc>272</hc><wk>1</wk><bd>2026-03-09</bd><sg>1</sg><sl>60</sl></prof>").birthDate,
                 QDate(2026, 3, 9));
    }

    void notAProfile_data()
    {
        QTest::addColumn<QByteArray>("xml");
        QTest::newRow("a request") << QByteArray("<req><ty>hr</ty></req>");
        QTest::newRow("broken") << QByteArray("<prof><gd>female</hc></prof>");
        QTest::newRow("unclosed") << QByteArray("<prof><gd>female</gd>");
        QTest::newRow("child in a value") << QByteArray("<prof><gd><b>female</b></gd></prof>");
        QTest::newRow("nothing") << QByteArray();
    }

    void notAProfile()
    {
        QFETCH(QByteArray, xml);
        bool ok = true;
        parsed(xml, &ok);
        QVERIFY(!ok);
        QCOMPARE(mService->profile(xml), -1);
        QVERIFY(MDConfItem::store().size() == 1);   // the data directory only
    }

    void emptyElementIsAProfileOfNothing()
    {
        bool ok = false;
        QVERIFY(!parsed("<prof/>", &ok).hasProfile());
        QVERIFY(ok);
        QCOMPARE(mService->profile("<prof/>"), (int)None);
        QCOMPARE(mService->profile("<prof></prof>"), (int)None);
        QCOMPARE(MDConfItem::sets(), 1);
    }

    /* who wins */

    void firstPhoneProfileWins()
    {
        watchOwnValues();
        const int sets = MDConfItem::sets();
        QCOMPARE(mService->profile("<prof><gd>female</gd><hc>168</hc><wk>61</wk><bd>1991-04-17</bd><sg>9000</sg>"
                                   "<sl>450</sl></prof>"),
                 Profile | StepsGoal | SleepGoal);
        expectWatch("female", 168, 61.0, "1991-04-17", 9000, 450);
        // what was sent: the profile as its hash only, the goals
        QCOMPARE(key(PhoneProfileKey).toString().size(), 64);
        QCOMPARE(key(PhoneStepsGoalKey).toInt(), 9000);
        QCOMPARE(key(PhoneSleepGoalKey).toInt(), 450);
        // four profile keys and the hash, each goal and what was sent of it
        QCOMPARE(MDConfItem::sets() - sets, 5 + 2 + 2);
        // the weight as dconf's double, the height and goals as integers
        QCOMPARE(key(WeightKey).typeId(), QMetaType::Double);
        QCOMPARE(key(HeightKey).typeId(), QMetaType::Int);
        QCOMPARE(key(StepsGoalKey).typeId(), QMetaType::Int);
    }

    void unchangedPhoneKeepsTheWatchEdits()
    {
        const QByteArray full = "<prof><gd>female</gd><hc>168</hc><wk>61</wk><bd>1991-04-17</bd><sg>9000</sg><sl>450</sl></prof>";
        QCOMPARE(mService->profile(full), Profile | StepsGoal | SleepGoal);
        // the user changes the weight in Settings and the goal in the Health app
        setKey(WeightKey, 65.5);
        setKey(StepsGoalKey, 11000);
        const int sets = MDConfItem::sets();
        // the next connection sends the same: nothing is written
        QCOMPARE(mService->profile(full), (int)None);
        QCOMPARE(MDConfItem::sets(), sets);
        expectWatch("female", 168, 65.5, "1991-04-17", 11000, 450);
        // the phone's weight changes: the profile is written as a whole (stock pushes the four together),
        // the unchanged steps goal is not
        QCOMPARE(mService->profile("<prof><gd>female</gd><hc>168</hc><wk>63</wk><bd>1991-04-17</bd><sg>9000</sg>"
                                   "<sl>450</sl></prof>"),
                 (int)Profile);
        expectWatch("female", 168, 63.0, "1991-04-17", 11000, 450);
        // the phone's goal changes: written
        QCOMPARE(mService->profile("<prof><gd>female</gd><hc>168</hc><wk>63</wk><bd>1991-04-17</bd><sg>9500</sg>"
                                   "<sl>450</sl></prof>"),
                 (int)StepsGoal);
        expectWatch("female", 168, 63.0, "1991-04-17", 9500, 450);
        // back to a value sent before (not the last one): a change, written
        QCOMPARE(mService->profile(full), Profile | StepsGoal);
        expectWatch("female", 168, 61.0, "1991-04-17", 9000, 450);
    }

    void groupsNotSentChangeNothing()
    {
        watchOwnValues();
        setKey(SleepGoalKey, 420);
        // a phone with only the height and the steps goal set
        QCOMPARE(mService->profile("<prof><hc>181</hc><sg>12000</sg></prof>"), Profile | StepsGoal);
        expectWatch("male", 181, 75.0, QString(), 12000, 420);
        QVERIFY(!key(BirthDateKey).isValid());
        QVERIFY(!key(PhoneSleepGoalKey).isValid());
        // nothing at all
        const int sets = MDConfItem::sets();
        QCOMPARE(mService->profile("<prof></prof>"), (int)None);
        QCOMPARE(MDConfItem::sets(), sets);
        // values the watch does not take are not sent for it: the rest is
        QCOMPARE(mService->profile("<prof><gd>other</gd><hc>400</hc><wk>2000</wk><bd>2999-01-01</bd><sg>9000</sg>"
                                   "<sl>30</sl></prof>"),
                 Profile | StepsGoal);
        expectWatch("other", 181, 75.0, QString(), 9000, 420);
    }

    void sameValueIsNotWrittenAgain()
    {
        setKey(GenderKey, QStringLiteral("female"));
        setKey(HeightKey, 168);
        setKey(WeightKey, 61.0);
        const int sets = MDConfItem::sets();
        // the watch has the phone's values already: only the hash is written
        QCOMPARE(mService->profile("<prof><gd>female</gd><hc>168</hc><wk>61</wk></prof>"), (int)Profile);
        QCOMPARE(MDConfItem::sets() - sets, 1);
        // a weight Settings left as an integer becomes dconf's double
        setKey(WeightKey, 61);
        MDConfItem(QString::fromLatin1(PhoneProfileKey)).unset();
        QCOMPARE(mService->profile("<prof><gd>female</gd><hc>168</hc><wk>61</wk></prof>"), (int)Profile);
        QCOMPARE(key(WeightKey).typeId(), QMetaType::Double);
    }

    void wipedWatchTakesThePhoneAgain()
    {
        const QByteArray full = "<prof><gd>female</gd><hc>168</hc><wk>61</wk><bd>1991-04-17</bd><sg>9000</sg><sl>450</sl></prof>";
        QCOMPARE(mService->profile(full), Profile | StepsGoal | SleepGoal);
        // a factory reset wipes dconf: the next connection writes all of it again
        MDConfItem::reset();
        QCOMPARE(mService->profile(full), Profile | StepsGoal | SleepGoal);
        expectWatch("female", 168, 61.0, "1991-04-17", 9000, 450);
    }

    /* the request characteristic */

    void longWriteInPiecesHasNoAnswer()
    {
        watchOwnValues();
        writeInPieces("<prof><gd>female</gd><hc>168</hc><wk>61</wk><bd>1991-04-17</bd><sg>9000</sg><sl>450</sl></prof>", 20);
        QTest::qWait(50);
        QVERIFY(mChunks.isEmpty());
        expectWatch("female", 168, 61.0, "1991-04-17", 9000, 450);
    }

    void unansweredRequestIsKept()
    {
        // the profile comes while a request's answer waits for the event loop: that answer goes out as it is
        write("<req><ty>hr</ty><si>0</si></req>");
        write("<prof><gd>female</gd><sg>9000</sg></prof>");
        QVERIFY(QTest::qWaitFor([this] { return !mChunks.isEmpty(); }, 2000));
        QTest::qWait(50);
        QByteArray all;
        for (const QByteArray &c : std::as_const(mChunks))
            all += c;
        QCOMPARE(all.count("<data>"), 1);
        QVERIFY2(all.startsWith("<data><ty>hr</ty>") && all.endsWith("</data>"), all);
        QCOMPARE(key(GenderKey).toString(), QStringLiteral("female"));
        QCOMPARE(key(StepsGoalKey).toInt(), 9000);
        // both in one write
        mChunks.clear();
        write("<prof><sl>450</sl></prof><req><ty>steps</ty><si>0</si></req>");
        QVERIFY(QTest::qWaitFor([this] { return !mChunks.isEmpty(); }, 2000));
        QVERIFY(mChunks.first().startsWith("<data><ty>steps</ty>"));
        QCOMPARE(key(SleepGoalKey).toInt(), 450);
    }

    void profileCutByANewRequest()
    {
        watchOwnValues();
        write("<prof><gd>female</gd><hc>16");
        write("<req><ty>hr</ty><si>0</si></req>");
        QVERIFY(QTest::qWaitFor([this] { return !mChunks.isEmpty(); }, 2000));
        QVERIFY(mChunks.first().startsWith("<data><ty>hr</ty>"));
        expectWatch("male", 175, 75.0, QString(), 10000, QVariant());
        // and a disconnection drops one left incomplete
        write("<prof><gd>female</gd>");
        QMetaObject::invokeMethod(mService, "phoneConnectedChanged", Q_ARG(bool, false));
        write("</prof>");
        QTest::qWait(20);
        QCOMPARE(key(GenderKey).toString(), QStringLiteral("male"));
    }

    void brokenProfileOverTheCharacteristic()
    {
        watchOwnValues();
        write("<prof><gd>female</hc></prof>");
        QTest::qWait(50);
        QVERIFY(mChunks.isEmpty());
        QCOMPARE(key(GenderKey).toString(), QStringLiteral("male"));
    }

    /* Gadgetbridge */

    // What Gadgetbridge reads (its AsteroidOSProfileTest: it sends the profile only to a watch that takes it)
    void capabilitiesFixture()
    {
        const QByteArray caps = mCaps->ReadValue({});
        QVERIFY(caps.contains("<takes>prof,synced</takes>"));
        if (qEnvironmentVariableIntValue("FIXTURES_WRITE") == 1) {
            QFile f(QStringLiteral(FIXTURE_DIR "/health-capabilities.frames.txt"));
            QVERIFY(f.open(QIODevice::WriteOnly | QIODevice::Truncate));
            f.write(caps + "\n");
            QSKIP("fixture written");
        }
        const QList<QByteArray> frames = fixtureFrames("health-capabilities.frames.txt");
        QCOMPARE(frames.size(), 1);
        QCOMPARE(frames.first(), caps);
    }

    // Gadgetbridge's writes (AsteroidOSProfileTest) in order, with the watch's own edits between them
    void gadgetbridgeFrames()
    {
        const QList<QByteArray> frames = fixtureFrames("health-profile.frames.txt");
        QCOMPARE(frames.size(), 5);
        for (const QByteArray &f : frames)
            QVERIFY2(f.startsWith("<prof>") && f.endsWith("</prof>") && f.size() <= 120, f);
        watchOwnValues();
        // 1: the phone's whole profile, the first one: written
        writeInPieces(frames[0], 20);
        expectWatch("female", 168, 61.0, "1991-04-17", 9000, 450);
        // the user edits the weight in Settings and the goal in the Health app
        setKey(WeightKey, 65.5);
        setKey(StepsGoalKey, 11000);
        // 2: the next connection, unchanged: the watch's edits stay
        writeInPieces(frames[1], 20);
        expectWatch("female", 168, 65.5, "1991-04-17", 11000, 450);
        // 3: the weight changed on the phone: the profile is written, the steps goal (unchanged there) is not
        writeInPieces(frames[2], 20);
        expectWatch("female", 168, 63.0, "1991-04-17", 11000, 450);
        // 4: a phone with only the height and the steps goal set
        writeInPieces(frames[3], 20);
        expectWatch("female", 181, 63.0, "1991-04-17", 12000, 450);
        // 5: values the watch does not take (height, weight, a birth date to come, a 30-minute sleep goal)
        writeInPieces(frames[4], 20);
        expectWatch("other", 181, 63.0, "1991-04-17", 9000, 450);
        QTest::qWait(20);
        QVERIFY(mChunks.isEmpty());
    }

    /* R5 E-35: the units */

    void units()
    {
        QCOMPARE(QByteArray(ImperialUnitsKey), QByteArray("/org/asteroidos/settings/imperial-units"));
        QCOMPARE(parsed("<prof><un>imperial</un></prof>").units, QStringLiteral("imperial"));
        QCOMPARE(parsed("<prof><un> metric </un><gd>male</gd></prof>").units, QStringLiteral("metric"));
        // anything else is no unit; the units alone are no profile
        QVERIFY(parsed("<prof><un>Imperial</un></prof>").units.isEmpty());
        QVERIFY(parsed("<prof><un>us</un></prof>").units.isEmpty());
        QVERIFY(!parsed("<prof><un>metric</un></prof>").hasProfile());
        // a phone without them (Gadgetbridge before R5 E-35): the key stays unset, the apps take the locale's
        QCOMPARE(apply(parsed("<prof><gd>female</gd></prof>")) & Units, 0);
        QVERIFY(!key(ImperialUnitsKey).isValid());
        QCOMPARE(apply(parsed("<prof><un>imperial</un></prof>")), int(Units));
        QCOMPARE(key(ImperialUnitsKey).toBool(), true);
        QCOMPARE(key(PhoneUnitsKey).toString(), QStringLiteral("imperial"));
        QCOMPARE(apply(parsed("<prof><un>imperial</un></prof>")), int(None));
        QCOMPARE(apply(parsed("<prof><un>metric</un></prof>")), int(Units));
        QCOMPARE(key(ImperialUnitsKey).toBool(), false);
    }

    // Gadgetbridge's unit frames (its AsteroidOSProfileTest: About you never filled, then a height entered)
    void unitFrames()
    {
        const QList<QByteArray> frames = fixtureFrames("health-profile-units.frames.txt");
        QCOMPARE(frames.size(), 3);
        watchOwnValues();
        // 1: the placeholders of About you's screen are not sent: the watch's profile stays, the units are written
        writeInPieces(frames[0], 20);
        expectWatch("male", 175, 75.0, QString(), 10000, QVariant());
        QCOMPARE(key(ImperialUnitsKey).toBool(), true);
        // the watch's own setting (no Settings row writes it today; an override) stays while the phone's is the same
        setKey(ImperialUnitsKey, false);
        writeInPieces(frames[1], 20);
        QCOMPARE(key(ImperialUnitsKey).toBool(), false);
        // 3: metric and a height: both written
        setKey(ImperialUnitsKey, true);
        writeInPieces(frames[2], 20);
        QCOMPARE(key(ImperialUnitsKey).toBool(), false);
        expectWatch("male", 176, 75.0, QString(), 10000, QVariant());
        QTest::qWait(20);
        QVERIFY(mChunks.isEmpty());
    }
};

BTSYNCD_TEST_MAIN(TestHealthProfile)
#include "tst_healthprofile.moc"
