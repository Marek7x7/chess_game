# Cross-checks the engine's tablebase root probes against python-chess on random 3-5 piece positions:
#   python3 tools/syzygy_check.py build-release/chess <syzygy-dir> 300   (needs python-chess and the 3-4-5 piece WDL+DTZ tables)
# For each position the engine's win/draw/loss (from "info score") must equal the table's and its move must keep it.
import chess, chess.syzygy, random, subprocess, sys, time, threading, queue, re, os
exe, tbdir, n = sys.argv[1], sys.argv[2], int(sys.argv[3])
random.seed(7)
tb = chess.syzygy.open_tablebase(tbdir)
SETS = ['KRK','KQK','KPK','KBNK','KRPKR','KQKR','KQPKQ','KBPKN','KNNKP','KRKB','KRKN','KQKP','KRBKR','KBBKN','KPPKP','KRNKR','KQRKQ','KBKP','KNPKB','KRPKB']
def rnd_board(spec):
    w, b = spec.split('K')[1:3] if spec.count('K')==2 else (None,None)
    parts = spec.split('K')   # '', white, black
    white, black = parts[1], parts[2]
    while True:
        bd = chess.Board(None)
        sqs = random.sample(range(64), 2 + len(white) + len(black))
        bd.set_piece_at(sqs[0], chess.Piece(chess.KING, chess.WHITE)); bd.set_piece_at(sqs[1], chess.Piece(chess.KING, chess.BLACK))
        i = 2
        for ch in white: bd.set_piece_at(sqs[i], chess.Piece(chess.PIECE_SYMBOLS.index(ch.lower()), chess.WHITE)); i += 1
        for ch in black: bd.set_piece_at(sqs[i], chess.Piece(chess.PIECE_SYMBOLS.index(ch.lower()), chess.BLACK)); i += 1
        bd.turn = random.random() < 0.5
        if any(p.piece_type == chess.PAWN and chess.square_rank(s) in (0, 7) for s, p in bd.piece_map().items()): continue
        if not bd.is_valid() or bd.is_game_over(): continue
        return bd
p = subprocess.Popen([exe, '--uci'], stdin=subprocess.PIPE, stdout=subprocess.PIPE, text=True, bufsize=1)
q = queue.Queue(); threading.Thread(target=lambda: [q.put(l.strip()) for l in p.stdout], daemon=True).start()
def send(c): p.stdin.write(c + '\n'); p.stdin.flush()
def until(prefix, timeout=20):
    out = []; end = time.time() + timeout
    while time.time() < end:
        try: l = q.get(timeout=0.1)
        except queue.Empty: continue
        out.append(l)
        if l.startswith(prefix): return out
    return out
send('uci'); until('uciok'); send('setoption name Threads value 1'); send('setoption name SyzygyPath value ' + tbdir)
print([l for l in until('info string Syzygy', 5)][-1])
bad = 0; checked = 0
def cat(w): return 'win' if w == 2 else 'loss' if w == -2 else 'draw'
for k in range(n):
    bd = rnd_board(random.choice(SETS))
    try: w = tb.probe_wdl(bd)
    except (KeyError, chess.syzygy.MissingTableError): continue
    send('position fen ' + bd.fen()); send('go depth 1'); out = until('bestmove')
    info = [l for l in out if l.startswith('info depth')]
    mv = out[-1].split()[1]
    m = re.search(r'score cp (-?\d+)', info[-1]) if info else None
    if not m: print('NO INFO', bd.fen(), out[-2:]); bad += 1; continue
    sc = int(m.group(1)); ecat = 'win' if sc > 0 else 'loss' if sc < 0 else 'draw'
    ok = ecat == cat(w)
    move = chess.Move.from_uci(mv)
    if ok and move in bd.legal_moves:
        bd.push(move)
        try: w2 = -tb.probe_wdl(bd)    # from the original mover's side
        except Exception: w2 = None
        # the played move must keep the result (a cursed win/blessed loss counts as the drawn category)
        if w2 is not None and cat(w2) != cat(w) : ok = False; print('MOVE LOSES THE RESULT', mv)
    elif ok: ok = False; print('ILLEGAL', mv)
    checked += 1
    if not ok: bad += 1; print('MISMATCH', chess.Board(bd.fen()).fen(), 'tb', w, 'engine', sc, mv)
send('quit'); p.wait(timeout=5)
print('checked', checked, 'mismatches', bad)
sys.exit(1 if bad else 0)
