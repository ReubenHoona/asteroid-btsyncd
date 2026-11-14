# Tests of the health records and the voice protocol without D-Bus (cmake -DBTSYNCD_TESTS=ON).
# Cross-built with the SDK, they run under qemu-arm; see the health sync
# build script.
add_executable(healthrecords_test healthrecords_test.cpp ../src/healthrecords.cpp)
target_include_directories(healthrecords_test PRIVATE ../src)
target_link_libraries(healthrecords_test PRIVATE Qt6::Core PkgConfig::SQLITE3)

# The assistant relay's XML
add_executable(assistant_test assistant_test.cpp ../src/assistantprotocol.cpp ../src/contactnames.cpp)
target_include_directories(assistant_test PRIVATE ../src)
target_link_libraries(assistant_test PRIVATE Qt6::Core)

# The assistant relay follows the phone's link, on a private bus like
# notifications_test
add_executable(assistantlink_test assistantlink_test.cpp ../src/assistantservice.cpp ../src/assistantprotocol.cpp ../src/contactnames.cpp
	../src/notificationservice.cpp ../src/batterystatus.cpp ../src/characteristic.cpp ../src/service.cpp ../src/descriptor.cpp ../src/wakehold.cpp)
target_include_directories(assistantlink_test PRIVATE ../src)
target_link_libraries(assistantlink_test PRIVATE Qt6::Core Qt6::DBus PkgConfig::MLITE PkgConfig::KEEPALIVE)

add_executable(voice_test voice_test.cpp ../src/voiceprotocol.cpp ../src/voicecond.cpp)
target_include_directories(voice_test PRIVATE ../src)
target_link_libraries(voice_test PRIVATE Qt6::Core)

# Voice replies recognised on the watch, with a fake asteroid-assistant, on a
# private bus like notifications_test
add_executable(voicelocal_test voicelocal_test.cpp ../src/voiceservice.cpp ../src/voicesource.cpp ../src/voiceprotocol.cpp ../src/voicecond.cpp
	../src/notificationservice.cpp ../src/batterystatus.cpp ../src/characteristic.cpp ../src/service.cpp ../src/descriptor.cpp ../src/wakehold.cpp)
target_include_directories(voicelocal_test PRIVATE ../src)
target_link_libraries(voicelocal_test PRIVATE Qt6::Core Qt6::DBus PkgConfig::MLITE PkgConfig::KEEPALIVE)

# A voice reply recognised by the phone ends when the phone's link drops
add_executable(voicelink_test voicelink_test.cpp ../src/voiceservice.cpp ../src/voicesource.cpp ../src/voiceprotocol.cpp ../src/voicecond.cpp
	../src/notificationservice.cpp ../src/batterystatus.cpp ../src/characteristic.cpp ../src/service.cpp ../src/descriptor.cpp ../src/wakehold.cpp)
target_include_directories(voicelink_test PRIVATE ../src)
target_link_libraries(voicelink_test PRIVATE Qt6::Core Qt6::DBus PkgConfig::MLITE PkgConfig::KEEPALIVE)

# The calls on the watch with a fake oFono and BlueZ, on a private bus with a dconf
# service like weather_test (see the top of the source)
add_executable(calls_test calls_test.cpp ../src/callaudio.cpp)
target_include_directories(calls_test PRIVATE ../src)
target_link_libraries(calls_test PRIVATE Qt6::Core Qt6::DBus PkgConfig::MLITE)

# The notification service on a private bus (see the top of the source):
# dbus-run-session -- sh -c 'DBUS_SYSTEM_BUS_ADDRESS=$DBUS_SESSION_BUS_ADDRESS notifications_test'
add_executable(notifications_test notifications_test.cpp ../src/notificationservice.cpp ../src/batterystatus.cpp
	../src/characteristic.cpp ../src/service.cpp ../src/descriptor.cpp ../src/wakehold.cpp)
target_include_directories(notifications_test PRIVATE ../src)
target_link_libraries(notifications_test PRIVATE Qt6::Core Qt6::DBus PkgConfig::MLITE PkgConfig::KEEPALIVE)

# Notification pictures (reassembly, checks, reconnections, cleanup) on a private bus
# like notifications_test
add_executable(pictures_test pictures_test.cpp ../src/notificationservice.cpp ../src/batterystatus.cpp
	../src/characteristic.cpp ../src/service.cpp ../src/descriptor.cpp ../src/wakehold.cpp)
target_include_directories(pictures_test PRIVATE ../src)
target_link_libraries(pictures_test PRIVATE Qt6::Core Qt6::DBus PkgConfig::MLITE PkgConfig::KEEPALIVE)

# The phone status service (battery, Do Not Disturb, disconnected notice)
# with the notification service, on a private bus like notifications_test
add_executable(phonestatus_test phonestatus_test.cpp ../src/phonestatusservice.cpp ../src/notificationservice.cpp ../src/batterystatus.cpp
	../src/characteristic.cpp ../src/service.cpp ../src/descriptor.cpp ../src/wakehold.cpp)
target_include_directories(phonestatus_test PRIVATE ../src)
target_link_libraries(phonestatus_test PRIVATE Qt6::Core Qt6::DBus PkgConfig::MLITE PkgConfig::KEEPALIVE)

# The modes on the phone status service: Bedtime with the phone's Do Not
# Disturb both ways, Bedtime and Theater silencing phone notifications, on a
# private bus like notifications_test (with a dconf database of its own)
add_executable(modes_test modes_test.cpp ../src/phonestatusservice.cpp ../src/notificationservice.cpp ../src/batterystatus.cpp
	../src/characteristic.cpp ../src/service.cpp ../src/descriptor.cpp ../src/wakehold.cpp)
target_include_directories(modes_test PRIVATE ../src)
target_link_libraries(modes_test PRIVATE Qt6::Core Qt6::DBus PkgConfig::MLITE PkgConfig::KEEPALIVE)

# The battery service (the level and whether it charges, from a fake mce) on a
# private bus like notifications_test
add_executable(battery_test battery_test.cpp ../src/batteryservice.cpp ../src/batterystatus.cpp
	../src/characteristic.cpp ../src/service.cpp ../src/descriptor.cpp)
target_include_directories(battery_test PRIVATE ../src)
target_link_libraries(battery_test PRIVATE Qt6::Core Qt6::DBus)

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

# M5 messaging polish: quiet on the charger (a fake mce), the user's own messages of a
# conversation (<me>, the x-asteroid-messages hint), the app's notifications off and on
# from the watch (SetAppNotifications); on a private bus with a dconf-service, like
# weather_test
add_executable(messaging_test messaging_test.cpp ../src/notificationservice.cpp ../src/batterystatus.cpp
	../src/characteristic.cpp ../src/service.cpp ../src/descriptor.cpp ../src/wakehold.cpp)
target_include_directories(messaging_test PRIVATE ../src)
target_link_libraries(messaging_test PRIVATE Qt6::Core Qt6::DBus PkgConfig::MLITE PkgConfig::KEEPALIVE)

# The pictures of a conversation's messages (<pc> in an <m>), on a private bus like pictures_test
add_executable(msgpictures_test msgpictures_test.cpp ../src/notificationservice.cpp ../src/batterystatus.cpp
	../src/characteristic.cpp ../src/service.cpp ../src/descriptor.cpp ../src/wakehold.cpp)
target_include_directories(msgpictures_test PRIVATE ../src)
target_link_libraries(msgpictures_test PRIVATE Qt6::Core Qt6::DBus PkgConfig::MLITE PkgConfig::KEEPALIVE)

# The notification protocol of the wearui-w2 wave (action ack, sub text, progress, ongoing, resident,
# mute model, calls in Do Not Disturb, avatars) with the frames Gadgetbridge records (tests/frames), on a private
# bus with a dconf service like weather_test
add_executable(notifw2_test notifw2_test.cpp ../src/notificationservice.cpp ../src/batterystatus.cpp
	../src/characteristic.cpp ../src/service.cpp ../src/descriptor.cpp ../src/wakehold.cpp)
target_include_directories(notifw2_test PRIVATE ../src)
target_compile_definitions(notifw2_test PRIVATE FRAMES_DIR="${CMAKE_CURRENT_SOURCE_DIR}/frames")
target_link_libraries(notifw2_test PRIVATE Qt6::Core Qt6::DBus PkgConfig::MLITE PkgConfig::KEEPALIVE)

# The media service's artwork, custom actions and queue (characteristics 00007008 and 00007009, media commands 5
# and 6) on a private bus
add_executable(media_test media_test.cpp ../src/mediaservice.cpp ../src/mediaphonedata.cpp
	../src/characteristic.cpp ../src/service.cpp ../src/descriptor.cpp ../src/wakehold.cpp)
target_include_directories(media_test PRIVATE ../src)
target_compile_definitions(media_test PRIVATE FRAMES_DIR="${CMAKE_CURRENT_SOURCE_DIR}/frames")
target_link_libraries(media_test PRIVATE Qt6::Core Qt6::DBus PkgConfig::AmberMpris PkgConfig::MLITE PkgConfig::KEEPALIVE)

# The advertising schedule against a fake BlueZ on a private bus (BTSYNCD_ADVERT_TIME_DIV=100)
add_executable(advert_test advert_test.cpp ../src/advertisement.cpp ../src/bluezmanager.cpp ../src/ancs.cpp
	../src/ancs_notification.cpp ../src/cts.cpp ../src/settime.cpp)
target_include_directories(advert_test PRIVATE ../src)
target_link_libraries(advert_test PRIVATE Qt6::Core Qt6::DBus PkgConfig::MLITE PkgConfig::KEEPALIVE)

# The screenshot hand-off to the phone on a private bus
add_executable(screenshot_test screenshot_test.cpp ../src/screenshotservice.cpp
	../src/characteristic.cpp ../src/service.cpp ../src/descriptor.cpp)
target_include_directories(screenshot_test PRIVATE ../src)
target_link_libraries(screenshot_test PRIVATE Qt6::Core Qt6::DBus)

# W2-btsyncd-data. The frames Gadgetbridge writes (W2-gb-data) are in tests/fixtures
# The time characteristic's 24-hour flag and the phone's language (<lc>), on a private bus
# with a dconf-service, like weather_test
add_executable(phonedata_test phonedata_test.cpp ../src/timeservice.cpp ../src/settime.cpp ../src/phonestatusservice.cpp
	../src/notificationservice.cpp ../src/batterystatus.cpp ../src/characteristic.cpp ../src/service.cpp ../src/descriptor.cpp ../src/wakehold.cpp)
target_include_directories(phonedata_test PRIVATE ../src)
target_link_libraries(phonedata_test PRIVATE Qt6::Core Qt6::DBus PkgConfig::MLITE PkgConfig::KEEPALIVE)

# The Messages API over the assistant relay (a fake phone), on a private bus like assistantlink_test
add_executable(messages_test messages_test.cpp ../src/messageservice.cpp ../src/assistantservice.cpp ../src/assistantprotocol.cpp ../src/contactnames.cpp
	../src/notificationservice.cpp ../src/batterystatus.cpp ../src/characteristic.cpp ../src/service.cpp ../src/descriptor.cpp ../src/wakehold.cpp)
target_include_directories(messages_test PRIVATE ../src)
target_link_libraries(messages_test PRIVATE Qt6::Core Qt6::DBus PkgConfig::MLITE PkgConfig::KEEPALIVE)

# "Share heart rate": the Heart Rate Service with a fake BlueZ and a fake sensor, on a private bus
# with a dconf-service, like weather_test
add_executable(heartrate_test heartrate_test.cpp ../src/heartrateservice.cpp
	../src/characteristic.cpp ../src/service.cpp ../src/descriptor.cpp)
target_include_directories(heartrate_test PRIVATE ../src)
target_link_libraries(heartrate_test PRIVATE Qt6::Core Qt6::DBus PkgConfig::MLITE PkgConfig::KEEPALIVE)

foreach(t weather_test calendar_test phonedata_test messages_test)
	if(TARGET ${t})
		target_compile_definitions(${t} PRIVATE FIXTURE_DIR="${CMAKE_CURRENT_SOURCE_DIR}/fixtures")
	endif()
endforeach()
