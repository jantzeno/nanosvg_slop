#!/bin/sh
set -eu
cd "$(dirname "$0")/.."
if ! command -v xmake >/dev/null 2>&1; then
    echo 'Xmake checks skipped: xmake is not installed'
    exit 0
fi
: "${CC:=cc}"
: "${CXX:=c++}"
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT HUP INT TERM
export XMAKE_GLOBALDIR="$tmp/global"
mkdir -p "$tmp/project/src" "$tmp/scratch"
cp xmake.lua "$tmp/project/"
cp src/*.h src/*.hpp "$tmp/project/src/"
for kind in static shared; do
    mode=debug
    if [ "$kind" = shared ]; then mode=release; fi
    xmake f -P "$tmp/project" -y -c -m "$mode" -k "$kind" \
        -o "$tmp/project/build" \
        --cc="$CC" --cxx="$CXX" --ld="$CXX" --sh="$CXX"
    xmake -P "$tmp/project" -y
    prefix="$tmp/install-$kind"
    xmake install -P "$tmp/project" -y -o "$prefix"
    test -f "$prefix/include/nanosvg/nanosvg.h"
    test -f "$prefix/include/nanosvg/nanosvgrast.h"
    test -f "$prefix/include/nanosvg/nanosvg.hpp"
    test -f "$prefix/include/nanosvg/nanosvgrast.hpp"
    test "$(ls -A "$prefix/include/nanosvg" | sort)" = 'nanosvg.h
nanosvg.hpp
nanosvgrast.h
nanosvgrast.hpp'
    test "$(ls -A "$tmp/project/src" | sort)" = 'nanosvg.h
nanosvg.hpp
nanosvgrast.h
nanosvgrast.hpp'
    "$CC" -std=c99 -I"$prefix/include/nanosvg" -c tests/functional.c -o "$tmp/functional.o"
    "$CXX" "$tmp/functional.o" -L"$prefix/lib" -lnanosvgrast -lnanosvg -o "$tmp/functional"
    "$CXX" -std=c++23 -I"$prefix/include/nanosvg" tests/native.cpp \
        -L"$prefix/lib" -lnanosvgrast -lnanosvg -o "$tmp/native"
    LD_LIBRARY_PATH="$prefix/lib${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}" timeout 10 "$tmp/functional" "$tmp/scratch"
    LD_LIBRARY_PATH="$prefix/lib${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}" timeout 10 "$tmp/native" "$tmp/scratch"
    echo "NanoSVG Xmake passed: $mode, $kind (C and native consumers)"
done
