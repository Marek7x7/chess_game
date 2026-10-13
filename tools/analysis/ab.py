# ab.py positions.csv out.csv MODE VALUE engine1.sh engine2.sh ...   (MODE: depth|movetime)
import chess,chess.engine,pandas as pd,sys,json,os
from multiprocessing import Pool
S=os.environ["S"]; CSV,OUT,MODE,VAL=sys.argv[1:5]; ENGS=sys.argv[5:]; VAL=int(VAL)
SF=S+"/sf/stockfish/stockfish-ubuntu-x86-64-avx2"; CACHE=S+"/sfcache.json"; GDEPTH=16
cache=json.load(open(CACHE)) if os.path.exists(CACHE) else {}
def clamp(v): return max(-1000,min(1000,v))
def work(rows):
    engs=[chess.engine.SimpleEngine.popen_uci(e) for e in ENGS]
    for e in engs: e.configure({"Threads":1,"Hash":64})
    out=[]
    for r in rows:
        b=chess.Board(r["fen"]); mv=[]
        for e in engs:
            lim=chess.engine.Limit(depth=VAL) if MODE=="depth" else chess.engine.Limit(time=VAL/1000)
            mv.append(e.play(b,lim,game=object()).move.uci())
        out.append(dict(r,**{"m%d"%i:m for i,m in enumerate(mv)}))
    for e in engs: e.quit()
    return out
def grade(items):
    sf=chess.engine.SimpleEngine.popen_uci(SF); sf.configure({"Threads":1,"Hash":128}); res={}
    L=chess.engine.Limit(depth=GDEPTH)
    for fen,moves in items:
        b=chess.Board(fen)
        res[fen+"|best"]=clamp(sf.analyse(b,L)["score"].pov(b.turn).score(mate_score=1000))
        for m in moves:
            res[fen+"|"+m]=clamp(sf.analyse(b,L,root_moves=[chess.Move.from_uci(m)])["score"].pov(b.turn).score(mate_score=1000))
    sf.quit(); return res
if __name__=="__main__":
    d=pd.read_csv(CSV).to_dict("records")
    with Pool(22) as p: rows=[x for c in p.map(work,[d[i::22] for i in range(22)]) for x in c]
    need={}
    for r in rows:
        for i in range(len(ENGS)):
            m=r["m%d"%i]
            if r["fen"]+"|"+m not in cache or r["fen"]+"|best" not in cache: need.setdefault(r["fen"],set()).add(m)
    items=list(need.items())
    if items:
        with Pool(22) as p: parts=p.map(grade,[items[i::22] for i in range(22)])
        for part in parts: cache.update(part)
        json.dump(cache,open(CACHE,"w"))
    o=pd.DataFrame(rows)
    for i in range(len(ENGS)):
        o["loss%d"%i]=[max(0,cache[r["fen"]+"|best"]-cache[r["fen"]+"|"+r["m%d"%i]]) for r in rows]
    o.to_csv(OUT,index=False)
    print("builds:",[os.path.basename(e) for e in ENGS],MODE,VAL)
    for src,g in list(o.groupby("source"))+[("ALL",o)]:
        line="%-18s n=%4d"%(src,len(g))
        for i in range(len(ENGS)): line+=" | avg %5.1f big(>=80) %3d"%(g["loss%d"%i].mean(),(g["loss%d"%i]>=80).sum())
        if len(ENGS)>1:
            ch=g[g.m0!=g.m1]; d1=ch.loss1-ch.loss0
            line+=" | changed %3d better %3d worse %3d"%(len(ch),(d1<-20).sum(),(d1>20).sum())
        print(line)
