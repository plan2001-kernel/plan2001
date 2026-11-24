#!/bin/sh
# Local browser Drawterm tests: build, bridge protocol, headless Chromium
# runtime, then the existing native 9pterm/drawterm smoke tests.
#
#   test.sh          build and test
#   NOBUILD=1 test.sh   test an existing build/
set -eu

root=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)

if [ "${NOBUILD:-0}" != 1 ]; then
	"$root/build.sh"
fi

python3 "$root/test_bridge.py"
python3 "$root/test_browser.py"

if [ -x "$root/../drawterm/test.sh" ]; then
	"$root/../drawterm/test.sh"
fi

echo "all browser drawterm tests passed"
