import os, subprocess, sys, time, glob
SC = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.abspath(os.path.join(SC, '..', '..'))
work = os.path.join('/tmp', 'gui_test_run'); os.makedirs(work, exist_ok=True)
for f in glob.glob(work + '/*'):
    if os.path.isfile(f): os.unlink(f)
cmdf = os.path.join(work, 'cmd')
env = dict(os.environ, SDL_VIDEODRIVER='offscreen', SDL_RENDER_DRIVER='software', LD_PRELOAD=SC + '/shim.so', SHIM_CMD=cmdf)
p = subprocess.Popen([REPO + '/build-release/chess'], cwd=work, env=env, stdout=open(work + '/out.log', 'w'), stderr=subprocess.STDOUT)
def send(*lines, wait=0.15):
    with open(cmdf + '.tmp', 'w') as f: f.write('\n'.join(lines) + '\n')
    os.rename(cmdf + '.tmp', cmdf)
    for _ in range(200):
        if not os.path.exists(cmdf): break
        time.sleep(0.02)
    time.sleep(wait)
def sq(f, r): return (f * 80 + 40, (7 - r) * 80 + 40)
def move(a, b):
    send('click %d %d' % sq(*a)); send('click %d %d' % sq(*b))
def shot(name): send('shot %s/%s.bmp' % (work, name), wait=0.3)
time.sleep(1.0)
send('click 320 245')                      # TWO PLAYERS
shot('01_start')
# Nf3 Nf6 Ng1 Ng8 twice: the start position occurs a third time
cycle = [((6,0),(5,2)), ((6,7),(5,5)), ((5,2),(6,0)), ((5,5),(6,7))]
for _ in range(2):
    for a, b in cycle: move(a, b)
shot('02_threefold')
send('key u'); shot('03_after_undo')
send('key f'); shot('04_flipped'); send('key f')
send('key p'); shot('05_saved')
send('key c')
send('key v'); shot('06_pasted')
send('quit'); p.wait(timeout=10)
print('exit', p.returncode)
print(open(work + '/out.log').read()[:500])
print(glob.glob(work + '/games/*'))
