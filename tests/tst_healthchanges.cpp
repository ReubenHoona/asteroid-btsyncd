/*
 * Copyright (C) 2026 - eos
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

/*
 * Version 3 of the health sync (healthchanges.h): workouts with their laps,
 * pauses and GPS tracks, sleep sessions with their stage timelines and the
 * days' resting heart rates, from sensorlogd's health.db by its change
 * sequence:
 *   - Gadgetbridge's sync byte for byte: every request it records in
 *     fixtures/health-v3-requests.frames.txt (a copy of the gb repo's
 *     app/src/test/resources/asteroidos/frames/) gets exactly the line of
 *     health-v3-answers.frames.txt, from the databases fixtures/health-v3.sql
 *     and heartrate-v3.sql make, in notifications of MTU - 3 bytes; two syncs,
 *     the second with nothing new. FIXTURES_WRITE=1 writes both files again
 *     with a phone that asks as Gadgetbridge's AsteroidOSHealthSync does (its
 *     own test then says whether it still asks so)
 *   - paging: every row once, in seq order, whatever the page size and byte
 *     limit; no answer over 8192 bytes; a record alone over the limit is cut
 *   - a workout still on (sent once it has ended), tombstones, the counter
 *     across restarts of sensorlogd, a database made again (hs below the
 *     phone's sq)
 *   - tracks: every point back from the deltas, unknown fields empty
 *   - no database, a broken one, a write of sensorlogd's while it is read
 *   - requests: wot without an id, bad numbers, a request cut by a disconnection
 * Records and positions (near 0,0) are invented.
 */
#include "fixtures.h"
#include "healthchanges.h"
#include "healthrecords.h"
#include "healthservice.h"
#include "mdconfitem.h"
#include "testmain.h"

#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QHash>
#include <QRegularExpression>
#include <QSet>
#include <QTemporaryDir>
#include <QTest>
#include <QVariant>

#include <sqlite3.h>

using namespace HealthChanges;

static const char DATA_DIR_KEY[] = "/org/asteroidos/btsyncd/health-data-dir";

// the fixture's workouts (ids in the order the watch made them) and times
static const qint64 WALK = 2, TREADMILL = 3, YOGA = 4, OPEN_WALK = 6;
static const qint64 OPEN_WALK_START = 1768147200000LL;

static QByteArray num(qint64 v)
{
    return QByteArray::number(v);
}

// Runs SQL on the database at path (made if not there), as another connection: sensorlogd, or the one after its restart
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

// The rows of a query (NULL: an invalid QVariant)
static QList<QVariantList> rows(const QString &path, const QByteArray &sql)
{
    QList<QVariantList> out;
    sqlite3 *db = nullptr;
    if (sqlite3_open_v2(QFile::encodeName(path).constData(), &db, SQLITE_OPEN_READWRITE, nullptr) != SQLITE_OK) {
        sqlite3_close(db);
        return out;
    }
    sqlite3_stmt *st = nullptr;
    if (sqlite3_prepare_v2(db, sql.constData(), -1, &st, nullptr) == SQLITE_OK) {
        while (sqlite3_step(st) == SQLITE_ROW) {
            QVariantList row;
            for (int c = 0; c < sqlite3_column_count(st); c++)
                row << (sqlite3_column_type(st, c) == SQLITE_NULL ? QVariant() : QVariant(sqlite3_column_int64(st, c)));
            out << row;
        }
    }
    sqlite3_finalize(st);
    sqlite3_close(db);
    return out;
}

static qint64 scalar(const QString &path, const QByteArray &sql)
{
    const QList<QVariantList> r = rows(path, sql);
    return r.isEmpty() || r.first().isEmpty() ? -1 : r.first().first().toLongLong();
}

// <name>N</name> of an answer, -1 if none
static qint64 element(const QByteArray &xml, const char *name)
{
    const QByteArray open = QByteArray("<") + name + ">";
    const int from = xml.indexOf(open);
    if (from < 0)
        return -1;
    const int begin = from + open.size();
    return xml.mid(begin, xml.indexOf('<', begin) - begin).toLongLong();
}

// the last record's time of an hr, steps, hrr or wot answer (t0 plus the deltas), -1 without records
static qint64 lastTime(const QByteArray &xml)
{
    const qint64 t0 = element(xml, "t0");
    if (t0 < 0)
        return -1;
    const int a = xml.indexOf("<r>") + 3;
    qint64 t = t0;
    for (const QByteArray &record : xml.mid(a, xml.indexOf("</r>") - a).split(';'))
        t += record.left(record.indexOf(',')).toLongLong();
    return t;
}

struct Rec {
    QString tag;
    QHash<QString, QString> a;
    QString body;
};

// the records of a wo, sl or rhr answer
static QList<Rec> recordsOf(const QByteArray &xml)
{
    static const QRegularExpression re(QStringLiteral("<(w|s|d)((?: [a-z]+=\"[^\"]*\")*)(?:/>|>(.*?)</\\1>)"));
    static const QRegularExpression attr(QStringLiteral(" ([a-z]+)=\"([^\"]*)\""));
    QList<Rec> out;
    QRegularExpressionMatchIterator it = re.globalMatch(QString::fromUtf8(xml));
    while (it.hasNext()) {
        const QRegularExpressionMatch m = it.next();
        Rec r;
        r.tag = m.captured(1);
        QRegularExpressionMatchIterator ai = attr.globalMatch(m.captured(2));
        while (ai.hasNext()) {
            const QRegularExpressionMatch am = ai.next();
            r.a.insert(am.captured(1), am.captured(2));
        }
        r.body = m.captured(3);
        out << r;
    }
    return out;
}

struct Point {
    qint64 t = 0, lat = 0, lon = 0;
    QVariant alt, hacc, speed;
};

// the points of a wot answer, from its deltas
static QList<Point> pointsOf(const QByteArray &xml)
{
    QList<Point> out;
    const qint64 t0 = element(xml, "t0");
    if (t0 < 0)
        return out;
    const int a = xml.indexOf("<r>") + 3;
    Point prev;
    prev.t = t0;
    qint64 alt = 0;
    for (const QByteArray &record : xml.mid(a, xml.indexOf("</r>") - a).split(';')) {
        const QList<QByteArray> f = record.split(',');
        if (f.size() != 6)
            return QList<Point>();
        Point p;
        p.t = prev.t + f[0].toLongLong();
        p.lat = prev.lat + f[1].toLongLong();
        p.lon = prev.lon + f[2].toLongLong();
        if (!f[3].isEmpty()) {
            alt += f[3].toLongLong();
            p.alt = alt;
        }
        if (!f[4].isEmpty())
            p.hacc = f[4].toLongLong();
        if (!f[5].isEmpty())
            p.speed = f[5].toLongLong();
        out << p;
        prev = p;
    }
    return out;
}

class TestHealthChanges : public QObject
{
    Q_OBJECT

private:
    QTemporaryDir mDir;
    QString mRoot;      // the fixture's databases, as the watch has them before every test
    HealthService *mService = nullptr;
    Characteristic *mRequest = nullptr;
    HealthDataChrc *mData = nullptr;
    QList<QByteArray> mChunks;
    int mBiggestChunk = 0;

    QString db() const { return mRoot + "/health.db"; }

    // A request as BlueZ hands it over (pieces of MTU - 3 bytes at their offsets), and the answer joined
    QByteArray ask(const QByteArray &request, int mtu = 185)
    {
        mChunks.clear();
        mBiggestChunk = 0;
        const int piece = mtu - 3;
        for (int pos = 0; pos < request.size(); pos += piece)
            mRequest->WriteValue(request.mid(pos, piece), {{"mtu", mtu}, {"offset", pos}});
        (void)QTest::qWaitFor([this] { return !mChunks.isEmpty(); }, 2000);
        QByteArray all;
        for (const QByteArray &c : std::as_const(mChunks)) {
            all += c;
            mBiggestChunk = qMax(mBiggestChunk, int(c.size()));
        }
        return all;
    }

    // The fixture's databases in a directory of its own
    void makeRoot(const QString &root)
    {
        QVERIFY(runSql(root + "/health.db", fixtureFile("health-v3.sql")));
        QVERIFY(runSql(root + "/heartrate.db", fixtureFile("heartrate-v3.sql")));
    }

    /*
     * A sync as Gadgetbridge's AsteroidOSHealthSync makes it: hr, steps and hrr
     * from 10 minutes (hrr: 600000 ms) before the last record it has, page by
     * page; then wo (4 a page; a page's workouts with a track it does not have
     * get their wot pages before the page's sq is kept), sl and rhr from the
     * last sq
     */
    void phoneSync(QHash<QByteArray, qint64> &last, QSet<qint64> &tracks, QList<QByteArray> &requests,
                   QList<QByteArray> &answers)
    {
        for (const QByteArray &ty : {QByteArray("hr"), QByteArray("steps"), QByteArray("hrr")}) {
            const qint64 overlap = ty == "hrr" ? 600000 : 600;
            qint64 since = last.value(ty) > overlap ? last.value(ty) - overlap : 0;
            for (;;) {
                const QByteArray request = "<req><ty>" + ty + "</ty><si>" + num(since) + "</si></req>";
                const QByteArray answer = ask(request);
                requests << request;
                answers << answer;
                const qint64 end = lastTime(answer);
                if (end > last.value(ty))
                    last[ty] = end;
                if (element(answer, "mo") == 1 && end > since)
                    since = end;
                else
                    break;
            }
        }
        for (const QByteArray &ty : {QByteArray("wo"), QByteArray("sl"), QByteArray("rhr")}) {
            qint64 sq = last.value(ty);
            for (;;) {
                const QByteArray request = "<req><ty>" + ty + "</ty><sq>" + num(sq) + "</sq>"
                        + (ty == "wo" ? "<mx>4</mx>" : "") + "</req>";
                const QByteArray answer = ask(request);
                requests << request;
                answers << answer;
                for (const Rec &r : recordsOf(answer)) {
                    if (r.tag != "w" || r.a.contains("dl") || r.a.value("tp").toLongLong() <= 0
                            || tracks.contains(r.a.value("st").toLongLong()))
                        continue;
                    qint64 since = 0;
                    for (;;) {
                        const QByteArray trackRequest = "<req><ty>wot</ty><id>" + r.a.value("id").toLatin1() + "</id><si>"
                                + num(since) + "</si></req>";
                        const QByteArray track = ask(trackRequest);
                        requests << trackRequest;
                        answers << track;
                        const qint64 end = lastTime(track);
                        if (element(track, "mo") == 1 && end > since)
                            since = end;
                        else
                            break;
                    }
                    tracks << r.a.value("st").toLongLong();
                }
                const qint64 next = element(answer, "sq");
                last[ty] = next;
                if (element(answer, "mo") == 1 && next > sq)
                    sq = next;
                else
                    break;
            }
        }
    }

    // Pages through a kind from 0: the sq attribute of every record, in order (checks each page's rules)
    QList<qint64> pageAll(Kind kind, int max, int maxBytes, qint64 *end = nullptr)
    {
        QList<qint64> seen;
        qint64 since = 0;
        for (int pages = 0; pages < 10000; pages++) {
            const QByteArray xml = changes(mRoot + "/", kind, since, max, maxBytes);
            const QList<Rec> recs = recordsOf(xml);
            if (xml.size() > maxBytes || element(xml, "n") != recs.size() || recs.size() > max
                    || (element(xml, "mo") == 1 && recs.isEmpty()) || element(xml, "sq") < since)
                return {-1};
            for (const Rec &r : recs) {
                if (r.a.value("sq").toLongLong() <= since)
                    return {-2};
                seen << r.a.value("sq").toLongLong();
            }
            since = element(xml, "sq");
            if (element(xml, "mo") != 1)
                break;
        }
        if (end)
            *end = since;
        return seen;
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
        QVERIFY(mData);
        connect(mData, &HealthDataChrc::valueChanged, this, [this] {
            mChunks << mData->property("Value").toByteArray();
        });
    }

    void init()
    {
        // a fresh copy of the fixture for every test (some change it)
        static int n = 0;
        mRoot = mDir.path() + "/root" + QString::number(n++);
        QVERIFY(QDir().mkpath(mRoot));
        makeRoot(mRoot);
        MDConfItem::reset();
        MDConfItem(DATA_DIR_KEY).set(mRoot);
    }

    void capabilities()
    {
        const QByteArray caps = "<health><version>3</version><types>hr,steps,hrr,wo,wot,sl,rhr</types>"
                                "<mx>1000</mx><mb>8192</mb></health>";
        QCOMPARE(mService->getCharacteristics()[2]->ReadValue({}), caps);
    }

    void fixtureIsAsWritten()
    {
        // what the fixture's statements left (the counter's triggers ran)
        QCOMPARE(scalar(db(), "PRAGMA user_version"), 2);
        QCOMPARE(scalar(db(), "SELECT COUNT(*) FROM workout"), 6);
        QCOMPARE(scalar(db(), "SELECT COUNT(*) FROM workout_track WHERE workout_id = 2"), 570);
        QCOMPARE(scalar(db(), "SELECT COUNT(*) FROM sleep_session WHERE deleted = 1"), 1);
        QCOMPARE(scalar(db(), "SELECT COUNT(*) FROM workout_state"), 1);
        QCOMPARE(scalar(db(), "SELECT value FROM meta WHERE key = 'seq'"),
                 scalar(db(), "SELECT MAX(seq) FROM (SELECT seq FROM workout UNION ALL SELECT seq FROM sleep_session "
                              "UNION ALL SELECT seq FROM daily)"));
    }

    // Gadgetbridge's frames, byte for byte (FIXTURES_WRITE=1: written again)
    void gadgetbridgeFrames()
    {
        if (qEnvironmentVariableIntValue("FIXTURES_WRITE") == 1) {
            QHash<QByteArray, qint64> last;
            QSet<qint64> tracks;
            QList<QByteArray> requests, answers;
            phoneSync(last, tracks, requests, answers);
            phoneSync(last, tracks, requests, answers);
            for (const auto &file : {qMakePair(QByteArray("health-v3-requests.frames.txt"), &requests),
                                     qMakePair(QByteArray("health-v3-answers.frames.txt"), &answers)}) {
                QFile f(QStringLiteral(FIXTURE_DIR "/") + QString::fromLatin1(file.first));
                QVERIFY(f.open(QIODevice::WriteOnly | QIODevice::Truncate));
                for (const QByteArray &frame : *file.second)
                    f.write(QByteArray(frame).replace("\\", "\\\\").replace("\n", "\\n").replace("\r", "\\r") + "\n");
            }
            QSKIP("fixtures written");
        }
        const QList<QByteArray> requests = fixtureFrames("health-v3-requests.frames.txt");
        const QList<QByteArray> answers = fixtureFrames("health-v3-answers.frames.txt");
        QCOMPARE(requests.size(), answers.size());
        QVERIFY(requests.size() >= 15);
        QSet<QByteArray> kinds;
        for (int i = 0; i < requests.size(); i++) {
            const QByteArray answer = ask(requests[i]);
            QCOMPARE(answer, answers[i]);
            QVERIFY(mBiggestChunk <= 182);
            const QByteArray ty = answer.mid(10, answer.indexOf("</ty>") - 10);
            kinds << ty;
            if (kindFromName(QString::fromLatin1(ty)) != None)
                QVERIFY2(answer.size() <= MaxAnswerBytes, answer.left(60).constData());
        }
        // every kind, a wo page with more after it, a track in two pages
        QCOMPARE(kinds, QSet<QByteArray>({"hr", "steps", "hrr", "wo", "wot", "sl", "rhr"}));
        int trackPages = 0, morePages = 0;
        for (const QByteArray &a : answers) {
            trackPages += a.startsWith("<data><ty>wot</ty>") ? 1 : 0;
            morePages += a.startsWith("<data><ty>wo</ty>") && a.contains("<mo>1</mo>") ? 1 : 0;
        }
        QCOMPARE(trackPages, 2);
        QCOMPARE(morePages, 1);
        // and a phone that asks as Gadgetbridge does asks exactly these
        QHash<QByteArray, qint64> last;
        QSet<qint64> tracks;
        QList<QByteArray> asked, got;
        phoneSync(last, tracks, asked, got);
        phoneSync(last, tracks, asked, got);
        QCOMPARE(asked, requests);
    }

    void recordsOfTheFixture()
    {
        // the first page of workouts: the imported ride, the walk (laps, the pause, its track), the treadmill
        // run, the deleted yoga workout; the HIIT session after it; the walk that is on not at all
        const QByteArray xml = changes(mRoot + "/", Workouts, 0, 4);
        const QList<Rec> recs = recordsOf(xml);
        QCOMPARE(recs.size(), 4);
        QCOMPARE(element(xml, "mo"), 1);
        QCOMPARE(recs[0].a.value("ty"), QStringLiteral("Bike"));
        QVERIFY(!recs[0].a.contains("zl"));   // the old log had no zones
        const Rec &walk = recs[1];
        QCOMPARE(walk.a.value("id").toLongLong(), WALK);
        QCOMPARE(walk.a.value("st"), QStringLiteral("1768028400000"));
        QCOMPARE(walk.a.value("en"), QStringLiteral("1768029660000"));
        QCOMPARE(walk.a.value("dm"), QStringLiteral("1581.6"));
        QCOMPARE(walk.a.value("tp"), QStringLiteral("570"));
        QCOMPARE(walk.a.value("lc"), QStringLiteral("2"));
        QCOMPARE(walk.body, QStringLiteral("<l>1,0,840000,1000.0,102,split-km;2,840000,360000,650.3,108,manual</l>"
                                           "<p>600000,120000,0</p>"));
        QCOMPARE(recs[2].a.value("ty"), QStringLiteral("TreadMill"));
        QCOMPARE(recs[2].a.value("eg"), QStringLiteral("0.0"));
        QCOMPARE(recs[3].a.value("id").toLongLong(), YOGA);
        QCOMPARE(recs[3].a.value("dl"), QStringLiteral("1"));
        QVERIFY(!recs[3].a.contains("en"));   // never ended
        QVERIFY(!recs[3].a.contains("ty"));

        const QByteArray next = changes(mRoot + "/", Workouts, element(xml, "sq"), 4);
        QCOMPARE(recordsOf(next).size(), 1);
        QCOMPARE(recordsOf(next)[0].a.value("sr"), QStringLiteral("auto"));
        QCOMPARE(element(next, "mo"), 0);
        // the next sq is the open walk's: every row up to it was sent or skipped
        QCOMPARE(element(next, "sq"), scalar(db(), "SELECT seq FROM workout WHERE id = 6"));
        QCOMPARE(element(next, "hs"), scalar(db(), "SELECT value FROM meta WHERE key = 'seq'"));

        // sleep: the imported night (no stages), the morning's tombstone, the night again and the nap
        const QList<Rec> sleep = recordsOf(changes(mRoot + "/", Sleep, 0));
        QCOMPARE(sleep.size(), 4);
        QCOMPARE(sleep[0].a.value("ns"), QStringLiteral("0"));
        QCOMPARE(sleep[0].a.value("al"), QStringLiteral("eos-steps-hr-1"));
        QCOMPARE(sleep[1].a.value("dl"), QStringLiteral("1"));
        QCOMPARE(sleep[1].a.value("mn"), QStringLiteral("1"));
        QCOMPARE(sleep[2].a.value("ns"), QStringLiteral("12"));
        QCOMPARE(sleep[2].a.value("hr"), QStringLiteral("56.4"));
        QVERIFY(sleep[2].body.startsWith(QStringLiteral("0,1500000,1;0,3000000,2;0,3900000,1;")));
        QCOMPARE(sleep[3].a.value("mn"), QStringLiteral("0"));
        QVERIFY(!sleep[3].a.contains("sc"));   // naps have no score
        QCOMPARE(sleep[3].body, QStringLiteral("0,600000,1;0,480000,2;0,420000,1"));

        // days: every closed day, the resting rate where there is one
        const QList<Rec> days = recordsOf(changes(mRoot + "/", RestingHeartRate, 0));
        QCOMPARE(days.size(), 3);
        QCOMPARE(days[0].a.value("dt"), QStringLiteral("2026-01-08"));
        QVERIFY(!days[0].a.contains("rh"));
        QCOMPARE(days[1].a.value("rh"), QStringLiteral("55"));
        QCOMPARE(days[2].a.value("rh"), QStringLiteral("53"));
    }

    void pagingVisitsEveryRowOnce_data()
    {
        QTest::addColumn<int>("kind");
        QTest::addColumn<int>("max");
        QTest::addColumn<int>("bytes");
        for (int kind : {int(Workouts), int(Sleep), int(RestingHeartRate)}) {
            for (int max : {1, 2, 3, 1000}) {
                for (int bytes : {700, 8192})
                    QTest::addRow("%s max %d bytes %d", kindName(Kind(kind)).constData(), max, bytes) << kind << max << bytes;
            }
        }
    }

    void pagingVisitsEveryRowOnce()
    {
        QFETCH(int, kind);
        QFETCH(int, max);
        QFETCH(int, bytes);
        const QByteArray table = kind == Workouts ? "workout" : kind == Sleep ? "sleep_session" : "daily";
        QList<qint64> expected;
        for (const QVariantList &r : rows(db(), "SELECT seq FROM " + table + (kind == Workouts
                ? " WHERE NOT (end_ms IS NULL AND deleted = 0)" : "") + " ORDER BY seq"))
            expected << r.first().toLongLong();
        qint64 end = 0;
        QCOMPARE(pageAll(Kind(kind), max, bytes, &end), expected);
        // the last sq: the table's last change (the open walk's for workouts: skipped, not lost)
        QCOMPARE(end, scalar(db(), "SELECT MAX(seq) FROM " + table));
    }

    void openWorkoutIsSentOnceEnded()
    {
        qint64 end = 0;
        pageAll(Workouts, 1000, MaxAnswerBytes, &end);
        // nothing after the end
        QCOMPARE(recordsOf(changes(mRoot + "/", Workouts, end)).size(), 0);
        QVERIFY(runSql(db(), "UPDATE workout SET end_ms = 1768147500000, active_s = 300, distance_m = 410.0, kcal = 21.0, "
                             "steps = 520, avg_bpm = 98, max_bpm = 104, light_s = 0, moderate_s = 0, vigorous_s = 0, "
                             "peak_s = 0, azm = 0, elev_gain_m = 0.0 WHERE id = 6"));
        const QList<Rec> recs = recordsOf(changes(mRoot + "/", Workouts, end));
        QCOMPARE(recs.size(), 1);
        QCOMPARE(recs[0].a.value("st").toLongLong(), OPEN_WALK_START);
        QCOMPARE(recs[0].a.value("en"), QStringLiteral("1768147500000"));
        QCOMPARE(recs[0].a.value("tp"), QStringLiteral("3"));
        // its track, south-west of 0,0: negative numbers
        QCOMPARE(track(mRoot + "/", OPEN_WALK, 0), QByteArray("<data><ty>wot</ty><id>6</id><t0>1768147200000</t0><n>3</n>"
                                                              "<mo>0</mo><r>0,-5000,-3000,950,80,120;1000,-100,-80,0,80,120;"
                                                              "1000,-100,-80,0,80,120</r></data>"));
    }

    void tombstones()
    {
        qint64 end = 0;
        pageAll(Workouts, 1000, MaxAnswerBytes, &end);
        // the treadmill run deleted on the watch (HealthDb::deleteWorkout)
        QVERIFY(runSql(db(), "UPDATE workout SET deleted = 1 WHERE id = 3 AND deleted = 0; "
                             "DELETE FROM workout_track WHERE workout_id = 3; DELETE FROM workout_lap WHERE workout_id = 3; "
                             "DELETE FROM workout_pause WHERE workout_id = 3; DELETE FROM workout_state WHERE workout_id = 3"));
        const qint64 seq = scalar(db(), "SELECT seq FROM workout WHERE id = 3");
        QVERIFY(seq > end);
        QCOMPARE(changes(mRoot + "/", Workouts, end),
                 "<data><ty>wo</ty><hs>" + num(seq) + "</hs><sq>" + num(seq) + "</sq><n>1</n><mo>0</mo>"
                 "<w sq=\"" + num(seq) + "\" id=\"3\" st=\"1768068000000\" en=\"1768069800000\" dl=\"1\"/></data>");
        // a deleted workout has no track; neither has one that is not there
        QCOMPARE(track(mRoot + "/", TREADMILL, 0), QByteArray("<data><ty>wot</ty><id>3</id><n>0</n><mo>0</mo></data>"));
        QCOMPARE(track(mRoot + "/", YOGA, 0), QByteArray("<data><ty>wot</ty><id>4</id><n>0</n><mo>0</mo></data>"));
        QCOMPARE(track(mRoot + "/", 99, 0), QByteArray("<data><ty>wot</ty><id>99</id><n>0</n><mo>0</mo></data>"));
    }

    void counterAcrossRestarts()
    {
        qint64 end = 0;
        pageAll(RestingHeartRate, 1000, MaxAnswerBytes, &end);
        const qint64 head = element(changes(mRoot + "/", RestingHeartRate, end), "hs");
        // sensorlogd restarted (a new connection) and closed the next day: the counter goes on from where it was
        QVERIFY(runSql(db(), "INSERT INTO daily (date, steps, kcal, distance_m, floors, azm, hours_met, closed_ms) "
                             "VALUES ('2026-01-11', 8000, 2100.0, 6000.0, 4, 30, 8, 1768176120000)"));
        QVERIFY(runSql(db(), "INSERT INTO daily (date, resting_hr) VALUES ('2026-01-11', 54) ON CONFLICT (date) DO UPDATE "
                             "SET resting_hr = excluded.resting_hr WHERE resting_hr IS NOT excluded.resting_hr"));
        const QByteArray xml = changes(mRoot + "/", RestingHeartRate, end);
        QCOMPARE(element(xml, "hs"), head + 2);
        QCOMPARE(xml, "<data><ty>rhr</ty><hs>" + num(head + 2) + "</hs><sq>" + num(head + 2) + "</sq><n>1</n><mo>0</mo>"
                      "<d sq=\"" + num(head + 2) + "\" dt=\"2026-01-11\" rh=\"54\"/></data>");
        // the same value again changes nothing
        QVERIFY(runSql(db(), "INSERT INTO daily (date, resting_hr) VALUES ('2026-01-11', 54) ON CONFLICT (date) DO UPDATE "
                             "SET resting_hr = excluded.resting_hr WHERE resting_hr IS NOT excluded.resting_hr"));
        QCOMPARE(element(changes(mRoot + "/", RestingHeartRate, head + 2), "n"), 0);
        // a cleared rate is a change too: the day without rh
        QVERIFY(runSql(db(), "INSERT INTO daily (date, resting_hr) VALUES ('2026-01-11', NULL) ON CONFLICT (date) DO UPDATE "
                             "SET resting_hr = excluded.resting_hr WHERE resting_hr IS NOT excluded.resting_hr"));
        QCOMPARE(recordsOf(changes(mRoot + "/", RestingHeartRate, head + 2)).value(0).a.contains("rh"), false);
        QCOMPARE(element(changes(mRoot + "/", RestingHeartRate, head + 2), "n"), 1);
    }

    void databaseMadeAgain()
    {
        qint64 end = 0;
        pageAll(Workouts, 1000, MaxAnswerBytes, &end);
        QVERIFY(end > 10);
        // the watch's data wiped (a factory reset): a new health.db with one day in it
        for (const char *f : {"/health.db", "/health.db-wal", "/health.db-shm"})
            QFile::remove(mRoot + f);
        QByteArray sql = fixtureFile("health-v3.sql");
        sql = sql.left(sql.indexOf("COMMIT;") + 7);
        QVERIFY(runSql(db(), sql + "INSERT INTO daily (date, resting_hr) VALUES ('2026-01-12', 57);"));
        // the phone's sq is above the counter: it learns the database is another one, and asks from 0
        QCOMPARE(changes(mRoot + "/", Workouts, end),
                 "<data><ty>wo</ty><hs>1</hs><sq>" + num(end) + "</sq><n>0</n><mo>0</mo></data>");
        QCOMPARE(changes(mRoot + "/", RestingHeartRate, 0),
                 QByteArray("<data><ty>rhr</ty><hs>1</hs><sq>1</sq><n>1</n><mo>0</mo><d sq=\"1\" dt=\"2026-01-12\" rh=\"57\"/>"
                            "</data>"));
    }

    void trackPaging_data()
    {
        QTest::addColumn<int>("max");
        QTest::addColumn<int>("bytes");
        for (int max : {1, 7, 1000})
            for (int bytes : {500, 8192})
                QTest::addRow("max %d bytes %d", max, bytes) << max << bytes;
    }

    void trackPaging()
    {
        QFETCH(int, max);
        QFETCH(int, bytes);
        const QList<QVariantList> stored = rows(db(), "SELECT t_ms, lat_e7, lon_e7, alt_dm, hacc_dm, speed_cms "
                                                      "FROM workout_track WHERE workout_id = 2 ORDER BY t_ms");
        QCOMPARE(stored.size(), 570);
        QList<Point> got;
        qint64 since = 0;
        int pages = 0;
        for (;;) {
            const QByteArray xml = track(mRoot + "/", WALK, since, max, bytes);
            QVERIFY(xml.size() <= bytes);
            QVERIFY(pages++ < 1000);
            const QList<Point> page = pointsOf(xml);
            QCOMPARE(element(xml, "n"), page.size());
            QVERIFY(page.size() <= max);
            for (const Point &p : page)
                QVERIFY(p.t > since);
            got += page;
            if (element(xml, "mo") != 1)
                break;
            QVERIFY(!page.isEmpty());
            since = page.last().t;
        }
        QCOMPARE(got.size(), stored.size());
        for (int i = 0; i < got.size(); i++) {
            QCOMPARE(got[i].t, stored[i][0].toLongLong());
            QCOMPARE(got[i].lat, stored[i][1].toLongLong());
            QCOMPARE(got[i].lon, stored[i][2].toLongLong());
            QCOMPARE(got[i].alt, stored[i][3]);
            QCOMPARE(got[i].hacc, stored[i][4]);
            QCOMPARE(got[i].speed, stored[i][5]);
        }
        if (bytes == MaxAnswerBytes && max == 1000)
            QCOMPARE(pages, 2);
    }

    void aRecordAloneOverTheLimitIsCut()
    {
        qint64 end = 0;
        pageAll(Workouts, 1000, MaxAnswerBytes, &end);
        // a ride with 400 laps (a lap a kilometre), and a run after it
        QVERIFY(runSql(db(), "INSERT INTO workout (start_ms, type, source, gps) VALUES (1768190400000, 'Bike', 'manual', 1); "
                             "WITH RECURSIVE k(i) AS (SELECT 1 UNION ALL SELECT i + 1 FROM k WHERE i < 400) "
                             "INSERT INTO workout_lap (workout_id, n, start_ms, end_ms, distance_m, avg_bpm, kind) "
                             "SELECT 7, i, 1768190400000 + (i - 1) * 120000, 1768190400000 + i * 120000, 1000.0, 140, 'split-km' FROM k; "
                             "UPDATE workout SET end_ms = 1768238400000, active_s = 48000 WHERE id = 7; "
                             "INSERT INTO workout (start_ms, end_ms, active_s, type) VALUES (1768240000000, 1768241000000, 1000, 'Run')"));
        const QByteArray xml = changes(mRoot + "/", Workouts, end);
        QVERIFY(xml.size() <= MaxAnswerBytes);
        QList<Rec> recs = recordsOf(xml);
        QCOMPARE(recs.size(), 1);
        QCOMPARE(element(xml, "mo"), 1);
        QCOMPARE(recs[0].a.value("lc"), QStringLiteral("400"));
        const QString laps = recs[0].body.mid(3, recs[0].body.indexOf("</l>") - 3);
        const QStringList cut = laps.split(';');
        QVERIFY(cut.size() > 100 && cut.size() < 400);
        for (int i = 0; i < cut.size(); i++)
            QVERIFY(cut[i].startsWith(QString::number(i + 1) + ","));
        // the next answer has the run
        recs = recordsOf(changes(mRoot + "/", Workouts, element(xml, "sq")));
        QCOMPARE(recs.size(), 1);
        QCOMPARE(recs[0].a.value("ty"), QStringLiteral("Run"));
    }

    void noDatabase()
    {
        QTemporaryDir empty;
        const QString root = empty.path() + "/";
        QCOMPARE(changes(root, Workouts, 5), QByteArray("<data><ty>wo</ty><hs>0</hs><sq>5</sq><n>0</n><mo>0</mo></data>"));
        QCOMPARE(changes(root, Sleep, 0), QByteArray("<data><ty>sl</ty><hs>0</hs><sq>0</sq><n>0</n><mo>0</mo></data>"));
        QCOMPARE(track(root, 2, 0), QByteArray("<data><ty>wot</ty><id>2</id><n>0</n><mo>0</mo></data>"));
    }

    void brokenDatabase()
    {
        QTemporaryDir dir;
        QFile f(dir.path() + "/health.db");
        QVERIFY(f.open(QIODevice::WriteOnly));
        f.write(QByteArray(4096, 'x'));
        f.close();
        const QString root = dir.path() + "/";
        QCOMPARE(changes(root, Workouts, 0), QByteArray("<data><ty>wo</ty><er>db</er></data>"));
        QCOMPARE(changes(root, RestingHeartRate, 0), QByteArray("<data><ty>rhr</ty><er>db</er></data>"));
        QCOMPARE(track(root, 2, 0), QByteArray("<data><ty>wot</ty><er>db</er></data>"));
    }

    void aWriteWhileReadingIsNotHalfSeen()
    {
        qint64 end = 0;
        pageAll(Workouts, 1000, MaxAnswerBytes, &end);
        // sensorlogd in the middle of a transaction (a workout and its laps)
        sqlite3 *writer = nullptr;
        QCOMPARE(sqlite3_open_v2(QFile::encodeName(db()).constData(), &writer, SQLITE_OPEN_READWRITE, nullptr), SQLITE_OK);
        QCOMPARE(sqlite3_exec(writer, "BEGIN IMMEDIATE; INSERT INTO workout (start_ms, end_ms, active_s, type) "
                                      "VALUES (1768240000000, 1768241000000, 1000, 'Run'); "
                                      "INSERT INTO workout_lap (workout_id, n, start_ms, end_ms, kind) "
                                      "VALUES (7, 1, 1768240000000, 1768240500000, 'manual')",
                              nullptr, nullptr, nullptr), SQLITE_OK);
        QElapsedTimer timer;
        timer.start();
        QCOMPARE(element(changes(mRoot + "/", Workouts, end), "n"), 0);
        QVERIFY(timer.elapsed() < 1000);    // a reader never waits for the writer (WAL)
        QCOMPARE(sqlite3_exec(writer, "COMMIT", nullptr, nullptr, nullptr), SQLITE_OK);
        sqlite3_close(writer);
        const QList<Rec> recs = recordsOf(changes(mRoot + "/", Workouts, end));
        QCOMPARE(recs.size(), 1);
        QCOMPARE(recs[0].a.value("lc"), QStringLiteral("1"));
        QCOMPARE(recs[0].body, QStringLiteral("<l>1,0,500000,,,manual</l>"));
    }

    void textIsEscaped()
    {
        qint64 end = 0;
        pageAll(Workouts, 1000, MaxAnswerBytes, &end);
        QVERIFY(runSql(db(), "INSERT INTO workout (start_ms, end_ms, active_s, type, source) "
                             "VALUES (1768240000000, 1768241000000, 1000, 'A<b>&\"c', 'manual'); "
                             "INSERT INTO workout_lap (workout_id, n, start_ms, end_ms, kind) "
                             "VALUES (7, 1, 1768240000000, 1768240500000, 'x,y;z')"));
        const QByteArray xml = changes(mRoot + "/", Workouts, end);
        QVERIFY2(xml.contains(" ty=\"A&lt;b&gt;&amp;&quot;c\""), xml.constData());
        QVERIFY2(xml.contains("<l>1,0,500000,,,x%2Cy%3Bz</l>"), xml.constData());
    }

    void requestsOverGatt()
    {
        // the answers on the data characteristic are those of HealthChanges, in pieces of MTU - 3
        QCOMPARE(ask("<req><ty>sl</ty><sq>0</sq></req>", 23), changes(mRoot + "/", Sleep, 0));
        QVERIFY(mBiggestChunk <= 20);
        QCOMPARE(ask("<req><ty>wot</ty><id>2</id><si>1768029000000</si><mx>2</mx></req>"),
                 track(mRoot + "/", WALK, 1768029000000LL, 2));
        // si is not a wo's; a negative sq is 0
        QCOMPARE(ask("<req><ty>wo</ty><si>1768029000000</si><sq>-4</sq><mx>2</mx></req>"), changes(mRoot + "/", Workouts, 0, 2));
        // a track needs its workout
        QCOMPARE(ask("<req><ty>wot</ty><si>0</si></req>"), QByteArray("<data><ty>wot</ty><er>request</er></data>"));
        QCOMPARE(ask("<req><ty>wo</ty><sq>many</sq></req>"), QByteArray("<data><ty></ty><er>request</er></data>"));
        QCOMPARE(ask("<req><ty>wot</ty><id>two</id></req>"), QByteArray("<data><ty></ty><er>request</er></data>"));
        // hr, steps and hrr as before (version 2's answers)
        QCOMPARE(ask("<req><ty>hrr</ty><si>0</si><mx>2</mx></req>"),
                 HealthRecords::toXml(HealthRecords::HeartRateReadings,
                                      HealthRecords::query(mRoot + "/", HealthRecords::HeartRateReadings, 0, 2)));
    }

    // A request cut by the disconnection: the next connection's first request is answered
    void requestCutByADisconnection()
    {
        mChunks.clear();
        mRequest->WriteValue("<req><ty>wot</ty><id>2", {{"mtu", 23}});
        QTest::qWait(10);
        QVERIFY(mChunks.isEmpty());
        QMetaObject::invokeMethod(mService, "phoneConnectedChanged", Q_ARG(bool, false));
        QMetaObject::invokeMethod(mService, "phoneConnectedChanged", Q_ARG(bool, true));
        QCOMPARE(ask("<req><ty>rhr</ty><sq>0</sq></req>"), changes(mRoot + "/", RestingHeartRate, 0));
    }
};

BTSYNCD_TEST_MAIN(TestHealthChanges)
#include "tst_healthchanges.moc"
