# Pararena 2 — Modernization Evaluation

An engineering evaluation of this codebase for the purpose of bringing Pararena 2
to modern macOS, Linux, and Windows, with game-controller support and full-screen
display.

**Verdict up front: this is an unusually good porting candidate.** The simulation
is pure integer C that compiles unmodified with gcc/clang today; every 68K assembly
routine has a readable C/QuickDraw equivalent in the same repo; the complete art,
sound, and physics-data assets are present in `Pararena.project.r`; the license is
MIT; and the game's mouse-as-analog-stick input model maps one-to-one onto a
gamepad thumbstick. The recommended path is a **native port over SDL3** that keeps
the original game logic verbatim and replaces the Mac Toolbox layer — the same
strategy proven by ports of John Calhoun's other games (Aerofoil for Glider PRO,
kainjow/Glypha for Glypha III).

---

## 1. What is in this repository

| Component | Contents | Status |
|---|---|---|
| `Sources/*.c`, `Headers/*.h` | ~27,900 lines of THINK C (v2.06/2.07, 1992), CR line endings, Mac Roman text | Complete |
| `Pararena.project.r` | DeRez'd resource file: 248 resources, ~659 KB of data — **all game art (41 PICTs), 25 sounds, physics tables, dialogs, menus, icons, cursors, strings** | Complete (verified: every resource the code references is present) |
| `Para Sounds.bin` | MacBinary resource file: 9 more sounds (crowd shouts + announcer voice) | Complete |
| `Sources/sms.a.π.bin`, `SMSCore.a.π.bin` | THINK C project files containing the **SMS sound driver as compiled 68K code only** (no assembly source) | Binary-only, but fully reverse-engineered below — not a blocker |
| `Pararena.project.bin` | THINK C project file (build metadata only) | Not needed |
| `Misc/` | Version history 2.00–2.07, help-authoring HyperCard stack, box art | Reference |
| `LICENSE` | **MIT** (not GPL — verified) | Very favorable |

The only 256-color assets referenced in headers (PICT 1003/1012) are explicitly
marked `<unimplemented>` — the game only ever renders 1-bit or 4-bit, so nothing
is actually missing.

## 2. Key findings

### 2.1 The game logic already compiles with modern compilers — zero edits

As an experiment, the seven game-logic files (`Dynamics.c`, `Ball.c`,
`Computer.c`, `CommonPerson.c`, `Human.c`, `PlayCore.c`, `PlayUtils.c`,
~5,800 LOC) were compiled with gcc 13 and clang on Linux x86-64
(`-std=c99 -fsigned-char`) using only a ~6 KB shim header defining classic Mac
types (`Rect`, `Point`, `Boolean`, `Str255`, …). **All seven compiled with zero
changes to the source.** The four purest files (physics, ball, AI, shared person
logic — 3,497 LOC) also link and run: a 240-frame smoke test produced sane,
deterministic ball trajectories.

The simulation core touches the OS through exactly *one* variable (`Ticks`, the
60 Hz tick counter, also used as AI randomness via `Ticks % 100`) and one
project-defined sound wrapper (`PlaySoundSMS`). There is **no floating point at
runtime anywhere in the codebase** — physics is 16-bit integer positions/velocities
with 32-bit intermediates (the only float literals precompute a 9-entry table at
init, `InitGameStructs.c`).

### 2.2 The 68K assembly never needs porting

About 8,000 LOC — `RenderAsm1.c` (1-bit blitters), `RenderAsm4.c` (4-bit
blitters), `DissBits.c`, and glue in `Show_help.c` — is THINK C `asm 68000`.
None of it needs translating, because the game ships a **complete pure-QuickDraw
fallback**: the `useQD` preference routes every per-frame operation through
`RenderSceneQD1`/`RenderSceneQDC` in `RenderQD.c` (`Render.c:154-184` dispatch).
The asm files are pure performance duplicates for 1992 hardware. `RenderQD.c` is
the readable specification of the renderer, and the 26 asm routines reduce to
**~6 generic primitives** over a framebuffer:

1. masked sprite blit — `dst = (dst & ~mask) | (src & mask)`
2. opaque rect copy (buffer → buffer)
3. present (work buffer → screen; replaces the direct-VRAM writes via `screenBase`/`screenRowAddrs[480]`)
4. 1-pixel plot (star twinkle)
5. rect fill (letterbox, timer bars)
6. LFSR-ordered dissolve (cosmetic, optional)

The per-frame algorithm is classic dirty-rect compositing: masked-blit cursor,
ball, and both players back-to-front into a work buffer, copy the dirty rects to
screen, then "patch" the work buffer back from the pristine background buffer.
Sprites are fixed-size (players 32×44 in 9 directions × 3 postures, ball 16×12,
door 32×21) in a single 400×264 parts sheet per depth.

### 2.3 The sound system is a closed, solved problem

Sound goes through SMS ("Sound Music System"), a third-party 68K driver present
only as binary. This looked like the biggest risk, so the driver's CODE resources
were disassembled. Everything needed is now known:

- **`SMSD` sample format:** 8-byte header — big-endian uint16 *priority* (byte-for-byte
  equal to the `k*Priority` constants in `Globals.h:215-239`), uint16 *repeat count*
  (`0x7FFF` = loop forever, used by the three crowd loops), 4 reserved zero bytes —
  followed by raw **unsigned 8-bit mono PCM at 22,254.5 Hz** (the classic Mac
  hardware rate; confirmed three independent ways, including the driver's own
  hardcoded rate constant `0x56EE8BA4` and the source's tick-duration comments).
- **Channel arbitration (from disassembly):** 4 voices; a new sound takes the
  highest-numbered free channel; if none is free it steals the lowest-priority
  busy channel whose priority ≤ its own, else it is silently dropped.
  `SMSSTARTCHAN` bypasses arbitration (the game pins crowd loops to channel 3).
- **The game uses only 7 of the 24 SMS entry points** (`SMSINIT`, `SMSEXIT`,
  `SMSSETMODE(4)`, `SMSSTART`, `SMSSTARTCHAN(id,3)`, `SMSSTOP`, `SMSSTOPCHAN(3)`).
  No shipped sound is compressed. All sequencing logic (crowd state machine,
  incidental-sound queue) is portable C in `SoundUtils.c`.

Replacement: a ~20-line converter (strip header → WAV) plus a 200–300 line
4-voice mixer with priority stealing over SDL audio. **One to two days of work.**

### 2.4 Assets are complete and trivially convertible

- All 41 PICTs use only simple opcodes (BitsRect/PackBitsRect/PackBitsRgn) — 1-bit
  or 4-bit indexed with the standard Apple 16-color palette embedded (byte-identical
  to `clut 128`). No QuickTime compression, no direct color. A ~100-line decoder
  written during this audit already rendered the sprite sheet and arenas to PNG.
- Physics data verified at byte level against the code's pointer math:
  `'forc'` = big-endian `int16[41][41][2]` dish force fields (5 leagues × 2 arena
  sizes; leagues differ *only* in goal-arc geometry, force values are identical);
  `'vert'` = `int16[41][81]` projected screen-Y height maps; `'sPts'` = 256 star
  points. The original loader is a raw `BlockMove` — a port needs only
  fread + byte-swap.
- The only compressed resources are the two `'4CMP'` help-text blobs (decompressor
  is a 136-byte 68K `CNVT` code resource). Options: decode the trivial 4-bit
  nibble scheme, pull the text from the HyperCard authoring stack in `Misc/`,
  or simply re-author the help screen.

### 2.5 Input maps perfectly to a controller

The original control scheme *is* a virtual analog stick: pointer deflection from
screen center (clamped to a ~211 px box, pointer warped back each frame via
low-memory pokes) divided by sensitivity 3 and clamped to ±`maxBoardForce`
becomes thrust added to velocity every frame (`Human.c:74-153`). Buttons:
mouse button = crouch/catch/throw; space = brake (−10% velocity per frame);
B/N/M = "bash" (3× directional thrust).

Gamepad mapping is direct:

| Original | Controller |
|---|---|
| Mouse deflection from center | Left stick (scaled to ±maxBoardForce) |
| Mouse button | A — crouch / catch / throw |
| Spacebar | Right trigger or X — brake |
| B/N/M keys | B — bash |
| Tab / R / S | Start = pause, Y = instant replay, etc. |

Even network play carries analog input in its packets (`slaveSendType` sends
`mouseH/mouseV` shorts), so a controller doesn't fight the architecture anywhere.
Mouse (SDL relative mode) and keyboard schemes should be kept alongside.

### 2.6 Full screen is the native mode

The game already runs as a borderless screen-sized window with the fixed play
area (640×480, 512×384, or 512×342 depending on machine) centered and
letterboxed (`MainWindow.c:558-596`). A port renders the fixed logical resolution
to a texture and scales — SDL3's default borderless fullscreen-desktop mode plus
integer or aspect-preserving scaling. Note the two arena sizes are genuinely
different game variants (different dish radius, force tables, art), worth keeping
as a selectable option rather than collapsing.

### 2.7 Timing: target a fixed 60 Hz step

The game loop is hard-throttled: one simulation step per Mac tick
(`kTickDelay = 1` with a busy-wait on `Ticks`, `PlayCore.c`), i.e. **60 Hz by
design**, and the author's constants confirm it (`kLoopLimitOnHeldBall 1200
/* 20 s */` = 60 fps). Physics has no delta-time — a port must use a fixed 60 Hz
timestep with an accumulator, never the display refresh rate. All tuning
constants are correct as-is at 60 Hz. The `doSkipFrames` PowerBook option
(draw every 6th tick, simulate every tick) shows sim cadence and draw rate were
already decoupled.

### 2.8 What must be rebuilt (not ported)

- **UI shell:** 14 modal dialogs + 8 alerts (Dialog Manager with custom filter
  procs), 4-menu menu bar, the drag-and-drop Teams Set-Up screen, About box,
  custom help browser (`Show_help.c`, third-party component), animated cursor.
  A port should replace these with simple in-game screens; this is the largest
  single chunk of new code, but none of it is technically hard, and a first
  playable build needs only a fraction of it.
- **Preferences:** a raw big-endian struct dump (`prefsInfo`, ~1.3 KB: per-player
  career stats, world records, adaptive AI thresholds, option flags) written to
  `Para Prefs` at quit. Re-serialize to a plain file; keep the fields — the
  10-profile stats/records system is a nice feature.
- **Copy protection:** `ValidInstall.c` is key-disk protection (boot-volume date
  XOR mask, master-floppy timestamp, a Nov-9-1965 date backdoor). Failure locks
  the game to network-only play. **Delete entirely**; hardcode `netOnly = FALSE`.
- **Networking (optional, phase 2):** AppleTalk DDP master/slave *state-sync* —
  slave sends 8-byte analog input, master simulates everything and broadcasts a
  36-byte render-state snapshot per frame. Cleanly isolated in `AppleTalkDDP.c`
  + `NetOpponent.c` behind `canNetwork` guards; the game already runs with
  networking disabled. A UDP/ENet transport swap is ~6 functions plus explicit
  serialization; fine on LAN, would need prediction work for internet play.

## 3. Options considered

### Option A — Native SDL3 port (recommended)

Keep `Dynamics.c`, `Ball.c`, `Computer.c`, `CommonPerson.c`, and the game-flow
logic verbatim; write an SDL3 platform layer (window/renderer/audio/gamepad);
convert assets offline to PNG/WAV/binary tables; rebuild the menu UI as simple
game screens.

- **Pros:** full control; every target platform including wasm; SDL3's mature
  gamepad API (hotplug, mappings DB, rumble) and borderless fullscreen; smallest
  runtime; MIT-clean; the approach every successful Calhoun-game port used.
- **Cons:** the UI shell must be rebuilt (true in every option).
- **Effort:** playable single-player core (render + sound + input + one menu
  screen): roughly **2–4 weeks** of focused work. Polished cross-platform release
  (all dialogs' functionality, persistence, packaging/signing, itch/Steam-ready):
  **1–3 months** part-time. Evidence the estimate is realistic: an AI-assisted
  SDL2 port reached "complete playable single-player" in weeks (see Option B).

### Option B — Adopt/extend the existing SDL2 port

**A direct port already exists:** [`gettierproblem/pararena2-port`](https://github.com/gettierproblem/pararena2-port)
(MIT, created May 2026, credited to "gettierproblem and Claude"). SDL2 +
an ~11-file Toolbox shim; Windows (MSVC/vcpkg) and WebAssembly targets, with a
[live playable browser build](https://gettierproblem.github.io/pararena2-port/).
Its README claims complete single-player (original AI, physics, both arenas, five
leagues, replay, announcer); its TODO lists what's missing: **no gamepad support,
no native macOS/Linux builds**, no persistence, no netplay, no pause, no
About/Help. It uses a fixed 60 Hz accumulator timestep — consistent with §2.7.

- **Pros:** working proof of the whole approach; MIT so code and asset tooling
  are freely reusable; could cut weeks off Option A.
- **Cons:** zero community review, AI-assisted code of unaudited quality, SDL2
  rather than SDL3, only builds documented are MSVC and Emscripten. *It was not
  built or run during this evaluation* (only read); adopt only after a hands-on
  review, or treat it as a reference implementation and salvage its asset
  pipeline while doing Option A cleanly.

**A pragmatic hybrid — Option A informed by B's shim, or B upgraded to SDL3 with
gamepad + macOS/Linux CI — is likely the fastest route to your stated goals.**

### Option C — Emulation-based distribution (not viable as the product)

- **Basilisk II / Mini vMac:** need Apple ROM + system software, which can't be
  redistributed — fine for development reference, dead end for distribution.
- **Executor 2000** (ROM-free Toolbox reimplementation, actively developed):
  no Windows support, no gamepad concept, only a 2019 pre-release; unknown
  compatibility with SMS's direct-hardware audio and the direct-VRAM blitters.
- **Advanced Mac Substitute:** 1-bit graphics only. **MACE:** closed source.

Use emulation as a *behavioral oracle* while porting (side-by-side comparison),
nothing more.

### Option D — Remake in a modern engine (Godot/Unity/raylib)

Rewriting the physics/AI would forfeit the one thing this codebase gives you for
free — an exact, already-portable simulation — and risks losing game feel that
lives in integer-math quirks. Every engine-remake attempt of Calhoun's games
found on GitHub stalled. Not recommended.

## 4. Suggested roadmap (Option A/hybrid)

1. **Asset pipeline** (2–4 days): parse `$"hex"` blocks out of `Pararena.project.r`
   → binary resources → PNG (PICT decode), WAV (SMSD, strip 8-byte header,
   22,254.5 Hz), raw big-endian tables (`forc`/`vert`/`sPts`, byte-swapped at load).
   Check in the converter, not just its output.
2. **Core loop** (1–2 weeks): SDL3 window + streaming texture at 640×480 logical;
   implement the ~6 blit primitives following `RenderSceneQDC`'s call sequence;
   fixed 60 Hz accumulator; keyboard+mouse input shim; compile the seven logic
   files unmodified (the shim header from this evaluation is a starting point —
   see note below). Milestone: CPU-vs-CPU demo mode renders correctly.
3. **Sound** (1–2 days): 4-voice priority mixer per §2.3.
4. **Playability** (1 week): human input (mouse relative mode + gamepad via
   SDL3 gamepad API per the §2.5 mapping), goal recolor polygons
   (`UpdateGoalPicts`), scoreboard/HUD, pause, instant replay window.
5. **Shell** (1–2 weeks): title/menu screens replacing the essential dialogs
   (new game, teams/league select, sound options, names/stats/records), prefs
   file, splash + dissolve, announcer intro.
6. **Ship** (ongoing): fullscreen/scaling options, packaging for macOS
   (universal binary, notarization), Linux (AppImage/Flatpak), Windows; optional
   wasm build; optional UDP netplay later.

Useful artifacts already produced during this evaluation (in the session
scratchpad, reproducible from the analysis in this document): a MacShim.h that
compiles all seven logic files, extracted+verified `forc`/`vert` binaries, an
annotated SMS driver disassembly, and PICT→PNG decoding of the sprite sheet.

## 5. Risks and open items

- **Small residual unknowns, none blocking:** exact SMS steal-tie-break nuances
  (decoded but worth A/B listening), PICT palette edge cases (validate converted
  art against emulator screenshots), the custom 7-pt bitmap font used only in
  512×342 mode (substitute a modern font), `vert` table out-of-bounds reads at
  the array edge (pad or clamp — harmless on a 68K heap, UB in a modern build).
- **Behavioral fidelity:** keep THINK C semantics — `int16_t`/`int32_t` for
  short/long, signed division truncation (C99 matches), big-endian data loads.
- **SMS driver licensing:** the SMS *binaries* are third-party (Steve Hales) and
  arguably not covered by the MIT grant — irrelevant if replaced (recommended),
  but don't ship the binaries.
- **Courtesy:** the MIT license permits everything, but John Calhoun is active
  (softdorothy/EngineersNeedArt on GitHub) and sells his own remake of Glypha;
  a courtesy note before a polished public release would be good citizenship.

## 6. Fact-check corrections to the brief

- The license is **MIT, not GPL** (`LICENSE`, © 2016 softdorothy) — strictly
  better for this project.
- The canonical upstream is `softdorothy/Pararena2`; the resources here are
  v2.07 (`vers` resource) while `Main.c`'s comment says 2.06 — the source is
  essentially the final retail version.
