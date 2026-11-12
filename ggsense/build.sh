#!/bin/bash
# Build and test ggsense from source (run from anywhere).
#   bash ggsense/build.sh host    unit tests and fuzz test on this computer only (needs a C compiler, "cc")
#   bash ggsense/build.sh         the same, then the tablet library ../prebuilt/ggsense.so and ../tools/ggtool
#                                 (needs the zig compiler as a Python package: python3 -m pip install ziglang)
set -e
cd "$(dirname "$0")"
D="$PWD"
F="-std=gnu11 -O1 -g -fsanitize=address,undefined -fno-omit-frame-pointer -Wall -Wextra -Wno-unused-function"
mkdir -p testrun testrunf
cc $F -DTESTRUN="\"$D/testrun\"" -o test_ggsense test_ggsense-1.8.30.c
cc $F -DTESTRUN="\"$D/testrunf\"" -o fuzz_ggsense fuzz_ggsense-1.8.30.c
./test_ggsense > testrun/out.txt 2>&1 || echo "unit tests rc=$?"
tail -n 2 testrun/out.txt
./fuzz_ggsense 200000 1826 > testrunf/out.txt 2>&1 || echo "fuzz rc=$?"
tail -n 3 testrunf/out.txt
[ "$1" = host ] && exit 0
Z="python3 -m ziglang cc -target aarch64-linux-gnu.2.31 -O2 -s -Wall -Wextra"
$Z -shared -fPIC -o ../prebuilt/ggsense.so ggsense-1.8.30.c -ldl -lpthread -lm
$Z -o ../tools/ggtool ../tools/ggtool.c
echo "built ../prebuilt/ggsense.so and ../tools/ggtool"
