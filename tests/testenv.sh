#!/bin/sh
# testenv.sh [--bus] [--dconf] -- COMMAND [ARGS...]
#
# Runs one btsyncd test (ctest calls this, see CMakeLists.txt) in a scratch directory of its own:
# HOME, XDG_* and the runtime directory in it, never the user's. With --bus a private dbus-daemon is
# both the session and the system bus (btsyncd's GATT objects live on the system bus, its APIs on the
# session bus; the tests play BlueZ, lipstick and the apps on it). With --dconf a dconf-service runs
# on that bus too (only for the real mlite: what a test writes through MDConfItem must reach a
# dconf database): $DCONF_SERVICE, else dconf-service from PATH or a libexec directory.
# BTSYNCD_TEST_TIMEOUT (seconds, default 280) limits the command; KEEP_TESTENV=1 keeps the directory.
set -u
bus=0 dconf=0
while [ $# -gt 0 ]; do
	case "$1" in
	--bus) bus=1 ;;
	--dconf) dconf=1 bus=1 ;;
	--) shift; break ;;
	*) break ;;
	esac
	shift
done
[ $# -gt 0 ] || { echo "usage: testenv.sh [--bus] [--dconf] -- COMMAND..." >&2; exit 2; }

T=$(mktemp -d "${TMPDIR:-/tmp}/btsyncd-test.XXXXXX") || exit 2
pids=""
cleanup() {
	for p in $pids; do kill "$p" 2>/dev/null; done
	wait 2>/dev/null
	[ "${KEEP_TESTENV:-0}" = 1 ] && echo "testenv: kept $T" >&2 || rm -rf "$T"
}
trap cleanup EXIT
trap 'exit 130' INT TERM

mkdir -p "$T/home/.config" "$T/home/.cache" "$T/home/.local/share" "$T/run"
chmod 700 "$T/run"
export HOME="$T/home" XDG_CONFIG_HOME="$T/home/.config" XDG_CACHE_HOME="$T/home/.cache" \
	XDG_DATA_HOME="$T/home/.local/share" XDG_RUNTIME_DIR="$T/run" TZ="${TZ:-UTC}"
unset DBUS_SESSION_BUS_ADDRESS DBUS_SYSTEM_BUS_ADDRESS

if [ "$bus" = 1 ]; then
	cat > "$T/bus.conf" <<CONF
<!DOCTYPE busconfig PUBLIC "-//freedesktop//DTD D-Bus Bus Configuration 1.0//EN"
 "http://www.freedesktop.org/standards/dbus/1.0/busconfig.dtd">
<busconfig>
  <type>session</type>
  <listen>unix:path=$T/bus</listen>
  <auth>EXTERNAL</auth>
  <policy context="default">
    <allow send_destination="*" eavesdrop="true"/>
    <allow eavesdrop="true"/>
    <allow own="*"/>
  </policy>
</busconfig>
CONF
	dbus-daemon --config-file="$T/bus.conf" --nofork --nopidfile 2> "$T/bus.err" &
	pids="$pids $!"
	i=0
	while [ ! -S "$T/bus" ] && [ $i -lt 100 ]; do sleep 0.05; i=$((i + 1)); done
	[ -S "$T/bus" ] || { echo "testenv: dbus-daemon did not start:" >&2; cat "$T/bus.err" >&2; exit 2; }
	export DBUS_SESSION_BUS_ADDRESS="unix:path=$T/bus" DBUS_SYSTEM_BUS_ADDRESS="unix:path=$T/bus"
fi

if [ "$dconf" = 1 ]; then
	ds=${DCONF_SERVICE:-}
	if [ -z "$ds" ]; then
		ds=$(command -v dconf-service 2>/dev/null) || ds=""
		for d in /usr/libexec /usr/lib/dconf /usr/lib/x86_64-linux-gnu; do
			[ -n "$ds" ] && break
			[ -x "$d/dconf-service" ] && ds="$d/dconf-service"
		done
	fi
	[ -n "$ds" ] || { echo "testenv: --dconf needs a dconf-service (DCONF_SERVICE)" >&2; exit 2; }
	printf 'user-db:user\n' > "$T/dconf.profile"
	export DCONF_PROFILE="$T/dconf.profile"
	env ${DCONF_LD_LIBRARY_PATH:+LD_LIBRARY_PATH=$DCONF_LD_LIBRARY_PATH} "$ds" 2> "$T/dconf.err" &
	pids="$pids $!"
	sleep 0.3
fi

timeout -k 10 "${BTSYNCD_TEST_TIMEOUT:-280}" "$@"
rc=$?
[ $rc = 124 ] && echo "testenv: timed out after ${BTSYNCD_TEST_TIMEOUT:-280} s" >&2
exit $rc
