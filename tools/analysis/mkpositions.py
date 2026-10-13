import chess,chess.engine,chess.pgn,csv,sys,math
from multiprocessing import Pool
S=sys.argv[1]; REPO=sys.argv[2]; OUT=sys.argv[3]
SF=S+"/sf/stockfish/stockfish-ubuntu-x86-64-avx2"
def clamp(v): return max(-1000,min(1000,v))
def grade(args):
    src,game,move,fen,played,note=args
    sf=chess.engine.SimpleEngine.popen_uci(SF); sf.configure({"Threads":1,"Hash":128})
    b=chess.Board(fen); L=chess.engine.Limit(depth=18)
    a=sf.analyse(b,L); best=clamp(a["score"].pov(b.turn).score(mate_score=1000))
    m=chess.Move.from_uci(played)
    mv=clamp(sf.analyse(b,L,root_moves=[m])["score"].pov(b.turn).score(mate_score=1000))
    sf.quit(); return [src,game,move,fen,played,a["pv"][0].uci(),best,max(0,best-mv),note]
rows=[]
# 1. games vs Komodo (engine = Marek7x7): engine-to-move positions from move 13
for g in (3,4,5,6):
    game=chess.pgn.read_game(open("%s/game%d.pgn"%(REPO,g))); b=game.board()
    white=game.headers["White"]=="Marek7x7"
    for m in game.mainline_moves():
        if b.turn==white and b.fullmove_number>=13: rows.append(("komodo","game%d"%g,b.fullmove_number,b.fen(),m.uci(),""))
        b.push(m)
# 2. the Lichess game (engine = White): White-to-move positions from move 7
game=chess.pgn.read_game(open(REPO+"/game7.pgn")); b=game.board()
for m in game.mainline_moves():
    if b.turn and b.fullmove_number>=7: rows.append(("lichess_Blummo9v","game7",b.fullmove_number,b.fen(),m.uci(),""))
    b.push(m)
with Pool(22) as p: res=p.map(grade,rows)
# keep positions that are not already decided
res=[r for r in res if abs(r[6])<=500]
# 3. self-play blunders (>=150 cp) from the 200 HARD-vs-HARD games
import pandas as pd
d=pd.read_csv(S+"/graded200.csv"); d=d[d.loss>=150]
for _,r in d.iterrows(): res.append(["selfplay_blunder",r.game,int(r.ply),r.fen,r.uci,"",int(r.before),int(r.loss),"SF depth14 loss"])
w=csv.writer(open(OUT,"w",newline="")); w.writerow("source game move fen played sf_best sf_eval played_loss note".split()); w.writerows(res)
import collections; print(collections.Counter(r[0] for r in res))
