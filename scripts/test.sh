#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-only
set -eu

repo=$(CDPATH='' cd -- "$(dirname -- "$0")/.." && pwd)
build=$(mktemp -d)
trap 'rm -rf "$build"' EXIT
"${CC:-cc}" -std=c11 -Wall -Wextra -Werror -O2 ${CFLAGS:-} \
	"$repo/src/sacn.c" "$repo/tests/test_sacn.c" -o "$build/test_sacn"
"$build/test_sacn"
