# Architecture

One game, one pipeline, one runtime: Wolfenstein 3-D v1.4 (`WOLF3D.EXE`, the
Steam and GOG release). The runtime is the Blake Stone recomp's DOS machine;
what Wolf3D needed on top is in [lifting.md](lifting.md).

```
original/wl6/WOLF3D.EXE  ──► tools/lift.py ──► work/wl6/gen/*.c   ──┐
   (your copy)                 │  unlzexe       (never committed)    │
                               │  discover                           ├─► MSVC ─► wolf3d_wl6.exe
                               │  lift16                             │
                               └─ pcrecomp                src/*.c ───┘
                                                    (the DOS machine)
```

## The parts

| Part | Owns | Code |
|------|------|------|
| Lift driver | Unpacking, finding every function, handing each to the lifter, writing the build tree | [`tools/lift.py`](../tools/lift.py) |
| pcrecomp | The x86-16 decoder, the lifter, the CPU model the C compiles against | [sp00nznet/pcrecomp](https://github.com/sp00nznet/pcrecomp): `tools/disasm/decode16.py`, `tools/lift/lift16.py`, `runtime/recomp16/cpu.h`, `tools/drm/unlzexe.py` |
| Generated code | One C function per guest function, the address table for indirect calls, the load image | `work/<game>/gen/` — local only |
| DOS + BIOS | Boot (PSP, environment, relocation), INT 21h files and memory, INT 10h/16h/1Ah/33h, the vectors nobody hooked | [`src/dos.c`](../src/dos.c) |
| Machine | PIC, PIT, keyboard controller, interrupt delivery, indirect dispatch, and the interpreter for code the game writes at run time | [`src/machine.c`](../src/machine.c) |
| VGA | Planar memory, latches, write modes, DAC, CRTC scanout, text mode | [`src/vga.c`](../src/vga.c) |
| Audio | OPL2 (ymfm), Sound Blaster DSP + 8237 DMA, PC speaker, mixing | [`src/audio.c`](../src/audio.c), [`src/opl.cpp`](../src/opl.cpp) |
| x87 | The FPU behind Borland's emulator interrupts | [`src/x87.c`](../src/x87.c) |
| Hi-res renderer | Walls, sprites and the weapon at N×, and the widescreen side passes, from the game's own per-column answers | [`src/hires.c`](../src/hires.c), [renderer](renderer.md) |
| Host | Win32 window, raw input, XInput pad, waveOut, `wolf3d.ini`; or headless frames to ffmpeg | [`src/host.c`](../src/host.c) |
| Presenter | Direct3D 11 scaling: sharp-bilinear, smooth, CRT, nearest, integer | [`src/present_d3d.c`](../src/present_d3d.c) |
| Harness | Scripted headless runs with pass/fail checks | [`tools/conformance.py`](../tools/conformance.py), [`tests/selftest.c`](../tests/selftest.c) |

## How a guest function becomes a C function

`tools/lift.py` decodes each code segment as a 64 KB window (`Decoder(image[cs*16 : cs*16+0x10000], cs*16)`),
so instruction addresses are offsets within the segment and branch targets wrap
the way IP does. A *function* is the closure of everything reachable from its
entry without a call: jumps, conditional branches and switch-table arms are
followed, calls and returns are not. Code two functions share is simply lifted
into both. Each function is emitted with its own code segment, which decides
what a `cs:` operand reads and what a near call means.

Calls become C calls. The lifted code pushes a return frame onto the guest stack
(a sentinel offset, `0xFFFF`, since there is nothing to return *to* in guest
memory) so stack-relative arguments stay where the guest expects them; `ret`
pops it and returns from the C function. Indirect calls and jumps go through
`recomp_dispatch` / `dispatch_near` / `dispatch_far`, which look the target up in
the sorted table `tools/lift.py` writes.

Finding every function is the hard part, and is described in
[lifting.md](lifting.md).

## Code the game writes at run time

Wolf3D draws wall and sprite columns with *compiled scalers*: routines it
writes into its heap at start-up, one per column height, and far-calls into.
They don't exist in the executable, so nothing is lifted for them. A far call
with no lifted target that lands outside the load image goes to
`run_generated()` (`machine.c`), which interprets the bytes as they are in
guest memory at that moment. It accepts only the three instruction forms the
generator emits (`mov al,[si+d]`, `mov es:[di+d],al`, `retf`), and anything
else is reported as a miss. Its writes go through the same planar VGA model as
lifted code, so pixel ownership and the hi-res hooks see them like any other
draw.

## Memory

A flat 1 MB + 64 KB array, real-mode addressing (`seg * 16 + off`).

| Linear | What |
|--------|------|
| `00000` | Interrupt vectors — every one points into `F000`, a segment no code lives in, so `F000:nn` *means* "the BIOS/DOS handler for vector nn" |
| `00400` | BIOS data area (tick count, video mode, cursor) |
| `00E00` | Environment block, with `BLASTER=A220 I5 D1 T3` |
| `01000` | PSP (`g_psp_seg` = `0100`) |
| `01100` | The program image, relocated by adding `0110` exactly as DOS's loader would |
| … `A0000` | Heap: the arena DOS hands out (`48h`/`4Ah`) |
| `A0000` | VGA window — routed to the planar model in `vga.c` |
| `B8000` | Text mode |

The load segment is baked into the lifted C: every relocated immediate (`mov ax,
DGROUP`, the segment half of a far pointer) is rebased by the lifter, and
`SEG_xxxx` constants carry the runtime segment. So `PSP_SEG` in `tools/lift.py`
and the runtime agree by construction (the generated `recomp_dispatch.c` exports
it).

## Time

The guest is never interrupted mid-instruction; it can't be, it is C. Instead
the lifter puts a `RECOMP_TICK` on every loop back-edge, which counts down a
budget and calls `recomp_tick()` (`machine.c`). That:

1. converts host wall-clock time into PIT ticks (1.193182 MHz);
2. for every timer-0 period that has elapsed, **renders audio up to that
   instant** and then runs the game's INT 8 handler — the sound driver
   reprograms the PIT to its own rate and writes OPL registers from that handler, so this is what
   puts each register write in the audio stream where it was made;
3. delivers queued keyboard scancodes through the game's INT 9 handler, one
   IRQ per byte, the way the 8042 does;
4. delivers the Sound Blaster's end-of-block IRQ;
5. lets the host present a frame and pump its messages.

Interrupts are only delivered with IF set and the PIC line unmasked, and never
nested. A guest that spins on `TimeCount` — which the id engine does
constantly — spins through back-edges, so its own timer handler advances the
count it is waiting on.

If the host falls far behind (a debugger, a slow disk), the backlog of ticks is
dropped rather than replayed, which the game sees as a short time warp.

**Vertical retrace is never missed.** The game paces its fades and palette
flashes by waiting for retrace (3DAh bit 3), which lasts 1.4 ms of each 14.3 ms
refresh. In a window, presenting a frame can take longer than that, and a
retrace that came and went while the host was busy would be missed, so a fade
step waited through several frames. `vga.c` reports such a retrace on the
next read, which is what a real CPU polling the register would have seen.

**Presenting never starves the game.** Everything runs on the game's thread.
A present that took C ms isn't repeated until the game has had C ms of its
own, and the compositor spreads its columns over the CPU's cores
([`src/pool.c`](../src/pool.c)). `WOLF_PERF=1` prints, once a second, the time
spent composing (the redraw, DLSS and its evaluation), presenting, and how
often the game was polled.

**Which clock.** In a window, emulated time is the wall clock, so the game
plays at its real speed. Headless, it is deterministic: each `recomp_tick` (one
per 2000 back-edges, or per waiting DOS/BIOS call) advances it by 300 µs, and
DOS reports a fixed date, noon plus emulated time. A headless run then
depends only on the guest and its inputs, never on host load. Two runs with the
same arguments and save folder produce identical frames. `--realtime` puts a
headless run back on the wall clock.

## Video

The engine runs in Mode X: BIOS mode 13h with chain-4 turned off. VRAM is
four 64 KB planes stored interleaved (`vram[offset*4 + plane]`), which makes
chain-4 a plain linear store as well. Writes honour the Map Mask, the four
write modes, set/reset, the data rotate/logical function and the bit mask;
reads fill the four latches. What is shown is composed from the CRTC start
address and row offset, so page flipping works the way the game does it — by
moving the scanout, not by copying.

## Sound

The OPL2 is ymfm's YM3812, run at its native 49,716 Hz, which is also the
output rate. The Sound Blaster is a DSP state machine (reset, version, time
constant, 8-bit single-cycle DMA, speaker on/off, halt/continue) reading from
the guest through an 8237 model; each finished block raises IRQ 5. The PC
speaker is PIT channel 2 gated by port 61h. All three are mixed to stereo
16-bit and go to waveOut, a WAV file, or ffmpeg.

## Host

Windowed: a Win32 window presented through Direct3D 11
([`present_d3d.c`](../src/present_d3d.c)), the same shader as the Gunman and
Red Alert 2 presenters. Mode X is 320×200 on a 4:3 monitor, so pixels are 1.2
times taller than wide; the picture is shown at 4:3 (or the widescreen's
shape) and scaled:

| `--scale` | F12 cycles |
|---|---|
| `sharp` (default) | sharp-bilinear: whole texels stay crisp, only their edges blend, so odd scales don't shimmer |
| `smooth` | bilinear |
| `crt` | scanlines and an aperture grille, faded in from 1.5× to 3× so it never moirés |
| `nearest` | plain pixels |
| `integer` | nearest at the largest whole multiple: square pixels |

F9 switches the colour look (natural, vivid). F11 or Alt+Enter toggles
borderless fullscreen. F10 cycles widescreen → original → hi-res 4:3, and
Ctrl+F12 saves a screenshot. The choices and fullscreen are kept in
`wolf3d.ini` beside the exe; the command line wins over the file. Without
Direct3D 11 (or with `--gdi`) the picture goes through GDI, nearest-neighbour.
Input: raw-input mouse while captured, scancodes straight from `WM_KEYDOWN`, an
XInput pad pressed as the keys the game knows.

Headless (`--headless`): no window and no audio device — it works over RDP and
in CI. `--record out.mp4` composes a frame every 1/35 s of emulated time into
ffmpeg and muxes the audio in at the end; `--keys` presses keys at fixed points
in emulated time.
