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

### Android

An Android app target lives in `android/` (SDL's `SDLActivity` loading the game
as `libmain.so`, touch controls, asset pack bundled in the APK). It needs a JDK
and the Android SDK/NDK; see `android/README.md`. In short:

```sh
cd port/android
./fetch-deps.sh && ./gradlew assembleDebug
```

## Four-player modes

The MODE row in the menu selects, besides the classic 1v1:

- **2 VS 2** — team play on the classic goals with standard-game rules
  (11 points, win by 2, three fouls concede a point). Teammates share a
  hoverboard color; the two big scoreboards show the team scores.
- **FFA - 2 GOALS** — everyone against everyone. Each goal *belongs* to one
  player at a time (its band is painted in their color) and scores for its
  owner no matter who throws the ball in; ownership rotates every 12 seconds
  (and right after a goal). First to 7 wins.
- **FFA - 4 GOALS** — everyone against everyone with four goals, one per
  player, at the four rim corners (the force table's mirror symmetry
  provides the southern pair). Score into *your* goal; defend it from the
  other three. First to 7 wins.

Player 1 plays with the keyboard/mouse **or the first gamepad**; the P2-P4
seats can be humans on the remaining gamepads (assigned in connect order:
P2 = 2nd pad, P3 = 3rd, P4 = 4th) or any of the six AI personas. Pause with
Esc or the pad's Start button; the pause screen shows a controls legend and
lets you resume or end the game (End = E / the pad's Back button).
Instant replay is disabled in four-player games for now.

`--four-demo N` (1=2v2, 2=FFA-2, 3=FFA-4) watches an all-AI four-player
game, like `--cpu-demo` does for 1v1.

## Bash steals

Bashing the ball carrier at speed knocks the ball loose. The strip is
deterministic — no dice rolls: it happens exactly when a fresh body check
connects with **bash held** and the **closing speed along the line of
impact** is high enough. A committed full-speed lunge strips; a slow
shoulder-rub while you grind along the rim only shoves. The ball pops free
*along the hit direction* — past the carrier, away from the basher — so a
strip starts a loose-ball scramble rather than handing over possession.

The fine print: a fresh catch is protected for about a second; the loose
ball is credited to the **basher**, so a fumble that rolls out of bounds is
the basher's foul, not the victim's; and the engine's collision cooldown
rules out rapid re-strips. Since bash has always been disabled while
carrying, the carrier can't strip back — holding the ball finally costs
something during the 20-second holding clock. (Heavy Otto is now exactly as
dangerous as he looks.)

Bash steals are a port addition, **disabled in CLASSIC MODE** (below). For
tuning or curiosity, `PARARENA_STEAL_LOG=1` logs every carrier impact with
its closing speed and verdict.

## HUD enhancements and Classic mode

In every play mode — including the classic 1v1 game — each human player wears a
small numbered plate ("P1"…"P4") above their skater, and whoever is holding the
ball gets a caret marker above their head, so possession is always readable. If
you'd rather have the untouched 1992 experience, turn on **CLASSIC MODE** in the
Options menu: it hides these overlays everywhere and **turns off bash steals**,
restoring the original rules exactly — it's the switch the port gates gameplay
changes behind, so the classic experience stays classic. The setting persists
across launches.

## Controls

| Action | Keyboard / mouse | Gamepad |
|---|---|---|
| Skate (thrust) | mouse movement, or arrow keys | left stick / d-pad |
| Crouch / catch / throw | left mouse button, or X | South (A) |
| Brake | Space | East (B) or right trigger |
| Bash | B, N or M | West (X) |
| Pause / resume | Esc or Tab | Start |
| Instant replay | R | North (Y) |
| Sound on/off | S | — |
| End game | E from the pause screen (or Ctrl+E) | Back, from the pause screen |
| Quit | Ctrl+Q / close window | — |
| Fullscreen | F11 or Alt+Enter | — |

Pausing (Esc / Tab / Start) works the same in every mode: it brings up the
pause screen that you resume from (Esc / Tab / Start) or end the game from
(E / Back).

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
