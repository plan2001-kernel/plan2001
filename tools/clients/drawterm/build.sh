#!/bin/sh
# Build the host drawterm clients with the host C compiler.
#
#   build.sh 9pterm          headless text client   build/9pterm/9pterm
#   build.sh drawterm        graphical X11/Wayland  build/drawterm/drawterm
#   build.sh all             both
#   build.sh clean           remove build/
#
# build.sh drawterm uses X11 when libX11 and libXt are available, otherwise
# Wayland (wayland-client, xkbcommon, libdecor-0).  DRAWTERM_GUI=x11 or
# DRAWTERM_GUI=wl forces one.
#
# The sources are copied to build/<target>/ first, so in-source objects and
# libraries from another configuration cannot be reused.
set -eu

root=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
outroot=${BUILD_DIR:-"$root/build"}
target=${1:-all}

copy() {
	out=$1
	rm -rf "$out"
	mkdir -p "$out"
	(
		cd "$root"
		tar -cf - \
			--exclude=./build \
			--exclude='*.o' --exclude='*.a' \
			--exclude=./9pterm --exclude=./drawterm \
			.
	) | tar -xf - -C "$out"
}

build_9pterm() {
	out=$outroot/9pterm
	copy "$out"
	echo "9pterm: make CONF=9pterm" >&2
	(cd "$out" && make CONF=9pterm -j"$(nproc)")
	echo "$out/9pterm"
}

has() {
	pkg-config --exists "$1" 2>/dev/null
}

has_gui() {
	[ -n "${DRAWTERM_GUI:-}" ] && return 0
	{ has x11 && has xt; } && return 0
	has wayland-client && has xkbcommon && has libdecor-0 && \
		has wayland-protocols && command -v wayland-scanner >/dev/null 2>&1
}

build_drawterm() {
	gui=${DRAWTERM_GUI:-}
	if [ -z "$gui" ]; then
		if has x11 && has xt; then
			gui=x11
		elif has wayland-client && has xkbcommon && has libdecor-0 && \
		     has wayland-protocols && command -v wayland-scanner >/dev/null 2>&1; then
			gui=wl
		else
			echo "drawterm: need libX11+libXt (Debian: libx11-dev libxt-dev)" >&2
			echo "drawterm: or wayland-client, xkbcommon, libdecor-0 and wayland-protocols" >&2
			exit 1
		fi
	fi
	base='-Wall -Wno-missing-braces -Wno-parentheses -ggdb -I$(ROOT) -I$(ROOT)/include -I$(ROOT)/kern -c -D_THREAD_SAFE -DPTHREAD -pthread -D_REENTRANT -O2'
	case $gui in
	x11)
		cflags=$base
		ldadd='-lX11 -ggdb -lm -lrt'
		;;
	wl)
		cflags="$base $(pkg-config --cflags wayland-client xkbcommon libdecor-0)"
		ldadd="$(pkg-config --libs wayland-client xkbcommon libdecor-0) -ggdb -lm -lrt"
		;;
	*)
		echo "drawterm: DRAWTERM_GUI must be x11 or wl" >&2
		exit 2
		;;
	esac
	out=$outroot/drawterm
	copy "$out"
	echo "drawterm: make CONF=linux GUI=$gui AUDIO=none" >&2
	(cd "$out" && make CONF=linux GUI="$gui" AUDIO=none -j"$(nproc)" \
		"CFLAGS=$cflags" "LDADD=$ldadd")
	echo "$out/drawterm"
}

case $target in
9pterm) build_9pterm ;;
drawterm) build_drawterm ;;
all)
	build_9pterm
	if has_gui; then
		build_drawterm
	else
		echo "drawterm: skipped (no X11/Xt or Wayland development libraries)" >&2
	fi
	;;
clean)
	rm -rf "$outroot"
	;;
*)
	echo "usage: $0 [9pterm|drawterm|all|clean]" >&2
	exit 2
	;;
esac
