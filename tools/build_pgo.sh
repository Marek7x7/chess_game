#!/usr/bin/env bash
# Profile-guided build of the engine with GCC: instrument, train on the benchmark searches, rebuild with the
# profile. The search tree is unchanged (the BENCH SIGNATURE is identical); on GCC 15 and this code the
# speed gain is about 5% (2.86 -> 2.99 Mnps on `--bench 13`).
#
#   tools/build_pgo.sh [build-dir] [extra cmake arguments...]      default build dir: build-pgo
#
# The profile belongs to the build directory (GCC names the .gcda files after the object paths), so both
# steps use the same one. Not for Clang (different flags) or cross builds (the instrumented binary has to run).
set -euo pipefail

REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD="$(realpath -m "${1:-$REPO/build-pgo}")"
[ $# -gt 0 ] && shift
DATA="$BUILD/profile-data"

rm -rf "$DATA"
mkdir -p "$DATA"
cmake -S "$REPO" -B "$BUILD" -DCMAKE_BUILD_TYPE=Release -DCHESS_BUILD_TESTS=OFF "$@" \
    -DCMAKE_CXX_FLAGS="-fprofile-generate -fprofile-dir=$DATA" >/dev/null
cmake --build "$BUILD" -j"$(nproc)" >/dev/null

echo "Training..."
for depth in 11 12 13; do "$BUILD/chess" --bench "$depth" | grep SIGNATURE; done
printf 'uci\nposition startpos moves e2e4 e7e5 g1f3\ngo depth 12\nposition fen r3k2r/p1ppqpb1/bn2pnp1/3PN3/1p2P3/2N2Q1p/PPPBBPPP/R3K2R w KQkq - 0 1\ngo depth 12\nquit\n' |
    "$BUILD/chess" --uci >/dev/null

cmake -S "$REPO" -B "$BUILD" -DCMAKE_BUILD_TYPE=Release -DCHESS_BUILD_TESTS=OFF "$@" \
    -DCMAKE_CXX_FLAGS="-fprofile-use -fprofile-correction -Wno-missing-profile -fprofile-dir=$DATA" >/dev/null
cmake --build "$BUILD" -j"$(nproc)" >/dev/null
echo "Built $BUILD/chess"
"$BUILD/chess" --bench 13 | grep -E "SPEED|SIGNATURE"
