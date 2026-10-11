# Contributing

Issues and pull requests are welcome.

## The one rule

**Nothing from the game goes in the repository.** No game files (`*.WL6`,
`WOLF3D.EXE`, saved games, CONFIG), no lifted C (`work/wl6/gen/`), no unpacked
executable, no upscaled art (`work/wl6/art/`), no disassembly listings, nothing
generated from the game. The tool ships; its output is made on each person's
machine from their own copy. Nor does anything from the optional NVIDIA DLSS
SDK or the upscaler (`external/`) go in. `.gitignore` covers these paths; check
`git status` before you commit anyway. Screenshots of the game running are fine.

## Where your code comes from

This repository is MIT, and contributions must be your own work or
MIT-compatible. **Don't write code with copyleft source open.** Reading a GPL
project's code and then writing "your own" version is derivation, whoever does
the typing, and it would put the whole repository under the GPL.

The ones most likely to be open while working on this:

- **id Software's Wolfenstein 3-D source release** (GPL), and everything built
  on it: **Wolf4SDL**, **ECWolf**, and other source ports.
- **DOSBox** and its forks (GPL), and **ScummVM** (GPL).
- **BStone** and the Blake Stone source releases (GPL).

What's fine: the game's own executable read with the pcrecomp disassembler,
hardware documentation and datasheets, file-format descriptions on wikis that
aren't code, and running a GPL program as a black box to compare its output.
Permissively licensed code is fine with its notice kept, recorded in `NOTICE`
with its licence under `LICENSES/`.

If a fix is unusually polished, say where the knowledge came from in the pull
request. Comments in the code say where knowledge came from, honestly ("checked
against DOSBox's output" is fine; "ported from" a GPL project is not).

**AI-assisted contributions** are welcome, provided a human understood and
verified the change.

## Before a pull request

1. Lift and build from clean:
   ```
   py tools\lift.py wl6
   cmake -B build -G "Visual Studio 17 2022" -A x64
   cmake --build build --config Release -- -m
   ```
2. Run the conformance harness and the hardware self-test:
   ```
   build\Release\selftest.exe
   py tools\conformance.py
   ```
   Both must pass, and the count must not drop below the baseline. A new
   behaviour comes with a check that shows it.

## Where a fix belongs

- A bug in the 16-bit decoder or lifter belongs in
  [pcrecomp](https://github.com/sp00nznet/pcrecomp), not here.
- The DOS machine (`src/`) is shared in spirit with the
  [Blake Stone recomp](https://github.com/sp00nznet/blakestone); a fix that
  applies to both is worth a note there too.
- Anything the renderer needs from the game is found in the game's code by
  `tools/lift.py`, never written down as an address.
- A wall you got past goes in `docs/lifting.md`: what it looked like, what it
  was, what moved it.
