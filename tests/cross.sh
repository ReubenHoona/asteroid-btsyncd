#!/bin/sh
# tests/cross.sh [-R REGEX] [-j JOBS] [--keep] [--clean]: the tests against AsteroidOS's own
# libraries (tests/run.sh --cross calls this; see tests/README.md).
#
# Builds tests/ (cmake -S tests, so the real mlite, libkeepalive, AmberMpris and mkcal are found
# and no stub is used; the dconf tests get a dconf-service) with an OpenEmbedded build's cross
# toolchain, and runs every test under qemu-aarch64 with the image's own libraries. Needs, from an
# OE build that is not running (nothing of it is changed):
#   OE_TMP      its tmp directory (default ~/mainline/agents/osimage/build/tmp): sysroots-components
#               (cortexa53 + the machine's for the target, x86_64 for the toolchain, Qt's host tools
#               and dconf-service) and the image rootfs (work/*/asteroid-image/*/rootfs)
#   QEMU        qemu-aarch64 (default: in PATH, else ~/mainline/agents/arm64/qemu/root/usr/bin)
# Private copies (cp -a, about 5 GB) go to CROSS_ROOT (default ../rootfs-bt beside the
# repository): rootfs/ (qemu -L), sysroot/ (headers and libraries), native/ (toolchain). They are
# made once and kept for the next run; --clean removes them (and the build directory) at the end.
set -eu
here=$(cd "$(dirname "$0")" && pwd)
root=$(cd "$here/.." && pwd)
regex="" jobs=$(nproc 2>/dev/null || echo 2) keep=0 clean=0
while [ $# -gt 0 ]; do
	case "$1" in
	-R) regex=$2; shift ;;
	-j) jobs=$2; shift ;;
	--keep) keep=1 ;;
	--clean) clean=1 ;;
	*) echo "cross.sh: unknown option $1" >&2; exit 2 ;;
	esac
	shift
done
OE_TMP=${OE_TMP:-$HOME/mainline/agents/osimage/build/tmp}
CROSS_ROOT=${CROSS_ROOT:-$(dirname "$root")/rootfs-bt}
QEMU=${QEMU:-$(command -v qemu-aarch64 2>/dev/null || echo "$HOME/mainline/agents/arm64/qemu/root/usr/bin/qemu-aarch64")}
comp=$OE_TMP/sysroots-components
[ -d "$comp/cortexa53" ] && [ -d "$comp/x86_64/gcc-cross-aarch64" ] || { echo "cross.sh: no OE sysroots in $OE_TMP" >&2; exit 2; }
[ -x "$QEMU" ] || { echo "cross.sh: no qemu-aarch64 ($QEMU)" >&2; exit 2; }

# The copies, made while no bitbake writes the OE tree
if [ ! -f "$CROSS_ROOT/.done" ]; then
	if pgrep -f bitbake > /dev/null 2>&1; then
		echo "cross.sh: bitbake is running; the copies are made only while it is not" >&2
		exit 2
	fi
	rootfs=$(ls -d "$OE_TMP"/work/*/asteroid-image/*/rootfs 2>/dev/null | head -1)
	[ -n "$rootfs" ] || { echo "cross.sh: no image rootfs in $OE_TMP/work" >&2; exit 2; }
	echo "cross.sh: copying the rootfs and sysroots to $CROSS_ROOT (once)" >&2
	rm -rf "$CROSS_ROOT"
	mkdir -p "$CROSS_ROOT/sysroot" "$CROSS_ROOT/native"
	cp -a "$rootfs" "$CROSS_ROOT/rootfs"
	for d in "$comp"/cortexa53/* "$comp"/eos_mainline64/*; do
		[ -d "$d/usr" ] || [ -d "$d/lib" ] || continue
		cp -a "$d"/. "$CROSS_ROOT/sysroot/"
	done
	for d in "$comp"/x86_64/*; do
		cp -a "$d"/. "$CROSS_ROOT/native/"
	done
	rm -rf "$CROSS_ROOT/sysroot/sysroot-providers" "$CROSS_ROOT/native/sysroot-providers"
	touch "$CROSS_ROOT/.done"
fi
S=$CROSS_ROOT/sysroot N=$CROSS_ROOT/native R=$CROSS_ROOT/rootfs
cxx=$(ls "$N"/usr/bin/aarch64-oe-linux/aarch64-oe-linux-g++)
cc=$(ls "$N"/usr/bin/aarch64-oe-linux/aarch64-oe-linux-gcc)
tools=$N/usr/bin/aarch64-oe-linux/aarch64-oe-linux-

build=$root/.test-build-cross
[ $keep = 1 ] || rm -rf "$build"
mkdir -p "$build"
cat > "$build/toolchain.cmake" <<EOF
set(CMAKE_SYSTEM_NAME Linux)
set(CMAKE_SYSTEM_PROCESSOR aarch64)
set(CMAKE_SYSROOT "$S")
set(CMAKE_C_COMPILER "$cc")
set(CMAKE_CXX_COMPILER "$cxx")
set(CMAKE_C_FLAGS_INIT "-B$tools -mcpu=cortex-a53")
set(CMAKE_CXX_FLAGS_INIT "-B$tools -mcpu=cortex-a53")
set(CMAKE_AR "${tools}ar" CACHE FILEPATH "")
set(CMAKE_RANLIB "${tools}ranlib" CACHE FILEPATH "")
set(CMAKE_FIND_ROOT_PATH "$S")
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)
set(QT_HOST_PATH "$N/usr")
set(QT_HOST_PATH_CMAKE_DIR "$N/usr/lib/cmake")
set(CMAKE_CROSSCOMPILING_EMULATOR "$QEMU;-L;$R")
EOF
export PKG_CONFIG_SYSROOT_DIR="$S" PKG_CONFIG_LIBDIR="$S/usr/lib/pkgconfig:$S/usr/share/pkgconfig" PKG_CONFIG_PATH=""
gen=""
command -v ninja > /dev/null 2>&1 && gen="-G Ninja"
if [ ! -f "$build/CMakeCache.txt" ]; then
	cmake -S "$root/tests" -B "$build" $gen -DCMAKE_TOOLCHAIN_FILE="$build/toolchain.cmake" -DCMAKE_BUILD_TYPE=Debug \
		> "$build/configure.log" 2>&1 || { tail -30 "$build/configure.log"; exit 1; }
fi
grep -E "btsyncd tests:" "$build/configure.log" 2>/dev/null || true
cmake --build "$build" -j "$jobs" > "$build/build.log" 2>&1 || { grep -E "error:" -A3 "$build/build.log" | head -60; exit 1; }

# testenv.sh: the native dconf-service for the dconf tests (the target's dconf client reads the
# same database files)
export DCONF_SERVICE="$N/usr/libexec/dconf-service" DCONF_LD_LIBRARY_PATH="$N/usr/lib"
export QT_QPA_PLATFORM=offscreen
rc=0
# half as many at once as the build: qemu and the dconf round trips are slow, and the timed
# waits of the tests are made for a watch, not for twelve emulators on one machine
tj=$(( jobs > 1 ? jobs / 2 : 1 ))
(cd "$build" && ctest --output-on-failure -j "$tj" ${regex:+-R "$regex"}) > "$build/ctest.log" 2>&1 || rc=$?
grep -E "tests passed|tests failed|\*\*\*|Failed|Not Run|Timeout" "$build/ctest.log" | tail -40
[ $rc = 0 ] || echo "cross.sh: failures; the output of each failed test is in $build/ctest.log" >&2
if [ $clean = 1 ]; then
	rm -rf "$CROSS_ROOT"
	[ $rc != 0 ] || rm -rf "$build"
elif [ $keep = 0 ] && [ $rc = 0 ]; then
	rm -rf "$build"
fi
exit $rc
