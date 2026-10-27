# Tests of asteroid-btsyncd.
#
# Two ways to build them:
#   - with the daemon (cmake -DBTSYNCD_TESTS=ON at the top): the SDK build of
#     the recipe, whose asteroid-btsyncd-tests package installs voice_test and
#     healthrecords_test (they run under qemu-arm or on the watch);
#   - on their own (cmake -S tests): a native build against the distribution's
#     Qt 6 that needs none of AsteroidOS's libraries (mlite, libkeepalive,
#     AmberMpris are replaced by stubs in stubs/), e.g. for coverage
#     (.sonar/coverage.sh, BTSYNCD_COVERAGE=ON).
#
# The Qt Test suites (tst_*) talk to fake lipstick / BlueZ peers on a private
# D-Bus: run them under dbus-run-session (ctest does, when it is installed);
# without a session bus the D-Bus cases are skipped.
cmake_minimum_required(VERSION 3.16)

if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
	project(asteroid-btsyncd-tests LANGUAGES CXX)
	set(CMAKE_CXX_STANDARD 17)
	set(CMAKE_CXX_STANDARD_REQUIRED ON)
	set(CMAKE_AUTOMOC ON)
	find_package(Qt6 REQUIRED COMPONENTS Core DBus Test)
	option(BTSYNCD_COVERAGE "Build with gcc --coverage" OFF)
	if(BTSYNCD_COVERAGE)
		add_compile_options(--coverage -O0 -g)
		add_link_options(--coverage)
	endif()
else()
	find_package(Qt6 QUIET COMPONENTS Test)
endif()

enable_testing()
set(SRC ${CMAKE_CURRENT_SOURCE_DIR}/../src)

# Plain checks (exit status = failures), as before
add_executable(healthrecords_test healthrecords_test.cpp ${SRC}/healthrecords.cpp)
target_include_directories(healthrecords_test PRIVATE ${SRC})
target_link_libraries(healthrecords_test PRIVATE Qt6::Core)
add_test(NAME healthrecords_test COMMAND healthrecords_test)

add_executable(voice_test voice_test.cpp ${SRC}/voiceprotocol.cpp)
target_include_directories(voice_test PRIVATE ${SRC})
target_link_libraries(voice_test PRIVATE Qt6::Core)
add_test(NAME voice_test COMMAND voice_test)

if(NOT TARGET Qt6::Test)
	message(STATUS "Qt6 Test not found: only healthrecords_test and voice_test")
	return()
endif()

find_program(DBUS_RUN_SESSION dbus-run-session)

# The GATT plumbing every service test needs
set(GATT_SRC ${SRC}/service.cpp ${SRC}/characteristic.cpp ${SRC}/descriptor.cpp)
set(FAKES common/fakepeers.cpp common/fakepeers.h common/testmain.h)

function(btsyncd_qtest name)
	add_executable(${name} ${name}.cpp ${ARGN})
	# stubs/ first: MDConfItem and backgroundactivity.h instead of mlite / libkeepalive
	target_include_directories(${name} BEFORE PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/stubs)
	target_include_directories(${name} PRIVATE ${SRC} ${CMAKE_CURRENT_SOURCE_DIR}/common)
	target_link_libraries(${name} PRIVATE Qt6::Core Qt6::DBus Qt6::Test)
	set_target_properties(${name} PROPERTIES AUTOMOC ON)
	if(DBUS_RUN_SESSION)
		add_test(NAME ${name} COMMAND ${DBUS_RUN_SESSION} -- $<TARGET_FILE:${name}>)
	else()
		add_test(NAME ${name} COMMAND ${name})
	endif()
	set_tests_properties(${name} PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen" TIMEOUT 120)
endfunction()

btsyncd_qtest(tst_wakehold ${SRC}/wakehold.cpp ${SRC}/wakehold.h stubs/backgroundactivity.h)
btsyncd_qtest(tst_healthservice ${SRC}/healthservice.cpp ${SRC}/healthservice.h ${SRC}/healthrecords.cpp
	${GATT_SRC} ${FAKES})
btsyncd_qtest(tst_notificationservice ${SRC}/notificationservice.cpp ${SRC}/notificationservice.h
	${SRC}/wakehold.cpp ${SRC}/wakehold.h ${GATT_SRC} ${FAKES})
btsyncd_qtest(tst_voiceservice ${SRC}/voiceservice.cpp ${SRC}/voiceservice.h ${SRC}/voicesource.cpp
	${SRC}/voicesource.h ${SRC}/voiceprotocol.cpp ${SRC}/notificationservice.cpp ${SRC}/notificationservice.h
	${SRC}/wakehold.cpp ${SRC}/wakehold.h ${GATT_SRC} ${FAKES})
btsyncd_qtest(tst_bluezmanager ${SRC}/bluezmanager.cpp ${SRC}/bluezmanager.h ${SRC}/ancs.cpp ${SRC}/ancs.h
	${SRC}/ancs_notification.cpp ${SRC}/cts.cpp ${SRC}/cts.h ${SRC}/settime.cpp ${FAKES})
