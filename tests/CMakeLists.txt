# Tests of asteroid-btsyncd (see README.md; tests/run.sh builds and runs them all).
#
# Two ways to build them:
#   - with the daemon (cmake -DBTSYNCD_TESTS=ON at the top): the SDK build of the recipe, against
#     AsteroidOS's libraries (mlite, libkeepalive, AmberMpris, mkcal). Its asteroid-btsyncd-tests
#     package installs voice_test, healthrecords_test and assistant_test. The Qt Test suites (tst_*)
#     are built there too with -DBTSYNCD_QTESTS=ON (tests/run.sh --cross does), not in the image build.
#   - on their own (cmake -S tests): a native build against the distribution's Qt 6. What Debian
#     does not have is replaced by the stubs in stubs/ (mlite's MDConfItem, libkeepalive's
#     BackgroundActivity); the tests that need AmberMpris or mkcal are left out when those are not
#     found. Coverage: -DBTSYNCD_COVERAGE=ON (.sonar/coverage.sh).
#
# Every test is registered with ctest. testenv.sh gives each one a scratch HOME and, for the tests
# that talk D-Bus, a private bus that is both the session and the system bus; with the real mlite
# the dconf tests get a dconf-service of their own on it (DCONF_SERVICE, or found in PATH / libexec).
# Cross builds run under CMAKE_CROSSCOMPILING_EMULATOR (qemu-aarch64 -L sysroot).
cmake_minimum_required(VERSION 3.16)

if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
	project(asteroid-btsyncd-tests LANGUAGES CXX)
	set(CMAKE_CXX_STANDARD 17)
	set(CMAKE_CXX_STANDARD_REQUIRED ON)
	set(CMAKE_AUTOMOC ON)
	find_package(Qt6 REQUIRED COMPONENTS Core DBus Test)
	find_package(PkgConfig REQUIRED)
	pkg_check_modules(SQLITE3 sqlite3 REQUIRED IMPORTED_TARGET)
	pkg_check_modules(MLITE mlite6 IMPORTED_TARGET)
	pkg_check_modules(KEEPALIVE keepalive IMPORTED_TARGET)
	pkg_check_modules(AmberMpris ambermpris6 IMPORTED_TARGET)
	pkg_check_modules(MKCAL libmkcal-qt6 IMPORTED_TARGET)
	pkg_check_modules(KCALCORE KF6CalendarCore IMPORTED_TARGET)
	option(BTSYNCD_COVERAGE "Build with gcc --coverage" OFF)
	if(BTSYNCD_COVERAGE)
		add_compile_options(--coverage -O0 -g)
		add_link_options(--coverage)
	endif()
	set(BTSYNCD_QTESTS ON)
else()
	find_package(Qt6 QUIET COMPONENTS Test)
	option(BTSYNCD_QTESTS "Also build the Qt Test suites (tst_*)" OFF)
endif()

enable_testing()
set(SRC ${CMAKE_CURRENT_SOURCE_DIR}/../src)
set(TESTENV ${CMAKE_CURRENT_SOURCE_DIR}/testenv.sh)

# mlite and libkeepalive, or the stubs (both or neither)
if(MLITE_FOUND AND KEEPALIVE_FOUND)
	set(BTSYNCD_STUBS OFF)
	set(MLITE PkgConfig::MLITE)
	set(KEEPALIVE PkgConfig::KEEPALIVE)
else()
	set(BTSYNCD_STUBS ON)
	set(MLITE "")
	set(KEEPALIVE "")
	message(STATUS "btsyncd tests: mlite6 / keepalive not found, stubs/ instead (in-memory dconf)")
endif()

# The GATT plumbing every service needs, and the notification service with what it uses
set(GATT ${SRC}/characteristic.cpp ${SRC}/service.cpp ${SRC}/descriptor.cpp)
set(NOTIF ${SRC}/notificationservice.cpp ${SRC}/batterystatus.cpp ${GATT} ${SRC}/wakehold.cpp)

# btsyncd_test(NAME [BUS] [DCONF] [QTEST] SOURCES ... LIBS ... ENV ...)
#   BUS: on a private D-Bus (session = system); DCONF: and a dconf-service (real mlite only);
#   QTEST: a Qt Test suite, always with the stubs (it checks what was written through them)
function(btsyncd_test name)
	cmake_parse_arguments(T "BUS;DCONF;QTEST" "" "SOURCES;LIBS;ENV" ${ARGN})
	add_executable(${name} ${name}.cpp ${T_SOURCES})
	set_target_properties(${name} PROPERTIES AUTOMOC ON)
	target_include_directories(${name} PRIVATE ${SRC} ${CMAKE_CURRENT_SOURCE_DIR} ${CMAKE_CURRENT_SOURCE_DIR}/common)
	target_compile_definitions(${name} PRIVATE FRAMES_DIR="${CMAKE_CURRENT_SOURCE_DIR}/frames"
		FIXTURE_DIR="${CMAKE_CURRENT_SOURCE_DIR}/fixtures")
	set(libs Qt6::Core ${T_LIBS})
	if(T_QTEST OR BTSYNCD_STUBS)
		# stubs/ first: MDConfItem and backgroundactivity.h instead of mlite / libkeepalive
		target_include_directories(${name} BEFORE PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/stubs)
		target_sources(${name} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/stubs/mdconfitem.h)
		list(REMOVE_ITEM libs PkgConfig::MLITE PkgConfig::KEEPALIVE)
	endif()
	if(T_QTEST)
		list(APPEND libs Qt6::Test)
	endif()
	target_link_libraries(${name} PRIVATE ${libs})
	set(envflags)
	if(T_BUS OR T_DCONF)
		list(APPEND envflags --bus)
	endif()
	if(T_DCONF AND NOT BTSYNCD_STUBS)
		list(APPEND envflags --dconf)
	endif()
	add_test(NAME ${name} COMMAND sh ${TESTENV} ${envflags} -- ${CMAKE_CROSSCOMPILING_EMULATOR} $<TARGET_FILE:${name}>)
	set_tests_properties(${name} PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen;${T_ENV}" TIMEOUT 300)
endfunction()

# Plain checks without D-Bus (exit status = failures). The recipe installs these three.
btsyncd_test(healthrecords_test SOURCES ${SRC}/healthrecords.cpp LIBS PkgConfig::SQLITE3)
btsyncd_test(voice_test SOURCES ${SRC}/voiceprotocol.cpp ${SRC}/voicecond.cpp)
# The assistant relay's XML
btsyncd_test(assistant_test SOURCES ${SRC}/assistantprotocol.cpp ${SRC}/contactnames.cpp)

# The assistant relay follows the phone's link; the Weather app's refresh (CR-40)
btsyncd_test(assistantlink_test DCONF SOURCES ${SRC}/assistantservice.cpp ${SRC}/assistantprotocol.cpp
	${SRC}/contactnames.cpp ${NOTIF} LIBS Qt6::DBus ${MLITE} ${KEEPALIVE})

# Voice replies recognised on the watch (a fake asteroid-assistant), and by the phone
foreach(t voicelocal_test voicelink_test)
	btsyncd_test(${t} BUS SOURCES ${SRC}/voiceservice.cpp ${SRC}/voicesource.cpp ${SRC}/voiceprotocol.cpp
		${SRC}/voicecond.cpp ${NOTIF} LIBS Qt6::DBus ${MLITE} ${KEEPALIVE})
endforeach()

# The calls on the watch with a fake oFono and BlueZ (BTSYNCD_CALLS_TIME_DIV: its waits 100 times shorter)
btsyncd_test(calls_test DCONF SOURCES ${SRC}/callaudio.cpp LIBS Qt6::DBus ${MLITE} ENV BTSYNCD_CALLS_TIME_DIV=100)

# The notification service (protocol versions 3 and 4) with a fake lipstick; pictures; messaging polish
foreach(t notifications_test pictures_test msgpictures_test)
	btsyncd_test(${t} BUS SOURCES ${NOTIF} LIBS Qt6::DBus ${MLITE} ${KEEPALIVE})
endforeach()
btsyncd_test(messaging_test DCONF SOURCES ${NOTIF} LIBS Qt6::DBus ${MLITE} ${KEEPALIVE})
# The notification protocol of the wearui-w2 wave with the frames Gadgetbridge records (frames/)
btsyncd_test(notifw2_test DCONF SOURCES ${NOTIF} ${SRC}/calllog.cpp LIBS Qt6::DBus ${MLITE} ${KEEPALIVE})

# The phone status service (battery, Do Not Disturb, disconnected notice); the modes on it
# (since f421265 the phone's Do Not Disturb and the watch's Bedtime meet in dconf: DCONF)
btsyncd_test(phonestatus_test DCONF SOURCES ${SRC}/phonestatusservice.cpp ${NOTIF} LIBS Qt6::DBus ${MLITE} ${KEEPALIVE})
btsyncd_test(modes_test DCONF SOURCES ${SRC}/phonestatusservice.cpp ${NOTIF} LIBS Qt6::DBus ${MLITE} ${KEEPALIVE})

# The battery service (level and charging from a fake mce)
btsyncd_test(battery_test BUS SOURCES ${SRC}/batteryservice.cpp ${SRC}/batterystatus.cpp ${GATT} LIBS Qt6::DBus)

# The weather service (location, full forecast) with fixtures/
btsyncd_test(weather_test DCONF SOURCES ${SRC}/weatherservice.cpp ${GATT} ${SRC}/wakehold.cpp
	LIBS Qt6::DBus ${MLITE} ${KEEPALIVE})

# The calendar service with a temporary mkcal database (MKCAL_STORAGEDB)
if(MKCAL_FOUND AND KCALCORE_FOUND)
	btsyncd_test(calendar_test DCONF SOURCES ${SRC}/calendarservice.cpp ${GATT} ${SRC}/wakehold.cpp
		LIBS Qt6::DBus PkgConfig::MKCAL PkgConfig::KCALCORE ${KEEPALIVE} ${MLITE})
else()
	message(STATUS "btsyncd tests: no mkcal / KF6CalendarCore, calendar_test left out")
endif()

# The media service's artwork, custom actions and queue (frames/)
if(AmberMpris_FOUND)
	btsyncd_test(media_test BUS SOURCES ${SRC}/mediaservice.cpp ${SRC}/mediaphonedata.cpp ${GATT} ${SRC}/wakehold.cpp
		LIBS Qt6::DBus PkgConfig::AmberMpris ${MLITE} ${KEEPALIVE})
else()
	message(STATUS "btsyncd tests: no AmberMpris, media_test left out")
endif()

# The advertising schedule against a fake BlueZ (BTSYNCD_ADVERT_TIME_DIV: 100 times shorter)
btsyncd_test(advert_test BUS SOURCES ${SRC}/advertisement.cpp ${SRC}/bluezmanager.cpp ${SRC}/ancs.cpp
	${SRC}/ancs_notification.cpp ${SRC}/cts.cpp ${SRC}/settime.cpp LIBS Qt6::DBus ${MLITE} ${KEEPALIVE}
	ENV BTSYNCD_ADVERT_TIME_DIV=100)

# The screenshot hand-off to the phone
btsyncd_test(screenshot_test BUS SOURCES ${SRC}/screenshotservice.cpp ${GATT} LIBS Qt6::DBus)

# W2-btsyncd-data (fixtures/): the time characteristic's 24-hour flag and the phone's language
btsyncd_test(phonedata_test DCONF SOURCES ${SRC}/timeservice.cpp ${SRC}/settime.cpp ${SRC}/phonestatusservice.cpp
	${NOTIF} LIBS Qt6::DBus ${MLITE} ${KEEPALIVE})
# The Messages API over the assistant relay (a fake phone)
btsyncd_test(messages_test BUS SOURCES ${SRC}/messageservice.cpp ${SRC}/assistantservice.cpp ${SRC}/assistantprotocol.cpp
	${SRC}/contactnames.cpp ${NOTIF} LIBS Qt6::DBus ${MLITE} ${KEEPALIVE})
# "Share heart rate": the Heart Rate Service with a fake BlueZ and a fake sensor
btsyncd_test(heartrate_test DCONF SOURCES ${SRC}/heartrateservice.cpp ${GATT} LIBS Qt6::DBus ${MLITE} ${KEEPALIVE})

# FP-B1: the phone's time against Settings > Date & time (a fake timedate1); vibrate on connectivity change
btsyncd_test(timeservice_test DCONF SOURCES ${SRC}/timeservice.cpp ${SRC}/settime.cpp ${GATT} LIBS Qt6::DBus ${MLITE})
btsyncd_test(connectivity_test DCONF SOURCES ${SRC}/connectivityfeedback.cpp ${SRC}/wakehold.cpp
	LIBS Qt6::DBus ${MLITE} ${KEEPALIVE})

# FP-B2: the call log object (CR-08)
btsyncd_test(calllog_test BUS SOURCES ${SRC}/calllog.cpp LIBS Qt6::DBus)

if(NOT BTSYNCD_QTESTS)
	return()
endif()
if(NOT TARGET Qt6::Test)
	message(STATUS "btsyncd tests: Qt6 Test not found, no Qt Test suites")
	return()
endif()

# Qt Test suites against fake lipstick / BlueZ peers (common/fakepeers.h); each makes its own
# private bus the system bus too (testmain.h)
set(FAKES common/fakepeers.cpp common/fakepeers.h common/testmain.h)
btsyncd_test(tst_wakehold QTEST SOURCES ${SRC}/wakehold.cpp stubs/backgroundactivity.h)
btsyncd_test(tst_healthservice QTEST BUS SOURCES ${SRC}/healthservice.cpp ${SRC}/healthrecords.cpp ${SRC}/healthchanges.cpp
	${GATT} ${FAKES} LIBS Qt6::DBus PkgConfig::SQLITE3)
# Version 3 of the health sync (workouts, tracks, sleep, resting heart rate from health.db) with Gadgetbridge's frames
btsyncd_test(tst_healthchanges QTEST BUS SOURCES ${SRC}/healthservice.cpp ${SRC}/healthrecords.cpp ${SRC}/healthchanges.cpp
	${GATT} ${FAKES} LIBS Qt6::DBus PkgConfig::SQLITE3)
btsyncd_test(tst_notificationservice QTEST BUS SOURCES ${NOTIF} ${FAKES} LIBS Qt6::DBus)
btsyncd_test(tst_voiceservice QTEST BUS SOURCES ${SRC}/voiceservice.cpp ${SRC}/voicesource.cpp ${SRC}/voiceprotocol.cpp
	${SRC}/voicecond.cpp ${NOTIF} ${FAKES} LIBS Qt6::DBus)
# The assistant relay's acts with Gadgetbridge's frames (fixtures/): forwarding, results, CR-50
# timeouts in real time (about 21 s), size limits, reconnections
btsyncd_test(tst_relay QTEST BUS SOURCES ${SRC}/assistantservice.cpp ${SRC}/assistantprotocol.cpp ${SRC}/contactnames.cpp
	${NOTIF} ${FAKES} LIBS Qt6::DBus)
# The phone's call cards: buttons, CallControl and CallObserver, the missed-call frame, reconnections
btsyncd_test(tst_calls QTEST BUS SOURCES ${NOTIF} ${FAKES} LIBS Qt6::DBus)
# The phone's <insert> element by element (versions 4, 5, CR-52) and Gadgetbridge's inserts; no bus
btsyncd_test(tst_notifparse QTEST SOURCES ${NOTIF} ${FAKES} LIBS Qt6::DBus)
btsyncd_test(tst_bluezmanager QTEST BUS SOURCES ${SRC}/bluezmanager.cpp ${SRC}/advertisement.cpp ${SRC}/ancs.cpp
	${SRC}/ancs_notification.cpp ${SRC}/cts.cpp ${SRC}/settime.cpp ${FAKES} LIBS Qt6::DBus)
