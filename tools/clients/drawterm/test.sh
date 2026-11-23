#!/bin/sh
# Local smoke tests for the host drawterm clients.  These do not need a CPU
# server: they check the host build, option handling, the documented exit
# codes for a refused connection, and the growable print buffer.
#
#   test.sh
#
# Builds 9pterm and, when its libraries are present, drawterm with build.sh,
# then tests both.
set -eu

root=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
build=${BUILD_DIR:-"$root/build"}
nine=$build/9pterm/9pterm
fails=0

ok() { echo "ok: $*"; }
bad() { echo "FAIL: $*" >&2; fails=$((fails+1)); }

mkdir -p "$build"
if [ "${NOBUILD:-0}" != 1 ]; then
	if ! "$root/build.sh" 9pterm >"$build/build.log" 2>&1; then
		tail -30 "$build/build.log" >&2
		exit 1
	fi
fi
[ -x "$nine" ] || { echo "no $nine (run build.sh 9pterm)" >&2; exit 1; }

file "$nine" | grep -q 'ELF 64-bit' && ok "9pterm is a host ELF" || bad "9pterm is not a host ELF"

# usage from stderr, status 2, no output on stdout
set +e
"$nine" -Z >"$build/t.out" 2>"$build/t.err"
rc=$?
set -e
[ "$rc" = 2 ] && ok "usage exits 2" || bad "usage exits $rc, wanted 2"
[ -s "$build/t.err" ] && ok "usage is on stderr" || bad "usage is missing"
[ ! -s "$build/t.out" ] && ok "usage writes nothing to stdout" || bad "usage wrote to stdout"

# a refused connection is a local error: status 2, stdout untouched
set +e
"$nine" -h 127.0.0.1 -a 'tcp!127.0.0.1!1' -u glenda -W 2 -c 'echo hi' \
	>"$build/t.out" 2>"$build/t.err"
rc=$?
set -e
[ "$rc" = 2 ] && ok "refused connection exits 2" || bad "refused connection exits $rc, wanted 2"
[ ! -s "$build/t.out" ] && ok "a failed command writes nothing to stdout" || bad "failed command wrote to stdout"
grep -q "can't dial" "$build/t.err" && ok "connection error is reported" || bad "connection error is missing"

set +e
"$nine" -P "$build/no-such-password" -h 127.0.0.1 -a 'tcp!127.0.0.1!1' -c true \
	>"$build/t.out" 2>"$build/t.err"
rc=$?
set -e
[ "$rc" = 2 ] && ok "missing password file exits 2" || bad "missing password file exits $rc, wanted 2"
grep -q "password file" "$build/t.err" && ok "missing password file is reported" || bad "password error is missing"

# the realloc cursor fix in libc/vsmprint.c and libc/runevsmprint.c
cc -I"$build/9pterm" -I"$build/9pterm/include" -I"$build/9pterm/kern" \
	-D_THREAD_SAFE -DPTHREAD -pthread -D_REENTRANT -O2 \
	"$root/tests/vsmprint.c" "$build/9pterm/libc/libc.a" \
	"$build/9pterm/libmachdep.a" -pthread -lm -o "$build/vsmprint-test"
"$build/vsmprint-test" && ok "vsmprint/runevsmprint long buffers" || bad "vsmprint long buffers"

# the graphical build, when the libraries are present, must run headless
# with -G and reach the same refused-connection local error
has() { pkg-config --exists "$1" 2>/dev/null; }
if [ ! -x "$build/drawterm/drawterm" ] && [ "${NOBUILD:-0}" != 1 ] && \
   { { has x11 && has xt; } || \
     { has wayland-client && has xkbcommon && has libdecor-0 && has wayland-protocols; }; }; then
	if ! "$root/build.sh" drawterm >"$build/build.log" 2>&1; then
		tail -30 "$build/build.log" >&2
		bad "drawterm build"
	fi
fi
if [ -x "$build/drawterm/drawterm" ]; then
	draw=$build/drawterm/drawterm
	file "$draw" | grep -q 'ELF 64-bit' && ok "drawterm is a host ELF" || bad "drawterm is not a host ELF"
	set +e
	"$draw" -G -h 127.0.0.1 -a 'tcp!127.0.0.1!1' -u glenda -c true \
		>"$build/t.out" 2>"$build/t.err"
	rc=$?
	set -e
	[ "$rc" -ne 0 ] && ok "drawterm refused connection fails" || bad "drawterm refused connection exits 0"
	cat "$build/t.out" "$build/t.err" | grep -q "can't dial" && ok "drawterm reports the connection error" || bad "drawterm error is missing"
fi

rm -f "$build/t.out" "$build/t.err" "$build/vsmprint-test" "$build/build.log"

if [ "$fails" -ne 0 ]; then
	echo "$fails test(s) failed" >&2
	exit 1
fi
echo "all drawterm host tests passed"
