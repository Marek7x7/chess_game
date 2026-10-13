import chess,chess.engine,pandas as pd,sys,csv
from multiprocessing import Pool
S=sys.argv[1]; SF=S+"/sf/stockfish/stockfish-ubuntu-x86-64-avx2"; DEPTH=int(sys.argv[4])
def work(rows):
    sf=chess.engine.SimpleEngine.popen_uci(SF); sf.configure({"Threads":1,"Hash":64})
    out=[]
    for game,fen in rows:
        b=chess.Board(fen)
        if b.is_check() or b.is_game_over(): continue
        info=sf.analyse(b,chess.engine.Limit(depth=DEPTH))
        pv=info.get("pv")
        if not pv: continue
        m=pv[0]
        if b.is_capture(m) or m.promotion: continue
        s=info["score"].white()
        if s.is_mate(): continue
        cp=s.score()
        if abs(cp)>=900: continue
        out.append((game,fen,cp))
    sf.quit(); return out
if __name__=="__main__":
    d=pd.read_csv(sys.argv[2]); rows=list(zip(d.game,d.fen))
    with Pool(22) as p: res=[x for c in p.map(work,[rows[i::22] for i in range(22)]) for x in c]
    w=csv.writer(open(sys.argv[3],"w")); w.writerow(["game","fen","sf_cp_white"]); w.writerows(res)
    print(len(res),"quiet labelled positions of",len(rows))
