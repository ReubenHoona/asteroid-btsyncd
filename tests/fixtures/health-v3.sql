-- health.db of asteroid-sensorlogd for the health sync of version 3 (tst_healthchanges, and Gadgetbridge's
-- AsteroidOSHealthSyncTest through the frames it gives: health-v3-*.frames.txt).
-- The schema is sensorlogd's of schema 2 (store/healthDb.cpp of r4-66 a6afac5: SCHEMA_V1, the triggers of the change
-- sequence as seqTriggers() makes them, SCHEMA_V2); the records are invented and written with the statements
-- sensorlogd's HealthDb uses, in the order a watch would write them, so the triggers give them their seq.
-- Times: epoch ms, 2026-01-09 to 2026-01-11 (UTC). Positions: near 0,0 (1e-7 degrees).
PRAGMA journal_mode=WAL;
BEGIN IMMEDIATE;
CREATE TABLE IF NOT EXISTS meta (key TEXT PRIMARY KEY, value);
INSERT OR IGNORE INTO meta (key, value) VALUES ('seq', 0);
CREATE TABLE IF NOT EXISTS workout (id INTEGER PRIMARY KEY, start_ms INTEGER NOT NULL UNIQUE, end_ms INTEGER, active_s INTEGER NOT NULL DEFAULT 0, type TEXT NOT NULL, distance_m REAL, kcal REAL, steps INTEGER, avg_bpm INTEGER, max_bpm INTEGER, light_s INTEGER, moderate_s INTEGER, vigorous_s INTEGER, peak_s INTEGER, azm INTEGER, elev_gain_m REAL, source TEXT NOT NULL DEFAULT 'manual', gps INTEGER NOT NULL DEFAULT 0, imported_from TEXT, seq INTEGER NOT NULL DEFAULT 0, deleted INTEGER NOT NULL DEFAULT 0);
CREATE INDEX IF NOT EXISTS workout_seq ON workout (seq);
CREATE TABLE IF NOT EXISTS workout_track (workout_id INTEGER NOT NULL, t_ms INTEGER NOT NULL, lat_e7 INTEGER NOT NULL, lon_e7 INTEGER NOT NULL, alt_dm INTEGER, hacc_dm INTEGER, speed_cms INTEGER, PRIMARY KEY (workout_id, t_ms)) WITHOUT ROWID;
CREATE TABLE IF NOT EXISTS workout_lap (workout_id INTEGER NOT NULL, n INTEGER NOT NULL, start_ms INTEGER NOT NULL, end_ms INTEGER NOT NULL, distance_m REAL, avg_bpm INTEGER, kind TEXT NOT NULL DEFAULT 'manual', PRIMARY KEY (workout_id, n));
CREATE TABLE IF NOT EXISTS workout_pause (workout_id INTEGER NOT NULL, start_ms INTEGER NOT NULL, end_ms INTEGER, auto INTEGER NOT NULL DEFAULT 0, PRIMARY KEY (workout_id, start_ms));
CREATE TABLE IF NOT EXISTS sleep_session (id INTEGER PRIMARY KEY, wake_date TEXT NOT NULL, start_ms INTEGER NOT NULL, end_ms INTEGER NOT NULL, main INTEGER NOT NULL DEFAULT 0, asleep_min INTEGER, awake_min INTEGER, rem_min INTEGER, light_min INTEGER, deep_min INTEGER, score INTEGER, goal_min INTEGER, avg_hr REAL, spo2_avg REAL, spo2_min REAL, spo2_max REAL, temp_dev_c REAL, resp_rate REAL, hrv_rmssd REAL, algo TEXT, imported_from TEXT, seq INTEGER NOT NULL DEFAULT 0, deleted INTEGER NOT NULL DEFAULT 0);
CREATE INDEX IF NOT EXISTS sleep_session_wake ON sleep_session (wake_date);
CREATE INDEX IF NOT EXISTS sleep_session_seq ON sleep_session (seq);
CREATE TABLE IF NOT EXISTS sleep_stage (session_id INTEGER NOT NULL, start_ms INTEGER NOT NULL, end_ms INTEGER NOT NULL, stage INTEGER NOT NULL, PRIMARY KEY (session_id, start_ms));
CREATE TABLE IF NOT EXISTS daily (date TEXT PRIMARY KEY, resting_hr INTEGER, hrv_rmssd REAL, resp_rate REAL, temp_dev_c REAL, steps INTEGER, kcal REAL, distance_m REAL, floors INTEGER, azm INTEGER, hours_met INTEGER, cardio_load REAL, readiness INTEGER, closed_ms INTEGER, seq INTEGER NOT NULL DEFAULT 0);
CREATE INDEX IF NOT EXISTS daily_seq ON daily (seq);
CREATE TABLE IF NOT EXISTS spo2 (t_ms INTEGER PRIMARY KEY, pct REAL NOT NULL, confidence INTEGER);
CREATE TABLE IF NOT EXISTS hrv (t_ms INTEGER PRIMARY KEY, rmssd_ms REAL NOT NULL);
CREATE TABLE IF NOT EXISTS skin_temp (t_ms INTEGER PRIMARY KEY, c REAL NOT NULL);
CREATE TABLE IF NOT EXISTS eda_event (id INTEGER PRIMARY KEY, start_ms INTEGER NOT NULL, end_ms INTEGER, level INTEGER, mood TEXT, seq INTEGER NOT NULL DEFAULT 0, deleted INTEGER NOT NULL DEFAULT 0);
CREATE INDEX IF NOT EXISTS eda_event_seq ON eda_event (seq);
CREATE TRIGGER IF NOT EXISTS workout_seq_insert AFTER INSERT ON workout BEGIN UPDATE meta SET value = value + 1 WHERE key = 'seq'; UPDATE workout SET seq = (SELECT value FROM meta WHERE key = 'seq') WHERE id = NEW.id; END;
CREATE TRIGGER IF NOT EXISTS workout_seq_update AFTER UPDATE ON workout WHEN NEW.seq IS OLD.seq BEGIN UPDATE meta SET value = value + 1 WHERE key = 'seq'; UPDATE workout SET seq = (SELECT value FROM meta WHERE key = 'seq') WHERE id = NEW.id; END;
CREATE TRIGGER IF NOT EXISTS sleep_session_seq_insert AFTER INSERT ON sleep_session BEGIN UPDATE meta SET value = value + 1 WHERE key = 'seq'; UPDATE sleep_session SET seq = (SELECT value FROM meta WHERE key = 'seq') WHERE id = NEW.id; END;
CREATE TRIGGER IF NOT EXISTS sleep_session_seq_update AFTER UPDATE ON sleep_session WHEN NEW.seq IS OLD.seq BEGIN UPDATE meta SET value = value + 1 WHERE key = 'seq'; UPDATE sleep_session SET seq = (SELECT value FROM meta WHERE key = 'seq') WHERE id = NEW.id; END;
CREATE TRIGGER IF NOT EXISTS daily_seq_insert AFTER INSERT ON daily BEGIN UPDATE meta SET value = value + 1 WHERE key = 'seq'; UPDATE daily SET seq = (SELECT value FROM meta WHERE key = 'seq') WHERE date = NEW.date; END;
CREATE TRIGGER IF NOT EXISTS daily_seq_update AFTER UPDATE ON daily WHEN NEW.seq IS OLD.seq BEGIN UPDATE meta SET value = value + 1 WHERE key = 'seq'; UPDATE daily SET seq = (SELECT value FROM meta WHERE key = 'seq') WHERE date = NEW.date; END;
CREATE TRIGGER IF NOT EXISTS eda_event_seq_insert AFTER INSERT ON eda_event BEGIN UPDATE meta SET value = value + 1 WHERE key = 'seq'; UPDATE eda_event SET seq = (SELECT value FROM meta WHERE key = 'seq') WHERE id = NEW.id; END;
CREATE TRIGGER IF NOT EXISTS eda_event_seq_update AFTER UPDATE ON eda_event WHEN NEW.seq IS OLD.seq BEGIN UPDATE meta SET value = value + 1 WHERE key = 'seq'; UPDATE eda_event SET seq = (SELECT value FROM meta WHERE key = 'seq') WHERE id = NEW.id; END;
CREATE TRIGGER IF NOT EXISTS workout_track_touch_insert AFTER INSERT ON workout_track BEGIN UPDATE workout SET seq = seq WHERE id = NEW.workout_id; END;
CREATE TRIGGER IF NOT EXISTS workout_track_touch_update AFTER UPDATE ON workout_track BEGIN UPDATE workout SET seq = seq WHERE id = NEW.workout_id; END;
CREATE TRIGGER IF NOT EXISTS workout_track_touch_delete AFTER DELETE ON workout_track BEGIN UPDATE workout SET seq = seq WHERE id = OLD.workout_id; END;
CREATE TRIGGER IF NOT EXISTS workout_lap_touch_insert AFTER INSERT ON workout_lap BEGIN UPDATE workout SET seq = seq WHERE id = NEW.workout_id; END;
CREATE TRIGGER IF NOT EXISTS workout_lap_touch_update AFTER UPDATE ON workout_lap BEGIN UPDATE workout SET seq = seq WHERE id = NEW.workout_id; END;
CREATE TRIGGER IF NOT EXISTS workout_lap_touch_delete AFTER DELETE ON workout_lap BEGIN UPDATE workout SET seq = seq WHERE id = OLD.workout_id; END;
CREATE TRIGGER IF NOT EXISTS workout_pause_touch_insert AFTER INSERT ON workout_pause BEGIN UPDATE workout SET seq = seq WHERE id = NEW.workout_id; END;
CREATE TRIGGER IF NOT EXISTS workout_pause_touch_update AFTER UPDATE ON workout_pause BEGIN UPDATE workout SET seq = seq WHERE id = NEW.workout_id; END;
CREATE TRIGGER IF NOT EXISTS workout_pause_touch_delete AFTER DELETE ON workout_pause BEGIN UPDATE workout SET seq = seq WHERE id = OLD.workout_id; END;
CREATE TRIGGER IF NOT EXISTS sleep_stage_touch_insert AFTER INSERT ON sleep_stage BEGIN UPDATE sleep_session SET seq = seq WHERE id = NEW.session_id; END;
CREATE TRIGGER IF NOT EXISTS sleep_stage_touch_update AFTER UPDATE ON sleep_stage BEGIN UPDATE sleep_session SET seq = seq WHERE id = NEW.session_id; END;
CREATE TRIGGER IF NOT EXISTS sleep_stage_touch_delete AFTER DELETE ON sleep_stage BEGIN UPDATE sleep_session SET seq = seq WHERE id = OLD.session_id; END;
CREATE TABLE IF NOT EXISTS workout_state (workout_id INTEGER PRIMARY KEY, saved_ms INTEGER NOT NULL, state TEXT NOT NULL);
PRAGMA user_version=2;
COMMIT;

-- 2026-01-10 06:50, the first start of this sensorlogd: the one-time import of the old files and days
INSERT INTO sleep_session (wake_date, start_ms, end_ms, main, asleep_min, awake_min, rem_min, light_min, deep_min, score, goal_min, avg_hr, algo, imported_from) VALUES ('2026-01-10', 1768000200000, 1768027200000, 1, 430, 20, 0, 0, 0, 74, 480, 58.2, 'eos-steps-hr-1', 'sleep/2026-01-10.json');
INSERT OR REPLACE INTO meta (key, value) VALUES ('import.sleep', 1);
INSERT OR IGNORE INTO workout (start_ms, end_ms, active_s, type, distance_m, kcal, steps, avg_bpm, max_bpm, source, gps, imported_from) VALUES (1767979800000, 1767982500000, 2700, 'Bike', 14250.0, 402.5, 0, 128, 151, 'manual', 0, 'exercise/2026-01-09.log');
INSERT OR REPLACE INTO meta (key, value) VALUES ('import.exercise', 1);
INSERT INTO daily (date, steps, kcal, distance_m, floors, azm, hours_met, closed_ms) VALUES ('2026-01-08', 6020, 1980.0, 4510.0, 3, 12, 7, 1768027800000);
INSERT INTO daily (date, steps, kcal, distance_m, floors, azm, hours_met, closed_ms) VALUES ('2026-01-09', 9310, 2205.5, 7020.0, 5, 41, 8, 1768027800000);
INSERT OR REPLACE INTO meta (key, value) VALUES ('import.daily', 1);
INSERT INTO daily (date, resting_hr) VALUES ('2026-01-09', 55) ON CONFLICT (date) DO UPDATE SET resting_hr = excluded.resting_hr WHERE resting_hr IS NOT excluded.resting_hr;
INSERT OR REPLACE INTO meta (key, value) VALUES ('import.resting', 1);

-- 07:00-07:21 a walk with GPS, a fix every 2 s, paused 07:10-07:12, a kilometre lap and a manual one
INSERT INTO workout (start_ms, type, source, gps) VALUES (1768028400000, 'Walk', 'manual', 1);
WITH RECURSIVE k(i) AS (SELECT 0 UNION ALL SELECT i + 1 FROM k WHERE i < 299)
INSERT INTO workout_track (workout_id, t_ms, lat_e7, lon_e7, alt_dm, hacc_dm, speed_cms)
SELECT (SELECT id FROM workout WHERE start_ms = 1768028400000), 1768028400000 + 2000 * i, 10000 + 200 * i, 20000 + 150 * i,
       CASE WHEN i % 50 = 25 THEN NULL ELSE 1200 + i % 7 END, 50 + 10 * (i % 3), CASE WHEN i = 0 THEN NULL ELSE 135 + 3 * (i % 5) END FROM k;
INSERT OR REPLACE INTO workout_state (workout_id, saved_ms, state) VALUES ((SELECT id FROM workout WHERE start_ms = 1768028400000), 1768029000000, '{"v":2,"paused":false}');
INSERT INTO workout_pause (workout_id, start_ms, auto) VALUES ((SELECT id FROM workout WHERE start_ms = 1768028400000), 1768029000000, 0);
UPDATE workout_pause SET end_ms = 1768029120000 WHERE workout_id = (SELECT id FROM workout WHERE start_ms = 1768028400000) AND end_ms IS NULL AND start_ms <= 1768029120000;
WITH RECURSIVE k(i) AS (SELECT 360 UNION ALL SELECT i + 1 FROM k WHERE i < 420)
INSERT INTO workout_track (workout_id, t_ms, lat_e7, lon_e7, alt_dm, hacc_dm, speed_cms)
SELECT (SELECT id FROM workout WHERE start_ms = 1768028400000), 1768028400000 + 2000 * i, 10000 + 200 * (i - 60), 20000 + 150 * (i - 60),
       CASE WHEN i % 50 = 25 THEN NULL ELSE 1200 + i % 7 END, 50 + 10 * (i % 3), 135 + 3 * (i % 5) FROM k;
INSERT OR REPLACE INTO workout_lap (workout_id, n, start_ms, end_ms, distance_m, avg_bpm, kind) VALUES ((SELECT id FROM workout WHERE start_ms = 1768028400000), 1, 1768028400000, 1768029240000, 1000.0, 102, 'split-km');
WITH RECURSIVE k(i) AS (SELECT 421 UNION ALL SELECT i + 1 FROM k WHERE i < 600)
INSERT INTO workout_track (workout_id, t_ms, lat_e7, lon_e7, alt_dm, hacc_dm, speed_cms)
SELECT (SELECT id FROM workout WHERE start_ms = 1768028400000), 1768028400000 + 2000 * i, 10000 + 200 * (i - 60), 20000 + 150 * (i - 60),
       CASE WHEN i % 50 = 25 THEN NULL ELSE 1200 + i % 7 END, 50 + 10 * (i % 3), 135 + 3 * (i % 5) FROM k;
INSERT OR REPLACE INTO workout_lap (workout_id, n, start_ms, end_ms, distance_m, avg_bpm, kind) VALUES ((SELECT id FROM workout WHERE start_ms = 1768028400000), 2, 1768029240000, 1768029600000, 650.3, 108, 'manual');
WITH RECURSIVE k(i) AS (SELECT 601 UNION ALL SELECT i + 1 FROM k WHERE i < 629)
INSERT INTO workout_track (workout_id, t_ms, lat_e7, lon_e7, alt_dm, hacc_dm, speed_cms)
SELECT (SELECT id FROM workout WHERE start_ms = 1768028400000), 1768028400000 + 2000 * i, 10000 + 200 * (i - 60), 20000 + 150 * (i - 60),
       CASE WHEN i % 50 = 25 THEN NULL ELSE 1200 + i % 7 END, 50 + 10 * (i % 3), 135 + 3 * (i % 5) FROM k;
UPDATE workout_pause SET end_ms = MAX(start_ms, 1768029660000) WHERE workout_id = (SELECT id FROM workout WHERE start_ms = 1768028400000) AND end_ms IS NULL;
UPDATE workout SET end_ms = 1768029660000, active_s = 1140, distance_m = 1581.6, kcal = 96.4, steps = 2150, avg_bpm = 104, max_bpm = 121, light_s = 640, moderate_s = 380, vigorous_s = 0, peak_s = 0, azm = 6, elev_gain_m = 4.2 WHERE id = (SELECT id FROM workout WHERE start_ms = 1768028400000) AND start_ms <= 1768029660000;
DELETE FROM workout_state WHERE workout_id = (SELECT id FROM workout WHERE start_ms = 1768028400000);

-- 18:00-18:30 a treadmill run, a lap every kilometre (steps' distance)
INSERT INTO workout (start_ms, type, source, gps) VALUES (1768068000000, 'TreadMill', 'manual', 0);
INSERT OR REPLACE INTO workout_lap (workout_id, n, start_ms, end_ms, distance_m, avg_bpm, kind) VALUES ((SELECT id FROM workout WHERE start_ms = 1768068000000), 1, 1768068000000, 1768068330000, 1000.0, 155, 'split-km');
INSERT OR REPLACE INTO workout_lap (workout_id, n, start_ms, end_ms, distance_m, avg_bpm, kind) VALUES ((SELECT id FROM workout WHERE start_ms = 1768068000000), 2, 1768068330000, 1768068655000, 1000.0, 161, 'split-km');
INSERT OR REPLACE INTO workout_lap (workout_id, n, start_ms, end_ms, distance_m, avg_bpm, kind) VALUES ((SELECT id FROM workout WHERE start_ms = 1768068000000), 3, 1768068655000, 1768068985000, 1000.0, 164, 'split-km');
UPDATE workout_pause SET end_ms = MAX(start_ms, 1768069800000) WHERE workout_id = (SELECT id FROM workout WHERE start_ms = 1768068000000) AND end_ms IS NULL;
UPDATE workout SET end_ms = 1768069800000, active_s = 1800, distance_m = 5420.0, kcal = 365.0, steps = 5200, avg_bpm = 158, max_bpm = 172, light_s = 120, moderate_s = 540, vigorous_s = 980, peak_s = 160, azm = 52, elev_gain_m = 0.0 WHERE id = (SELECT id FROM workout WHERE start_ms = 1768068000000) AND start_ms <= 1768069800000;

-- 2026-01-11 00:02, the first delivery after midnight closes 2026-01-10
INSERT INTO daily (date, steps, kcal, distance_m, floors, azm, hours_met, closed_ms) VALUES ('2026-01-10', 11234, 2310.5, 8650.0, 7, 58, 9, 1768089720000);
INSERT INTO daily (date, resting_hr) VALUES ('2026-01-10', 53) ON CONFLICT (date) DO UPDATE SET resting_hr = excluded.resting_hr WHERE resting_hr IS NOT excluded.resting_hr;

-- 07:20 the night 22:40-06:50 stored with its stage timeline (version 2 of the estimate)
UPDATE sleep_session SET deleted = 1 WHERE wake_date = '2026-01-11' AND deleted = 0;
INSERT INTO sleep_session (wake_date, start_ms, end_ms, main, asleep_min, awake_min, rem_min, light_min, deep_min, score, goal_min, avg_hr, algo, imported_from) VALUES ('2026-01-11', 1768084800000, 1768114200000, 1, 480, 10, 90, 305, 85, 86, 480, 56.4, 'eos-motion-hr-2', NULL);
INSERT INTO sleep_stage (session_id, start_ms, end_ms, stage)
SELECT (SELECT id FROM sleep_session WHERE wake_date = '2026-01-11' AND main = 1 AND deleted = 0), 1768084800000 + a * 60000, 1768084800000 + b * 60000, s
FROM (SELECT 0 AS a, 25 AS b, 1 AS s UNION ALL SELECT 25, 75, 2 UNION ALL SELECT 75, 140, 1 UNION ALL SELECT 140, 165, 3
      UNION ALL SELECT 165, 175, 0 UNION ALL SELECT 175, 245, 1 UNION ALL SELECT 245, 280, 2 UNION ALL SELECT 280, 330, 1
      UNION ALL SELECT 330, 365, 3 UNION ALL SELECT 365, 420, 1 UNION ALL SELECT 420, 450, 3 UNION ALL SELECT 450, 490, 1);

-- 08:00 a yoga workout begun, and deleted at 08:03 (the tombstone stays)
INSERT INTO workout (start_ms, type, source, gps) VALUES (1768118400000, 'Yoga', 'manual', 0);
INSERT OR REPLACE INTO workout_state (workout_id, saved_ms, state) VALUES ((SELECT id FROM workout WHERE start_ms = 1768118400000), 1768118580000, '{"v":2,"paused":false}');
UPDATE workout SET deleted = 1 WHERE id = (SELECT id FROM workout WHERE start_ms = 1768118400000) AND deleted = 0;
DELETE FROM workout_track WHERE workout_id = (SELECT id FROM workout WHERE start_ms = 1768118400000);
DELETE FROM workout_lap WHERE workout_id = (SELECT id FROM workout WHERE start_ms = 1768118400000);
DELETE FROM workout_pause WHERE workout_id = (SELECT id FROM workout WHERE start_ms = 1768118400000);
DELETE FROM workout_state WHERE workout_id = (SELECT id FROM workout WHERE start_ms = 1768118400000);

-- 12:00-12:20 a HIIT session the watch noticed by itself
INSERT INTO workout (start_ms, type, source, gps) VALUES (1768132800000, 'HIIT', 'auto', 0);
UPDATE workout_pause SET end_ms = MAX(start_ms, 1768134000000) WHERE workout_id = (SELECT id FROM workout WHERE start_ms = 1768132800000) AND end_ms IS NULL;
UPDATE workout SET end_ms = 1768134000000, active_s = 1200, distance_m = 0.0, kcal = 188.0, steps = 900, avg_bpm = 141, max_bpm = 176, light_s = 200, moderate_s = 300, vigorous_s = 500, peak_s = 200, azm = 37, elev_gain_m = 0.0 WHERE id = (SELECT id FROM workout WHERE start_ms = 1768132800000) AND start_ms <= 1768134000000;

-- 14:55 a nap 14:00-14:25 is over: the morning's sessions stored again with it (the old ones tombstoned)
UPDATE sleep_session SET deleted = 1 WHERE wake_date = '2026-01-11' AND deleted = 0;
INSERT INTO sleep_session (wake_date, start_ms, end_ms, main, asleep_min, awake_min, rem_min, light_min, deep_min, score, goal_min, avg_hr, algo, imported_from) VALUES ('2026-01-11', 1768084800000, 1768114200000, 1, 480, 10, 90, 305, 85, 86, 480, 56.4, 'eos-motion-hr-2', NULL);
INSERT INTO sleep_stage (session_id, start_ms, end_ms, stage)
SELECT (SELECT id FROM sleep_session WHERE wake_date = '2026-01-11' AND main = 1 AND deleted = 0), 1768084800000 + a * 60000, 1768084800000 + b * 60000, s
FROM (SELECT 0 AS a, 25 AS b, 1 AS s UNION ALL SELECT 25, 75, 2 UNION ALL SELECT 75, 140, 1 UNION ALL SELECT 140, 165, 3
      UNION ALL SELECT 165, 175, 0 UNION ALL SELECT 175, 245, 1 UNION ALL SELECT 245, 280, 2 UNION ALL SELECT 280, 330, 1
      UNION ALL SELECT 330, 365, 3 UNION ALL SELECT 365, 420, 1 UNION ALL SELECT 420, 450, 3 UNION ALL SELECT 450, 490, 1);
INSERT INTO sleep_session (wake_date, start_ms, end_ms, main, asleep_min, awake_min, rem_min, light_min, deep_min, score, goal_min, avg_hr, algo, imported_from) VALUES ('2026-01-11', 1768140000000, 1768141500000, 0, 25, 0, 0, 17, 8, NULL, NULL, 57.0, 'eos-motion-hr-2', NULL);
INSERT INTO sleep_stage (session_id, start_ms, end_ms, stage)
SELECT (SELECT id FROM sleep_session WHERE wake_date = '2026-01-11' AND main = 0 AND deleted = 0), 1768140000000 + a * 60000, 1768140000000 + b * 60000, s
FROM (SELECT 0 AS a, 10 AS b, 1 AS s UNION ALL SELECT 10, 18, 2 UNION ALL SELECT 18, 25, 1);

-- 16:00 a walk that is still on: three fixes so far (south-west of 0,0), its snapshot for the app's recovery
INSERT INTO workout (start_ms, type, source, gps) VALUES (1768147200000, 'Walk', 'manual', 1);
WITH RECURSIVE k(i) AS (SELECT 0 UNION ALL SELECT i + 1 FROM k WHERE i < 2)
INSERT INTO workout_track (workout_id, t_ms, lat_e7, lon_e7, alt_dm, hacc_dm, speed_cms)
SELECT (SELECT id FROM workout WHERE start_ms = 1768147200000), 1768147200000 + 1000 * i, -5000 - 100 * i, -3000 - 80 * i, 950, 80, 120 FROM k;
INSERT OR REPLACE INTO workout_state (workout_id, saved_ms, state) VALUES ((SELECT id FROM workout WHERE start_ms = 1768147200000), 1768147203000, '{"v":2,"paused":false}');
