#!/usr/bin/env python3
"""
conformance.py - does the recompiled game still do what the original does?

    py tools/conformance.py              # run it, if the game data and a build exist
    py tools/conformance.py --update     # accept the current count as the baseline

There is no reference build to diff against frame by frame (the original runs
only under DOSBox), so the ground truth is behaviour the original visibly has:
it boots, reaches its menus, starts a game, renders a textured 3D view that
changes as the player moves, plays AdLib music and digitized sound through
the Sound Blaster, saves and loads, and never lands on an address with no
lifted code. Each scenario drives the game headless with scripted keys and
checks those things; then the remaster -- the hi-res redraw and widescreen --
is checked against the same moments. See docs/conformance.md.

The game data is not redistributable, so on a machine without it (CI) the
game is reported as skipped and the run passes. A count below the committed
baseline (tests/conformance_baseline.json) fails, so a regression cannot land
quietly; --update records a new, higher figure.
"""
import json
import os
import shutil
import struct
import subprocess
import sys
import time

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
BASELINE = os.path.join(ROOT, 'tests', 'conformance_baseline.json')
GAMES = {'wl6': 'WOLF3D.EXE'}

# Signon, PG-13, title, menu: New Game, episode 1, "Bring 'em on!". Play
# starts at about 22 s. Measured frame by frame (--shot-at); headless time is
# deterministic, so these land on the same screens every run.
START = '4000:ENTER,9000:ENTER,13000:ENTER,15000:ENTER,17000:ENTER,19000:ENTER'
# Movement is the CONFIG's: the Steam/GOG release ships W/A/S/D (dirscan
# 11 1E 1F 20). Fire the pistol -- a digitized sound -- then walk and turn.
PLAY = '23000:CTRL,24000:CTRL,25000:W+1500,27000:D+600,28000:W+1500,30000:A+400'


def read_bmp(path):
    """The runtime's own BMPs: 32-bit, top-down. -> (w, h, list of 0xRRGGBB)"""
    d = open(path, 'rb').read()
    w, h = struct.unpack_from('<ii', d, 18)
    h = abs(h)
    px = struct.unpack_from(f'<{w * h}I', d, 54)
    return w, h, [p & 0xFFFFFF for p in px]


def region(img, x0, y0, x1, y1):
    w, h, px = img
    return [px[y * w + x] for y in range(y0, min(y1, h)) for x in range(x0, min(x1, w))]


def wav_rms(path):
    d = open(path, 'rb').read()
    n = (len(d) - 44) // 2
    if n <= 0:
        return 0.0
    s = struct.unpack_from(f'<{n}h', d, 44)
    step = max(1, n // 200000)
    sub = s[::step]
    return (sum(v * v for v in sub) / len(sub)) ** 0.5


def fresh_save(out, viewsize=None):
    """A save folder of its own. With viewsize, a copy of the game's CONFIG
    set to that view size (20 is the full 320-column view widescreen needs);
    without, the game reads the install's CONFIG, as it would on its own."""
    save = os.path.join(out, 'save')
    os.makedirs(save, exist_ok=True)
    if viewsize:
        cfg = bytearray(open(os.path.join(ROOT, 'original', 'wl6', 'CONFIG.WL6'), 'rb').read())
        struct.pack_into('<H', cfg, len(cfg) - 4, viewsize)    # viewsize, then mouseadjustment
        open(os.path.join(save, 'CONFIG.WL6'), 'wb').write(cfg)
    return save


def run(game, out, seconds, keys, shots, trace=False, extra=(), viewsize=None):
    exe = os.path.join(ROOT, 'build', 'Release', f'wolf3d_{game}.exe')
    save = os.path.join(out, 'save')
    if not os.path.isdir(save):
        save = fresh_save(out, viewsize)
    shot_arg = ','.join(f'{ms}:{os.path.join(out, name + ".bmp")}' for name, ms in shots.items())
    cmd = [exe, '--headless', '--seconds', str(seconds), '--save', save,
           '--data', os.path.join(ROOT, 'original', game), '--wav', os.path.join(out, 'audio.wav')]
    if keys:
        cmd += ['--keys', keys]
    if shot_arg:
        cmd += ['--shot-at', shot_arg]
    if trace:
        cmd += ['--trace']
    cmd += list(extra)
    p = subprocess.run(cmd, cwd=ROOT, capture_output=True, text=True, errors='replace',
                       timeout=seconds * 3 + 60)
    open(os.path.join(out, 'stderr.txt'), 'w').write(p.stderr)
    return p.returncode, p.stderr


def check_game(game):
    stamp = time.strftime('%Y%m%d-%H%M%S')
    base = os.path.join(ROOT, 'work', game, 'conformance', stamp)
    results = []

    def ok(name, cond, why=''):
        results.append((name, bool(cond), why))

    lj = os.path.join(ROOT, 'work', game, 'lift.json')
    lift = json.load(open(lj)) if os.path.exists(lj) else {}
    # '?' is an invalid opcode: bytes a Borland RTL routine reaches only at an
    # alignment the CPU never executes (it jumps into its own instructions).
    real = [k for k in lift.get('unhandled', {}) if not k.startswith('?')]
    ok('lift: every instruction form handled', lift and not real,
       ', '.join(real) if lift else 'no lift.json')

    out = os.path.join(base, 'boot')
    rc, err = run(game, out, 8, '', {'signon': 6000})
    ok('boot: clean exit', rc == 0, f'exit {rc}')
    ok('boot: no dispatch misses', '[miss]' not in err, err.count('[miss]'))
    sig = read_bmp(os.path.join(out, 'signon.bmp'))
    ok('boot: the signon screen', len(set(region(sig, 0, 0, 320, 200))) >= 8,
       f'{len(set(region(sig, 0, 0, 320, 200)))} colours')

    out = os.path.join(base, 'play')
    rc, err = run(game, out, 33, START + ',' + PLAY, {'view1': 22500, 'view2': 32000}, trace=True)
    ok('play: clean exit', rc == 0, f'exit {rc}')
    ok('play: no dispatch misses (compiled scalers interpreted)', '[miss]' not in err, err.count('[miss]'))
    v1, v2 = (read_bmp(os.path.join(out, f'view{i}.bmp')) for i in (1, 2))
    view1, view2 = region(v1, 16, 16, 304, 140), region(v2, 16, 16, 304, 140)
    ok('play: textured 3D view', len(set(view1)) >= 40, f'{len(set(view1))} colours')
    moved = sum(a != b for a, b in zip(view1, view2)) / max(1, len(view1))
    ok('play: view changes as the player moves', moved > 0.2, f'{moved:.0%} of pixels')
    rms = wav_rms(os.path.join(out, 'audio.wav'))
    ok('play: audio', rms > 200, f'rms {rms:.0f}')
    ok('play: Sound Blaster initialised (DSP reset, rate, speaker on)',
       '[sb] DSP 40' in err and '[sb] DSP D1' in err)
    ok('play: Sound Blaster digitized playback (the pistol)', err.count('[sb] play') > 0,
       f"{err.count('[sb] play')} blocks")

    # The hi-res renderer: found in this game's code at lift time, and the
    # same moment of play drawn at 4x. Its view must not be the original
    # enlarged: walls resampled from the textures carry texels between the
    # original columns that a 4x copy cannot. More than 2% means it redrew.
    ok('hires: renderer found in the game\'s code', lift.get('renderer'))
    out = os.path.join(base, 'hires')
    rc, err = run(game, out, 23, START, {'view': 22500}, extra=['--hires', '4'], viewsize=20)
    big = read_bmp(os.path.join(out, 'view.bmp'))
    out43 = os.path.join(base, 'plain')
    run(game, out43, 23, START, {'view': 22500}, viewsize=20)
    small = read_bmp(os.path.join(out43, 'view.bmp'))
    bv = region(big, 64, 64, 1216, 560)
    up = [small[2][(y // 4) * 320 + x // 4] for y in range(64, 560) for x in range(64, 1216)]
    differs = sum(a != b for a, b in zip(bv, up)) / max(1, len(bv))
    ok('hires: the 3D view is redrawn at 4x', rc == 0 and big[0] == 1280 and big[1] == 800 and differs > 0.02,
       f'{big[0]}x{big[1]}, {differs:.0%} of view pixels differ from the 4x original')
    # Widescreen: the same moment at 16:9. The side strips must show the view
    # (walls, floor, sprites cast by the game's own code turned left and
    # right), and the 4:3 middle must match the run above pixel for pixel --
    # the side passes put every byte back, so the game cannot have noticed.
    # (The outermost column each side is left out: it blends into the strip.)
    ok('widescreen: found in the game\'s code', lift.get('widescreen'))
    out = os.path.join(base, 'wide')
    rc, err = run(game, out, 23, START, {'view': 22500}, extra=['--hires', '4', '--widescreen', '16:9'],
                  viewsize=20)
    wide = read_bmp(os.path.join(out, 'view.bmp'))
    e = (wide[0] - 1280) // 2
    mid = region(wide, e + 4, 0, e + 1276, 800)
    same = mid == region(big, 4, 0, 1276, 800)
    # Inside the view, every row of each strip must carry picture: Wolf3D's
    # stone walls have black mortar, so a black pixel is not a hole, but a
    # row with nothing but black is.
    sides = [(0, e), (wide[0] - e, wide[0])]
    filled = min(sum(any(p != 0 for p in region(wide, x0, y, x1, y + 1)) for y in range(64, 560)) / 496
                 for x0, x1 in sides)
    cols = min(len(set(region(wide, x0, 64, x1, 560))) for x0, x1 in sides)
    ok('widescreen: the view reaches into both sides, the 4:3 picture unchanged',
       rc == 0 and wide[0] == 1704 and same and filled > 0.98 and cols >= 8,
       f'{wide[0]}x{wide[1]}, middle {"identical" if same else "DIFFERS"}, strip rows {filled:.0%} drawn, {cols}+ colours')

    # Save from inside a game, then load it in a fresh process. ESC opens the
    # menu on New Game; Save Game is four down. Slot 0, type a name, ENTER.
    # The load: title, menu, Load Game is three down (Save Game is greyed out
    # there and skipped), slot 0.
    out = os.path.join(base, 'save')
    keys = START + (',24000:W+1000,26000:ESC,28000:DOWN,28700:DOWN,29400:DOWN,30100:DOWN,'
                    '31000:ENTER,33000:ENTER,34000:14,34300:12,34600:1F,34900:14,35500:ENTER')
    rc, err = run(game, out, 38, keys, {})
    saves = [f for f in os.listdir(os.path.join(out, 'save')) if f.upper().startswith('SAVEGAM0')]
    size = os.path.getsize(os.path.join(out, 'save', saves[0])) if saves else 0
    ok('save: a game saves to slot 0', rc == 0 and size > 1000 and '[miss]' not in err,
       f'{saves[0] if saves else "no file"}, {size} bytes')

    load = os.path.join(base, 'load')
    os.makedirs(os.path.join(load, 'save'), exist_ok=True)
    for f in os.listdir(os.path.join(out, 'save')):
        shutil.copy(os.path.join(out, 'save', f), os.path.join(load, 'save', f))
    rc, err = run(game, load, 24, '4000:ENTER,9000:ENTER,13000:ENTER,15000:DOWN,15700:DOWN,16400:DOWN,'
                                  '17500:ENTER,19000:ENTER', {'loaded': 23500})
    view = region(read_bmp(os.path.join(load, 'loaded.bmp')), 16, 16, 304, 140)
    ok('load: the saved game loads into the 3D view',
       rc == 0 and '[miss]' not in err and len(set(view)) >= 40, f'{len(set(view))} colours')
    return results


def main():
    update = '--update' in sys.argv
    base = json.load(open(BASELINE)) if os.path.exists(BASELINE) else {}
    totals, failed_regression = {}, False
    for game, exe in GAMES.items():
        have_data = os.path.exists(os.path.join(ROOT, 'original', game, exe))
        have_exe = os.path.exists(os.path.join(ROOT, 'build', 'Release', f'wolf3d_{game}.exe'))
        if not (have_data and have_exe):
            print(f'{game}: SKIP -- needs original/{game}/{exe} (your own copy) and a build; '
                  'see README, Getting Started')
            continue
        res = check_game(game)
        passed = sum(1 for _, good, _ in res if good)
        totals[game] = {'passed': passed, 'total': len(res)}
        print(f'{game}: {passed}/{len(res)}')
        for name, good, why in res:
            print(f'  {"PASS" if good else "FAIL"}  {name}' + (f'  ({why})' if why != '' else ''))
        if passed < base.get(game, {}).get('passed', 0):
            print(f'  REGRESSION: baseline is {base[game]["passed"]}/{base[game]["total"]}')
            failed_regression = True
    if update and totals:
        base = totals
        os.makedirs(os.path.dirname(BASELINE), exist_ok=True)
        json.dump(base, open(BASELINE, 'w'), indent=1)
        print(f'baseline updated: {BASELINE}')
    return 1 if failed_regression else 0


if __name__ == '__main__':
    sys.exit(main())
