#!/bin/sh
# Package script for Linux and Windows releases.
# This file is a part of Web Fortress
#
# Usage:
#   ./package.sh [linux] [windows]
#   ./package.sh          -- builds both
#
# Binaries are picked up from the standard CMake build directories:
#   Linux   : ../../build/plugins/external/webfort/server/webfort.plug.so
#   Windows : ../../build/win64-cross/output/webfort.plug.dll

set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
BUILD_DIR="$SCRIPT_DIR/../../../build"

LINUX_SO="$BUILD_DIR/plugins/external/webfort/server/webfort.plug.so"
WIN_DLL="$BUILD_DIR/win64-cross/output/hack/plugins/webfort.plug.dll"

VERSION="$(git -C "$SCRIPT_DIR" describe --tags)"

BUILD_LINUX=0
BUILD_WINDOWS=0

if [ $# -eq 0 ]; then
	BUILD_LINUX=1
	BUILD_WINDOWS=1
else
	for arg in "$@"; do
		case "$arg" in
			linux)   BUILD_LINUX=1 ;;
			windows) BUILD_WINDOWS=1 ;;
			*) echo "Unknown target: $arg"; echo "Usage: $0 [linux] [windows]"; exit 1 ;;
		esac
	done
fi

make_package() {
	local binary="$1"
	local zipname="$2"

	if [ ! -r "$binary" ]; then
		echo "Error: binary not found: $binary"
		exit 1
	fi

	rm -rf package
	mkdir -p package/hack/plugins
	mkdir -p package/hack/webfort
	cp -v "$binary" package/hack/plugins/
	cp -vr static package/hack/webfort/static
	cp -v README.md    package/WF-README.md
	cp -v INSTALLING.txt package/WF-INSTALLING.txt
	cp -v LICENSE      package/WF-LICENSE

	rm -f "$zipname"
	(cd package && zip -r "../$zipname" ./*)
	rm -rf package
	echo "$zipname: Done."
}

cd "$SCRIPT_DIR"

if [ "$BUILD_LINUX" -eq 1 ]; then
	make_package "$LINUX_SO" "webfort-${VERSION}-linux.zip"
fi

if [ "$BUILD_WINDOWS" -eq 1 ]; then
	make_package "$WIN_DLL" "webfort-${VERSION}-windows.zip"
fi
