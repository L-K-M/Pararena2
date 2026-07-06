# Pararena 2 — SDL port

A native port of John Calhoun's **Pararena 2** (1992, Casady & Greene) to
modern macOS, Linux, and Windows, with game-controller support and
fullscreen. The original game code — physics, AI, ball dynamics, scoring,
the QuickDraw renderer, instant replay, sound sequencing — compiles
**verbatim** from `../Sources`; only the classic Mac Toolbox underneath it
is replaced by a small shim over [SDL3](https://libsdl.org). All art,
sound, and physics data come from the original resource files in this
repository, converted by `tools/build_assets.py`.

![what runs](../Misc/Pararena%20Sample%20Art.png)

## Building

Requirements: a C compiler, CMake ≥ 3.16, Python 3. SDL3 is found via
`find_package` or downloaded/built automatically (on Linux you'll want the
usual SDL build dependencies, e.g. `libx11-dev libxext-dev libwayland-dev
libxkbcommon-dev libegl-dev libpulse-dev` or your distro's equivalent —
see the [SDL Linux README](https://wiki.libsdl.org/SDL3/README-linux)).

```sh
cd port
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build
./build/pararena2
```

The asset pack (`assets/pararena2.dat`) is committed; it can be
regenerated any time from the original `Pararena.project.r` /
`Para Sounds.bin` with `python3 tools/build_assets.py`.

## Controls

| Action | Keyboard / mouse | Gamepad |
|---|---|---|
| Skate (thrust) | mouse movement, or arrow keys | left stick / d-pad |
| Crouch / catch / throw | left mouse button, or X | South (A) |
| Brake | Space | East (B) or right trigger |
| Bash | B, N or M | West (X) |
| Pause / resume | Tab | Start |
| Instant replay | R | North (Y) |
| Sound on/off | S | — |
| End game | Esc (or Ctrl+E) | — |
| Quit | Ctrl+Q / close window | — |
| Fullscreen | F11 or Alt+Enter | — |

The original controls were mouse-only: pointer deflection from screen
center is the thrust vector (the game warps the pointer back each frame).
The port feeds that same deflection from whichever source you use.

## Command-line flags

```
--fullscreen        start fullscreen
--cpu-demo          watch one CPU vs CPU game and exit
--headless          no window (testing/CI)
--frames N          quit after N ticks (testing)
--fast N            run time N x faster (testing)
--dump DIR          write frame_XXXXXX.ppm screenshots
--dump-every N      screenshot cadence in ticks (default 60)
```

## What works / what doesn't yet

Working: full single-player game — all four game types (standard,
tournament, both practice modes), all six AI personas, five leagues, goal
recoloring, scoreboards and fouls, held-ball timers, instant replay
(automatic and on R), crowd/announcer/effect sounds with the original
priority-mixing behavior, star field, splash screen, preferences
(including career stats data), pause, CPU-vs-CPU demo.

Not yet ported: network play (AppleTalk in the original; see
`../MODERNIZATION.md` for the protocol notes), the player-stats /
world-records / names dialogs (stats are tracked and saved, just not
viewable), the Help viewer, the About box, the animated cursor, the
512×384 small-arena mode (all its data and art are in the pack), and most of
the original alert dialogs. The two alerts that matter during play — the
tournament forfeit confirmation (Esc/Ctrl+E) and the repeat-tournament
notice — are shown as an in-game overlay answered with Y/N (or Return/Esc,
or gamepad A/B); the rest are suppressed with sensible defaults.

## How the port works

See `PORTING_NOTES.md`. Short version: every offscreen buffer is 8-bit
indexed color using the Apple 16-color palette; `CopyBits`/`CopyMask`/
regions/pens/`DrawString` are reimplemented on that; `Ticks` is recomputed
from the wall clock on every read so the original busy-wait game loop
paces itself at its designed 60 Hz (and yields the CPU); the binary-only
SMS sound driver is replaced by a 4-voice mixer implementing the semantics
recovered from its disassembly; input synthesizes the original's
mouse-deflection model from mouse, keyboard, or gamepad.

## License

The original source and assets are MIT-licensed (see `../LICENSE`,
© 2016 softdorothy). The port code in this directory is MIT as well.
The embedded 8×8 font is public domain (from dhepper/font8x8).
