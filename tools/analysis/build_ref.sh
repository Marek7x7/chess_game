#!/bin/sh
# build_ref.sh <ref>  -> $S/bins/<shorthash>.bin (and prints it)
S=${S:?set S to the scratch directory holding sf/ and bins/}
cd "$(dirname "$0")/../.." || exit 1
H=$(git rev-parse --short "$1")
[ -x $S/bins/$H.bin ] && { echo $S/bins/$H.bin; exit 0; }
rm -rf $S/wt-$H; git worktree add --detach $S/wt-$H "$1" >/dev/null 2>&1
cmake -S $S/wt-$H -B $S/wt-$H/b -DCMAKE_BUILD_TYPE=Release >/dev/null && cmake --build $S/wt-$H/b -j >/dev/null 2>&1
cp $S/wt-$H/b/chess $S/bins/$H.bin; git worktree remove --force $S/wt-$H
echo $S/bins/$H.bin
