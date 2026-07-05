# Pararena 2 SDL Port — Full Review & Roadmap

*A thorough review of the `port/` SDL3 port and the original game code it runs,
covering: a design for four-player modes, bugs, general issues, performance,
missing features, visual issues, interface quality, and ideas for delightful
improvements.*

**How this review was done.** Every file in `port/` was read line-by-line, along
with the core original game code (`Dynamics.c`, `Ball.c`, `PlayCore.c`,
`PlayUtils.c`, `Human.c`, `CommonPerson.c`, `Computer.c`, `Render.c`,
`RenderQD.c`, `Replay.c`, `InitGameStructs.c`, `MainWindow.c`, `Main.c`,
`SoundUtils.c`, `Globals.h`) and the original UI files the port replaces
(`Menu.c`, `IdleRoutines.c`, `ConfigureSound.c`, `TeamSetUp.c`, `PlayerStats.c`).
The port was **built and run** in this environment (SDL3 console build,
headless): a full CPU-vs-CPU game was played with frame dumps every 2 seconds
and the frames inspected visually; real-time pacing and CPU load were measured.
The asset pack was decoded and the sprite/parts sheets inspected pixel-level.

Line numbers for `Sources/`/`Headers/` files refer to LF-converted copies
(the originals are CR-only single-line files); they match what the build's
`convert_sources.py` produces.

**Verified working** (from the headless run + frame inspection): arena
rendering, both sprite banks, physics, AI (George vs. Mara played a full game
to 11), scoring and scoreboards with names, foul lights, goal recoloring,
automatic instant replay with the tracking camera window, the ball doors,
star field, Earth, splash screen, 60.00 Hz pacing at 5.5 % CPU / 10 MB RSS.
This port is *good*. The findings below are on top of a very solid base.

---

## 1. Four-player mode — feasibility & design

The centerpiece request: 4-player play in three flavors —
**(A) 2v2**, **(B) free-for-all on the two existing goals with switching goal
ownership**, **(C) free-for-all on four goals** (new arena graphics).

### 1.1 What is hardwired to "exactly two players, exactly two goals"

| Subsystem | Hardwiring | Where |
|---|---|---|
| Entities | `thePlayer` / `theOpponent` globals | `Sources/Main.c:37-38` |
| Game loop | `HandlePerson(&thePlayer); HandlePerson(&theOpponent)` | `Sources/PlayCore.c:676-678` |
| Person-person collision | one hardcoded pair | `Sources/Dynamics.c:204-252, 307-346` |
| Ball possession | `whosGotBall` ∈ {none, free, player, opponent}; `theBall.modifier` ∈ {playerHolding, opponentHolding, …LastHeld} | `Headers/Globals.h` |
| Scoring | `playerScore`/`opponentScore`, duplicated `DoPlayerScores`/`DoOpponentScores` | `Sources/PlayUtils.c:560-769` |
| Fouls | `HandlePlayerFoul`/`HandleOpponentFoul`, foul attribution by `selector` | `Sources/PlayUtils.c:774-820`, `Sources/CommonPerson.c:359-404` |
| Goal → scorer mapping | ball x-sign picks goal; `leftGoalIsPlayers` picks scorer | `Sources/Ball.c:363-384` |
| Goals in physics | goals are `kGoalPath` cells **baked into the force tables**, gated by `zPos >= 0` | `Sources/Ball.c:254-259`, `Sources/CommonPerson.c:207-219` |
| Renderer | exactly 2 sprites, z-order via boolean `playerInBack` | `Sources/RenderQD.c:136-254`, `Sources/Render.c:105-114` |
| Sprite banks | 2 banks (`playerSrcRects`/`opponentSrcRects`), selected by goal side | `Sources/Render.c:23-49` |
| HUD | 2 scoreboards (`scoreDisplays[2]`), 2 held-ball timers, 2 possession arrows, 2 name plates | `Sources/RenderQD.c`, `Sources/PlayUtils.c:339-493` |
| Replay | `frameType` records exactly player+opponent | `Headers/Globals.h` (`frameType`), `Sources/Replay.c` |
| AI | all six personas read `thePlayer`/`theOpponent` globals as "me vs. the other" | `Sources/Computer.c` (45 references) |
| Input | one merged deflection source, one gamepad | `port/shim/shim_input.c:28,84` |
| Team setup | `WhosOnFirst()` places 2 entities | `port/src/port_shell.c:42-129` |

That looks like a wall, but three architectural facts make 4P very tractable:

1. **The physics/motion primitives are already generic.** `MovePerson`,
   `HandlePersonWallCollision`, `PersonRectFromPosition`, `CheckPersonBallCollision`,
   the beam-in/out state machines — all take `playerType *who`. (Amusingly, the
   `selector` branches inside `ResetPerson`/`DoPersonBeamingIn/Out` are
   *identical in both arms* — they're already entity-agnostic in effect.)
2. **A second-human input path already exists.** `kNetHuman` personas are driven
   by `ProcessNetPlayerInput()` from pre-filled `hMouse/vMouse/buttonIs/
   brakeApplied/bashApplied` fields (`Sources/Human.c:158-245`,
   `Sources/CommonPerson.c:329-332`). Local multiplayer = filling those fields
   from a second/third/fourth device instead of from AppleTalk.
3. **The port already has the precedent of superseding files** (Initialize.c,
   TeamSetUp.c, …) without touching `Sources/`. A 4-player engine can be a
   **new, additive port file** (`port/src/port_four.c`) that reuses the verbatim
   primitives, leaving the classic 1v1 path byte-identical.

### 1.2 Common foundation (all three modes)

- **Entities.** `playerType entities[4]` in the port; seats 0/1 alias
  `thePlayer`/`theOpponent` so verbatim helpers keep working. Each seat has:
  persona (human/George/…/Teak), input slot, team, color.
- **Input slots** (`port/shim/shim_input.c`). Generalize to 4 slots:
  slot 0 = mouse+keyboard (current merged model), slots 1–3 = gamepads in
  connect order. Each slot produces {deflection h/v, button, brake, bash}.
  The current single-pad code becomes slot 1. Menu assigns seats → slots.
- **Per-entity human forces.** A port-side `ApplyHumanForces(entity)` modeled
  on `ProcessNetPlayerInput` (it is exactly this, minus the globals),
  so any seat can be human. AI seats keep using the persona code (below).
- **AI reuse without touching Computer.c.** The personas read the
  `thePlayer`/`theOpponent` globals directly, always as "target vs. me".
  The port loop runs each AI seat through a *pair view*: save both globals,
  copy the seat into `theOpponent` and its chosen target into `thePlayer`,
  map ball-ownership globals (`whosGotBall`, `theBall.modifier`) into the
  2-player vocabulary from that seat's perspective, call `XxxDecides(&theOpponent)`
  verbatim, copy the seat back, restore. All six personalities work unmodified
  in 4P. Target selection: the enemy ball-carrier if any, else nearest enemy;
  in 2v2, partners de-duplicate targets so they don't stack.
- **Collisions.** Generalize `DoPersonPersonCollided` to take two entity
  pointers (it's ~50 lines of symmetric math, `Sources/Dynamics.c:204-252`) and
  run all 6 pairs; ball-person collision loops the 4 entities with a port copy
  of the ~40-line attribution branch.
- **Possession.** `portBallOwner` ∈ {-1..3} replaces the 2-value modifier;
  a small mapping keeps the legacy globals coherent for any verbatim code still
  watching them (arrows, timers).
- **Renderer.** `RenderScene4` modeled directly on `RenderSceneQDC`
  (`Sources/RenderQD.c:136-254`): union dirty rects for 4 entities, z-sort by
  `zPos` (farthest first — generalizes `playerInBack`), CopyMask each sprite,
  blit dirty unions to screen, plug holes from the back buffer. Mechanical.
- **Four distinct player colors from two sprite banks.** The parts sheet has
  two banks (green-cap/yellow-shirt and blue-cap/pink-shirt), pixel-aligned
  with one shared mask sheet — teammates share a bank in 2v2 (free!). For FFA,
  add a `CopyMaskRemap` blit to the shim (8bpp indexed pixels → a 16-entry
  per-entity index remap recolors shirts/caps at zero cost). Player colors
  double as goal-ownership colors in mode B.
- **Replay.** The `frameType` ring buffer records 2 entities. v1: disable
  replay in 4P modes (honest, low-risk). v2: parallel port-side ring for
  seats 2/3 + a port replay renderer.
- **HUD for 4.** The two big scoreboards remain for team/lead scores; per-player
  chips for FFA drawn with the parts-sheet **colored digit banks** (the sheet
  carries digit strips in several colors at x ≥ 320 — made for this) plus
  8×8 labels. Held-ball timer: draw the shrinking bar next to the carrier's chip.

### 1.3 Mode A — 2v2 (smallest step, biggest payoff)

- Teams = goal sides; seat 0/1 → left team, 2/3 → right team (menu-assignable).
- **Scoring reuses the existing 2-team path unchanged**: `DoPlayerScores` /
  `DoOpponentScores`, scoreboards, arrows, foul lights, game-over hoopla all
  already think in "left team vs right team". Foul/possession attribution maps
  entity → team.
- Sprite banks = team colors (exactly what the art was drawn for).
- Rules identical to standard game (11 points, win by 2, 3 fouls = point).
- New rules choice: friendly-fire bash on teammates? (Default: yes — it's funnier,
  and Otto will hit you anyway.)
- Difficulty: **moderate** — the engine loop, input slots, renderer, and
  pair-view AI are the work; scoring/HUD are nearly free.

### 1.4 Mode B — FFA, two goals, switching ownership

- `perPlayerScore[4]`; the two goals each have a **beneficiary** seat that
  rotates (suggestion: every 15 s staggered so both goals never switch at once,
  *plus* immediately after that goal is scored on). A goal scores **for its
  current beneficiary**, whoever throws the ball in — so you attack "your" goal
  and body-check anyone near the other one. 3-second warning flash before a switch.
- **Visual language already exists**: `UpdateGoalPicts` paints the goal bands
  by polygon region (`Sources/MainWindow.c:136+`) — repaint the band in the
  beneficiary's player color on each rotation; the possession arrows point at
  "your" current goal.
- Win: first to 7 (FFA games are faster); everyone-beams-out finale, winner
  gets the hoopla.
- Difficulty: **moderate**, entirely port-side once the Mode A foundation exists.

### 1.5 Mode C — FFA, four goals (needs new graphics)

- **Physics:** goals live in the force table as `kGoalPath` cells at the ±x rim,
  gated by `zPos >= 0` (`Sources/Ball.c:254-259`). The table is one quadrant
  mirrored by sign (`[41][41][2]` over |x|,|z|) — so adding goal cells at high
  |z| near x≈0 gives north *and* south goals automatically. Patch the table at
  asset build time (a ~20-line addition to `build_assets.py`, new FORC id), and
  a port-side `DoBallRolling4` replaces the `zPos < 0 → out of bounds` gate
  with a 4-way (x-sign, z-sign) goal decision. Persons treat the new goal
  paths exactly like the existing ones (rebound logic is already sign-symmetric).
- **Graphics:** the honest plan is the same trick the game itself uses — the
  E/W goal "art" is largely *painted* over the arena PICT as polygon-region
  bands + backboard, plus small door/hoop parts. N/S goals: paint perspective-
  correct bands along the near and far rim (far rim band sits right where the
  ball doors are — the doors move to the E/W rim corners in this mode, or the
  ball simply beams in at center, which the engine already supports via
  `kBallInStasis`). Iterate with the frame-dump harness until it reads well.
- Goals owned by fixed seats (one each), tinted in player colors. Own-goal =
  point for everyone else? No — simplest scoring: ball in goal *g* scores for
  the thrower unless it's their own goal (then last other toucher, else nobody,
  ball resets). Defend yours, raid the others.
- Difficulty: **larger** — the force-table patch and goal logic are mechanical;
  making the N/S goals *look right* is the real work and needs visual iteration.

### 1.6 Staging & risk

1. **PR 1 — foundation + 2v2**: input slots, `entities[4]`, engine loop,
   `RenderScene4`, pair-view AI, menu "MODE: 1V1 / 2V2 / FFA" + seat rows.
2. **PR 2 — FFA-2 with switching goals** (builds on 1): per-player scores,
   beneficiary rotation, goal recolor, chips HUD, sprite remap for 4 colors.
3. **PR 3 — FFA-4**: force-table variant, 4-goal ball/person logic, arena art
   pass, door/beam-in change.

Highest-risk unknowns: (a) pair-view AI fidelity — personas may behave oddly
when their implicit "the other guy" changes identity mid-possession (mitigate:
re-target only when the ball changes hands); (b) N/S goal art quality; (c)
replay parity in 4P (deferred deliberately).

---

## 2. Bugs

Confirmed by code-reading (and where possible by running the game). Ordered by
severity.

### B1. Tournament cannot be aborted — the suppressed alert always answers "No thanks" ⚠
`Alert()` in `port/shim/shim_os.c:136-141` returns button 1 for every alert.
In `DoCommandKey` (`Sources/PlayCore.c:60-92`), button 1 of the abort-game
alert is `kNoThanksButton` — i.e. *don't* abort. Net effect: during a
tournament, Esc/Ctrl+E flushes the sound queues, redraws the screen… and
resumes play. You can never concede a tournament (except by quitting the whole
app, which skips `UpdateStats`). `port/README.md:79` claims the opposite
("tournament-abort asks no confirmation"). **Fix:** per-alert-ID default in the
shim (`rAbortGameAlertID` → return 2), or a real port-side confirm overlay;
correct the README.

### B2. Pause can instantly self-resume while Tab is held
Game code sets `pausing = TRUE` when Tab is *down* (`Sources/PlayCore.c:182`);
the port's pause screen then resumes on `keyPressedOnce(SDL_SCANCODE_TAB)`
(`port/src/port_shell.c:293`). `keyPressedOnce`'s static `prev[]` was last
updated in *menu* mode, so the still-held Tab reads as a fresh press on the
first pause frame → pause/unpause bounce (sound stops/starts, full redraw).
**Fix:** prime the edge-detector state on entering pause (require a release
first).

### B3. Menu ignores saved preferences (settings amnesia)
`LoadThePreferences()` restores `whichGame`, `theOpponent.persona`, `isLeague`,
`leftGoalIsPlayers`… but the menu's `selGame/selOpponent/selLeague/selSide`
are compile-time constants (`port/src/port_shell.c:26-29`) and `startGame()`
overwrites the loaded globals from them. So game type, opponent, league, and
side never actually persist across launches (sound on/off does). Bonus wart:
the hardcoded default opponent is **Ms. Teak (index 5), the hardest persona**,
while the game's own default is Simple George (`Sources/InitGameStructs.c:45`)
— brutal first-run experience. **Fix:** seed menu state from the loaded
globals at startup.

### B4. Latent heap over-read: `CopyMask` never clips against the mask bitmap
`port/shim/shim_qd.c:89-111` clips src and dst but not `maskBits`. The color
mask sheet is 320×264 while the parts sheet is 400×264, and mask rects are
taken from *parts-sheet* coordinate tables (`Sources/Render.c:41-48`). Today
every masked sprite happens to live at x < 320 so nothing breaks — but any new
sprite (e.g. 4P work, or the balloon at different coords) placed past x=320
silently reads out of bounds. **Fix:** clip against mask bounds too (5 lines).

### B5. Window contents not restored after expose/un-minimize (idle/splash)
Presents happen only when the 8bpp screen is dirty (`ShimPresentIfDirty`,
`port/shim/shim_input.c:258-269`). No `SDL_EVENT_WINDOW_EXPOSED` handling
exists, and while the splash sits idle nothing marks the screen dirty — on
compositors that don't retain window contents you get a stale/black window
until the next twinkle. **Fix:** handle expose events with `ShimForcePresent()`.

### B6. Tearing possible — renderer created without vsync
`SDL_CreateRenderer` (`port/src/port_video.c:26`) never calls
`SDL_SetRenderVSync`. Presents are capped at ~120/s but land at arbitrary
scanout phase; on most desktops this shows as occasional tearing in fast play.
Because `Ticks` is wall-clock-derived, enabling vsync cannot slow the game
loop. **Fix:** enable (adaptive) vsync; keep the 120/s cap as fallback.

### B7. Silent tournament→standard conversion (suppressed dialogs)
`CantRepeatTournament`/`MustProLeagueTournament` (`Sources/PlayUtils.c:971-1008`)
return TRUE under the always-button-1 stub, which the game interprets as
"convert my tournament to a standard game". Combined with no stats viewer, a
player who replays a beaten persona gets a standard game with zero explanation.
**Fix:** with B1's per-ID table, choose the informative default; better, show a
port-side toast ("TOURNAMENT VS. CLAIRE ALREADY WON — PLAYING STANDARD").

### B8. `GetHandleSize()` returns 0 (`port/shim/shim_res.c:80`)
Currently only reachable through `DumpPict` (which bails earlier on the file
stubs), but it's a booby trap for any future code that sizes a handle. Return
the tracked allocation size (store size in the handle block).

### Minor / cosmetic
- **B9.** Second and later gamepads are ignored (`shim_input.c:82-90` opens
  only the first) — relevant the moment 4P lands.
- **B10.** Esc in the menu jumps the cursor to QUIT but a second Esc doesn't
  activate it; Return then quits with no confirm (`port_shell.c:326`).
  Surprising combo — make Esc-on-QUIT quit, or add a confirm.
- **B11.** Starting a tournament silently snaps the league row to
  PROFESSIONAL (`port_shell.c:215-216`) — correct per game rules
  (`MustProLeagueTournament`) but unexplained in the UI.
- **B12.** `CopyBits` ignores the `maskRgn` parameter (`shim_qd.c:58-61`); all
  current callers pass the full-screen visRgn so it's harmless today, but
  Replay.c *does* swap visRgn contents around expecting clipping — latent.
- **B13.** SDL3 tarball fetched without `URL_HASH` (`port/CMakeLists.txt:13-15`)
  — unpinned supply chain / non-reproducible builds.
- **B14.** The scoreboard name is always "PLAYER" — `GetChooserName` returns a
  constant (`port/src/port_univ.c:89-93`) and there's no names UI
  (`whichHumanNumber` locked to 1), so all stats accrue to one anonymous slot.

---

## 3. General issues

- **`port_shell.c` is becoming the junk drawer** — menu model, WhosOnFirst,
  announcer, pause. Fine at 400 lines; worth splitting (`port_menu.c`,
  `port_flow.c`) before the 4P work triples it.
- **No tests beyond the CI smoke run.** The frame-dump harness is great — a
  golden-frame comparison (hash N dumped frames of a seeded CPU demo) would
  catch renderer regressions for free. The RNG is already deterministic
  (`randSeed` LCG, `shim_os.c:99-110`); add a `--seed` flag and the whole
  game becomes replayable/testable.
- **CI never builds a desktop (X11/Wayland) Linux binary** — only the console
  build. Add a job with the real deps (`libxcursor-dev` etc.) so "the game
  opens a window on Ubuntu" stays true; optionally upload release artifacts.
- **`--fast N` only accelerates; `--dump` cadence is in ticks** — good; but a
  `--seed` + `--quit-after-game` combo would make scripted verification runs
  cleaner than `--frames`.
- **Docs:** `port/README.md` control table omits Alt+Z (ball teleport cheat —
  see §7!) and misstates tournament abort (B1).

---

## 4. Performance

Measured (this container, Release build, headless dummy video/audio):

| Metric | Result |
|---|---|
| Pacing | 1800 ticks in 30.01 s — **exactly 60 Hz** |
| CPU | **5.5 %** of one core (user+sys) during gameplay |
| Memory | **10 MB** max RSS |

- The `Ticks`-from-wall-clock model (`shim_os.c:37-77`) is the right call: the
  original busy-wait self-paces, the 1 ms / 0.1 ms tiered sleep keeps CPU low,
  and the 200-read yield catches the empty `while (Ticks < …);` spins
  (hoopla, replay). No beat-frequency stutter mechanism exists because ticks
  don't quantize to frames — worst case is a single frame rendered ≤1 tick late.
- Palette expansion is 307 k pixels/present (`port_video.c:50-54`) — sub-ms.
  Not worth optimizing; if ever needed, expand only dirty rows.
- **Input latency:** the frame is composited during the game loop, but the
  *present* happens on the next event pump plus an up-to-8 ms throttle
  (`shim_input.c:263`) — ~half a frame of avoidable latency. Present eagerly
  right after `RenderScene` finishes (hook `HandlePostGraphics`'s dirty state)
  if it ever bothers anyone; with vsync (B6) this also stabilizes.
- **Audio:** callback mixes 4 voices with the mutex held (`shim_sound.c:55-92`)
  — trivial work, no allocation, no priority inversion risk at this size.
  Stream latency is SDL default (~10-20 ms); fine for this game.
- The menu idles at a 100 Hz poll with `SDL_Delay(10)` — fine.
- No per-frame allocations anywhere in the hot path. Replay ring is a single
  ~20 KB block. 👍 No stutter sources found in code or in the frame dumps.

---

## 5. Missing features (original → port inventory)

Verified against the original sources; port README's own list is accurate but
incomplete. **The data layer for most of this already works** — prefs
load/save round-trips every field (`Sources/Main.c:185-276`).

| Feature | Original | Port state | Effort to restore |
|---|---|---|---|
| Player stats dialog | `PlayerStats.c` `DoPlayerStats` — per-name games/goals/fouls/crits/titles | tracked & saved, **no viewer** | moderate (8×8-font panel) |
| World records dialog | `IdleRoutines.c:899` `DisplayRecords` | tracked & saved, no viewer | small |
| Names / 10 profiles | `IdleRoutines.c:569` `ConfigureNames`; stats per name | locked to profile 1, "PLAYER" | moderate |
| Sound volume 0–7 | `ConfigureSound.c` slider (+ keys 0-7) | engine supports it (`SetSoundVol`), **no UI** | trivial |
| Sound sub-toggles | beam / incidental / collision / crowd | globals honored by `SoundUtils.c`, no UI | trivial |
| Replay options | replay-goals / replay-fouls / replay-on-R (`IdleRoutines.c:417`) | loaded from prefs, no UI | trivial |
| Board cursor | `showBoardCursor` pref — the thrust-direction dot; **the best beginner aid in the game** | supported by engine, no UI (default off) | trivial |
| Announcer toggle | prefs dialog | always on, unskippable ~10 s intro | trivial |
| Small arena (512×384) | full data + art shipped in the pack | `arenaSize` hardwired large | moderate |
| Help viewer | `Show_help.c`, PICT pages 3100-3104 (in the pack!) | missing | small-moderate |
| About box | `About.c`, art 1030-1042 in the pack | missing | small |
| Alert dialogs | abort/can't-repeat/pro-league | suppressed with wrong/confusing defaults (B1/B7) | small |
| Game timer display | `colonDest`/timer digits (tournament clock) | present in engine; untested visually | — |
| Network play | AppleTalk master/slave, 36-byte frames | stubbed | large (see §7) |
| Animated beach-ball cursor, menu bar, dialog zooms | Mac chrome | intentionally gone | n/a |
| Replay→PICT dump (Cmd+Opt in replay) | `DumpPict` | file stubs return errors (safe) | small (see §7 — better: GIF) |

---

## 6. Visual & layout review

From pixel-inspection of dumped frames vs. the decoded original art:

- **Faithful**: arena, dish shading, goal bands & league wedges, sprites,
  beam-in dither fades, doors (the white outline at the top-center rim is
  *original art*, verified against PICT 1002 — not a port artifact), score
  digits, foul lights, Earth, star field, splash.
- **Splash→arena dissolve** is a 32-column strip reveal
  (`port_stubs.c:39-63`) vs. the original's pixel-level LFSR dissolve
  (`DissBits.c`). It reads as "wipe", not "dissolve". A faithful LFSR dissolve
  over the 8bpp buffer is ~30 lines and pure delight.
- **Menu panel**: black rect + white double frame + 8×8 text. Functional,
  legible… and the least Pararena-looking thing on screen. See §8.
- **Name plates**: 8×8 font truncated to the 69 px plate (8 chars) vs. the
  original's proportional Geneva — "PLAYER" fits, but custom names will clip
  hard. Acceptable; worth a 5×7 condensed font eventually.
- **"INSTANT REPLAY" blink** renders correctly (white 8×8, blinking) in the
  1:1 tracking camera window — matches original behavior (not a zoom; the
  original's camera is also 192×128 at 1:1).
- **Letterboxing** to 4:3 with nearest-neighbor at integer-ish scales: correct
  choice. (Optional: integer-scale lock to avoid shimmer at odd window sizes.)
- Un-minimize/expose staleness — see B5. Tearing — see B6.

---

## 7. Ideas — novel, cool, delightful, quirky

Grounded in the actual architecture; effort S/M/L. ★ = the three favourites.

1. ★ **Replay GIF export (S/M).** The replay system already re-renders any
   moment; the frame-dump path already palette-expands. Press G during a
   replay → write an animated GIF (16-color palette = tiny, period-perfect
   files). Instant shareable "look at this goal!" artifacts.
2. ★ **Attract mode (S).** 30 s idle on the splash → CPU-vs-CPU demo with
   "PRESS ANY KEY" overlay (the `--cpu-demo` machinery *is* this); any input
   returns to menu. The game demos itself like a 1992 kiosk.
3. ★ **Records & medals cabinet (S/M).** The prefs already persist world
   records + copper/bronze/silver/gold/platinum tournament titles per profile
   — render the trophy shelf nobody has seen since 1992. Platinum (skunk all
   six personas) deserves its glow.
4. **Board-cursor-on by default + first-run coach (S).** `showBoardCursor` is
   the original's own training wheel; enable it for new prefs and draw a
   3-line control hint on the first game start.
5. **CRT/scanline toggle (M).** One post pass on the single streaming texture.
   16 colors + scanlines = instant time machine.
6. **Palette themes (S).** Everything renders through one 16-entry CLUT —
   alternate palettes (System 7 grayscale, vaporwave, Game Boy pea-soup) are a
   64-byte swap. Quirky, zero-risk, very screenshotable.
7. **True LFSR dissolve (S).** Restore `DissBits`-style dissolve using the
   8bpp buffer (see §6).
8. **Seeded daily match (M).** `randSeed` is a deterministic LCG; a
   `--seed`/menu "DAILY" mode gives everyone the same ball fires and AI rolls.
   Share your score with a 6-char code.
9. **Mutators (S each).** Low gravity (scale the `vert`/`forc` tables at
   load), bouncy ball (`kBallDampening`), sudden-death overtime, turbo
   (`shimTickMult=2` exposed as a wink). All data-driven, no engine surgery.
10. **"She learns you" tell (S).** Miss Teak's `teaksThresh` literally adapts
    to your play and is saved per profile (`Sources/PlayUtils.c:303-305`).
    Surface it: her visor glints when the threshold crosses 50. Players will
    swear she's alive.
11. **Ghost replay in Practice Scorin' (M).** Record your best scoring run's
    entity track (replay ring format), then race a translucent (dither-mask)
    ghost of yourself — the fade-mask sprites already exist.
12. **Net play revival (L).** The original protocol is documented
    (MODERNIZATION §2.8): 36-byte master frame / 8-byte slave frame,
    master-authoritative — a natural fit for UDP + rollback-free lockstep at
    60 Hz. The `kNetHuman` plumbing survives in the compiled code.
13. **Announcer skip + crowd shouts (S).** Click/button skips the intro;
    the unused "Beer man!"/"Hot dog!"/"Program!" incidentals get rarer
    variants at high scores.
14. **Colorblind-safe goal identity (S).** With mode B's colored goals, add
    pattern fills (stripes/checks) via the existing `FillRgn` pattern path.
15. **Photo mode (S).** Pause → hide HUD → arrow-key the 192×128 replay camera
    anywhere → PNG. Uses the camera code that already exists.
16. **Speedrun clock (S).** `tournamentTime`/`bestPlatinumTime` are already
    tracked in prefs — show a live tournament timer and PB splits.
17. **Bash cam shake (S).** ±1-2 px blit offset for 6 frames on
    person-person clash. Toggleable; the 1992 kid in everyone smiles.
18. **Window title score (S).** "Pararena 2 — YOU 7 : 5 CLAIRE" while playing;
    free spectator UX when alt-tabbed.

---

## 8. Interface: toward fast, convenient, appealing

**What's good:** launch-to-playing in 2 keypresses; menu draws over the actual
splash/arena art; gamepad navigation works; F11 fullscreen; window opens at
crisp 2×.

**Gaps (ordered by pain):**
1. **Nothing teaches the controls.** The game's whole vocabulary
   (crouch/catch/throw on one button, brake, bash, replay) is invisible. A
   CONTROLS row → one 8×8 panel, plus the board cursor (idea 4), fixes 90 %.
2. **Settings amnesia + hardest-opponent default** (B3) — the single worst
   first-session experience.
3. **No volume control** — only ON/OFF, engine supports 0–7 (§5).
4. **Buried options** — replay/announcer/cursor/sound-detail toggles all load
   from prefs but have no UI. One "OPTIONS" submenu solves it.
5. **Game end is abrupt** — winner beams out, menu reappears, scores gone. A
   result card (final score, fouls, goals, "TOURNAMENT: 3 OF 6 BEATEN") would
   land every match emotionally.
6. **No pause menu** — Tab shows text only; Esc semantics differ in-game vs.
   menu (B10). Pause should offer RESUME / END GAME / QUIT.
7. **Menu look**: it deserves the game's own art — the yellow title in the
   arena's letterforms, selection cursor = the little ball sprite, goal-blue
   panel frame. Same 8×8 font is fine; it's the framing that's drab.
8. **Mouse can't drive the menu** even though the game is mouse-first (hover +
   click = select would take ~30 lines).

---

## 9. Prioritized implementation shortlist

Branches implemented from this review (separate PRs, ordered to minimize
overlap):

1. `fix/alert-defaults` — B1 + B7 + README correction (shim_os.c).
2. `fix/pause-rearm` — B2 (port_shell.c, pause block only).
3. `fix/copymask-clip` — B4 (+B12 note) (shim_qd.c only).
4. `feat/window-present-polish` — B5 + B6 (shim_input.c, port_video.c).
5. `feat/menu-prefs-and-options` — B3 + volume + board cursor + announcer +
   replay toggles + Simple-George default (port_shell.c).
6. `feat/four-player` — §1 foundation + Mode A (2v2) + Mode B (FFA-2,
   switching goals) + Mode C (FFA-4) staged in one branch (new files
   port_four.c / port_input slots; menu MODE row).

Everything else stays on this list as future work.

---

*Review artifacts: headless build (`-DSDL_UNIX_CONSOLE_BUILD=ON`), 94 dumped
frames of a full George-vs-Mara game, decoded asset pack (all 99 entries),
timing run (1800 ticks / 30.01 s / 5.5 % CPU / 10 MB).*
