# Records what an install script draws in a terminal, for the replay on the install page
# (src/terminal.mjs). It runs this checkout's tools/install script, the one calxlang.com serves,
# with colour forced on, installing the newest nightly into a folder of its own in the system's
# temp folder, and deletes that folder afterwards. Run it on the system the script is for:
#
#     py -I site/src/terminal/record.py site/src/terminal/install-windows.json       (Windows)
#     python3 -I site/src/terminal/record.py site/src/terminal/install-linux.json    (Linux)
#
# What's kept is what the script wrote, standard output and error together, as frames: each the
# text that came at one time, its escapes as they are. The folder it installed into reads as the
# usual ~/.calx, under a home folder called "you". A status line or bar that redraws itself for
# more than RUN seconds plays faster, by a whole number, so its count of seconds still says how
# long it took, and a wait with nothing drawn is at most GAP seconds.
import base64
import codecs
import datetime
import json
import math
import os
import shutil
import subprocess
import sys
import tempfile
import time

RUN = 2.5
GAP = 1.0
# Pieces closer together than this are one frame.
JOIN = 0.03
REDRAW = '\r\x1b[2K'

out = sys.argv[1]
repo = os.path.abspath(os.path.join(os.path.dirname(__file__), '..', '..', '..'))
windows = os.name == 'nt'

work = tempfile.mkdtemp(prefix='calx-record-')
home = os.path.join(work, '.calx')
env = dict(os.environ, CALX_INSTALL=home, CALX_CACHE=os.path.join(work, 'cache'), CALX_CHANNEL='nightly',
           CALX_NO_MODIFY_PATH='1', CLICOLOR_FORCE='1', COLORTERM='truecolor')
for name in ('NO_COLOR', 'CI', 'CALX_CC'):
    env.pop(name, None)
if windows:
    title = 'irm https://calxlang.com/install.ps1 | iex'
    prompt = 'PS C:\\Users\\you> '
    shown = 'C:\\Users\\you\\.calx'
    cmd = ['powershell.exe', '-NoProfile', '-ExecutionPolicy', 'Bypass', '-File',
           os.path.join(repo, 'tools', 'install', 'install.ps1')]
else:
    title = 'curl -fsSL https://calxlang.com/install.sh | sh'
    prompt = '$ '
    shown = '/home/you/.calx'
    env.update(LANG='C.UTF-8', TERM='xterm-256color', XDG_CACHE_HOME=os.path.join(work, 'xdg'))
    cmd = ['sh', os.path.join(repo, 'tools', 'install', 'install.sh')]

pieces = []
try:
    start = time.monotonic()
    p = subprocess.Popen(cmd, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, stdin=subprocess.DEVNULL, env=env, cwd=work)
    while True:
        b = os.read(p.stdout.fileno(), 65536)
        if not b:
            break
        pieces.append((time.monotonic() - start, b))
    status = p.wait()
finally:
    # Only the folder made above, which mkdtemp named, in the temp folder.
    ours = os.path.basename(work).startswith('calx-record-') and \
        os.path.dirname(os.path.realpath(work)) == os.path.realpath(tempfile.gettempdir())
    if ours:
        shutil.rmtree(work, ignore_errors=True)
    if os.path.exists(work):
        print(f'{work} is still there; delete it by hand', file=sys.stderr)
if status != 0:
    sys.exit(f'the install script failed, with status {status}')

# The text, with the temp folder as ~/.calx and the home folder as "you"'s.
names = [(home, shown), (work, os.path.dirname(shown)), (os.path.expanduser('~'), os.path.dirname(shown))]
decoder = codecs.getincrementaldecoder('utf-8')('replace')
frames = []
for t, b in pieces:
    text = decoder.decode(b)
    for a, z in names:
        text = text.replace(a, z)
    if frames and t - frames[-1][0] < JOIN:
        frames[-1][1] += text
    else:
        frames.append([t, text])


def redraw(text):
    # A frame that only draws one line again over itself.
    return text.startswith(REDRAW) and '\n' not in text and len(text) > len(REDRAW)


kept = []
shift = 0.0
i = 0
while i < len(frames):
    j = i
    while j + 1 < len(frames) and redraw(frames[i][1]) and redraw(frames[j + 1][1]):
        j += 1
    span = frames[j][0] - frames[i][0]
    k = math.ceil(span / RUN) if span > RUN else 1
    t0 = frames[i][0]
    for n in range(i, j + 1):
        if (n - i) % k == 0 or n == j:
            kept.append([t0 - shift + (frames[n][0] - t0) / k, frames[n][1]])
    shift += span - span / k
    i = j + 1

timed = []
clock = 0.0
for n, (t, text) in enumerate(kept):
    if n:
        clock += min(t - kept[n - 1][0], GAP)
    timed.append([round(clock * 1000), text])

with open(out, 'w', encoding='utf-8', newline='\n') as f:
    json.dump({
        'title': title,
        'prompt': prompt,
        'recorded': datetime.date.today().isoformat(),
        'took': round(pieces[-1][0], 1) if pieces else 0,
        'frames': timed,
    }, f, ensure_ascii=True, separators=(',', ':'))
    f.write('\n')
print(f'{out}: {len(timed)} frames, {timed[-1][0] / 1000:.1f} s played, {pieces[-1][0]:.1f} s recorded')
