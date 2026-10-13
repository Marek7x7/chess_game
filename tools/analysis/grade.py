import chess, chess.engine, chess.pgn, glob, sys, csv
from multiprocessing import Pool
SF="%s/sf/stockfish/stockfish-ubuntu-x86-64-avx2"%sys.argv[1]
def grade(path):
    eng=chess.engine.SimpleEngine.popen_uci(SF); eng.configure({"Threads":1,"Hash":128})
    g=chess.pgn.read_game(open(path)); b=g.board(); rows=[]
    def ev(bd):
        if bd.is_checkmate(): return -10000
        if bd.is_game_over(): return 0
        s=eng.analyse(bd,chess.engine.Limit(depth=14))["score"].pov(bd.turn)
        return max(-1000,min(1000,s.score(mate_score=1000)))
    prev=ev(b); ply=0
    for m in g.mainline_moves():
        mover=b.turn; san=b.san(m); fen=b.fen()
        cap=b.is_capture(m); pt=chess.piece_name(b.piece_type_at(m.from_square))
        b.push(m); ply+=1
        cur=ev(b); after=-cur
        loss=max(0,prev-after)
        npieces=len(b.piece_map())
        rows.append([path.split("botmatch_")[-1].replace(".pgn",""),ply,"W" if mover else "B",san,fen,m.uci(),pt,int(cap),int(b.is_check()),npieces,prev,after,loss])
        prev=cur
    eng.quit(); return rows
if __name__=="__main__":
    files=sorted(glob.glob(sys.argv[2]+"/*.pgn"))
    with Pool(20) as p: res=p.map(grade,files,chunksize=1)
    w=csv.writer(open(sys.argv[3],"w")); w.writerow("game ply side san fen uci piece cap check npieces before after loss".split())
    for r in res: w.writerows(r)
