#!/usr/bin/env bash
# One-time setup: fetches 8moves_v3.pgn (github.com/official-stockfish/books)
# into tools/openings/ -- the balanced opening suite every real keep/revert
# SPRT decision must use (see STRENGTH_LOG.md). Not committed to this repo
# (8MB, trivially re-fetchable, already covered by .gitignore's blanket
# *.pgn rule) -- run by hand, not invoked automatically by sprt.sh.
set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
DEST="$REPO_ROOT/tools/openings/8moves_v3.pgn"

if [ -f "$DEST" ]; then
    echo "Already present: $DEST"
    exit 0
fi

TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT
git clone --depth 1 https://github.com/official-stockfish/books.git "$TMP/books"
unzip -o "$TMP/books/8moves_v3.pgn.zip" -d "$REPO_ROOT/tools/openings/"

echo "Fetched: $DEST"
