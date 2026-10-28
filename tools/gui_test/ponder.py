"""Pondering in the GUI, headless. usage: ponder.py <chess binary> hit|miss|undo|newgame|quit
Plays off-book pawn moves against EXPERT until the bot pondering starts, then ends the scenario as named and
checks the log lines ([ponder] start / hit / miss) and that the GUI exits cleanly."""
import os, subprocess, sys, time, re
S = os.path.dirname(os.path.abspath(__file__))  # tools/gui_test: shim.so is built here (see README.md)
EXE = sys.argv[1]
work = '/tmp/gui_ponder_run'; os.makedirs(work, exist_ok=True)
for f in os.listdir(work):
    fp = os.path.join(work, f)
    if os.path.isfile(fp): os.unlink(fp)
cmdf = work + '/cmd'
env = dict(os.environ, SDL_VIDEODRIVER='offscreen', SDL_RENDER_DRIVER='software', LD_PRELOAD=S + '/shim.so', SHIM_CMD=cmdf)
log = work + '/out.log'
p = subprocess.Popen([EXE], cwd=work, env=env, stdout=open(log, 'w'), stderr=subprocess.STDOUT)
def send(*lines, wait=0.2):
    with open(cmdf + '.tmp', 'w') as f: f.write('\n'.join(lines) + '\n')
    os.rename(cmdf + '.tmp', cmdf)
    for _ in range(300):
        if not os.path.exists(cmdf): break
        time.sleep(0.02)
    time.sleep(wait)
def sq(f, r): return (f * 80 + 40, (7 - r) * 80 + 40)
def uci_sq(s): return (ord(s[0]) - 97, int(s[1]) - 1)
def move(a, b):
    send('click %d %d' % sq(*a)); send('click %d %d' % sq(*b))
def lines(): return open(log).read().splitlines()
def wait_for(pred, timeout):
    t0 = time.time()
    while time.time() - t0 < timeout:
        r = pred(lines())
        if r: return r
        assert p.poll() is None, 'GUI died'
        time.sleep(0.2)
    raise SystemExit('timeout waiting')
def starts(): return [l for l in lines() if l.startswith('[ponder] start')]
SCEN = sys.argv[2]
time.sleep(1.0)
send('click 320 325'); send('click 320 245'); send('click 320 365')   # VS COMPUTER, WHITE, EXPERT
pushes = [(0,1,0,2),(7,1,7,2),(1,1,1,2),(6,1,6,2),(2,1,2,2),(5,1,5,2),(3,1,3,2)]
used = 0
def starts(): return [l for l in lines() if l.startswith('[ponder] start')]
def endings(): return [l for l in lines() if l.startswith('[ponder] hit') or l.startswith('[ponder] miss')]
for fr in pushes:                                                        # off-book pawn pushes until the table has a reply
    move((fr[0],fr[1]),(fr[2],fr[3])); used += 1
    time.sleep(8)
    if starts(): break
assert starts(), 'no ponder start: ' + str(lines())
exp = starts()[-1].split()[-1]; print(SCEN, 'expecting', exp)
time.sleep(6)
if SCEN == 'hit':
    move(uci_sq(exp[:2]), uci_sq(exp[2:4])); time.sleep(8)
    assert any('hit' in l for l in endings()), lines()
elif SCEN == 'miss':
    fr = next(f for f in pushes[used:] if (chr(97 + f[0]) + str(f[1] + 1) + chr(97 + f[2]) + str(f[3] + 1)) != exp)
    move((fr[0],fr[1]),(fr[2],fr[3])); time.sleep(3)                      # then quit while the bot is searching
    assert any('miss' in l for l in endings()), lines()
elif SCEN == 'undo':
    send('key u'); time.sleep(1)
elif SCEN == 'newgame':
    send('click 320 %d' % (640 + 55 + 18)); time.sleep(1)
# SCEN == 'quit': quit while pondering
send('quit')
t0 = time.time()
rc = p.wait(timeout=30); print('exit', rc, 'quit took %.1fs' % (time.time() - t0))
print('\n'.join(l for l in lines() if l.startswith('[ponder]')))
