#!/usr/bin/env python3
"""Pixel drift moves the finished picture one pixel right and/or down, so on every screen it applies to
(flight layouts A-C, weather layouts A-C, the idle clock, the problem message) column 63 and row 31 must stay black,
whatever the data. This draws those screens with worst-case data and exits 1 if any picture lights column 63 or row 31.
The two firmware-update screens are never drifted, so they may use both: for them this only checks that the board and
renderer.js draw the same pixels for the example states and a few hundred made-up ones.

  1. shared/renderer.js (preview page and portal thumbnails) in headless Chrome.
  2. arduino/ledmatrix/screens.cpp (the real board code and the real Adafruit GFX library, built for the
     laptop against a fake panel, see tools/check_margins.d/). It also has to draw exactly the same pixels as
     renderer.js for the example screens. Skipped, with a note, when there is no C++ compiler or no GFX library.

Usage: python3 tools/check_margins.py [--gfx DIR_OF_Adafruit_GFX_Library] [--chrome PATH] [--no-board]
Chrome comes from --chrome, $CHROME, the usual macOS path or the PATH. The GFX library is the one the firmware
uses (default ~/Documents/Arduino/libraries/Adafruit_GFX_Library, as in tools/extract_fonts.py)."""
import argparse, html, json, os, re, shutil, subprocess, sys, tempfile

root = os.path.normpath(os.path.join(os.path.dirname(os.path.abspath(__file__)), '..'))
rd = lambda p: open(os.path.join(root, p)).read()

# ---- the page that runs inside Chrome: appended to renderer.js, so it sees FB, FLIGHTS, WX, wxA, ... ----------
TEST_JS = r'''
const HOST = __HOST__;
const UPDATES = __UPDATES__;
const SAMPLES = FLIGHTS.slice();
const RealDate = Date;
let fakeNow = new RealDate(2026, 10, 21, 21, 47, 0);  // Sat 21 Nov, 21:47: the board's demo clock
Date = class extends RealDate { constructor(...a) { if (a.length) super(...a); else super(fakeNow.getTime()); } };

let seed = 12345;
const rnd = (a, b) => { seed = (seed * 1664525 + 1013904223) >>> 0; return a + seed % (b - a + 1); };
const rstr = (lo, hi, cs) => { let r = ''; for (let n = rnd(lo, hi); n > 0; n--) r += cs[rnd(0, cs.length - 1)]; return r; };
const ANY = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789 -.'/&,()", WIDE = 'MWHNQ1gjpqy ';
const sign = n => (n >= 0 ? '+' : '') + n;
// strings right at the width limits: wide letters of every length, with a narrow character at either end
const edge = []; for (let n = 1; n <= 25; n++) { const w = 'W'.repeat(n); edge.push(w, w.slice(1) + '1', '1' + w.slice(1), w.slice(1) + 'y', w.slice(1) + '.'); }

const stats = {};
function draw(group, what, fn, t) {
  const fb = new FB(); fn(fb, t || 0, { warn() {} });
  const s = stats[group] || (stats[group] = { n: 0, bad: 0, maxx: -1, maxy: -1, example: '' }); s.n++;
  let bad = false;
  for (let y = 0; y < H; y++) for (let x = 0; x < W; x++) {
    const i = (y * W + x) * 3; if (!(fb.d[i] | fb.d[i + 1] | fb.d[i + 2])) continue;
    s.maxx = Math.max(s.maxx, x); s.maxy = Math.max(s.maxy, y); if (x === 63 || y === 31) bad = true;
  }
  if (bad && s.bad++ === 0) s.example = what;
}
const L = { A: [flightA, wxA], B: [flightB, wxB], C: [flightC, wxC] };

// flights
const flightList = SAMPLES.slice();
edge.forEach(s => flightList.push({ air: s, route: s, type: s, to: s, alt: s, spd: '', trk: '359°', vr: '-1234ft/s', logo: 'globe' }));
for (let i = 0; i < 1500; i++) {
  const cs = i % 3 === 0 ? WIDE : ANY;
  flightList.push({ air: rstr(1, 25, cs), route: rstr(0, 8, i % 3 === 0 ? WIDE : 'ABCDEFGHIJKLMNOPQRSTUVWXYZ->'), type: rstr(0, 19, cs), to: rstr(1, 21, cs),
    alt: rnd(0, 999999) + 'ft', spd: rnd(0, 9999) + ['kt', 'mph', 'kts'][i % 3], trk: rnd(0, 359) + '°', vr: sign(rnd(-9999, 9999)) + 'ft/s', logo: ['roundel', 'stripes', 'orb', 'globe'][i % 4] });
}
FLIGHTS.length = 0; FLIGHTS.push(null); state.flight = 0;
flightList.forEach(f => { FLIGHTS[0] = f; for (const l of 'ABC') draw('Flight ' + l, JSON.stringify(f), L[l][0]); });
FLIGHTS.length = 0; SAMPLES.forEach(f => FLIGHTS.push(f));

// weather
const wxBase = JSON.parse(JSON.stringify(WX));
const icons = ['sun', 'part', 'rain', 'cloud'], texts = ['Clear', 'Partly cloudy', 'Rain', 'Cloudy'], pad = n => String(n).padStart(2, '0');
const temp = lim => { const v = rnd(0, 9) ? String(rnd(-lim, lim)) : '--'; return v; };
for (let i = 0; i < 1500 + edge.length; i++) {
  const lim = i % 5 === 0 ? 120 : 55, k = rnd(0, 3), h = rnd(0, 23);
  Object.assign(WX, { loc: i < edge.length ? edge[i] : rstr(0, 23, i % 3 === 0 ? WIDE : ANY), temp: temp(lim), icon: icons[k], text: texts[k],
    line: 'H' + rnd(-lim, lim) + ' L' + rnd(-lim, lim) + (rnd(0, 9) ? ' ' + rnd(0, 100) + '%' : ''),
    cols: [0, 1, 2].map(c => [pad((h + 3 * c) % 24) + 'h', icons[rnd(0, 3)], temp(lim)]) });
  WX.cols[0][2] = WX.temp;
  const what = JSON.stringify(WX); for (const l of 'ABC') draw('Weather ' + l, what, L[l][1]);
}
Object.assign(WX, wxBase);

// idle clock: every day of a leap year, four hours, two colon phases (renderer.js has no 12 h clock; the board check covers it)
for (let day = 0; day < 366; day++) for (const hh of [0, 9, 12, 23]) for (const mm of [0, 59]) for (const t of [0, 0.5]) {
  fakeNow = new RealDate(2024, 0, 1 + day, hh, mm, 0); draw('Idle clock', fakeNow.toString(), idleA, t);
}
fakeNow = new RealDate(2026, 10, 21, 21, 47, 0);

// the problem message: fixed text, but it replaces weather and the clock, which drift
for (const t of [0, 0.5]) draw('Problem msg', 'problem message', problemMsg, t);

// the example screens against the board's pixels
const q = c => ((c[0] & 0xF8) << 8) | ((c[1] & 0xFC) << 3) | (c[2] >> 3);
const same = [];
function compare(name, fn) {
  const h = HOST[name]; if (!h) return;
  const fb = new FB(); fn(fb, 0, { warn() {} }); let diff = 0;
  for (let y = 0; y < H; y++) for (let x = 0; x < W; x++) { const i = (y * W + x) * 3; if (q([fb.d[i], fb.d[i + 1], fb.d[i + 2]]) !== h[y][x]) diff++; }
  same.push([name, diff]);
}
SAMPLES.forEach((f, i) => { FLIGHTS.length = 0; FLIGHTS.push(f); state.flight = 0; for (const l of 'ABC') compare('flight' + l + i, L[l][0]); });
FLIGHTS.length = 0; SAMPLES.forEach(f => FLIGHTS.push(f));
for (const l of 'ABC') compare('weather' + l, L[l][1]);
compare('idleA', idleA);
compare('problem', problemMsg);

// the firmware-update screens: never drifted, so no margin to keep; only the pixels must equal the board's
const updBase = JSON.parse(JSON.stringify(UPD));
for (const [name, code, secs, ver, pct, line] of UPDATES) {
  Object.assign(UPD, { code, secondsLeft: secs, version: ver, percent: pct, line });
  compare(name + '.code', updateCode); compare(name + '.prog', updateProgress);
  for (const [grp, fn] of [['Update code', updateCode], ['Update progress', updateProgress]]) {
    const fb = new FB(); fn(fb, 0, { warn() {} });
    const s = stats[grp] || (stats[grp] = { n: 0, bad: 0, maxx: -1, maxy: -1, example: '', free: true }); s.n++;
    for (let y = 0; y < H; y++) for (let x = 0; x < W; x++) { const i = (y * W + x) * 3; if (fb.d[i] | fb.d[i + 1] | fb.d[i + 2]) { s.maxx = Math.max(s.maxx, x); s.maxy = Math.max(s.maxy, y); } }
  }
}
Object.assign(UPD, updBase);
document.getElementById('o').textContent = JSON.stringify({ stats, same });
'''


def update_states():
    """States of the two firmware-update screens: (name, code, seconds left, version, percent, status word). The examples first
    (a code with 150 s left; version 0.10.1 at 42 %; an upload with no version), then made-up ones with a fixed seed."""
    import random
    rng = random.Random(4242)
    st = [('updateCode', '012345', 150, '', 0, ''), ('updateProgress', '', 0, '0.10.1', 42, 'Downloading'), ('updateUpload', '', 0, '', 60, 'Uploading')]
    words = ['Checking', 'Downloading', 'Installing', 'Restarting', 'Uploading', 'Verifying', 'Writing', 'Done']
    for i in range(400):
        code = ''.join(rng.choice('0123456789') for _ in range(6 if i % 8 else rng.randint(0, 7)))
        secs = rng.choice([0, 1, 29, 30, 31, 179, 180, 181, 5999, 7000, -5]) if i % 3 == 0 else rng.randint(0, 180)
        if i % 5 == 0: ver = ''
        elif i % 5 == 1: ver = '%d.%d.%d' % (rng.randint(0, 9999), rng.randint(0, 9999), rng.randint(0, 9999))
        else: ver = '%d.%d.%d' % (rng.randint(0, 99), rng.randint(0, 99), rng.randint(0, 99))
        pct = rng.choice([0, 1, 50, 99, 100, 101, -3]) if i % 4 == 0 else rng.randint(0, 100)
        line = rng.choice(words) if i % 6 else ''.join(rng.choice('ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789 .') for _ in range(rng.randint(0, 23)))
        st.append(('upd%d' % i, code, secs, ver, pct, line))
    return st


def find_chrome(arg):
    for c in [arg, os.environ.get('CHROME'), '/Applications/Google Chrome.app/Contents/MacOS/Google Chrome',
              shutil.which('google-chrome'), shutil.which('chromium'), shutil.which('chromium-browser')]:
        if c and os.path.exists(c):
            return c
    sys.exit('Chrome not found: pass --chrome PATH or set CHROME')


def board_run(gfx, tmp, states):
    """Builds tools/check_margins.d/main.cpp with screens.cpp and the GFX library. Returns (ok, text, demo pixels)."""
    cxx = shutil.which('c++') or shutil.which('clang++') or shutil.which('g++')
    if not cxx:
        return None, 'no C++ compiler found', {}
    if not os.path.exists(os.path.join(gfx, 'Adafruit_GFX.cpp')):
        return None, 'Adafruit_GFX_Library not found at %s (use --gfx)' % gfx, {}
    exe = os.path.join(tmp, 'check_margins_board')
    sk = os.path.join(root, 'arduino/ledmatrix')
    cmd = [cxx, '-std=c++17', '-O1', '-w', '-DARDUINO=100', '-I' + os.path.join(root, 'tools/check_margins.d'), '-I' + gfx, '-I' + sk, '-o', exe,
           os.path.join(root, 'tools/check_margins.d/main.cpp'), os.path.join(sk, 'screens.cpp'), os.path.join(sk, 'art.cpp'), os.path.join(gfx, 'Adafruit_GFX.cpp')]
    b = subprocess.run(cmd, capture_output=True, text=True)
    if b.returncode:
        return False, 'the board code did not build for the laptop:\n' + b.stderr[-3000:], {}
    run = subprocess.run([exe], capture_output=True, text=True)
    demo, cur = {}, None
    for line in subprocess.run([exe, '-d'], capture_output=True, text=True).stdout.splitlines():
        if line.startswith('DUMP '):
            cur = demo.setdefault(line[5:], [])
        elif cur is not None and len(line) == 256:
            cur.append([int(line[i:i + 4], 16) for i in range(0, 256, 4)])
    feed = ''.join('\t'.join(str(v) for v in st) + '\n' for st in states)
    for line in subprocess.run([exe, '-u'], input=feed, capture_output=True, text=True).stdout.splitlines():
        if line.startswith('DUMP '):
            cur = demo.setdefault(line[5:], [])
        elif cur is not None and len(line) == 256:
            cur.append([int(line[i:i + 4], 16) for i in range(0, 256, 4)])
    return run.returncode == 0, run.stdout.rstrip(), demo


def main():
    ap = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    ap.add_argument('--gfx', default=os.path.expanduser('~/Documents/Arduino/libraries/Adafruit_GFX_Library'))
    ap.add_argument('--chrome')
    ap.add_argument('--no-board', action='store_true')
    a = ap.parse_args()
    failed = False
    tmp = tempfile.mkdtemp(prefix='check_margins_')
    try:
        demo, states = {}, update_states()
        print('Board code (screens.cpp, built for the laptop):')
        if a.no_board:
            print('  skipped (--no-board)')
        else:
            ok, text, demo = board_run(a.gfx, tmp, states)
            print('\n'.join('  ' + l if not l.startswith('  ') else l for l in text.splitlines()) if ok is not None else '  skipped: ' + text)
            failed |= ok is False

        print('Preview and portal renderer (shared/renderer.js, headless Chrome):')
        page = '<!doctype html><meta charset=utf-8><pre id=o></pre><script>' + rd('shared/renderer.js').replace('__FONTS__', rd('shared/fonts.json')) + '\n' + \
               TEST_JS.replace('__HOST__', json.dumps(demo)).replace('__UPDATES__', json.dumps(states)) + '</script>'
        path = os.path.join(tmp, 'check.html')
        open(path, 'w').write(page)
        r = subprocess.run([find_chrome(a.chrome), '--headless=new', '--disable-gpu', '--virtual-time-budget=60000', '--dump-dom', 'file://' + path],
                           capture_output=True, text=True, timeout=300)
        m = re.search(r'<pre id="o">(.*?)</pre>', r.stdout, re.S)
        if not m or not m.group(1).strip():
            print('  the page produced no result; Chrome said:\n' + r.stderr[-2000:])
            return 1
        res = json.loads(html.unescape(m.group(1)))
        for name in sorted(res['stats']):
            s = res['stats'][name]
            print('  %-15s %7d pictures, lit up to column %2d and row %2d: %s' % (name, s['n'], s['maxx'], s['maxy'], 'FAIL' if s['bad'] else 'ok (may use column 63 and row 31)' if s.get('free') else 'ok'))
            if s['bad']:
                failed = True
                print('      %d of them touch column 63 or row 31, first: %s' % (s['bad'], s['example']))
        if res['same']:
            diffs = [(n, d) for n, d in res['same'] if d]
            print('  same pixels as the board on the %d example and made-up screens: %s' % (len(res['same']), 'yes' if not diffs else 'NO, differing: ' + ', '.join('%s (%d px)' % x for x in diffs)))
            failed |= bool(diffs)
        else:
            print('  (no board run, so the example screens were not compared with the board)')
    finally:
        shutil.rmtree(tmp, ignore_errors=True)
    print('FAILED' if failed else 'OK: column 63 and row 31 stay black')
    return 1 if failed else 0


if __name__ == '__main__':
    sys.exit(main())
