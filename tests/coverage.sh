#!/bin/sh
set -eu
cd "$(dirname "$0")/.."
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT HUP INT TERM
: "${CC:=clang}"
: "${LLVM_PROFDATA:=llvm-profdata}"
: "${LLVM_COV:=llvm-cov}"
for suite in regression functional; do
    "$CC" -std=c99 -O0 -g -fprofile-instr-generate -fcoverage-mapping \
        -Isrc "tests/$suite.c" -lm -o "$tmp/$suite"
    LLVM_PROFILE_FILE="$tmp/$suite.profraw" timeout 10 "$tmp/$suite" "$tmp"
done
"$LLVM_PROFDATA" merge -sparse "$tmp/regression.profraw" "$tmp/functional.profraw" -o "$tmp/coverage.profdata"
"$LLVM_COV" report "$tmp/regression" -object "$tmp/functional" \
    -instr-profile="$tmp/coverage.profdata" src/nanosvg.h src/nanosvgrast.h
