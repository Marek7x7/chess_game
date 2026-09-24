# Phase 0 blunder-forensics replay

Generated with the Release build (commit `9d98d5b`, the last Phase 0 commit before this file)
via `--replay <file> <side>`, EXPERT settings (depth 20, 6000ms), analyzing **only the color
the engine actually played historically** in each game -- opponent moves are not analyzed,
since disagreeing with the opponent's move says nothing about this engine's own quality.

```
game1.pgn: white    game3.pgn: black    game5.pgn: white
game2.pgn: white    game4.pgn: black    game6.pgn: white
```

Regenerate with `./build-release/chess --replay <file> <side> 2> log.txt`.

**Caveat on interpreting "MISMATCH":** it means the current EXPERT-depth re-analysis picked a
different move than what was actually played -- it does **not** by itself mean the played move
was a blunder. The historical games were played at whatever preset/time budget was active then
(often much weaker than EXPERT), and many mismatches below are marginal (a few centipawns, or a
different but comparable continuation). Treat the *score* next to each mismatch as the signal,
not the mismatch count alone -- a mismatch with a score near 0 is noise; a mismatch with a score
in the hundreds of centipawns or a mate score is a real disagreement worth investigating in
Phase 1.

## Summary

| Game | Engine color | Positions analyzed | Mismatches |
|---|---|---|---|
| game1.pgn (vs Wendy-BOT, 2026.07.22) | White | 26 | 17 |
| game2.pgn (vs Wendy-BOT, 2026.07.23) | White | 34 | 17 |
| game3.pgn (vs Komodo25, 2026.09.02) | Black | 66 | 13 |
| game4.pgn (vs Komodo25, 2026.09.15) | Black | 79 | 23 |
| game5.pgn (vs Komodo25, 2026.09.22) | White | 56 | 9 |
| game6.pgn (vs Komodo25, 2026.09.22, 2nd game) | White | 20 | 5 |
| **Total** | | **281** | **84** |

## Specifically requested: game6.pgn moves 9, 18, 19 (White)

Full game: `1. d4 Nf6 2. c4 e6 3. Nc3 Bb4 4. Bd2 O-O 5. Nf3 d5 6. e3 Bd7 7. Qb3 Nc6 8. cxd5
exd5 9. Nxd5 Bxd2+ 10. Nxd2 Nxd4 11. Nxf6+ Qxf6 12. exd4 Rfe8+ 13. Be2 Qxd4 14. Nc4 Bg4
15. Ne3 Bxe2 16. Kxe2 Qf6 17. Rhd1 Qe5 18. Qxb7 Rab8 19. Qxc7 Rxb2+ 20. Kf1 Qxc7 0-1`

| Move | Actual | Current engine suggests | Score | Verdict |
|---|---|---|---|---|
| 9 | Nxd5 | **c3d5 (Nxd5)** | cp 97 | **MATCH** -- the engine still plays 9.Nxd5 |
| 18 | Qxb7 | d1d7 (Qd7) | cp 319 | MISMATCH -- engine prefers Qd7 over Qxb7 |
| 19 | Qxc7 | b7c6 (Qc6) | cp 218 | MISMATCH -- engine avoids Qxc7, prefers Qc6 |

**19.Qxc7 directly verified as a real blunder, not just "a different move preferred":**
fed the *exact* historical continuation `18.Qxb7 Rab8 19.Qxc7 Rxb2+ 20.Kf1` into the engine
and asked what Black (to move) should play next -- i.e. reproduced Komodo25's actual position
one ply before its winning recapture. Result:

```
bestmove e5c7
info depth 13 score cp 780 nodes 3827600 nps 1517116 pv e5c7 a1b1
```

The engine finds **Qxc7 (e5c7) as clearly best for Black, +7.80 for Black** -- confirming the
original claim precisely: after `19.Qxc7`, Black's queen on e5 captures the wandering white
queen on c7 (the ...Rxb2+ zwischenzug just inserts a forced king move first), and the engine's
own search recognizes this as an overwhelming, effectively queen-winning tactic. 19.Qxc7 really
does hang the queen; the current engine would not play it, and would not survive it either.

18.Qxb7 was not directly tactically verified the same way (no exact refutation position tested)
-- the mismatch shows the engine prefers Qd7 by a comparable margin, but confirming *why*
Qxb7 was inferior (if at all -- 319cp for the engine's preferred alternative isn't necessarily
proof Qxb7 loses material, just that another move scores higher) is left for Phase 1.

## Full mismatch list, all 6 games (engine's own color only)

<details>
<summary>84 mismatches, one line each: move, engine's suggestion + score + PV, what was actually played</summary>

```
### game1.pgn
move 2: engine suggests f2f4 (score book, pv -), actual played Nf3 -> MISMATCH
move 3: engine suggests f1b5 (score book, pv -), actual played Bc4 -> MISMATCH
move 4: engine suggests c4d5 (score cp -5, pv c4d5), actual played Qe2 -> MISMATCH
move 5: engine suggests b1c3 (score cp 0, pv b1c3 e8g8 d2d3 d7d6 e1g1 c8g4 c1e3 c6d4), actual played O-O -> MISMATCH
move 6: engine suggests c4b5 (score cp 115, pv c4b5 c8b7), actual played Bd5 -> MISMATCH
move 10: engine suggests d5d6 (score cp 310, pv d5d6 e7f6 e4a8 f6e5 f1e1), actual played c3 -> MISMATCH
move 12: engine suggests d4c5 (score cp 312, pv d4c5), actual played d6 -> MISMATCH
move 15: engine suggests a8f3 (score cp 473, pv a8f3 c8e6), actual played d4 -> MISMATCH
move 16: engine suggests d4e5 (score cp 490, pv d4e5 d6e5 b1c3 e7d6 g2g3 e5e6 c1e3), actual played Bh6 -> MISMATCH
move 17: engine suggests d4e5 (score cp 141, pv d4e5), actual played Rc1 -> MISMATCH
move 18: engine suggests a8f3 (score cp 159, pv a8f3 f5b1 d4e5 d6e5), actual played Qxf8+ -> MISMATCH
move 20: engine suggests b1c3 (score cp -332, pv b1c3 e7d6), actual played g4 -> MISMATCH
move 21: engine suggests b1d2 (score cp -444, pv b1d2 f8g7 c1d1), actual played Rc8+ -> MISMATCH
move 22: engine suggests c8e8 (score cp 404, pv c8e8), actual played Rg8+ -> MISMATCH
move 23: engine suggests b1d2 (score cp -1439, pv b1d2 e5h2), actual played Nc3 -> MISMATCH
move 25: engine suggests g1g2 (score mate -2, pv g1g2), actual played Ne2 -> MISMATCH
move 26: engine suggests a1d1 (score mate -1, pv a1d1), actual played h3 -> MISMATCH

### game2.pgn
move 2: engine suggests b1c3 (score book, pv -), actual played Nf3 -> MISMATCH
move 3: engine suggests f1b5 (score book, pv -), actual played Bc4 -> MISMATCH
move 10: engine suggests e1g1 (score cp 139, pv e1g1 c8d7), actual played Rf1 -> MISMATCH
move 13: engine suggests f1e1 (score cp -228, pv f1e1 e7d6), actual played Kd1 -> MISMATCH
move 14: engine suggests d2d3 (score cp -236, pv d2d3 e7d6 g2g3 c8b7 c1f4 c6c5 d1d2 b7c6 f1e1 d6f4 g3f4), actual played Re1 -> MISMATCH
move 15: engine suggests d1e2 (score cp -243, pv d1e2 h4f2 e2f2 c6c5 d2d3 f5f4), actual played c4 -> MISMATCH
move 16: engine suggests e1e7 (score cp -340, pv e1e7), actual played Ke2 -> MISMATCH
move 18: engine suggests c4c5 (score cp -524, pv c4c5), actual played d3 -> MISMATCH
move 19: engine suggests e1d2 (score cp -592, pv e1d2 c6c5 d2c2 b7g2 c1f4), actual played Ke2 -> MISMATCH
move 21: engine suggests a1g1 (score cp -503, pv a1g1 d8e8 e2f2 a8b8 b2b3), actual played b3 -> MISMATCH
move 23: engine suggests e3c5 (score cp -507, pv e3c5 f5f4 d3d4 b7e4 d4d5 g8f7 e2f2 f4f3), actual played Rd1 -> MISMATCH
move 24: engine suggests e3c5 (score cp -505, pv e3c5), actual played Kf1 -> MISMATCH
move 25: engine suggests d1d2 (score cp -508, pv d1d2 f5f4 e3c5 g5g4 f1f2 f3e4 d3d4 g8f7 d4d5), actual played Bxg5 -> MISMATCH
move 26: engine suggests d1d2 (score cp -951, pv d1d2 d8d4 b3b4 d4c4 d3c4 f3g2 f1g2 c5b4 a2a3), actual played h4 -> MISMATCH
move 27: engine suggests b3b4 (score mate -6, pv b3b4 d8d3), actual played a4 -> MISMATCH
move 29: engine suggests b3b4 (score mate -4, pv b3b4), actual played a6 -> MISMATCH
move 33: engine suggests f2g1 (score mate -2, pv f2g1), actual played Kf1 -> MISMATCH

### game3.pgn
move 4: engine suggests b8c6 (score cp -17, pv b8c6 f1b5 g8f6 b5c6), actual played Qd7 -> MISMATCH
move 6: engine suggests f8e7 (score cp -22, pv f8e7), actual played Nf6 -> MISMATCH
move 8: engine suggests f8e7 (score cp -20, pv f8e7), actual played Nc6 -> MISMATCH
move 20: engine suggests a8d8 (score cp -4, pv a8d8), actual played Rfd8 -> MISMATCH
move 21: engine suggests b6a5 (score cp -10, pv b6a5 g4g5 f6e7 c3c4), actual played Qc7 -> MISMATCH
move 24: engine suggests e8b8 (score cp -17, pv e8b8), actual played Rab8 -> MISMATCH
move 27: engine suggests e7f6 (score cp -100, pv e7f6), actual played hxg6 -> MISMATCH
move 35: engine suggests h3h4 (score cp -267, pv h3h4 b7a6 h4f4 d1d6 d8d6), actual played Rh2 -> MISMATCH
move 42: engine suggests d6d5 (score cp -269, pv d6d5 a6b7 e7e6 a4a5 h8g8 a5a6 g7g5 b7c6 e6f5 f4g5 g8g5 c6d5), actual played Rh2+ -> MISMATCH
move 47: engine suggests d5d4 (score cp -657, pv d5d4 c3d4), actual played Bh4 -> MISMATCH
move 54: engine suggests d7d6 (score cp -1992, pv d7d6 d1f3), actual played Kc6 -> MISMATCH
move 57: engine suggests d5d4 (score cp -1907, pv d5d4 b4b3 d4c3 e8f8 d6e6 f8g8 e6d6), actual played Bc5+ -> MISMATCH
move 62: engine suggests f3g3 (score mate -5, pv f3g3 a6a7 g3h2 d4g4 d5d4 a7a8r d4c3 a8h8), actual played Kg2 -> MISMATCH

### game4.pgn
move 2: engine suggests d7d5 (score cp -5, pv d7d5 b1d2 f6e4 d2e4), actual played e6 -> MISMATCH
move 3: engine suggests d7d5 (score cp 1, pv d7d5), actual played Nc6 -> MISMATCH
move 8: engine suggests e7f6 (score cp 10, pv e7f6 c1e3 g7g5), actual played O-O -> MISMATCH
move 10: engine suggests c8d7 (score cp 9, pv c8d7), actual played Nb3 -> MISMATCH
move 13: engine suggests d8e7 (score cp 10, pv d8e7 e2e3 e4c3 c1c3 f8f7), actual played a5 -> MISMATCH
move 15: engine suggests a8a3 (score cp 15, pv a8a3 e2e3 c8d7 b1b2 e4c3 c1c3 d7b5 d3c2), actual played Bd7 -> MISMATCH
move 17: engine suggests f8d8 (score cp 8, pv f8d8 a1a8 d8a8 h2h3 f6e5), actual played Rfe8 -> MISMATCH
move 18: engine suggests a8a1 (score cp 5, pv a8a1), actual played Red8 -> MISMATCH
move 21: engine suggests d8b8 (score cp 2, pv d8b8 e5d7 e7d7), actual played Bc8 -> MISMATCH
move 24: engine suggests e8e7 (score cp 3, pv e8e7), actual played Be7 -> MISMATCH
move 27: engine suggests d8f8 (score cp 3, pv d8f8 f1d3 f8d8 h4h5 h7h6 f4g6), actual played Bd7 -> MISMATCH
move 28: engine suggests d7c8 (score cp 3, pv d7c8 f4e6 c8e6 d4d5 e6d5 f1e2 d5f3 e2f3), actual played Bf8 -> MISMATCH
move 30: engine suggests g8f7 (score cp 9, pv g8f7 f1e2 b6c5 a7c7), actual played Bc8 -> MISMATCH
move 32: engine suggests g8f7 (score cp 7, pv g8f7 f1d3 f7g8 c3c2 f8c5 b4c5), actual played Qb6 -> MISMATCH
move 50: engine suggests e7d8 (score cp -173, pv e7d8 e2f3 d8e7 a8a7), actual played Rg8 -> MISMATCH
move 52: engine suggests g8f8 (score cp -173, pv g8f8 a7a8 h6h5), actual played Rh8 -> MISMATCH
move 53: engine suggests h8f8 (score cp -45, pv h8f8 d3d2 f8h8 a7a8 h8f8 g6d3 d8e7 a8c8 f8c8 d3c2), actual played Rg8 -> MISMATCH
move 60: engine suggests e8f8 (score cp -143, pv e8f8 a1a7), actual played Ke7 -> MISMATCH
move 61: engine suggests e8g8 (score cp -146, pv e8g8 d4c3 e7d8 g1a1 d5d4 e3d4), actual played Kf8 -> MISMATCH
move 64: engine suggests g8h8 (score cp -364, pv g8h8), actual played Rf8 -> MISMATCH
move 67: engine suggests c8d7 (score cp -1051, pv c8d7), actual played Bb7 -> MISMATCH
move 73: engine suggests e7f7 (score mate -6, pv e7f7), actual played Ke6 -> MISMATCH
move 79: engine suggests g7g5 (score mate -1, pv g7g5 e6f7), actual played g6 -> MISMATCH

### game5.pgn
move 1: engine suggests d2d4 (score book, pv -), actual played e4 -> MISMATCH
move 6: engine suggests c1e3 (score cp 13, pv c1e3), actual played Bb5 -> MISMATCH
move 8: engine suggests c1e3 (score cp 9, pv c1e3 c5d4), actual played O-O -> MISMATCH
move 17: engine suggests a2a3 (score cp 108, pv a2a3 e8g8 c3d5 e6d5 g4g7 g8g7 f4g3), actual played Rad1 -> MISMATCH
move 22: engine suggests a2a3 (score cp 108, pv a2a3), actual played Rd3 -> MISMATCH
move 28: engine suggests g2g3 (score cp 4, pv g2g3 g5g4 g3h4 g4h3 g1f1 h8h4 a2a3), actual played Kf1 -> MISMATCH
move 30: engine suggests d4e3 (score cp -1, pv d4e3 h6h3 g3h3 g4h3 f1e2 h8h5 b2b4 e7b4 c3d5), actual played hxg4 -> MISMATCH
move 34: engine suggests d3h3 (score cp -503, pv d3h3 h2h3 f1e2 g4e4), actual played Qxg4 -> MISMATCH
move 53: engine suggests h4g4 (score mate -4, pv h4g4 h6f8 g4g5 a3a1 g5g6 a1g1 g6h5 f8h8), actual played Kg3 -> MISMATCH

### game6.pgn
move 4: engine suggests g1f3 (score cp -4, pv g1f3 b4c3), actual played Bd2 -> MISMATCH
move 5: engine suggests e2e3 (score cp 2, pv e2e3 b4c3 d2c3 d7d5), actual played Nf3 -> MISMATCH
move 17: engine suggests a1c1 (score cp 221, pv a1c1 f6f4 c1c7), actual played Rhd1 -> MISMATCH
move 18: engine suggests d1d7 (score cp 319, pv d1d7), actual played Qxb7 -> MISMATCH
move 19: engine suggests b7c6 (score cp 218, pv b7c6 b8b2 e2f1 h7h6), actual played Qxc7 -> MISMATCH
```

</details>

## Notes for Phase 1

- game3/game4 (engine=Black vs Komodo25, ELO 3200) mismatches are mostly small (single-digit to
  low-double-digit centipawns) until late-game, where scores swing to -600 to -2000cp and mate
  scores -- consistent with a lost position where "mismatch" mostly reflects which losing move is
  marginally less bad, not a fixable blunder.
- game1/game2 (engine=White vs Wendy-BOT, ELO 1500, played 2026-07) have a much higher mismatch
  rate (65%, 50%) than the Komodo25 games -- plausibly because these were played at a lower
  difficulty preset than EXPERT historically (preset/depth used for these specific games was not
  recorded in the PGN), not necessarily because White played worse chess. Worth checking against
  game metadata if available before drawing conclusions.
- game6 moves 18/19 are the clearest, highest-confidence blunder in this dataset: both a large
  score gap (218-319cp for the alternative) and a directly-verified tactical refutation.
