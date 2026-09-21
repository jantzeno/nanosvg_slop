#!/bin/sh
set -eu
cd "$(dirname "$0")/.."
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT HUP INT TERM
: "${CC:=cc}"
: "${CFLAGS:=-std=c99 -O1 -g -fsanitize=address,undefined,float-cast-overflow -fno-sanitize-recover=all -fno-omit-frame-pointer}"
for suite in regression functional; do
    # CFLAGS intentionally supports multiple compiler arguments.
    "$CC" $CFLAGS -Isrc "tests/$suite.c" -lm -o "$tmp/$suite"
    ASAN_OPTIONS="${ASAN_OPTIONS:-detect_leaks=0}" timeout 10 "$tmp/$suite" "$tmp"
done
