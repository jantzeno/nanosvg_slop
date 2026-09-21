#!/bin/sh
set -eu
cd "$(dirname "$0")/.."
: "${CC:=cc}"
: "${CXX:=c++}"
: "${CFLAGS:=-O1 -g -fsanitize=address,undefined,float-cast-overflow -fno-sanitize-recover=all -fno-omit-frame-pointer}"
echo 'C++23 native API and C99 compatibility API'
CC="$CC" CXX="$CXX" CFLAGS="$CFLAGS" sh tests/run.sh
echo 'All SVG color keywords'
CC="$CC" CXX="$CXX" CFLAGS="$CFLAGS -DNANOSVG_ALL_COLOR_KEYWORDS" CXXFLAGS="${CXXFLAGS:-$CFLAGS} -DNANOSVG_ALL_COLOR_KEYWORDS" sh tests/run.sh
echo 'CMake source and installed consumers'
CC="$CC" CXX="$CXX" sh tests/install.sh
echo 'Xmake static/shared builds and consumers'
CC="$CC" CXX="$CXX" sh tests/xmake.sh
