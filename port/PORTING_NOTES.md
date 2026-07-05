# Pararena 2 SDL Port — Architecture & Decisions

This port keeps John Calhoun's original game code **verbatim** and replaces the
classic Mac Toolbox underneath it with a small shim over SDL3. Nothing in
`Sources/` or `Headers/` is modified; original files are converted (line
endings, Pascal string literals) at build time into the build directory.

## What runs verbatim (compiled from `../Sources`)

| File | Role | Shim surface it needs |
|---|---|---|
| Dynamics.c, Ball.c, Computer.c, CommonPerson.c | physics, ball, AI | `Ticks`, `PlaySoundSMS`, `RandomInt/RandomCoin`, render-update calls |
| Human.c | player input → forces | `GetMouse`, `LocalToGlobal`, `Button`, `BitTst`, `SetMouse`, `ForcePointInRect` |
| PlayCore.c | game loops (60 Hz busy-wait on `Ticks`) | `GetKeys`, `Alert`, cursor stubs, `GetOSEvent`, `FlushEvents` |
| PlayUtils.c | scoring/fouls/game flow, hoopla text | QD text calls, `GetIndString`, `GetDateTime`, `Delay` |
| Render.c | frame orchestration, HUD dispatch | (portable; `useQD=TRUE` forces the QuickDraw path) |
| RenderQD.c | the actual renderer (CopyBits/CopyMask) | QD core: `CopyBits`, `CopyMask`, pen/port ops |
| Replay.c | instant-replay capture + playback | QD + region stubs |
| InitGameStructs.c | tables, rects, prefs defaults | `GetResource`, `NewPtr`, `BlockMove`, `HLock/…` |
| MainWindow.c | background/goal painting, splash | `GetPicture`/`DrawPicture`, regions (polygon fill), window stubs |
| SoundUtils.c | sound sequencing/queues | the 7 SMS entry points |
| TeamSetUp.c → `WhosOnFirst` only (transcribed) | game-state assignment | — |
| Main.c | globals + RootLoop (`main` renamed via `-Dmain=PararenaMain`) | `HandleEvent` (provided by the port shell) |

**Replaced, not compiled:** Initialize.c, Environ.c (→ `port_init.c`),
Idle.c/Menu.c/IdleRoutines.c/TeamSetUp.c/PlayerStats.c/About.c/
ConfigureSound.c/Show_help.c (→ `port_shell.c`), Prefs.c (→ `port_prefs.c`),
UnivUtilities.c (→ `port_univ.c` — the original pokes low-memory addresses),
AppleTalkDDP.c/NetOpponent.c (stubbed; `canNetwork = FALSE`),
RenderAsm1.c/RenderAsm4.c/DissBits.c (superseded by the QD path),
ValidInstall.c (copy protection — deleted), FileError.c, AnimCursor.c,
DumpPict.c, Abandoned Routines.c.

## Key shim decisions

- **Pixel model:** all offscreen buffers and the "screen" are 8 bits/pixel
  holding Apple 4-bit palette indices 0–15 (0=white … 15=black). `BitMap.baseAddr`
  points at the 8bpp buffer, `rowBytes` = width. `CGrafPort` is defined with the
  same layout as `GrafPort` so the original's `((GrafPtr)offCWorkPtr)->portBits`
  casts work unchanged. Present = palette-expand the screen buffer into an RGBA
  streaming texture, letterboxed by SDL's logical presentation.
- **QuickDraw subset implemented:** CopyBits (srcCopy, clipped), CopyMask,
  Get/SetPort, MoveTo/Line (pen: fore color + patXor), Paint/Fill/Erase/Frame/
  InvertRect, RGBForeColor (nearest palette index), regions only as recorded
  polygons (Open/CloseRgn + Line) with a scanline `PaintRgn`/`FillRgn` — enough
  for `UpdateGoalPicts`. `DrawPicture(id)` blits the pre-converted image asset.
  Text: `DrawString` renders with an embedded 8×8 public-domain bitmap font
  (used for hoopla banners, names, "INSTANT REPLAY").
- **Timing:** `Ticks` is a real global, advanced from `SDL_GetTicksNS()` at a
  60.0 Hz rate whenever the shim pumps events (every `GetKeys`/`Button`/
  `GetMouse` call — each game-loop iteration calls `CheckAbortiveInput` →
  `GetKeys`, so the original busy-wait paces correctly). The pump sleeps ~1 ms
  when the next tick is far, so the busy-wait doesn't burn a core.
- **Input:** a *virtual cursor* replaces the warped hardware mouse.
  Deflection sources (combined): relative mouse motion (SDL relative mode),
  gamepad left stick / d-pad (deflection = stick × frame half-width), arrow
  keys (ramped; WASD is not used because S/B/N/M/R/E are game keys).
  `Button()` = LMB / gamepad South / X key. Brake = Space / gamepad East or
  right trigger. Bash = B/N/M / gamepad West. `GetKeys` also synthesizes the
  Mac keymap bits PlayCore polls (Q/E/Tab/S/R, command = Ctrl/⌘; Esc maps to
  Cmd+E "end game", window-close to Cmd+Q).
- **Sound:** 4-voice mixer implementing the disassembled SMS semantics
  (priority stealing: steal lowest priority ≤ new, else drop; `SMSSTARTCHAN`
  always preempts; loop when repeat = 0x7FFF). Clips are unsigned 8-bit mono
  22,254.5 Hz, resampled by an `SDL_AudioStream`.
- **Display mode:** `kDisplay13Inch` (640×480, large arena, 16 colors) is the
  canonical mode; small arena (512×384) retained as a selectable variant later.
  `useQD = TRUE` always; the asm and 1-bit paths are never entered.
- **Prefs:** same `prefsInfo` fields serialized natively (magic + version
  header) to the SDL pref path. No compatibility with original `Para Prefs`.
- **Copy protection / networking:** deleted / disabled (`canNetwork = FALSE`,
  `netOnly = FALSE`, `wereLegit = TRUE`). Networking is a future work item
  (protocol notes in ../MODERNIZATION.md §2.8).

## Assets

`port/tools/build_assets.py` extracts everything from `Pararena.project.r`
(DeRez text) and `Para Sounds.bin` (MacBinary) into `port/assets/pararena2.dat`
(single little-endian pack) plus PNG/WAV previews for inspection:

- images: PICT → 8bpp indexed (id, w, h, pixels); masks stay 0/1
- sounds: SMSD → (id, priority, loopFlag, pcm bytes)
- tables: `forc`/`vert`/`sPts` kept big-endian, byte-swapped at load
- palette: `clut 128` (16 RGBA entries)
- strings: `STR#` resources (Mac Roman → UTF-8)

Pack format: `"PAR2"`, u32 version, u32 count, then TOC entries
(u32 fourcc, s32 id, u32 offset, u32 size, u32 w, u32 h, u32 flags).

## Shim subtleties discovered while bringing it up

- QuickDraw's default text transfer mode is `srcOr`; a zeroed port would
  default to `srcCopy` (mode 0) and paint glyph background cells.
- Several loops spin on `Ticks` with empty bodies (e.g. `DisplayHoopla`), so
  `Ticks` must advance on *every read*, not just when events are pumped; the
  read also yields the CPU after ~200 same-tick reads.
- `CGrafPort` is typedef'd to `GrafPort` (with a vestigial `portPixMap` field)
  so both `((GrafPtr)offCPartsPtr)->portBits` and `DumpPict`'s
  `(*ptr).portPixMap` compile against one struct.
- Alert dialogs return button 1 ("default") — the tournament-abort
  confirmation is therefore skipped; a future in-game confirm overlay could
  restore it.

## Verification strategy (headless container)

SDL3 built with the `dummy` video driver; a frame-dump hook writes the 8bpp
screen buffer as PNG every N ticks. CPU-vs-CPU games (both personas AI) drive
the full loop with zero input. Visual inspection of dumped frames + sound-event
log checks compositing, physics, scoring, goal recoloring.
