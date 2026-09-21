#!/bin/sh
set -eu
cd "$(dirname "$0")/.."
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT HUP INT TERM
: "${CC:=clang}"
: "${CXX:=clang++}"
: "${LLVM_PROFDATA:=llvm-profdata}"
: "${LLVM_COV:=llvm-cov}"
flags='-O0 -g -fprofile-instr-generate -fcoverage-mapping'
for source in nanosvg nanosvgrast; do
    macro=$(printf "%s_IMPLEMENTATION" "$source" | tr '[:lower:]' '[:upper:]')
    "$CXX" -std=c++23 $flags -Isrc -D"$macro" -x c++ -c "src/$source.h" -o "$tmp/$source.o"
done
"$CC" -std=c99 $flags -Isrc -c tests/functional.c -o "$tmp/functional.o"
"$CXX" $flags "$tmp/functional.o" "$tmp/nanosvg.o" "$tmp/nanosvgrast.o" -o "$tmp/functional"
"$CXX" -std=c++23 $flags -Isrc tests/regression.cpp -o "$tmp/regression"
"$CXX" -std=c++23 $flags -Isrc tests/native.cpp "$tmp/nanosvg.o" "$tmp/nanosvgrast.o" -o "$tmp/native"
for suite in regression functional native; do
    LLVM_PROFILE_FILE="$tmp/$suite.profraw" timeout 10 "$tmp/$suite" "$tmp"
done
"$LLVM_PROFDATA" merge -sparse "$tmp/regression.profraw" "$tmp/functional.profraw" "$tmp/native.profraw" -o "$tmp/coverage.profdata"
"$LLVM_COV" report "$tmp/regression" -object "$tmp/functional" -object "$tmp/native" \
    -instr-profile="$tmp/coverage.profdata" src/nanosvg.hpp src/nanosvgrast.hpp src/nanosvg.h src/nanosvgrast.h
