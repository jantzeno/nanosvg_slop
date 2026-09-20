#!/bin/sh
set -eu
cd "$(dirname "$0")/.."
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT HUP INT TERM
: "${CC:=cc}"
: "${CFLAGS:=-std=c99 -O1 -g -fsanitize=address,undefined,float-cast-overflow -fno-sanitize-recover=all -fno-omit-frame-pointer}"
# CFLAGS intentionally supports multiple compiler arguments.
"$CC" $CFLAGS -Isrc tests/regression.c -lm -o "$tmp/regression"
ASAN_OPTIONS="${ASAN_OPTIONS:-detect_leaks=0}" timeout 10 "$tmp/regression"
