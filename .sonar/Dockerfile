# Image for .sonar/coverage.sh: ~/stacks/sonarqube/scan.sh builds it (context: this .sonar/ directory only) and runs
# the script in it as the scanning user. A native x86_64 build of the tests (the watch builds with the AsteroidOS
# SDK, which is not available here): Debian's Qt 6 (Core, D-Bus, Test), gcc with --coverage, gcovr for the
# Cobertura report, and a private D-Bus session bus (dbus-run-session) for the tests that talk to fake lipstick and
# BlueZ peers (libnss-wrapper: dbus-daemon needs a passwd entry for the scanning user, who has none in the image).
# The base image is pinned; nothing is downloaded when the coverage runs.
FROM debian:trixie-20261005-slim

RUN apt-get update \
    && apt-get install -y --no-install-recommends \
        g++ make cmake ninja-build pkgconf \
        qt6-base-dev qt6-base-dev-tools \
        dbus-daemon dbus-session-bus-common libnss-wrapper \
        gcovr \
    && rm -rf /var/lib/apt/lists/*
