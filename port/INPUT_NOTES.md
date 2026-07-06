# Input & Acceleration — Notes and Possible Changes

How player acceleration works today, and some ideas we considered for
controller play. **Nothing here is implemented** — it's a design scratchpad so
the reasoning isn't lost.

## How acceleration works today

Every human seat turns its input into a per-frame "board force" `bx`/`bz` that
is added to the player's velocity. The 1v1/2v2 path is verbatim `Sources/Human.c`;
the four-player path mirrors it in `apply4HumanForces()`
(`port/src/port_four.c`). Both share the same tuning constants:

| Constant | Value | Meaning |
|---|---|---|
| `kMaxBoardForceLg` / `Sm` | 64 / 57 | max board force per frame (large / small arena) |
| `kPlayerInputSensitive` | 3 | input divisor (smaller = more sensitive) |
| `kAddForceFract` | 4 | crouch/button adds `force / 4` (i.e. +25%) |
| `kFrictionFraction` | 128 | velocity lost per frame = `vel / 128` |
| bash multiplier | 3 | bash sets force to `boardForce × 3` |

The mouse frame is sized so its edge lines up just past the cap:
`halfWide = kMaxBoardForce·kPlayerInputSensitive + kPlayerInputSensitive + 19`
(= 214 large), set in `Sources/InitGameStructs.c`.

### The key finding: controller and mouse hit the *same* ceiling

It's tempting to assume the stick is normalized below the mouse (otherwise full
stick would fling you off the rim). It isn't — they cap at the same place:

- **Mouse**: `hMouse = cursorOffset / 3`. The cursor is clamped to the frame
  (±214), so `hMouse` tops out at ~71 → clamped to **64**. The outer ~10% of the
  frame is a saturated dead band.
- **Controller**: `hMouse = (px · 214) / 3` (`ShimPadRead`, `apply4HumanForces`).
  Full stick (`px = 1`) → ~71 → clamped to **64**. Anything past ~90% stick
  already saturates the cap.

Full stick doesn't launch you out of bounds not because it's weaker, but because
the **dish contains it**: the bowl's restoring force plus friction (`vel/128`)
settle you at a stable position partway up the bowl at 64/frame. You only leave
the rim when something exceeds that.

### The three acceleration tiers (same for mouse and pad)

| Tier | Force | Input | Notes |
|---|---|---|---|
| Steer | up to 64/frame | mouse offset / left stick | contained by the dish |
| Boost | ×1.25 (up to 80) | X-key / left-click · **A/✕ (SOUTH)** | applies *after* the clamp; works while carrying |
| **Bash** | `boardForce × 3` (up to **192**) | B/N/M · **X/□ (WEST)** | big directional lunge; **only when not carrying the ball** and not braking |

Gamepad mapping (`ShimPadRead`, `port/shim/shim_input.c`): left stick steer
(0.12 dead zone), **A/✕** = button/boost, **X/□** = bash, **B/○ or right
trigger** = brake.

**So the "risk/reward speed at the cost of flying out" mechanic already exists —
it's bash.** The one real gap: **bash is disabled while carrying the ball**
(`ballOwner != seat`), so a breakaway *with* the ball caps at 64 (or 80 holding
A). That limitation is identical for mouse and pad.

## Possible changes (not implemented)

### A. Analog trigger boost — *preferred if we do anything*

Map the free **left trigger** to a graduated boost that scales the board force
from ×1 up to ~×3 by how far the trigger is pulled, and — unlike bash — **allow
it while carrying the ball**.

- **Pros**: continuous with the existing momentum feel; analog (finer than bash's
  on/off lunge); closes the real gap (fast breaks with the ball); natural "pull
  to boost" mapping.
- **Cons**: overlaps conceptually with bash; right trigger is already brake, so
  keep this on the left. Best framed as "analog bash that also works with the
  ball," not a fourth system.
- **Sketch**: in `apply4HumanForces`, read `LEFT_TRIGGER` in `ShimPadRead`; after
  the clamp, `bx += bx * (trigger * (BOOST_MAX-1))`. Gate behind a tunable
  (`BOOST_MAX`, carry-only on/off) to feel it. Only a shim + `port_four.c`
  change; no `Sources/` edits.

### B. Charge-and-release button

Hold to charge, release for a speed burst proportional to charge time.

- **Pros**: adds a timing/skill layer.
- **Cons**: modal — it fights the continuous-steering feel and the frantic 4-way
  pace (what does the stick do while you wind up?). Better suited to a slower,
  more deliberate game than Pararena.

### C. Cheapest option: make bash discoverable on the pad

Before adding anything, bash (**X/□**) may already be ~80% of what's wanted for
non-carrying bursts. Consider surfacing it in the on-screen help / controls.

### Recommendation

Leave the core as-is. If we revisit this, start with **C** (surface bash), and
if a carry-time burst is still wanted, prototype **A** behind a tunable. Avoid
**B** — it changes the genre feel.

## Pointers

- `port/src/port_four.c` — `apply4HumanForces()` (4-player input → force)
- `port/shim/shim_input.c` — `ShimPadRead()` (gamepad axis/button mapping)
- `Sources/Human.c` — verbatim 1v1/2v2 input path
- `Sources/InitGameStructs.c` — `mouseFrame` / `maxBoardForce` setup
