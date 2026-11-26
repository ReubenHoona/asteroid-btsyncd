#!/bin/sh
# tests/run.sh: build asteroid-btsyncd's tests and run them all (see tests/README.md).
#
#   tests/run.sh [--host | --docker | --cross] [--coverage] [-R REGEX] [-j JOBS] [--keep]
#
#   --host    native build with the Qt 6 of this machine (cmake -S tests, stubs for mlite and
#             libkeepalive); the default when pkg-config finds Qt6Core, Qt6DBus and Qt6Test
#   --docker  the same build in the image .sonar/Dockerfile makes (the one SonarQube's coverage
#             uses, sonar-cov-eos-btsyncd:local); the default otherwise
#   --cross   the SDK build (-DBTSYNCD_TESTS=ON -DBTSYNCD_QTESTS=ON) with an OpenEmbedded build's
#             cross toolchain and sysroots, against AsteroidOS's own libraries (mlite with a real
#             dconf, libkeepalive, AmberMpris, mkcal), run under qemu-aarch64; see tests/cross.sh
#   --coverage  gcc --coverage, then a gcovr summary of src/ (host or docker)
#   -R REGEX  only the tests whose names match (ctest -R); -j JOBS build jobs (default: all CPUs)
#   --keep    keep the build directory (.test-build, or .test-build-cross) for a second run
#
# Exit status: 0 when every test passed. Each test's output: .test-build/Testing/Temporary/LastTest.log
set -eu
here=$(cd "$(dirname "$0")" && pwd)
root=$(cd "$here/.." && pwd)
mode="" coverage=0 regex="" jobs="" keep=0
while [ $# -gt 0 ]; do
	case "$1" in
	--host) mode=host ;;
	--docker) mode=docker ;;
	--cross) mode=cross ;;
	--coverage) coverage=1 ;;
	-R) regex=$2; shift ;;
	-j) jobs=$2; shift ;;
	--keep) keep=1 ;;
	-h|--help) sed -n '2,18p' "$0"; exit 0 ;;
	*) echo "run.sh: unknown option $1" >&2; exit 2 ;;
	esac
	shift
done
jobs=${jobs:-$(nproc 2>/dev/null || echo 2)}

if [ -z "$mode" ]; then
	if pkg-config --exists Qt6Core Qt6DBus Qt6Test 2>/dev/null; then
		mode=host
	elif command -v docker > /dev/null 2>&1; then
		mode=docker
	else
		echo "run.sh: no Qt 6 here and no docker: install Qt 6 (Core, D-Bus, Test) or use --cross" >&2
		exit 2
	fi
fi

if [ "$mode" = cross ]; then
	exec sh "$here/cross.sh" ${regex:+-R "$regex"} -j "$jobs" $([ $keep = 1 ] && echo --keep)
fi

if [ "$mode" = docker ]; then
	img=sonar-cov-eos-btsyncd:local
	echo "run.sh: building $img from .sonar/Dockerfile" >&2
	docker build -q -t "$img" -f "$root/.sonar/Dockerfile" "$root/.sonar" > /dev/null
	tty=""
	[ -t 1 ] && tty=-t
	exec docker run --rm $tty --init --user "$(id -u):$(id -g)" -e HOME=/tmp -v "$root:/usr/src" -w /usr/src "$img" \
		sh tests/run.sh --host $([ $coverage = 1 ] && echo --coverage) ${regex:+-R "$regex"} -j "$jobs" \
		$([ $keep = 1 ] && echo --keep)
fi

# --host
cd "$root"
build=.test-build
[ $keep = 1 ] || rm -rf "$build"
export QT_QPA_PLATFORM=offscreen LANG=C.UTF-8
cov=OFF
[ $coverage = 1 ] && cov=ON
gen=""
command -v ninja > /dev/null 2>&1 && gen="-G Ninja"
[ -f "$build/CMakeCache.txt" ] || cmake -S tests -B "$build" $gen -DCMAKE_BUILD_TYPE=Debug -DBTSYNCD_COVERAGE=$cov > "$build.configure.log" 2>&1 \
	|| { tail -30 "$build.configure.log"; exit 1; }
rm -f "$build.configure.log"
cmake --build "$build" -j "$jobs" > "$build/build.log" 2>&1 || { grep -E "error:" -A3 "$build/build.log" | head -60; exit 1; }

# dbus-daemon refuses a user without a passwd entry (a container's --user): give it one
if ! getent passwd "$(id -u)" > /dev/null 2>&1 && [ -n "$(ls /usr/lib/*/libnss_wrapper.so 2>/dev/null)" ]; then
	printf 'test:x:%s:%s:test:%s:/bin/sh\n' "$(id -u)" "$(id -g)" "${HOME:-/tmp}" > "$build/passwd"
	printf 'test:x:%s:\n' "$(id -g)" > "$build/group"
	export LD_PRELOAD=libnss_wrapper.so NSS_WRAPPER_PASSWD="$root/$build/passwd" NSS_WRAPPER_GROUP="$root/$build/group"
fi

rc=0
(cd "$build" && ctest --output-on-failure -j "$jobs" ${regex:+-R "$regex"}) > "$build/ctest.log" 2>&1 || rc=$?
grep -E "tests passed|tests failed|\*\*\*|Failed|Not Run|Timeout" "$build/ctest.log" | grep -v "^\s*$" | tail -40
if [ $rc != 0 ]; then
	echo "run.sh: failures; the output of each failed test is in $build/ctest.log" >&2
fi
if [ $coverage = 1 ] && command -v gcovr > /dev/null 2>&1; then
	unset LD_PRELOAD
	gcovr --root "$root" --object-directory "$build" --filter "$root/src/" --exclude-unreachable-branches \
		--exclude-throw-branches --print-summary -o /dev/null 2>/dev/null | tail -3
fi
[ $keep = 1 ] || [ $rc != 0 ] || rm -rf "$build"
exit $rc
