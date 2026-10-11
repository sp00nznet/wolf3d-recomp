# Roadmap

## Next

- **A full playthrough**, all six episodes, by hand. Only E1M1 has been played
  so far; bosses, the other episodes' art and the end sequences are untried.
- **Mouse and gamepad, tested** with real hardware in a window.
- **A mission route in the harness**: walk E1M1 to the elevator with `--route`
  (it works here; `WOLF_POS` gives the coordinates) and check the floor ends.
- **Widescreen at any view size.** Today the side strips need the full-width
  view (Change View all the way up). Smaller views could be widened too by
  treating the view's own edges as the edges of the cast.

## Later

- **DLSS for moving surfaces.** Doors and push walls slide, and walking actors
  move; they get the camera's motion only. Their own motion is in the game's
  state (door positions, actor x/y) and could go into the motion vectors.

- **Sprites between frames.** Sprite heights are the game's integers, so they
  step as they approach. Smoother needs motion the game doesn't report.
- **Spear of Destiny and the shareware/registered 1.0-1.2 builds.** The finder
  reads everything from the code, so other builds may work as they are; they
  need their own Setup entry and a conformance run.
- **The scaler interpreter in pcrecomp.** "Run what the guest generated" is a
  toolkit-shaped mechanism. If a second title needs it, it moves to
  `runtime/recomp16` behind an opt-in, with this one as the first user.

## Out of scope

- Using id's source release (GPL) for anything. This repository is derived
  from the executable only.
- Redistributing the game, its data, or anything lifted from it.
