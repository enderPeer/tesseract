#!/bin/sh
# Builds alpha with gcc or clang (Linux).
#   ./build.sh         -> bin/alpha
#   ./build.sh test    -> also builds and runs the tests
set -e
cd "$(dirname "$0")"

CC=${CC:-cc}
CFLAGS="-std=c11 -O2 -Wall -Wextra -D_GNU_SOURCE"
mkdir -p bin build

# Only the lane engine is compiled for AVX2, so everything else still runs on any x86-64 CPU
# and can check for AVX2 before using it.
$CC $CFLAGS -mavx2 -c src/lanes.c -o build/lanes.o
CORE="src/asm.c src/ref.c src/check.c src/platform.c src/x86.c src/jit.c src/parallel.c src/tester.c
      src/build.c src/net.c src/gen.c src/spec.c src/enum.c src/work.c src/benes.c src/cubedemo.c
      src/beta.c src/betacmd.c src/super.c src/supercmd.c src/gamma.c src/gammacmd.c src/gammaserve.c
      build/lanes.o"

$CC $CFLAGS src/main.c $CORE -o bin/alpha -pthread

[ "$1" = test ] || exit 0
for t in test_ref test_lanes test_jit test_tester test_work test_cube test_beta test_super test_gamma; do
    $CC $CFLAGS -Isrc tests/$t.c $CORE -o bin/$t -pthread
done
bin/test_ref
bin/test_lanes
bin/test_jit
bin/test_tester
bin/test_work
bin/test_cube
bin/test_beta
bin/test_super
bin/test_gamma
