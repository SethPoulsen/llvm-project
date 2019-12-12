#!/usr/bin/env bash

# usage:
# $ path/to/test_simple.sh [regalloc pass] [source.c]
# bracketed items are optional
# source.c defaults to quadratic.c
# regalloc pass defaults to ranaive

set -x -e

cd "$(dirname "${0}")"

#cmake -C ../build
ninja -C ../build llc

regalloc="${1:-rass}"
source="$(echo """${2:-quadratic.c}""" | head -c -3)"

echo "compiling ${source} with ${regalloc}"

clang -c -emit-llvm "${source}.c" -o "${source}.bc"
# In RegAlloc*.cpp:
# if (VerifyEnabled)
#   MF->verify(this, "After splitting live range around region");
../build/bin/llc -regalloc="${regalloc}" -verify-regalloc "${source}.bc" -o "${source}.s"
clang "${source}.s" -lm -o "${source}.exe"
"./${source}.exe"
