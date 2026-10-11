# Conformance

```
py tools/conformance.py            # needs original/wl6 (your copy) and a build
py tools/conformance.py --update   # record the current count as the new baseline
```

Current figure (2026-10-10): **18/18**, and **20/20** with the optional upscaled art and DLSS build. The run takes about two minutes:
headless time is deterministic and runs faster than real time
([architecture](architecture.md#time)).

```
wl6: 17/17
  PASS  lift: every instruction form handled
  PASS  boot: clean exit  (exit 0)
  PASS  boot: no dispatch misses  (0)
  PASS  boot: the signon screen  (94 colours)
  PASS  play: clean exit  (exit 0)
  PASS  play: no dispatch misses (compiled scalers interpreted)  (0)
  PASS  play: textured 3D view  (91 colours)
  PASS  play: view changes as the player moves  (95% of pixels)
  PASS  play: audio  (rms 750)
  PASS  play: Sound Blaster initialised (DSP reset, rate, speaker on)
  PASS  play: Sound Blaster digitized playback (the pistol)  (5 blocks)
  PASS  hires: renderer found in the game's code
  PASS  hires: the 3D view is redrawn at 4x  (1280x800, 23% of view pixels differ from the 4x original)
  PASS  widescreen: found in the game's code
  PASS  widescreen: the view reaches into both sides, the 4:3 picture unchanged  (1704x800, middle identical, strip rows 100% drawn, 21+ colours)
  PASS  save: a game saves to slot 0  (SAVEGAM0.WL6, 19273 bytes)
  PASS  load: the saved game loads into the 3D view  (52 colours)
```

## What it checks, and why that is the ground truth

There is no reference build to diff frames against: the original only runs
under DOSBox. So the checks are things the original visibly does, run headless
with scripted keys (`--keys`) and fixed-time frame grabs (`--shot-at`).

| Check | Fails when |
|-------|-----------|
| lift: every instruction form handled | `tools/lift.py` met an instruction lift16 emitted as `UNHANDLED`. Invalid opcodes (`?`) are left out: one Borland RTL routine jumps into its own instructions, and one alignment of those bytes decodes as `FF FF`, which a CPU never executes |
| boot: clean exit / no dispatch misses | the process crashed, or the guest jumped somewhere with no lifted function |
| boot: the signon screen | 6 s in, the frame is not the signon picture (8+ colours) |
| play: no dispatch misses (compiled scalers interpreted) | a column went through code the game generated and `run_generated()` could not run it |
| play: textured 3D view | the view has fewer than 40 colours |
| play: view changes as the player moves | under 20% of the view changed between 22.5 s and 32 s while walking and turning |
| play: audio | the WAV is near silent (AdLib music and effects) |
| play: Sound Blaster initialised | the game never reset the DSP, set a rate and turned the speaker on |
| play: Sound Blaster digitized playback | firing the pistol (a digitized sound) never started a DMA block |
| hires: renderer found | `find_renderer()` could not read the raycaster, Hit routines, ScalePost, the line scaler and its callers out of this build |
| hires: the 3D view is redrawn at 4x | the same moment at `--hires 4` is not 1280×800, or its view is the original enlarged (≤ 2% of pixels differ) |
| widescreen: found | `find_widescreen()` could not read WallRefresh, DrawScaleds and the player fields |
| widescreen: the view reaches into both sides, the 4:3 picture unchanged | at `--widescreen 16:9` the frame is not 1704×800, a row of either side strip is all black, or the 4:3 middle differs from the 4:3 run's (the side passes disturbed the game) |
| dlss: the view through DLSS quality, the HUD untouched (*only with the optional DLSS build and an RTX GPU; otherwise `SKIP`, not counted*) | the run did not upscale 1136×427 → 1704×640, the view is not close to the plain redraw (mean difference 12 or more per channel), or the HUD below it changed. See [dlss.md](dlss.md) |
| art: walls and sprites from the upscaled art, the HUD untouched (*only when `work/wl6/art/art.bin` exists; otherwise `SKIP`, not counted*) | the hi-res frame does not differ from the same frame with `--art off` in over 30% of the view, or the HUD changed. See [art.md](art.md) |
| mouse aim: the mouse turns the player, vertical motion does not walk | a scripted 300-mickey sweep right (`MX300+1000` in `--keys`) did not turn the player 5–60°, or a 400-mickey sweep up moved the player (the game itself would walk) |
| save: a game saves to slot 0 | ESC → Save Game → slot 0 → a typed name did not write a `SAVEGAM0.WL6` over 1 KB |
| load: the saved game loads into the 3D view | a fresh process, Load Game → slot 0, is not back in a textured view |

## The scripts

Headless time is deterministic, so a key lands on the same frame every run.
The timings were measured frame by frame with `--shot-at`:

- **Into play.** ENTER at 4, 9 and 13 s (signon, PG-13, title), then New Game,
  episode 1 and "Bring 'em on!" at 15, 17 and 19 s. Play starts at about 22 s.
- **Movement is the CONFIG's.** The Steam and GOG release ships W/A/S/D (the
  CONFIG's `dirscan` is `11 1E 1F 20`), so the scripts walk with `W` and turn
  with `A`/`D`. The arrow keys move the menu cursor.
- **Menus.** Arrows move the cursor; ESC in play opens the menu on New Game,
  with Save Game four down. On the title menu Save Game is greyed out and
  skipped, so Load Game is three down.
- **Widescreen and hi-res runs** copy the install's CONFIG into their own save
  folder with the view size set to 20, the full-width view widescreen needs.
  Every run starts from a fresh save folder: a CONFIG left behind changes
  start-up and the timings with it.

Each run's frames, WAV and stderr go to `work/wl6/conformance/<stamp>/`.

## Baseline and regressions

`tests/conformance_baseline.json` holds the pass count. A run that passes
fewer fails with `REGRESSION` and a non-zero exit. `--update` writes the
current count; do that only when a change adds checks or fixes one.

CI has no game data, so it reports the game as skipped and passes; it still
builds and runs `tests/selftest.c` (the VGA, x87, Sound Blaster and AdLib
models on their own).
