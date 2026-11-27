# asteroid-btsyncd tests

Unit and integration tests of the daemon that links the watch to the phone's Gadgetbridge over
Bluetooth LE. Each test drives btsyncd's GATT characteristics the way BlueZ does, plays the
phone, lipstick, BlueZ, oFono or the apps on a private D-Bus, and checks what btsyncd writes back.
Nothing here talks to a real watch, phone or bus.

## Running them

    tests/run.sh                 # build and run everything (native, see below)
    tests/run.sh -R relay        # only the tests whose names match
    tests/run.sh --keep          # keep .test-build/ for a quicker second run
    tests/run.sh --coverage      # gcc --coverage and a gcovr summary of src/
    tests/run.sh --cross         # against AsteroidOS's own libraries, under qemu-aarch64

The exit status is 0 when every test passed; the output of each test is in
`.test-build/ctest.log` (and `Testing/Temporary/LastTest.log` there).

Three ways to build:

| mode | how | libraries | used by |
| --- | --- | --- | --- |
| `--host` | `cmake -S tests` with this machine's Qt 6 (Core, D-Bus, Test) and sqlite3 | stubs for mlite and libkeepalive (`stubs/`); calendar_test and media_test need mkcal and AmberMpris and are left out without them | the default when pkg-config finds Qt 6 |
| `--docker` | the same in the image `.sonar/Dockerfile` makes (Debian, Qt 6.8) | as `--host` | the default otherwise; SonarQube's `.sonar/coverage.sh` |
| `--cross` | `tests/cross.sh`: an OpenEmbedded build's cross toolchain and sysroots, every test under qemu-aarch64 with the image rootfs's libraries | the real mlite (with a dconf-service), libkeepalive, AmberMpris, mkcal: every test | checking against the image |

`--cross` copies the rootfs and the sysroot components of a finished OE build once (about 5 GB,
`../rootfs-bt` by default, only while no bitbake runs) and keeps them; `tests/cross.sh --clean`
removes them. `OE_TMP`, `CROSS_ROOT` and `QEMU` say where things are (see the top of the script).

The recipe's own build (`-DBTSYNCD_TESTS=ON` at the top) builds the same `*_test` programs and
installs voice_test, healthrecords_test and assistant_test in the asteroid-btsyncd-tests package;
the Qt Test suites (`tst_*`) are built there only with `-DBTSYNCD_QTESTS=ON`, so the image build
compiles nothing new.

## What runs a test

`CMakeLists.txt` registers every test with ctest through `btsyncd_test(NAME [BUS] [DCONF] [QTEST]
SOURCES ... LIBS ... ENV ...)`: the test's source plus the daemon's sources it needs (no library
of the daemon is linked). ctest runs each one through `testenv.sh`:

- a scratch HOME, XDG directories and runtime directory of its own (never the user's);
- `BUS`: a private dbus-daemon that is both the session and the system bus (btsyncd's GATT objects
  live on the system bus, its APIs on the session bus);
- `DCONF`: with the real mlite also a dconf-service on that bus (`DCONF_SERVICE`); with the stubs
  MDConfItem is an in-memory store whose valueChanged() comes queued, as dconf's does;
- `QTEST`: a Qt Test suite (`tst_*`, `common/testmain.h`), always built with the stubs, whose
  counters it checks;
- a time limit (`BTSYNCD_TEST_TIMEOUT`, 280 s); `KEEP_TESTENV=1` keeps the scratch directory.

## The tests

| test | covers |
| --- | --- |
| tst_relay | the assistant relay's acts (CR-49, CR-50): every request and result of Gadgetbridge's fixtures byte for byte, the act timeouts in real time (15 s, 20 s), the 8192-byte limits, btsyncd's own acts, reconnections, a reply cut by a disconnection |
| assistant_test, assistantlink_test | the relay's XML; availability with the link, actions, phone events, contact names, the Weather refresh (CR-40) |
| messages_test | the Messages API over the relay (convs, msgs, msend) with Gadgetbridge's frames |
| tst_notificationservice | inserts, removals, buttons, typed replies, canned replies, restore after a restart, write framing, dismissals across a link loss |
| tst_notifparse | the insert element by element (versions 4, 5, CR-52) and every insert Gadgetbridge records |
| notifications_test, notifw2_test, messaging_test | protocol 3 and 4 (conversations, alerts, icons, keys, resync, Do Not Disturb), the W2 wave (acks, sub text, progress, mute model, call log), quiet on the charger, own messages, app on and off |
| pictures_test, msgpictures_test | notification and message pictures |
| tst_calls | call cards: answer, decline, end on the watch or the phone, answer-phone and replies (CR-53), the call log hooks, the missed-call card, stale calls after a reconnection |
| calls_test | calls on the watch through oFono's hands-free link (CR-07) |
| calllog_test | the call log object (CR-08) |
| phonestatus_test, modes_test, phonedata_test, timeservice_test | phone battery, Do Not Disturb and the modes, the time format and language, the phone's time against Settings (CR-27, CR-28) |
| weather_test, calendar_test, media_test, screenshot_test, battery_test, heartrate_test | the other services (calendar_test and media_test only with mkcal / AmberMpris) |
| tst_voiceservice, voice_test, voicelocal_test, voicelink_test | voice replies: the phone's recogniser, the watch's, the link |
| tst_healthservice, healthrecords_test | the health sync |
| tst_healthchanges | the health sync's version 3 (M4 WO-09): workouts with laps, pauses and tracks, sleep stages, resting heart rate from sensorlogd's health.db by its change sequence; Gadgetbridge's sync byte for byte (fixtures/health-v3-*.frames.txt from fixtures/health-v3.sql and heartrate-v3.sql; `FIXTURES_WRITE=1` writes the frames again), paging, the 8192-byte answers, tombstones, a workout still on, restarts, a database made again |
| tst_bluezmanager, advert_test | registration with BlueZ, advertising, the connection |
| tst_wakehold, connectivity_test | the keepalive hold; vibrate on connectivity change (CR-26) |

## Fixtures

`fixtures/` and `frames/` hold copies of the frames Gadgetbridge records in its own tests
(`app/src/test/resources/asteroidos/frames` of the gb repository): one write per line, `\\`, `\n`
and `\r` escaped. When Gadgetbridge's frames change, copy them again; the tests then say what
btsyncd must follow. They use invented names, example.org addresses and 555 numbers only; so must
new tests (no real names, numbers, addresses or places).

## SonarQube

`.sonar/coverage.sh` builds this directory with `--coverage` in the `.sonar/Dockerfile` image, runs
ctest one test at a time and writes `.sonar-reports/cobertura.xml` (src/ only) and
`.sonar-reports/ctest-junit.xml`. It uses the same CMakeLists.txt, so a test added here is
covered there.
