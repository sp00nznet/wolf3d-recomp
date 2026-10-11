# Lifting Wolfenstein 3-D: what it took

Wolf3D v1.4 (the Steam and GOG release) is the same kind of program as Blake
Stone, which is built on its engine: Borland C++ 3.x, medium model, 386
instructions in the hot paths, LZEXE-packed. So this repo started as a copy of
the [Blake Stone recomp](https://github.com/sp00nznet/blakestone), and
everything in its
[lifting notes](https://github.com/sp00nznet/blakestone/blob/main/docs/lifting.md)
(the 8087 emulator interrupts, switch tables, stored code pointers, the
self-modifying raycaster) applies here unchanged. This file is what Wolf3D
needed on top. Code: [`tools/lift.py`](../tools/lift.py).

## 1. The first lift

```
> py tools/lift.py wl6
WOLF3D.EXE: DGROUP 398D, 29 code segments, 747 functions, 48334 instructions lifted,
  237 stored code pointers, 0 extra entries, 51 switch tables
  1 undecodable targets, first: 2005:25D7 (in fn_221A2)
  20 self-modified code bytes
  1 far calls into nothing (stubbed as dispatch)
  unhandled instruction forms: 1 (1 kinds), first: ['? di']
```

`WOLF3D.EXE` carries `LZ91` at 1Ch and unpacks with pcrecomp's
`tools/drm/unlzexe.py` like Blake Stone's executables. It built and ran first
time, then stopped at a black screen.

The `? di` is an invalid opcode, `FF FF`. A Borland RTL string routine jumps
into the middle of its own instructions, and one alignment of those bytes
decodes as `FF FF`. The CPU never executes it, and the conformance check
leaves invalid opcodes out.

## 2. A page flip that needs five blanks in a row

The black screen was the signon's first page flip, spinning. The routine reads
the timer count, adds two, then polls the input status register (3DAh). It
writes the CRTC start address only after it sees display-disabled (bit 0) set,
outside vertical retrace, on five reads in a row:

```
1D44:0059  ec        in al, dx
1D44:005A  a808      test al, 0x8      ; vertical retrace: start again
1D44:005C  75ec      jne 0x004A
1D44:005E  a801      test al, 0x1      ; display enabled: start again
1D44:0060  74ed      je 0x004F
             ... four more times ...
```

Blake Stone's VGA model flipped bit 0 on every read ("display enable
flickers per line"), which never gives five in a row. And the timer escape
couldn't fire either: the game installs its timer interrupt later. On real
hardware bit 0 stays set for the whole horizontal blank. `vga.c` now returns
it in runs (six set, two clear), which satisfies both games.

## 3. Code the game writes at run time

Past the signon, through the PG-13 screen, the title, the menus and into
E1M1: a status bar, a weapon in the HUD, and a 3D view of plain grey floor and
ceiling. No walls, no sprites, and a stream of misses into the heap:

```
[miss] call far 5259:0104 (image 51594)
8A 44 00 26 88 85 20 08 8A 44 01 26 88 85 70 08 26 88 85 C0 08 8A 44 02 26 88 85 10 09 ...
[miss] call far 521E:0104 (image 511E4)
8A 44 00 26 88 85 70 08 8A 44 01 26 88 85 C0 08 26 88 85 10 09 8A 44 02 26 88 85 60 09 ...
```

These are Wolf3D's compiled scalers. Blake Stone replaced them with
self-modifying loops, but Wolf3D generates one routine per column height at
start-up and far-calls into it. The bytes decode as `mov al,[si+n]` /
`mov es:[di+m],al` pairs ending in `retf`. For a sprite post, the caller
patches a `retf` over the post's end and calls into the middle.

There is no static code to lift, so `src/machine.c` `run_generated()`
interprets it. When a far call has no lifted target and lands outside the load
image, the bytes are run from guest memory as they are at that moment. It
accepts only the three forms the generator emits; anything else is still a
reported miss.

`WOLF_MISSDUMP=1` prints the bytes at every miss, which is how these were read.

## 4. "UP does nothing"

With walls on screen, scripted UP presses didn't move the player. The keyboard
model was fine: the ISR saw `E0 48` and set `Keyboard[0x48]`. The game just
doesn't move on it. The movement code reads its keys from `dirscan[]`
(DGROUP 1270h), filled from CONFIG.WL6, and the Steam/GOG CONFIG maps them
to W, D, S and A:

```
> xxd -s 0x1DC original/wl6/CONFIG.WL6
000001dc: 0000 1100 2000 1f00 1e00 1d00 3800 2a00  .... .......8.*.
```

That's the release's choice, not a bug. The arrow keys move the menu cursor
and nothing in play.

## 5. Finding the renderer in this executable

Blake Stone's renderer finder looked for its 386 asm scalers (`gs:` reads,
patched `add edx, imm32`). Wolf3D has none of those, so `find_renderer()` was
rewritten around the 3D refresh's call graph: raycaster → Hit routines →
ScalePost; WallRefresh → ThreeDRefresh → the clear, DrawScaleds and the
weapon; and the one routine that stores `0xCB`. It still reads every address
out of the code. See [renderer.md](renderer.md).

## Debugging kit

All inherited from Blake Stone and renamed:

| | |
|---|---|
| `cmake -B build-trace -DWOLF_TRACE=ON` | a build whose lifted functions record themselves |
| `WOLF_CALLS=from-to` | (trace build) each lifted function entered in that window of game time, once; diff two runs to find what a key press reaches |
| `WOLF_WATCH=<linear hex>` | (trace build) every write to that byte, with the host call chain (lifted functions keep their `fn_` names) |
| `WOLF_POS=1` | the player's tile, position and angle every 250 ms of game time |
| `WOLF_WARP=ms:x,y,angle` | move the player there once (looking around while writing a script) |
| `WOLF_MISSDUMP=1` | the bytes at every dispatch miss |
| `WOLF_PERF=1` | in a window, once a second: frames, compose time (redraw, DLSS, evaluate), present time, game polls |
| `--trace` | DOS calls, Sound Blaster commands |
