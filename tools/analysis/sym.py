import chess,chess.engine,pandas as pd,sys
from multiprocessing import Pool
ENG=sys.argv[1]; S=sys.argv[2]
def work(fens):
    e=chess.engine.SimpleEngine.popen_uci(ENG); bad=0
    for f in fens:
        b=chess.Board(f)
        if b.is_game_over(): continue
        a=e.analyse(b,chess.engine.Limit(depth=1))
        if "score" not in a: continue
        s1=a["score"].relative.score(mate_score=100000)
        e.protocol  # fresh TT between
        e.analyse(chess.Board(),chess.engine.Limit(depth=1))
        a=e.analyse(b.mirror(),chess.engine.Limit(depth=1))
        if "score" not in a: continue
        s2=a["score"].relative.score(mate_score=100000)
        if s1!=s2: bad+=1; print("ASYM",f,s1,s2)
    e.quit(); return bad
if __name__=="__main__":
    d=pd.read_csv(S+"/graded.csv"); fens=list(d.fen)[::2]
    with Pool(20) as p: print("asymmetric:",sum(p.map(work,[fens[i::20] for i in range(20)])),"of",len(fens))
