#!/bin/sh
set -eu
cd "$(dirname "$0")/.."
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT HUP INT TERM
: "${CC:=cc}"
: "${CXX:=c++}"
: "${CFLAGS:=-O1 -g -fsanitize=address,undefined,float-cast-overflow -fno-sanitize-recover=all -fno-omit-frame-pointer}"
: "${CXXFLAGS:=$CFLAGS}"
export ASAN_OPTIONS="${ASAN_OPTIONS:-detect_leaks=0}"
CC="$CC" CXX="$CXX" CFLAGS="$CFLAGS" CXXFLAGS="$CXXFLAGS" sh tests/headers.sh
for source in nanosvg nanosvgrast; do
    macro=$(printf "%s_IMPLEMENTATION" "$source" | tr '[:lower:]' '[:upper:]')
    "$CXX" $CXXFLAGS -std=c++23 -Isrc -D"$macro" -x c++ -c "src/$source.h" -o "$tmp/$source.o"
done
"$CC" $CFLAGS -std=c99 -Isrc -c tests/functional.c -o "$tmp/functional.o"
"$CXX" $CXXFLAGS "$tmp/functional.o" "$tmp/nanosvg.o" "$tmp/nanosvgrast.o" -o "$tmp/functional"
"$CXX" $CXXFLAGS -std=c++23 -Isrc tests/regression.cpp -o "$tmp/regression"
"$CXX" $CXXFLAGS -std=c++23 -Isrc tests/native.cpp "$tmp/nanosvg.o" "$tmp/nanosvgrast.o" -o "$tmp/native"
for suite in regression functional native; do
    timeout 10 "$tmp/$suite" "$tmp"
done
if [ "$(uname -s)" = Linux ]; then
    "$CXX" $CXXFLAGS -std=c++23 -Isrc tests/failures.cpp "$tmp/nanosvg.o" "$tmp/nanosvgrast.o" -Wl,--wrap=calloc -Wl,--wrap=free -o "$tmp/failures"
    timeout 10 "$tmp/failures"
fi
