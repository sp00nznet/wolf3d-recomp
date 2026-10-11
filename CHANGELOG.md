# Changelog

All notable changes to this project are documented here. The format follows
[Keep a Changelog](https://keepachangelog.com/en/1.1.0/), and the project uses
[Semantic Versioning](https://semver.org/).

## [Unreleased]

### Added
- Wolfenstein 3-D v1.4 (the Steam and GOG release) recompiled: `tools/lift.py wl6`
  lifts `WOLF3D.EXE` (747 functions) against pcrecomp `61edd49`, and the DOS machine
  from the Blake Stone recomp runs it. Boots, plays, saves and loads.
- An interpreter for the compiled scalers the game writes into its heap at run
  time (`src/machine.c` `run_generated()`), so walls and sprites draw.
- Hi-res 3D view (`--hires N`, F10): walls, doors, actors, objects and the weapon
  redrawn at N× from the game's own art, from hooks `find_renderer()` reads out of
  this executable (the raycaster's Hit routines, ScalePost, the sprite line scaler).
- Widescreen 16:9 / 21:9 (`--widescreen`): the game's WallRefresh and DrawScaleds
  run again turned left and right, every byte restored after.
- Direct3D 11 presenter (`src/present_d3d.c`, Gunman's and Red Alert 2's shader):
  `--scale sharp|smooth|crt|nearest|integer` (F12), colour look (F9), borderless
  fullscreen (F11 / Alt+Enter), `--gdi` fallback.
- `wolf3d.ini` beside the exe keeps the scaling, look, fullscreen and F10 choice.
- Conformance harness for the game, 17/17: boot, play, digitized sound, hi-res,
  widescreen, save and load (`tools/conformance.py`, docs/conformance.md).
- `Setup.cmd` finds Steam's `Wolfenstein 3D\base` (or GOG's) and builds a shortcut.
- `WOLF_MISSDUMP=1`: the bytes at a dispatch miss.
- The art, redrawn by an AI (optional; F6, `--art`): `tools/upscale_art.py`
  runs every wall and sprite page of your VSWAP through Real-ESRGAN at 4x
  (walls tiled 3x3 for seamless wrap, sprites with an upscaled coverage mask)
  into gitignored `work/wl6/art/`. The renderer finds pages in memory by a
  content hash, samples the 256x256 art bilinearly at the exact texel
  position, and fits the live palette so flashes and fades still apply.
  Setup offers it (`-Art`); a conformance check runs when it exists.
- Mouse aim (on by default, F7, `--mouse-aim`, `--mouse-sens`): the mouse turns
  and only turns, captured while the window has focus; scripted sweeps
  (`ms:MX<dx>+<ms>` in `--keys`) and a conformance check for it.
- Optional NVIDIA DLSS on the 3D view (`--dlss`, F8): DLAA, or Super Resolution
  from fewer pixels, fed exact depth and motion vectors from the raycaster's hit
  points and the eye's movement (`src/dlss.cpp`, `dlss_view()`, docs/dlss.md).
  The SDK is never in the repository: `tools/get_dlss.py` fetches it under
  NVIDIA's licence, Setup offers it on NVIDIA GPUs, and CMake builds the DLSS
  path only when it is there. An 18th conformance check runs when it is.

### Changed (from the Blake Stone runtime this started as)
- VGA input status (3DAh): display-disabled now comes in runs of reads, as it does
  through a real horizontal blank. Wolf3D's page flip waits for five in a row.
- The gamepad's directions press the arrows and W/S/A/D, the Steam CONFIG's
  movement keys.
- `--scale` now picks the scaling mode; the window size is `--window N`.
- Setup no longer checks out a different revision in an existing pcrecomp
  checkout; it pins only one it cloned itself.
- The exe links the CRT statically, so it runs without the VC++ redistributable.
