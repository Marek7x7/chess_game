import chess,chess.pgn,sys,csv,random
random.seed(1)
src,dst=sys.argv[1],sys.argv[2]
rows=[]; n=0
f=open(src)
while True:
    g=chess.pgn.read_game(f)
    if g is None: break
    n+=1; b=g.board(); ply=0
    moves=list(g.mainline_moves())
    for i,m in enumerate(moves):
        b.push(m); ply+=1
        if ply<16 or i>=len(moves)-3: continue   # skip the book moves and the last plies
        if ply%2: continue                       # one position per move pair
        if b.is_check(): continue
        rows.append((sys.argv[3]+"%d"%n,b.fen()))
with open(dst,"w",newline="") as o:
    w=csv.writer(o); w.writerow(["game","fen"]); w.writerows(rows)
print(n,"games",len(rows),"positions")
