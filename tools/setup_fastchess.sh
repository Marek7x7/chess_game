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

git clone --depth 1 https://github.com/Disservin/fastchess.git "$DEST"
# LDFLAGS override: fastchess's Makefile unconditionally links -lstdc++fs,
# which GCC 9+ no longer ships as a separate library (std::filesystem moved
# into libstdc++ itself) -- linking fails on this system's GCC 15.2.0
# without dropping it. -pthread is still required (used directly).
make -C "$DEST" -j"$(nproc)" LDFLAGS="-pthread"

echo "Built: $DEST/fastchess"
echo "Add it to PATH, or reference $DEST/fastchess directly from sprt.sh/smoke_test.sh."
