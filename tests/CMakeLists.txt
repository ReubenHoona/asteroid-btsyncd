# Tests of the health records and the voice protocol without D-Bus (cmake -DBTSYNCD_TESTS=ON).
# Cross-built with the SDK, they run under qemu-arm; see the health sync
# build script.
add_executable(healthrecords_test healthrecords_test.cpp ../src/healthrecords.cpp)
target_include_directories(healthrecords_test PRIVATE ../src)
target_link_libraries(healthrecords_test PRIVATE Qt6::Core)

# The assistant relay's XML
add_executable(assistant_test assistant_test.cpp ../src/assistantprotocol.cpp)
target_include_directories(assistant_test PRIVATE ../src)
target_link_libraries(assistant_test PRIVATE Qt6::Core)

# The assistant relay follows the phone's link, on a private bus like
# notifications_test
add_executable(assistantlink_test assistantlink_test.cpp ../src/assistantservice.cpp ../src/assistantprotocol.cpp
	../src/notificationservice.cpp ../src/characteristic.cpp ../src/service.cpp ../src/descriptor.cpp ../src/wakehold.cpp)
target_include_directories(assistantlink_test PRIVATE ../src)
target_link_libraries(assistantlink_test PRIVATE Qt6::Core Qt6::DBus PkgConfig::MLITE PkgConfig::KEEPALIVE)

add_executable(voice_test voice_test.cpp ../src/voiceprotocol.cpp)
target_include_directories(voice_test PRIVATE ../src)
target_link_libraries(voice_test PRIVATE Qt6::Core)

# Voice replies recognised on the watch, with a fake asteroid-assistant, on a
# private bus like notifications_test
add_executable(voicelocal_test voicelocal_test.cpp ../src/voiceservice.cpp ../src/voicesource.cpp ../src/voiceprotocol.cpp
	../src/notificationservice.cpp ../src/characteristic.cpp ../src/service.cpp ../src/descriptor.cpp ../src/wakehold.cpp)
target_include_directories(voicelocal_test PRIVATE ../src)
target_link_libraries(voicelocal_test PRIVATE Qt6::Core Qt6::DBus PkgConfig::MLITE PkgConfig::KEEPALIVE)

# The notification service on a private bus (see the top of the source):
# dbus-run-session -- sh -c 'DBUS_SYSTEM_BUS_ADDRESS=$DBUS_SESSION_BUS_ADDRESS notifications_test'
add_executable(notifications_test notifications_test.cpp ../src/notificationservice.cpp
	../src/characteristic.cpp ../src/service.cpp ../src/descriptor.cpp ../src/wakehold.cpp)
target_include_directories(notifications_test PRIVATE ../src)
target_link_libraries(notifications_test PRIVATE Qt6::Core Qt6::DBus PkgConfig::MLITE PkgConfig::KEEPALIVE)

# The phone status service (battery, Do Not Disturb, disconnected notice)
# with the notification service, on a private bus like notifications_test
add_executable(phonestatus_test phonestatus_test.cpp ../src/phonestatusservice.cpp ../src/notificationservice.cpp
	../src/characteristic.cpp ../src/service.cpp ../src/descriptor.cpp ../src/wakehold.cpp)
target_include_directories(phonestatus_test PRIVATE ../src)
target_link_libraries(phonestatus_test PRIVATE Qt6::Core Qt6::DBus PkgConfig::MLITE PkgConfig::KEEPALIVE)

# The weather service (the forecast's location from the phone) on a private bus
# with a dconf-service, like notifications_test
add_executable(weather_test weather_test.cpp ../src/weatherservice.cpp
	../src/characteristic.cpp ../src/service.cpp ../src/descriptor.cpp ../src/wakehold.cpp)
target_include_directories(weather_test PRIVATE ../src)
target_link_libraries(weather_test PRIVATE Qt6::Core Qt6::DBus PkgConfig::MLITE PkgConfig::KEEPALIVE)

# The calendar service with a temporary mkcal database (MKCAL_STORAGEDB), on a
# private bus like notifications_test
if(MKCAL_FOUND AND KCALCORE_FOUND)
	add_executable(calendar_test calendar_test.cpp ../src/calendarservice.cpp
		../src/characteristic.cpp ../src/service.cpp ../src/descriptor.cpp ../src/wakehold.cpp)
	target_include_directories(calendar_test PRIVATE ../src)
	target_link_libraries(calendar_test PRIVATE Qt6::Core Qt6::DBus PkgConfig::MKCAL PkgConfig::KCALCORE PkgConfig::KEEPALIVE)
endif()
