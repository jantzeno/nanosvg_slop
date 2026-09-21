#!/bin/sh
set -eu
cd "$(dirname "$0")/.."
: "${CC:=cc}"
: "${CXX:=c++}"
: "${CFLAGS:=-O1 -g -fsanitize=address,undefined,float-cast-overflow -fno-sanitize-recover=all -fno-omit-frame-pointer}"

echo 'C99: default header configuration'
CC="$CC" CFLAGS="-std=c99 $CFLAGS" sh tests/run.sh
echo 'C99: all color keywords'
CC="$CC" CFLAGS="-std=c99 $CFLAGS -DNANOSVG_ALL_COLOR_KEYWORDS" sh tests/run.sh
echo 'C++11: default C linkage'
CC="$CXX" CFLAGS="-x c++ -std=c++11 $CFLAGS" sh tests/run.sh
echo 'C++23: all color keywords with C linkage'
CC="$CXX" CFLAGS="-x c++ -std=c++23 $CFLAGS -DNANOSVG_ALL_COLOR_KEYWORDS" sh tests/run.sh
echo 'C++23: optional C++ linkage'
CC="$CXX" CFLAGS="-x c++ -std=c++23 $CFLAGS -DNANOSVG_CPLUSPLUS -DNANOSVGRAST_CPLUSPLUS" sh tests/run.sh
echo 'CMake: C and C++ consumers'
CC="$CC" CXX="$CXX" sh tests/install.sh
