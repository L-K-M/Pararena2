# Pararena 2 — Android

Builds the SDL port as an Android app (`libmain.so` loaded by SDL's
`SDLActivity`, with `libSDL3.so`), packaging the asset pack into the APK.

## Prerequisites

- JDK 17
- Android SDK with:
  - `platforms;android-35`, `build-tools;35.0.0`
  - `ndk;28.2.13676358` (the NDK SDL 3.4.12 targets — change `ndkVersion` in
    `app/build.gradle` if you have another and adjust as needed)
  - `cmake;3.22.1` (or newer)
- `ANDROID_HOME` / `ANDROID_SDK_ROOT` pointing at the SDK
- Python 3 (the native build converts the original THINK C sources at compile time)

## Build

```sh
cd port/android
./fetch-deps.sh            # downloads SDL3 source into app/jni/SDL, stages the asset pack
./gradlew assembleDebug    # -> app/build/outputs/apk/debug/app-debug.apk
```

Install the APK with `adb install -r app/build/outputs/apk/debug/app-debug.apk`.

`fetch-deps.sh` is required once: SDL's source is used both to build `libSDL3.so`
and for its Java glue (`org.libsdl.app.*`), which `app/build.gradle` compiles
straight from `app/jni/SDL/android-project/...`. Neither the SDL source nor the
staged asset pack is committed (see `.gitignore`).

Only `arm64-v8a` is built by default; widen `abiFilters` in `app/build.gradle`
for other ABIs.

## Touch controls

There are two touch schemes, chosen by **Options → TOUCH CONTROLS**:

- **ON-SCREEN** (default) — a visible analog stick (bottom-left) you drag to
  skate, plus visible action buttons bottom-right: **catch / throw** (the ball
  dot) and **brake** (the bar).
- **SWIPE** — no visible controls: drag the **left half** of the screen to skate
  (a relative joystick from wherever you press), tap the **right half** for
  **catch** (upper) / **brake** (lower).

Either way, an always-visible **pause button** (the ‖ at top-centre) opens the
pause screen; the hardware **Back button** pauses too. **Menus** are driven by
taps — top / middle / bottom third = up / select / down.

The pause screen (a dithered Sony Ericsson P800 showing the touch layout) is
dismissed by touch: **tap the left half to resume, the right half to end the
game** (Back also resumes).

A connected gamepad (Bluetooth or USB) also works everywhere and adds *bash*
(West button), which touch doesn't expose yet.

## Notes / limitations

- This is a debug APK (debug-signed, no shrinking) — fine for sideloading; wire a
  signing config into `app/build.gradle` for a Play-ready release build.
- Built and verified in CI (`.github/workflows/release.yml`, the `android` job);
  it isn't part of the desktop `cmake` build.
