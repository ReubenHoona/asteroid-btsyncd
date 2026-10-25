# Tests of the health records and the voice protocol without D-Bus (cmake -DBTSYNCD_TESTS=ON).
# Cross-built with the SDK, they run under qemu-arm; see the health sync
# build script.
add_executable(healthrecords_test healthrecords_test.cpp ../src/healthrecords.cpp)
target_include_directories(healthrecords_test PRIVATE ../src)
target_link_libraries(healthrecords_test PRIVATE Qt6::Core)

add_executable(voice_test voice_test.cpp ../src/voiceprotocol.cpp)
target_include_directories(voice_test PRIVATE ../src)
target_link_libraries(voice_test PRIVATE Qt6::Core)
