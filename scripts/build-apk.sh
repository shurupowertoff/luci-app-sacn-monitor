#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-only
set -eu

repo=$(CDPATH='' cd -- "$(dirname -- "$0")/.." && pwd)
out=${1:-"$repo/artifacts"}
mkdir -p "$out"
out=$(CDPATH='' cd -- "$out" && pwd)
build=$(mktemp -d)
trap 'rm -rf "$build"' EXIT
cd "$build"

case "$(uname -sm)" in
	'Linux x86_64') ;;
	*) printf '%s\n' 'The SDK build script requires x86_64 Linux.' >&2; exit 1 ;;
esac

release=https://downloads.openwrt.org/releases/25.12.6/targets/ramips/mt7621
curl --fail --location --silent --show-error --retry 3 "$release/sha256sums" -o sha256sums
awk '
	$1 ~ /^[0-9a-f]+$/ && length($1) == 64 &&
	$2 ~ /^\*?openwrt-sdk-25\.12\.6-ramips-mt7621[_-][A-Za-z0-9_.+-]+\.Linux-x86_64\.tar\.zst$/ {
		sub(/^\*/, "", $2)
		print $1 "  " $2
		count++
	}
	END { if (count != 1) exit 1 }
' sha256sums > sdk.sha256 || {
	printf '%s\n' 'Expected exactly one x86_64 Linux SDK in the official checksum list.' >&2
	exit 1
}
sdk=$(awk '{ print $2 }' sdk.sha256)
curl --fail --location --silent --show-error --retry 3 "$release/$sdk" -o "$sdk"
sha256sum --check sdk.sha256
tar --zstd -xf "$sdk"
cd "${sdk%.tar.zst}"

# Use the release SDK's feeds and revisions.
./scripts/feeds update -a
./scripts/feeds install luci-base
package=package/luci-app-sacn-monitor
mkdir -p "$package"
cp -R "$repo/Makefile" "$repo/LICENSE" "$repo/src" "$repo/root" "$repo/htdocs" "$package/"
printf '%s\n' 'CONFIG_PACKAGE_luci-app-sacn-monitor=m' >> .config
make defconfig
grep -qx 'CONFIG_PACKAGE_luci-app-sacn-monitor=m' .config
make -j"${JOBS:-2}" "$package/compile" V=s

set -- bin/packages/mipsel_24kc/*/luci-app-sacn-monitor-*.apk
if [ "$#" -ne 1 ] || [ ! -f "$1" ]; then
	printf '%s\n' 'Expected one SACNMON APK in bin/packages/mipsel_24kc.' >&2
	exit 1
fi
cp "$1" "$out/"
cd "$out"
sha256sum "${1##*/}" > "${1##*/}.sha256"
