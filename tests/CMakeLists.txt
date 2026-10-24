# Tests of the health records without D-Bus (cmake -DBTSYNCD_TESTS=ON).
# Cross-built with the SDK, they run under qemu-arm; see the health sync
# build script.
add_executable(healthrecords_test healthrecords_test.cpp ../src/healthrecords.cpp)
target_include_directories(healthrecords_test PRIVATE ../src)
target_link_libraries(healthrecords_test PRIVATE Qt6::Core)
