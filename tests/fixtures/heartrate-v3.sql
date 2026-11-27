-- heartrate.db of asteroid-sensorlogd next to health-v3.sql's health.db: invented readings during that fixture's walk
-- (2026-01-10 07:00-07:21 UTC), one every 5 s with the hub's jitter, every 40th unreliable (not sent)
PRAGMA journal_mode=WAL;
CREATE TABLE IF NOT EXISTS heartrate (time INTEGER PRIMARY KEY, bpm INTEGER NOT NULL, accuracy INTEGER NOT NULL);
PRAGMA user_version=1;
WITH RECURSIVE k(i) AS (SELECT 0 UNION ALL SELECT i + 1 FROM k WHERE i < 251)
INSERT INTO heartrate (time, bpm, accuracy)
SELECT 1768028400000 + 5000 * i + i % 3, 96 + i % 25, CASE WHEN i % 40 = 7 THEN 1 ELSE 3 END FROM k;
