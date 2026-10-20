#!/usr/bin/env bash
# One-time setup: clones and builds fastchess (github.com/Disservin/fastchess)
# from source, since it's not packaged for this system. Run by hand, not
# invoked automatically by sprt.sh/smoke_test.sh.
set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
DEST="$REPO_ROOT/tools/fastchess-src"

if [ -x "$DEST/fastchess" ]; then
    echo "Already built: $DEST/fastchess"
    exit 0
fi

# Pinned: every SPRT in STRENGTH_LOG.md ran on this build (fastchess alpha 1.8.2, 60d7a7a), and a newer one
# could change defaults (the SPRT model, for one) under the same command line.
FASTCHESS_COMMIT=60d7a7a26c6b0582a15c112fb29a1829bef2adb3
git init -q "$DEST"
git -C "$DEST" remote add origin https://github.com/Disservin/fastchess.git
git -C "$DEST" fetch -q --depth 1 origin "$FASTCHESS_COMMIT"
git -C "$DEST" checkout -q FETCH_HEAD
# LDFLAGS override: fastchess's Makefile unconditionally links -lstdc++fs,
# which GCC 9+ no longer ships as a separate library (std::filesystem moved
# into libstdc++ itself) -- linking fails on this system's GCC 15.2.0
# without dropping it. -pthread is still required (used directly).
make -C "$DEST" -j"$(nproc)" LDFLAGS="-pthread"

echo "Built: $DEST/fastchess"
echo "Add it to PATH, or reference $DEST/fastchess directly from sprt.sh/smoke_test.sh."
