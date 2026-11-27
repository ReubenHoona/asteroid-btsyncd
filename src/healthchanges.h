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

#ifndef HEALTHCHANGES_H
#define HEALTHCHANGES_H

#include <QByteArray>
#include <QString>

/*
 * Version 3 of the health sync: the records asteroid-sensorlogd keeps in
 * <root>/health.db (schema 2 and later, its store/healthDb.h) that are made
 * once and may change later. sensorlogd is the only writer; every insert or
 * update of a workout, sleep session or day row gives the row the next value
 * of the database's change counter (meta 'seq'), a change of a workout's
 * track, laps or pauses or of a session's stages gives it to the parent, and
 * rows are never deleted (deleted = 1 is the tombstone). So the phone asks for
 * "the rows changed after sq N" and keeps the sq of the last answer.
 *
 * Requests (the phone's <req>, as for hr/steps/hrr):
 *   <req><ty>wo|sl|rhr</ty><sq>N</sq>[<mx>MAX</mx>]</req>
 *   <req><ty>wot</ty><id>WORKOUT</id><si>T_MS</si>[<mx>MAX</mx>]</req>
 * Answers, each at most MaxAnswerBytes (one page; the phone asks again while
 * mo is 1):
 *   <data><ty>wo</ty><hs>HEAD</hs><sq>NEXT</sq><n>N</n><mo>0|1</mo>RECORDS</data>
 *     hs: the counter now. A client whose sq is above it knows another
 *     database (made again): it asks again from 0.
 *     sq: the client's next sq (every row up to it was sent, or skipped: a
 *     workout still on is sent once it has ended, which changes its seq).
 *   <data><ty>wot</ty><id>ID</id><t0>T0</t0><n>N</n><mo>0|1</mo><r>POINTS</r></data>
 * Records (times in epoch ms; attributes of NULL columns left out; text
 * escaped as XML; reals with one decimal):
 *   <w sq id st en ty sr gp as dm kc sp ab xb zl zm zv zp az eg tp lc><l>LAPS</l><p>PAUSES</p></w>
 *     LAPS: n,start,length,distance m,avg bpm,kind; ... (start from st)
 *     PAUSES: start,length,auto; ... (start from st; length empty while open)
 *     tp: the track points (wot), lc: the laps (more than <l> has when the
 *     record alone was over the size: the last laps are cut)
 *   <w sq id st [en] dl="1"/>                       a deleted workout
 *   <s sq id wd st en mn as aw rm li dp sc gl hr al ns>STAGES</s>
 *     STAGES: gap,length,stage; ... (gap: from the end of the span before,
 *     the first from st; stage 0 awake, 1 light, 2 deep, 3 REM; ns: the spans)
 *   <s sq id wd st en mn dl="1"/>                   a session replaced or deleted
 *   <d sq dt [rh]/>                                 a day (local ISO date) and its resting heart rate
 * POINTS: dt,dlat,dlon,dalt,hacc,speed; ... dt from the point before (the
 * first from t0, so 0), lat/lon (1e-7 degrees) and altitude (dm) from the
 * point before that has one (the first from 0), hacc (dm) and speed (cm/s) as
 * they are; empty: not known.
 */
namespace HealthChanges {

enum Kind { None, Workouts, WorkoutTrack, Sleep, RestingHeartRate };

// The most bytes of one answer of these kinds: what the phone joins of a
// relay reply (8192, Gadgetbridge's MAX_REPLY_WIRE_BYTES), so a page cut by
// a disconnection costs little
const int MaxAnswerBytes = 8192;

// The most records of one answer (a client may ask for fewer)
const int MaxRecords = 1000;

Kind kindFromName(const QString &name);
QByteArray kindName(Kind kind);

// wo, sl, rhr: the rows changed after sinceSeq, oldest change first, at most max and maxBytes
QByteArray changes(const QString &root, Kind kind, qint64 sinceSeq, int max = MaxRecords,
                   int maxBytes = MaxAnswerBytes);

// wot: the track points of a workout after sinceMs (exclusive), at most max and maxBytes
QByteArray track(const QString &root, qint64 workoutId, qint64 sinceMs, int max = MaxRecords,
                 int maxBytes = MaxAnswerBytes);

/*
 * What a watch-started sync looks for (R5 E-08, healthservice.h): the newest
 * workout that has ended and the newest main night, as health.db has them now.
 * A later read with a higher workout id or a later wake date is a workout
 * that ended or a night that was added since (a night estimated again keeps
 * its wake date). valid: the database was read (there is none yet: false).
 */
struct SyncMarks {
    bool valid = false;
    qint64 workoutId = 0;
    QString wakeDate;
};
SyncMarks syncMarks(const QString &root);

}

#endif // HEALTHCHANGES_H
