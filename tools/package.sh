#!/usr/bin/env bash
# Stages a release archive: the executable, the README and the licences of what is compiled in.
#
#   tools/package.sh <executable> <archive-base-name> <tar.gz|zip> [out-dir]
#
# e.g. tools/package.sh build/chess chess-linux-x86_64 tar.gz dist
#      tools/package.sh build/chess.exe chess-windows-x86_64 zip dist
# The archive is <out-dir>/<archive-base-name>.<format> and holds one directory of the same name.
set -euo pipefail
EXE=${1:?executable}; NAME=${2:?archive base name}; FORMAT=${3:?tar.gz or zip}; OUT=${4:-dist}
REPO=$(cd "$(dirname "$0")/.." && pwd)
[ -f "$EXE" ] || { echo "no such executable: $EXE" >&2; exit 1; }
STAGE=$(mktemp -d)
trap 'rm -rf "$STAGE"' EXIT
mkdir -p "$STAGE/$NAME" "$OUT"
cp "$EXE" "$STAGE/$NAME/"
cp "$REPO/README.md" "$STAGE/$NAME/"
# Fathom (MIT) is compiled in: its licence must travel with the binary.
cp "$REPO/third_party/fathom/LICENSE" "$STAGE/$NAME/LICENSE-fathom.txt"
OUT=$(mkdir -p "$OUT" && cd "$OUT" && pwd)
case "$FORMAT" in
    tar.gz) (cd "$STAGE" && tar czf "$OUT/$NAME.tar.gz" "$NAME") ;;
    zip)    (cd "$STAGE" && python3 -m zipfile -c "$OUT/$NAME.zip" "$NAME") ;;
    *) echo "unknown format: $FORMAT" >&2; exit 1 ;;
esac
echo "$OUT/$NAME.$FORMAT"
