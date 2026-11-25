#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-only
set -eu

repo=$(CDPATH='' cd -- "$(dirname -- "$0")/.." && pwd)
out=${1:-"$repo/artifacts"}
mkdir -p "$out"
out=$(CDPATH='' cd -- "$out" && pwd)

image=ghcr.io/openwrt/sdk:ramips-mt7621-25.12.6@sha256:0d28e4f80933a32cef954e8e476ca258582283984771b952d8f9d9bea1bd789f
container=$(docker create --platform linux/amd64 -i -e JOBS="${JOBS:-2}" "$image" sh -ec '
	package=package/luci-app-sacn-monitor
	mkdir -p "$package"
	tar -xf - -C "$package"

	# Use the feed revisions shipped with the SDK.
	./scripts/feeds update base packages luci
	./scripts/feeds install libpcap libubox libubus luci-base rpcd
	cat > .config <<CONFIG
# CONFIG_ALL is not set
# CONFIG_ALL_NONSHARED is not set
# CONFIG_ALL_KMODS is not set
CONFIG_PACKAGE_luci-app-sacn-monitor=m
CONFIG
	make defconfig
	grep -qx "CONFIG_PACKAGE_luci-app-sacn-monitor=m" .config
	make -j"$JOBS" "$package/compile" V=s

	set -- bin/packages/mipsel_24kc/*/luci-app-sacn-monitor-*.apk
	if [ "$#" -ne 1 ] || [ ! -f "$1" ]; then
		printf "%s\n" "Expected one SACNMON APK in bin/packages/mipsel_24kc." >&2
		exit 1
	fi
	mkdir /tmp/sacnmon-apk
	cp "$1" /tmp/sacnmon-apk/
	cd /tmp/sacnmon-apk
	sha256sum "${1##*/}" > "${1##*/}.sha256"
')
trap 'docker rm -f "$container" >/dev/null' EXIT

tar -C "$repo" -cf - Makefile LICENSE src root htdocs | docker start -ai "$container"
docker cp "$container:/tmp/sacnmon-apk/." "$out"
