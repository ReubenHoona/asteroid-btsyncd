# Tests of the health records and the voice protocol without D-Bus (cmake -DBTSYNCD_TESTS=ON).
# Cross-built with the SDK, they run under qemu-arm; see the health sync
# build script.
add_executable(healthrecords_test healthrecords_test.cpp ../src/healthrecords.cpp)
target_include_directories(healthrecords_test PRIVATE ../src)
target_link_libraries(healthrecords_test PRIVATE Qt6::Core)

add_executable(voice_test voice_test.cpp ../src/voiceprotocol.cpp)
target_include_directories(voice_test PRIVATE ../src)
target_link_libraries(voice_test PRIVATE Qt6::Core)

# The notification service on a private bus (see the top of the source):
# dbus-run-session -- sh -c 'DBUS_SYSTEM_BUS_ADDRESS=$DBUS_SESSION_BUS_ADDRESS notifications_test'
add_executable(notifications_test notifications_test.cpp ../src/notificationservice.cpp
	../src/characteristic.cpp ../src/service.cpp ../src/descriptor.cpp ../src/wakehold.cpp)
target_include_directories(notifications_test PRIVATE ../src)
target_link_libraries(notifications_test PRIVATE Qt6::Core Qt6::DBus PkgConfig::MLITE PkgConfig::KEEPALIVE)

# The calendar service with a temporary mkcal database (MKCAL_STORAGEDB), on a
# private bus like notifications_test
if(MKCAL_FOUND AND KCALCORE_FOUND)
	add_executable(calendar_test calendar_test.cpp ../src/calendarservice.cpp
		../src/characteristic.cpp ../src/service.cpp ../src/descriptor.cpp ../src/wakehold.cpp)
	target_include_directories(calendar_test PRIVATE ../src)
	target_link_libraries(calendar_test PRIVATE Qt6::Core Qt6::DBus PkgConfig::MKCAL PkgConfig::KCALCORE PkgConfig::KEEPALIVE)
endif()
