set(
	SRC
	main.cpp
	notificationservice.cpp
	weatherservice.cpp
	mediaservice.cpp
	batteryservice.cpp
	batterystatus.cpp
	screenshotservice.cpp
	settime.cpp
	timeservice.cpp
	service.cpp
	characteristic.cpp
	descriptor.cpp
	application.cpp
	advertisement.cpp
	bluezmanager.cpp
	ancs.cpp
	ancs_notification.cpp
	cts.cpp
	healthrecords.cpp
	healthservice.cpp
	voiceprotocol.cpp
	voiceservice.cpp
	voicesource.cpp
	wakehold.cpp
	phonestatusservice.cpp
	assistantprotocol.cpp
	assistantservice.cpp
	contactnames.cpp
	voicecond.cpp
)
set(
	HEADERS
	notificationservice.h
	weatherservice.h
	mediaservice.h
	batteryservice.h
	batterystatus.h
	screenshotservice.h
	settime.h
	timeservice.h
	service.h
	characteristic.h
	descriptor.h
	common.h
	application.h
	advertisement.h
	bluezmanager.h
	ancs_protocol_constants.h
	ancs.h
	ancs_notification.h
	cts.h
	healthrecords.h
	healthservice.h
	voiceprotocol.h
	voiceservice.h
	voicesource.h
	wakehold.h
	messagejoiner.h
	phonestatusservice.h
	assistantprotocol.h
	assistantservice.h
)

add_executable(asteroid-btsyncd ${SRC} ${HEADERS})

target_link_libraries(
	asteroid-btsyncd
	PRIVATE
		Qt6::Core
		Qt6::DBus
		PkgConfig::AmberMpris
		PkgConfig::MLITE
		PkgConfig::KEEPALIVE
		PkgConfig::SQLITE3
)

# The phone's calendar goes to mkcal (without it there is no calendar service)
if(MKCAL_FOUND AND KCALCORE_FOUND)
	target_sources(asteroid-btsyncd PRIVATE calendarservice.cpp calendarservice.h)
	target_compile_definitions(asteroid-btsyncd PRIVATE BTSYNCD_HAVE_CALENDAR)
	target_link_libraries(asteroid-btsyncd PRIVATE PkgConfig::MKCAL PkgConfig::KCALCORE)
endif()

# The microphone for voice replies (without it only WAV test sources work)
if(PulseSimple_FOUND)
	target_compile_definitions(asteroid-btsyncd PRIVATE BTSYNCD_HAVE_PULSE)
	target_link_libraries(asteroid-btsyncd PRIVATE PkgConfig::PulseSimple)
endif()

install(
	TARGETS asteroid-btsyncd
	DESTINATION ${CMAKE_INSTALL_BINDIR}
)
