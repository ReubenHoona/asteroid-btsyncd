#!/bin/sh
# Coverage for SonarQube (~/stacks/sonarqube/scan.sh runs this in the image .sonar/Dockerfile builds, cwd = the
# repository root, as the scanning user, HOME=/tmp, JOBS=1). A native x86_64 build of tests/ only (cmake -S tests:
# the units under test plus the stubs for mlite / libkeepalive; the daemon itself needs AsteroidOS's libraries),
# with gcc --coverage; every test runs under its own dbus-run-session (ctest), headless.
# Reports: .sonar-reports/cobertura.xml (gcovr, paths relative to the repository root) and
# .sonar-reports/ctest-junit.xml. Exit status: the tests' (the coverage report is written either way).
set -eu
cd "$(dirname "$0")/.."
root=$(pwd)
build=.sonar-build
mkdir -p .sonar-reports
rm -rf "$build" .sonar-reports/cobertura.xml .sonar-reports/ctest-junit.xml

export QT_QPA_PLATFORM=offscreen
export TZ=UTC LANG=C.UTF-8

cmake -S tests -B "$build" -G Ninja -DCMAKE_BUILD_TYPE=Debug -DBTSYNCD_COVERAGE=ON
cmake --build "$build" -j "${JOBS:-1}"

# dbus-daemon (dbus-run-session) refuses to start for a user without a passwd entry: the scanning user has none in
# the image, so give it one through nss_wrapper for the tests
if ! getent passwd "$(id -u)" > /dev/null 2>&1; then
    printf 'scan:x:%s:%s:scan:%s:/bin/sh\n' "$(id -u)" "$(id -g)" "${HOME:-/tmp}" > "$build/passwd"
    printf 'scan:x:%s:\n' "$(id -g)" > "$build/group"
    export LD_PRELOAD=libnss_wrapper.so NSS_WRAPPER_PASSWD="$root/$build/passwd" NSS_WRAPPER_GROUP="$root/$build/group"
fi

rc=0
(cd "$build" && ctest --output-on-failure -j 1 --output-junit "$root/.sonar-reports/ctest-junit.xml") || rc=$?

# Only the daemon's sources (src/), not the tests, stubs or fakes
unset LD_PRELOAD
gcovr --root "$root" --object-directory "$build" --filter "$root/src/" \
    --exclude-unreachable-branches --exclude-throw-branches \
    --cobertura .sonar-reports/cobertura.xml --print-summary

exit "$rc"
