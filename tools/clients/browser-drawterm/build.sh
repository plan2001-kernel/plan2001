#!/bin/sh
# Build the standalone browser Drawterm.
#
#   build.sh          build/drawterm/{drawterm.js,drawterm.wasm}
#                     build/site/{index.html,page.js,drawterm.js,drawterm.wasm}
#   build.sh clean    remove build/
#
# gui-web and Make.emscripten live in the one drawterm source tree at
# ../drawterm; this script only assembles an out-of-source copy so the
# native host builds cannot reuse Emscripten objects.
set -eu

root=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
draw=$root/../drawterm
out=${BUILD_DIR:-"$root/build"}

if [ "${1:-}" = clean ]; then
	rm -rf "$out"
	exit 0
fi

command -v emcc >/dev/null 2>&1 || {
	echo "browser-drawterm: emcc (Emscripten) is required" >&2
	exit 1
}

rm -rf "$out/drawterm" "$out/site"
mkdir -p "$out/drawterm" "$out/site"
(
	cd "$draw"
	tar -cf - \
		--exclude=./build \
		--exclude='*.o' --exclude='*.a' \
		--exclude=./9pterm --exclude=./drawterm \
		.
) | tar -xf - -C "$out/drawterm"

echo "browser-drawterm: make CONF=emscripten" >&2
(cd "$out/drawterm" && make CONF=emscripten -j"$(nproc)")

cp "$root/index.html" "$root/page.js" "$out/site/"
cp "$out/drawterm/drawterm.js" "$out/drawterm/drawterm.wasm" "$out/site/"
echo "$out/site/index.html"
