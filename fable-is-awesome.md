# Pararena 2 SDL Port — Review, Round 2 (Android first)

*A fresh full review of the port as it stands after the round-1 work landed
(four-player modes, menu/options, pause screens, Android target, touch
controls). Focus, per request: **Android/touch bugs first**, then general
issues, performance, missing features, visual issues, interface quality, and
ideas.*

**How this review was done.** Every file under `port/` was read line-by-line
(shim, src, android project, build scripts, workflows), cross-checked against
the original `Sources/` where the port interacts with them (`PlayCore.c`,
`Main.c`, `PlayUtils.c`). The desktop console build was **built and run** in
this environment (SDL 3.4.12 fetched and built from source): the 1v1 CPU demo
and all three four-player demos ran to completion; pacing re-measured. The
**SDL 3.4.12 source itself was inspected** to verify the Android-specific
claims below (immersive-mode plumbing in `SDLActivity.java`, touch coordinate
normalization in `SDL_androidtouch.c`, the letterbox math in
`SDL_RenderCoordinatesFromWindow`, hint availability). The touch-coordinate
findings were worked through numerically for real phone aspect ratios (16:9
and 20:9 examples below).

**Current state**: the desktop port is in very good shape — 60.00 Hz pacing at
~6 % of one core, 1v1 + all three 4P modes verified headless, the round-1
fixes (alert overlays, pause re-arm, CopyMask clipping, vsync, expose
handling, prefs seeding, volume/options menu) all confirmed in the code.
**The Android build, however, ships with its default control scheme broken on
essentially every real phone** — the root cause is a single coordinate-space
bug, compounded by several touch-only dead ends. Details below.

---

## 1. Android / touch — the headline problems

### A1. ⚠⚠ Touch hit-testing ignores the letterbox — the default control scheme is broken on every non-4:3 screen

The game renders 640×480 through
`SDL_SetRenderLogicalPresentation(..., SDL_LOGICAL_PRESENTATION_LETTERBOX)`
(`port/src/port_video.c:49`), so on a phone the 4:3 image is **pillarboxed**
with wide black bars. But every touch consumer maps normalized *window*
coordinates straight to 640×480 as if the window were the game surface:

- `MC_HIT(nx, ny, ...)` computes `nx * 640` (`port/src/mobile_controls.h:32`)
  — used for the on-screen stick, catch/brake buttons, and pause button, in
  both the event path (`touchDown`, `port/shim/shim_input.c:92`) and the
  per-frame polled path (`ShimPumpEvents`, `shim_input.c:261-293`);
- `menuTapHit` computes `nx * screenWide` (`port/src/port_shell.c:749`);
- the pause screens' tap-left/tap-right check uses raw `tapX`
  (`port_shell.c:825`, `port_four.c:1637`).

Worked examples (landscape, image height fills the screen so **y is correct,
x is shifted by the bar width**):

- **2400×1080 (20:9)**: image occupies x ∈ [480, 1920]. The stick is *drawn*
  at window x ≈ 674, but its *hit zone* is centred at window x ≈ 323 — in the
  left black bar. The catch/brake hit zones land at x ≈ 2122+ — in the right
  black bar. **Touching the drawn widgets does nothing; touching the black
  bars plays the game.**
- **1920×1080 (16:9)**: image x ∈ [240, 1680]. Touching the drawn stick
  *centre* barely registers — but reads as ~+2.0 deflection, i.e. **resting a
  finger on the stick centre skates hard right**. The catch/brake buttons
  miss entirely.
- The **pause button works** on any aspect — it sits at x = 320 = the screen
  centre, where the letterbox offset cancels, and y is unscaled. (This is why
  "only the pause button works" on a phone.)
- The **menu mostly works** by luck: rows are hit-tested over the full panel
  width and y maps 1:1, so taps land — but the mapping is still wrong in x,
  and any window that isn't full-height-4:3 (desktop `--mobile`, resized
  window, F11 on a non-4:3 monitor) breaks it completely.
- The **SWIPE scheme happens to work** (its tests are "left half of the
  window" / "upper/lower right half"), which is ironic since ON-SCREEN is the
  default.

**Fix** (small, one place): convert window→logical once at the input boundary
with `SDL_RenderCoordinatesFromWindow()` (verified present in SDL 3.4.12, and
verified *not* to clamp — bar touches map outside 0..640, which the half-screen
tests should tolerate). Feed logical coordinates to `MC_HIT`, `menuTapHit`,
the tap-left/right checks, and the swipe split. This is the single highest
-value fix in the port.
**→ Resolution:** _pending_

### A2. ⚠ CONTROLS screen soft-locks a touch-only device

`ShowControlsScreen` waits for `controlsDismiss()` (`port_shell.c:432-472`),
which polls **keyboard and gamepads only**. On a phone with neither, tapping
the menu's CONTROLS row wedges the app on the controls card forever — taps
(`tapFresh`) and the Back button (`backEdge`) are not consulted. The only exit
is killing the app. **Fix**: dismiss on a fresh tap and on Back; while here,
show touch-appropriate content on mobile (the card currently shows a gamepad
bitmap and a keyboard legend to phone users).
**→ Resolution:** _pending_

### A3. ⚠ The tournament alert overlay can't be answered by touch

`alertModal` (`port/shim/shim_os.c:162-242`) answers to Y/N/Return/Esc and pad
A/B. On Android in the menu (not in play mode), touch sets none of these —
starting a TOURNAMENT against an already-beaten persona pops the
"tournament not available" alert (ID 1011) that **cannot be answered on a
touch-only device**: another soft-lock. **Fix**: hit-test taps against the two
answer lines (or top/bottom halves), and treat Back as "no".
**→ Resolution:** _pending_

### A4. ⚠ Pause screen: tapping where the pause button sits can end the game

Both pause loops implement "tap left = resume, tap right = end game"
(`port_shell.c:823-829`, `port_four.c:1635-1641`). The always-visible pause
button is centred at x = 320 — exactly the 0.5 boundary. A player who taps
the pause button to pause and then **taps it again to unpause has ~50 %
odds of instantly ending the game** (the tap-handling branch runs before the
"press pause again to resume" key logic in the 4P loop, and the 1v1 loop has
no pause-button path at all). More broadly, half the screen is a
no-confirmation "end game" button. **Fix**: treat taps on/near the pause
button's position as *resume*, and keep tap-right = end for the rest (it's
what the pause art documents). A stale-tap leak belongs to the same family:
a tap that lands during play (missing all controls) stays latched in
`tapFresh` and fires into the menu after the game ends.
**→ Resolution:** _pending_

### A5. ⚠ No app-lifecycle handling: clock jumps and prefs that never save

Nothing handles `SDL_EVENT_DID_ENTER_BACKGROUND` / `WILL_ENTER_FOREGROUND` /
`TERMINATING` (`handleEvent`, `shim_input.c:136-212`). Consequences on
Android:

- **Ticks are wall-clock-derived** (`shim_os.c:38-60`), and SDL blocks the app
  thread while backgrounded (`SDL_ANDROID_BLOCK_ON_PAUSE`, default on). Take a
  phone call mid-game and on return `Ticks` has jumped by minutes: the
  tournament game clock (`timeElapsed = Ticks/60 - baseTime`) eats the whole
  gap, held-ball/idle tick waits fire instantly, and the game effectively
  fast-forwards. Nothing rebases the clock.
- The game is **not auto-paused** when the app loses the screen — you come
  back to a live goal against you.
- **Preferences, stats, and settings only save on menu QUIT**
  (`DumpTheDefaults` runs when `main()` returns). Android users never "quit";
  the process is killed by the OS or swiped away — so **on a phone, nothing
  is ever saved** (volume, options, career stats, world records, Teak's
  learned threshold — all lost).

**Fix**: on background — record the time, auto-pause if playing
(`pausing = TRUE; timePaused = Ticks`, exactly what `DoPausing` does), and
save prefs; on foreground — advance the tick epoch by the gap so `Ticks`
stays continuous.
**→ Resolution:** _pending_

### A6. Navigation bar stays visible — immersive mode never engages

SDL only enters Android immersive fullscreen (hiding the nav bar / gesture
pill) when the window has the fullscreen flag —
`Android_SetWindowFullscreen → COMMAND_CHANGE_WINDOW_STYLE`
(verified in SDL 3.4.12's `SDLActivity.java:920-941`). The port creates its
window without it: `PortVideoOpen(640, 480, fullscreen)` gets
`fullscreen = 0` on Android because the flag only comes from the `--fullscreen`
CLI argument (`port_main.c:46-52,126`), which SDLActivity never passes. Result:
the system bars shrink the (already letterboxed) play surface and sit right
where thumbs rest. **Fix**: default `fullscreen = 1` under `__ANDROID__`.
**→ Resolution:** _pending_

### A7. Touch exposes no BASH — a core move is unreachable on a phone

The touch schemes provide skate, catch/throw, and brake. **Bash — the lunge /
tackle, a third of the game's vocabulary — has no touch mapping at all**
(`android/README.md` admits it: "adds *bash* … which touch doesn't expose
yet"). FFA against Otto without bash is basically unarmed. **Fix**: add a bash
button to the on-screen cluster (the bottom-right has room for the classic
two-button diamond).
**→ Resolution:** _pending_

### A8. The announcer intro is ~10 s, unskippable, and blocks input

`DoOpeningAnnouncer` (`port_shell.c:976-1007`) plays six clips back-to-back
with `Delay()` between them — ≈ 9.7 s of dead time before *every* game (also
in 4P). There's an ANNOUNCER toggle buried in Options, but no tap/key/button
skip. On a phone this reads as a hang. **Fix**: end the intro early on any
tap/click/button; keep the toggle.
**→ Resolution:** _pending_

### A9. Mobile UX papercuts (smaller, still worth fixing)

- The **pause art documents only the SWIPE scheme** ("Drag the LEFT half …")
  even when ON-SCREEN (the default) is active — first-run users are taught
  controls they don't have. The controls card, conversely, never shows touch.
- Menu rows are ~19 logical px tall (~7 mm on a typical phone) — below the
  ~48 dp Android guidance; value rows only cycle *forward* on tap; two-tap
  activation ("tap to select, tap again to choose") is written on-screen but
  still surprises.
- Options shows **FULLSCREEN** and **REPLAY KEY R** rows on a phone, where
  they're meaningless; TOUCH CONTROLS shows on desktop where it does nothing.
- The `VIBRATE` permission is declared (`AndroidManifest.xml:14`) but nothing
  ever vibrates — bash/goal/foul haptics are begging for it.
- No keep-screen-on: watching a replay or a CPU demo long enough lets the
  screen sleep.
- FFA fouls (`fouls4`) are tracked and punished (`port_four.c:1011-1034`) but
  **never displayed** — a critical foul silently deducts a point with only a
  mob sound as a clue. The 4P chips have room for foul pips.

---

## 2. Other bugs

- **B1. Ending a tournament from the pause screen skips the forfeit
  bookkeeping.** The pause-screen END path sets `primaryMode = kIdleMode`
  directly (`port_shell.c:834`), while the Ctrl+E path records
  `playerWonTheGame = kOpponentWon; UpdateStats(); UpdateWorldRecords()`
  (`Sources/PlayCore.c:72-82`). Two abort paths, two different career-stat
  outcomes. (Original-menu parity argues it's mild; the inconsistency is the
  bug.)
- **B2. `GetHandleSize()` still returns 0** (`shim_res.c:83`) — carried over
  from round 1; a booby trap for future handle-sizing code.
- **B3. `CopyBits` still ignores `maskRgn`** (`shim_qd.c:58-61`) — latent;
  `Replay.c` swaps visRgns expecting clipping. Harmless today.
- **B4. Supply chain still unpinned**: the SDL3 tarball is fetched without
  `URL_HASH` (`port/CMakeLists.txt:16-17`) and `android/fetch-deps.sh`
  downloads it with no checksum either — neither build is reproducible, and a
  compromised mirror would go unnoticed.
- **B5. The scoreboard name is still the constant "PLAYER"**
  (`port_univ.c:89-93`); all stats accrue to profile 1. (Round-1 leftover.)
- **B6. Stale `keyPressedOnce` state across mode transitions** is handled for
  the pause loops (armed/release patterns) — verified fine — but the menu's
  edge detector still shares one static array across pages; harmless today.

Round-1 bugs **verified fixed** in the current tree: tournament abort alert
(now a real overlay), pause instant self-resume (armed/release), CopyMask
mask-bounds clipping, expose/vsync handling, settings amnesia + Ms. Teak
default, second-gamepad support.

---

## 3. General issues

- **`port_shell.c` (1007 lines) and `port_four.c` (1880 lines) are the new
  junk drawers** — menu model, pause loops, announcer, touch drawing, and
  WhosOnFirst in one file; engine + AI-pair-view + HUD + goal painting in the
  other. A `port_touch.c` (all touch reading/drawing) would have prevented
  the A1 class of bug from spreading across three files.
- **The touch pipeline has two parallel implementations** — the event path
  (`touchDown/touchMotion/touchUp`) and the per-pump polled path
  (`SDL_GetTouchFingers`) — with subtly different rules (the polled path
  allocates and frees the device/finger lists every pump at 60+ Hz, purely to
  re-derive state the event path already saw). One owner would be simpler and
  cheaper.
- **No automated check covers touch at all** — CI runs headless demos only
  (`.github/workflows/build.yml`), which is exactly why A1 shipped. A tiny
  unit test of the window→logical mapping (pure math) would have caught it.
- **CI never builds a desktop windowed Linux binary** (console build only)
  and never builds the Android target on push (only in `release.yml`).
- Docs drift: `port/README.md` says pausing works "the same in every mode"
  (true) but the Android README's promise that "menus are driven by direct
  taps" is only true on 4:3 windows today (A1).

---

## 4. Performance

Re-measured this round (Release console build, headless):

| Metric | Result |
|---|---|
| Pacing | 1800 ticks in 30.01 s — **60.00 Hz** |
| CPU | ~5.9 % of one core (user+sys) |
| Sim + demos | 1v1 and all three 4P demos ran clean at `--fast 8` |

- No stutter mechanisms found in code; the tick model self-corrects.
- The **polled touch path** (`shim_input.c:263-289`) calls
  `SDL_GetTouchDevices` + `SDL_GetTouchFingers` (two mallocs + frees) every
  pump — that's every `GetKeys`, i.e. hundreds of times/second during play on
  Android. Trivial per call but pure waste; fold into the event path (see §3).
- `PortVideoPresent` expands the full 640×480 every present — fine on phones
  (sub-ms), not worth touching.
- The Android build compiles only `arm64-v8a` — fine as a default; noted in
  the README.

---

## 5. Missing features

Since round 1, the port gained: volume + options UI, board-cursor and
announcer and replay toggles, prefs seeding, alert overlays, 2v2/FFA-2/FFA-4,
controls card, pause screens, app icons, `--dist` packaging, and the Android
target. Still missing (unchanged from round 1 unless noted):

| Feature | State | Effort |
|---|---|---|
| Bash on touch | **missing entirely** (A7) | small |
| Player stats / world-records viewers | data tracked & saved, no UI | small–moderate |
| Names / 10 profiles | locked to "PLAYER" | moderate |
| Help viewer / About box | art is in the pack | small |
| Small arena (512×384) | data + art shipped, hardwired large | moderate |
| Replay in 4P modes | deliberately disabled | large-ish |
| Net play | stubbed | large |
| Replay→GIF export | round-1 idea, still open | small–moderate |
| Attract mode | `--cpu-demo` machinery exists | small |
| True LFSR dissolve | still the 32-column strip reveal | small |

---

## 6. Visual & layout

- **Desktop**: faithful; letterboxing + nearest-neighbour still correct.
  The strip-reveal "dissolve" (`port_stubs.c:39-63`) remains the one obvious
  divergence from the original's LFSR dissolve.
- **Android**: the 4:3 image on a 20:9 phone leaves ~40 % of the display as
  dead black bars *while the on-screen controls overlap the play area and the
  corner scoreboards*. The natural phone layout is the opposite: play field
  clear, controls in the bars. That means drawing the touch widgets at the
  window layer (after `SDL_RenderTexture`) instead of into the 8bpp buffer —
  a contained change to `PortDrawMobileControls` + `port_video.c`, and it
  would also stop the widgets from being captured into `--dump` frames and
  overdrawn by replays.
- The on-screen widgets are 1992-styled (good instinct) but the stick's
  46-px-diameter thumb travel is small on a phone; consider growing it ~1.5×
  on mobile once coordinates are fixed (A1) so fine steering is possible.
- The P800 pause art is a delight. It deserves a variant that matches the
  ON-SCREEN scheme (A9), or a small dynamic legend line.
- 2v2 keeps the classic scoreboards; FFA chips are clear. Foul pips missing
  (A9). The four goal-band colors are distinguishable, but red-green
  colorblind players get green (P1) vs orange (P3) bands — the round-1
  pattern-fill idea still applies.

---

## 7. Interface quality (fast, convenient, appealing)

Ordered by pain, current state:

1. **Android default controls are broken** (A1) — everything else is noise
   until this lands.
2. **Touch dead-ends** (A2, A3) — two screens a phone user cannot leave.
3. **Destructive tap semantics** (A4) — pausing should never risk the game.
4. **Nothing persists on Android** (A5) — options reset every launch.
5. **First-run teaching**: the controls card teaches gamepad+keyboard even on
   touch; the pause art teaches swipe even when on-screen is active.
6. Game end is still abrupt (round-1 item): winner beams out, menu returns,
   no result card. The 4P path even has a "PLAYER N WINS!" banner — 1v1
   deserves the same.
7. Menu aesthetics: still the plain black panel; the game's own yellow
   letterforms and ball-sprite cursor remain the obvious upgrade.
8. Desktop mouse still can't drive the menu (hover+click) even though the
   game is mouse-first — and now there's a tap path (`menuTapHit`) it could
   share once coordinates are logical.

---

## 8. Ideas — novel, cool, delightful, quirky

Carried forward from round 1 (all still valid): replay GIF export ★, attract
mode ★, records & medals cabinet ★, palette themes, CRT toggle, LFSR
dissolve, seeded daily match, mutators, Teak's visor tell, ghost replay,
net-play revival, colorblind goal patterns, photo mode, speedrun clock, bash
cam shake, window-title score.

New this round, mobile-flavored:

1. **Haptic bash** (S): the manifest already asks for VIBRATE; a 30 ms rumble
   on bash contact / goal / foul would make touch play feel physical.
2. **Controls in the pillarbox** (M): see §6 — the black bars are free real
   estate on every phone; put the stick and buttons *there*, leaving the
   arena unobstructed. No other mobile port of a 4:3 game gets this right;
   this one could.
3. **Tap-anywhere-to-continue result card** (S): after game over, a card with
   the final score + fouls + "TAP TO CONTINUE" — solves §7.6 and gives touch
   players a natural end-of-game beat.
4. **Two-finger tap = pause** (S): a standard mobile gesture; keeps the
   pause button but adds the gesture players try first.
5. **P800 boot splash** (S): the pause art's Sony Ericsson frame, shown for a
   second at launch with the arena "booting" inside it — pure fan service,
   zero risk.
6. **Per-scheme pause art** (S/M): swipe art for swipe mode, on-screen art
   (stick + buttons) for on-screen mode.
7. **Demo attract on the splash** (S): after 30 s idle on the menu, run a CPU
   demo with "TAP TO PLAY" — doubly good on a shop-shelf Android tablet.

---

## 9. Implementation shortlist (this round's branches)

Ordered to minimize cross-branch conflicts; each lands on its own branch.

| # | Branch | Covers | Files | Resolution |
|---|---|---|---|---|
| 1 | `fix/touch-letterbox-mapping` | A1 (+ desktop `--mobile`) | shim_input.c, port_video.c, shim_internal.h, mobile_controls.h | _pending_ |
| 2 | `fix/touch-modal-softlocks` | A2, A3 | port_shell.c, shim_os.c | _pending_ |
| 3 | `fix/pause-tap-safety` | A4 (+ stale-tap leak) | port_shell.c, port_four.c | _pending_ |
| 4 | `fix/android-lifecycle` | A5 | shim_input.c, shim_os.c, shim_internal.h | _pending_ |
| 5 | `fix/android-immersive` | A6 | port_main.c | _pending_ |
| 6 | `feat/touch-bash-button` | A7 | mobile_controls.h, shim_input.c, port_shell.c | _pending_ |
| 7 | `feat/announcer-skip` | A8 | port_shell.c | _pending_ |
| 8 | `fix/handle-size` | B2 | shim_res.c | _pending_ |
| 9 | `fix/pin-sdl-fetch` | B4 | CMakeLists.txt, fetch-deps.sh | _pending_ |

Left for future rounds (lower confidence or larger scope): B1 forfeit-stats
parity, A9 papercuts batch, §6 window-layer controls, foul pips, result card,
stats viewers, small arena, GIF export, net play.

---

*Review artifacts: console build (SDL 3.4.12 built from source), 1v1 +
FFA-2/FFA-4 headless demo runs, 30 s timing run (1800 ticks / 30.01 s /
~5.9 % CPU), SDL source inspection for the Android claims, letterbox math
worked for 16:9 and 20:9 panels.*
