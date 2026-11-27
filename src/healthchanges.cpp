/*
 * Copyright (C) 2026 - eos
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

#include "healthchanges.h"
#include "healthrecords.h"

#include <QFile>
#include <QList>
#include <QUrl>

#include <sqlite3.h>

#include <cmath>

namespace HealthChanges {

namespace {

// Room kept for what is around the records: <data><ty>..</ty><hs>..</hs><sq>..</sq><n>..</n><mo>..</mo> and
// </data>, or wot's <id>, <t0>, <r></r> (numbers of up to 19 digits)
const int HeadBytes = 160;

/*
 * sensorlogd's health.db, read in one read transaction: a snapshot, so a
 * write of sensorlogd's between two statements is never half seen. Opened
 * read-write, though only read, as heartrate.db (a WAL database is read
 * through its -shm file, which a read-only connection cannot make when
 * sensorlogd has none open). Not there: a watch without it (yet).
 */
class Db
{
public:
    explicit Db(const QString &root)
    {
        const QString path = root + QStringLiteral("health.db");
        m_exists = QFile::exists(path);
        if (!m_exists)
            return;
        if (sqlite3_open_v2(QFile::encodeName(path).constData(), &m_db, SQLITE_OPEN_READWRITE, nullptr) != SQLITE_OK) {
            sqlite3_close(m_db);
            m_db = nullptr;
            return;
        }
        sqlite3_busy_timeout(m_db, 2000); // a checkpoint of sensorlogd's
        if (sqlite3_exec(m_db, "BEGIN", nullptr, nullptr, nullptr) != SQLITE_OK) {
            sqlite3_close(m_db);
            m_db = nullptr;
        }
    }
    ~Db()
    {
        if (m_db)
            sqlite3_exec(m_db, "COMMIT", nullptr, nullptr, nullptr);
        sqlite3_close(m_db);
    }
    Db(const Db &) = delete;
    Db &operator=(const Db &) = delete;

    bool exists() const { return m_exists; }
    sqlite3 *db() const { return m_db; }

private:
    bool m_exists = false;
    sqlite3 *m_db = nullptr;
};

// a prepared statement, finalized with the scope
class Stmt
{
public:
    Stmt(sqlite3 *db, const char *sql)
    {
        if (db && sqlite3_prepare_v2(db, sql, -1, &m_st, nullptr) != SQLITE_OK) {
            sqlite3_finalize(m_st);
            m_st = nullptr;
        }
    }
    ~Stmt() { sqlite3_finalize(m_st); }
    Stmt(const Stmt &) = delete;
    Stmt &operator=(const Stmt &) = delete;

    bool ok() const { return m_st && !m_failed; }
    bool failed() const { return !m_st || m_failed; }
    Stmt &bind(int i, qint64 v)
    {
        if (m_st)
            sqlite3_bind_int64(m_st, i, v);
        return *this;
    }
    void reset()
    {
        if (m_st)
            sqlite3_reset(m_st);
    }
    bool row()
    {
        if (!m_st || m_failed)
            return false;
        const int rc = sqlite3_step(m_st);
        if (rc == SQLITE_ROW)
            return true;
        if (rc != SQLITE_DONE)
            m_failed = true;
        return false;
    }
    bool null(int col) const { return sqlite3_column_type(m_st, col) == SQLITE_NULL; }
    qint64 i(int col) const { return sqlite3_column_int64(m_st, col); }
    double d(int col) const { return sqlite3_column_double(m_st, col); }
    QByteArray text(int col) const
    {
        return QByteArray(reinterpret_cast<const char *>(sqlite3_column_text(m_st, col)), sqlite3_column_bytes(m_st, col));
    }

private:
    sqlite3_stmt *m_st = nullptr;
    bool m_failed = false;
};

QByteArray num(qint64 v)
{
    return QByteArray::number(v);
}

// one decimal (distances in m, energy in kcal, heart rate averages)
QByteArray real1(double v)
{
    const double r = std::round(v * 10.0) / 10.0;
    return QByteArray::number(r == 0.0 ? 0.0 : r, 'f', 1);
}

QByteArray xmlText(const QByteArray &utf8)
{
    return QString::fromUtf8(utf8).toHtmlEscaped().toUtf8();
}

// a word in a list (a lap's kind): anything but letters, digits and -._~ percent-encoded (no ',' or ';' then)
QByteArray listWord(const QByteArray &utf8)
{
    return QUrl::toPercentEncoding(QString::fromUtf8(utf8));
}

// The attributes of a record's start tag, from a row: those of NULL columns left out
class Attrs
{
public:
    explicit Attrs(const Stmt &q) : m_q(q) {}
    void add(const char *name, const QByteArray &value)
    {
        out += ' ';
        out += name;
        out += "=\"";
        out += value;
        out += '"';
    }
    void integer(const char *name, int col)
    {
        if (!m_q.null(col))
            add(name, num(m_q.i(col)));
    }
    void real(const char *name, int col)
    {
        if (!m_q.null(col))
            add(name, real1(m_q.d(col)));
    }
    void text(const char *name, int col)
    {
        if (!m_q.null(col))
            add(name, xmlText(m_q.text(col)));
    }

    QByteArray out;

private:
    const Stmt &m_q;
};

QByteArray joined(const QList<QByteArray> &items, int n)
{
    QByteArray out;
    for (int i = 0; i < n; i++) {
        if (i)
            out += ';';
        out += items[i];
    }
    return out;
}

/*
 * A record: its start tag's attributes and its lists (a workout's laps and
 * pauses, a session's stage spans as the element's text)
 */
struct Record {
    QByteArray tag;
    QByteArray attrs;
    QList<QByteArray> laps, pauses, spans;

    QByteArray xml(int nLaps, int nPauses, int nSpans) const
    {
        QByteArray x = '<' + tag + attrs;
        if (!nLaps && !nPauses && !nSpans)
            return x + "/>";
        x += '>';
        if (nLaps)
            x += "<l>" + joined(laps, nLaps) + "</l>";
        if (nPauses)
            x += "<p>" + joined(pauses, nPauses) + "</p>";
        x += joined(spans, nSpans);
        return x + "</" + tag + '>';
    }

    // The record if it fits in room bytes; else, if it may be cut (the first of an answer), as much of it as fits
    // (the stage spans, then the laps, then the pauses cut from their ends); else nothing
    QByteArray fit(int room, bool cut) const
    {
        int l = laps.size(), p = pauses.size(), s = spans.size();
        QByteArray x = xml(l, p, s);
        if (x.size() <= room)
            return x;
        if (!cut)
            return QByteArray();
        while (x.size() > room && (l || p || s)) {
            if (s)
                s--;
            else if (l)
                l--;
            else
                p--;
            x = xml(l, p, s);
        }
        return x.size() <= room ? x : QByteArray();
    }
};

const char WORKOUT_SQL[] =
    "SELECT seq, id, start_ms, end_ms, deleted, type, source, gps, active_s, distance_m, kcal, steps, avg_bpm, "
    "max_bpm, light_s, moderate_s, vigorous_s, peak_s, azm, elev_gain_m FROM workout WHERE seq > ? ORDER BY seq";
const char SLEEP_SQL[] =
    "SELECT seq, id, wake_date, start_ms, end_ms, main, deleted, asleep_min, awake_min, rem_min, light_min, "
    "deep_min, score, goal_min, avg_hr, algo FROM sleep_session WHERE seq > ? ORDER BY seq";
const char DAILY_SQL[] = "SELECT seq, date, resting_hr FROM daily WHERE seq > ? ORDER BY seq";

// what a record needs besides its row, prepared once an answer
struct Children {
    explicit Children(sqlite3 *db)
        : points(db, "SELECT COUNT(*) FROM workout_track WHERE workout_id = ?"),
          laps(db, "SELECT n, start_ms, end_ms, distance_m, avg_bpm, kind FROM workout_lap WHERE workout_id = ? "
                   "ORDER BY n"),
          pauses(db, "SELECT start_ms, end_ms, auto FROM workout_pause WHERE workout_id = ? ORDER BY start_ms"),
          stages(db, "SELECT start_ms, end_ms, stage FROM sleep_stage WHERE session_id = ? ORDER BY start_ms") {}
    Stmt points, laps, pauses, stages;
};

// A workout row (WORKOUT_SQL); false if the database failed
bool workoutRecord(const Stmt &q, Children &c, Record *r)
{
    r->tag = "w";
    Attrs a(q);
    a.integer("sq", 0);
    a.integer("id", 1);
    a.integer("st", 2);
    a.integer("en", 3);
    if (q.i(4) != 0) {
        a.add("dl", "1");
        r->attrs = a.out;
        return true;
    }
    a.text("ty", 5);
    a.text("sr", 6);
    a.integer("gp", 7);
    a.integer("as", 8);
    a.real("dm", 9);
    a.real("kc", 10);
    a.integer("sp", 11);
    a.integer("ab", 12);
    a.integer("xb", 13);
    a.integer("zl", 14);
    a.integer("zm", 15);
    a.integer("zv", 16);
    a.integer("zp", 17);
    a.integer("az", 18);
    a.real("eg", 19);
    const qint64 id = q.i(1), start = q.i(2);

    c.points.reset();
    c.points.bind(1, id);
    a.add("tp", num(c.points.row() ? c.points.i(0) : 0));

    c.laps.reset();
    c.laps.bind(1, id);
    while (c.laps.row()) {
        const qint64 from = c.laps.i(1);
        QByteArray lap = num(c.laps.i(0)) + ',' + num(from - start) + ',' + num(c.laps.i(2) - from) + ',';
        if (!c.laps.null(3))
            lap += real1(c.laps.d(3));
        lap += ',';
        if (!c.laps.null(4))
            lap += num(c.laps.i(4));
        lap += ',' + listWord(c.laps.text(5));
        r->laps << lap;
    }
    a.add("lc", num(r->laps.size()));

    c.pauses.reset();
    c.pauses.bind(1, id);
    while (c.pauses.row()) {
        const qint64 from = c.pauses.i(0);
        QByteArray pause = num(from - start) + ',';
        if (!c.pauses.null(1))
            pause += num(c.pauses.i(1) - from);
        pause += ',' + num(c.pauses.i(2) ? 1 : 0);
        r->pauses << pause;
    }
    r->attrs = a.out;
    return !c.points.failed() && !c.laps.failed() && !c.pauses.failed();
}

// A sleep session row (SLEEP_SQL)
bool sleepRecord(const Stmt &q, Children &c, Record *r)
{
    r->tag = "s";
    Attrs a(q);
    a.integer("sq", 0);
    a.integer("id", 1);
    a.text("wd", 2);
    a.integer("st", 3);
    a.integer("en", 4);
    a.integer("mn", 5);
    if (q.i(6) != 0) {
        a.add("dl", "1");
        r->attrs = a.out;
        return true;
    }
    a.integer("as", 7);
    a.integer("aw", 8);
    a.integer("rm", 9);
    a.integer("li", 10);
    a.integer("dp", 11);
    a.integer("sc", 12);
    a.integer("gl", 13);
    a.real("hr", 14);
    a.text("al", 15);

    c.stages.reset();
    c.stages.bind(1, q.i(1));
    qint64 previous = q.i(3);
    while (c.stages.row()) {
        const qint64 from = c.stages.i(0), to = c.stages.i(1);
        r->spans << num(from - previous) + ',' + num(to - from) + ',' + num(c.stages.i(2));
        previous = to;
    }
    a.add("ns", num(r->spans.size()));
    r->attrs = a.out;
    return !c.stages.failed();
}

// A day row (DAILY_SQL)
bool dayRecord(const Stmt &q, Record *r)
{
    r->tag = "d";
    Attrs a(q);
    a.integer("sq", 0);
    a.text("dt", 1);
    a.integer("rh", 2);
    r->attrs = a.out;
    return true;
}

QByteArray dbError(const QByteArray &ty)
{
    return HealthRecords::errorXml(QString::fromLatin1(ty), QStringLiteral("db"));
}

}

Kind kindFromName(const QString &name)
{
    if (name == QLatin1String("wo"))
        return Workouts;
    if (name == QLatin1String("wot"))
        return WorkoutTrack;
    if (name == QLatin1String("sl"))
        return Sleep;
    if (name == QLatin1String("rhr"))
        return RestingHeartRate;
    return None;
}

QByteArray kindName(Kind kind)
{
    switch (kind) {
    case Workouts: return "wo";
    case WorkoutTrack: return "wot";
    case Sleep: return "sl";
    case RestingHeartRate: return "rhr";
    default: return QByteArray();
    }
}

QByteArray changes(const QString &root, Kind kind, qint64 sinceSeq, int max, int maxBytes)
{
    const QByteArray ty = kindName(kind);
    const char *sql = kind == Workouts ? WORKOUT_SQL : kind == Sleep ? SLEEP_SQL
            : kind == RestingHeartRate ? DAILY_SQL : nullptr;
    if (!sql)
        return HealthRecords::errorXml(QString::fromLatin1(ty), QStringLiteral("type"));
    if (max <= 0 || max > MaxRecords)
        max = MaxRecords;
    if (sinceSeq < 0)
        sinceSeq = 0;

    qint64 head = 0, last = sinceSeq;
    int n = 0;
    bool more = false;
    QByteArray records;
    Db db(root);
    if (db.exists()) {
        if (!db.db())
            return dbError(ty);
        Stmt counter(db.db(), "SELECT value FROM meta WHERE key = 'seq'");
        if (counter.row())
            head = counter.i(0);
        if (counter.failed())
            return dbError(ty);

        Stmt q(db.db(), sql);
        Children children(db.db());
        q.bind(1, sinceSeq);
        const int room = maxBytes - HeadBytes;
        while (q.row()) {
            const qint64 seq = q.i(0);
            // a workout still on: sent once it has ended (its end gives it a new seq)
            if (kind == Workouts && q.null(3) && q.i(4) == 0) {
                last = seq;
                continue;
            }
            if (n == max) {
                more = true;
                break;
            }
            Record r;
            const bool read = kind == Workouts ? workoutRecord(q, children, &r)
                    : kind == Sleep ? sleepRecord(q, children, &r) : dayRecord(q, &r);
            if (!read)
                return dbError(ty);
            const QByteArray xml = r.fit(room - records.size(), n == 0);
            if (xml.isEmpty()) {
                more = true;    // the next answer's first
                break;
            }
            records += xml;
            n++;
            last = seq;
        }
        if (q.failed())
            return dbError(ty);
    }
    return "<data><ty>" + ty + "</ty><hs>" + num(head) + "</hs><sq>" + num(last) + "</sq><n>" + num(n)
            + "</n><mo>" + (more ? "1" : "0") + "</mo>" + records + "</data>";
}

QByteArray track(const QString &root, qint64 workoutId, qint64 sinceMs, int max, int maxBytes)
{
    const QByteArray ty = kindName(WorkoutTrack);
    if (max <= 0 || max > MaxRecords)
        max = MaxRecords;
    const QByteArray start = "<data><ty>" + ty + "</ty><id>" + num(workoutId) + "</id>";
    const QByteArray none = start + "<n>0</n><mo>0</mo></data>";
    Db db(root);
    if (!db.exists())
        return none;
    if (!db.db())
        return dbError(ty);
    {
        // a deleted workout's track went with it; one that is not there has none
        Stmt w(db.db(), "SELECT deleted FROM workout WHERE id = ?");
        w.bind(1, workoutId);
        const bool found = w.row();
        if (w.failed())
            return dbError(ty);
        if (!found || w.i(0) != 0)
            return none;
    }
    Stmt q(db.db(), "SELECT t_ms, lat_e7, lon_e7, alt_dm, hacc_dm, speed_cms FROM workout_track "
                    "WHERE workout_id = ? AND t_ms > ? ORDER BY t_ms");
    q.bind(1, workoutId).bind(2, sinceMs);
    const int room = maxBytes - HeadBytes;
    QByteArray points;
    int n = 0;
    bool more = false;
    qint64 t0 = 0, prevT = 0, prevLat = 0, prevLon = 0, prevAlt = 0;
    while (q.row()) {
        if (n == max) {
            more = true;
            break;
        }
        const qint64 t = q.i(0), lat = q.i(1), lon = q.i(2);
        const bool hasAlt = !q.null(3);
        const qint64 alt = q.i(3);
        QByteArray p = num(n ? t - prevT : 0) + ',' + num(lat - prevLat) + ',' + num(lon - prevLon) + ',';
        if (hasAlt)
            p += num(alt - prevAlt);
        p += ',';
        if (!q.null(4))
            p += num(q.i(4));
        p += ',';
        if (!q.null(5))
            p += num(q.i(5));
        if (points.size() + (n ? 1 : 0) + p.size() > room) {
            more = true;
            break;
        }
        if (n)
            points += ';';
        else
            t0 = t;
        points += p;
        n++;
        prevT = t;
        prevLat = lat;
        prevLon = lon;
        if (hasAlt)
            prevAlt = alt;
    }
    if (q.failed())
        return dbError(ty);
    QByteArray xml = start;
    if (n)
        xml += "<t0>" + num(t0) + "</t0>";
    xml += "<n>" + num(n) + "</n><mo>" + (more ? "1" : "0") + "</mo>";
    if (n)
        xml += "<r>" + points + "</r>";
    return xml + "</data>";
}

}
