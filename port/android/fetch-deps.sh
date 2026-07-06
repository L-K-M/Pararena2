#!/usr/bin/env bash
# Stage the two things the Gradle build needs but that aren't committed:
#   1. the SDL3 source (its CMake builds libSDL3.so, and its Java glue is
#      compiled straight from the tree — see app/build.gradle sourceSets)
#   2. the game's asset pack, into the APK's assets/ (Gradle also does this via
#      the stageAssetPack task, but copying here keeps a plain build working too)
#
# Run once before ./gradlew.  Safe to re-run (skips SDL if already present).
set -euo pipefail
cd "$(dirname "$0")"

SDL_VER="3.4.12"                       # keep in sync with ../CMakeLists.txt
SDL_URL="https://www.libsdl.org/release/SDL3-${SDL_VER}.tar.gz"

if [ ! -d app/jni/SDL ]; then
    echo "Fetching SDL ${SDL_VER} ..."
    tmp="$(mktemp -d)"
    curl -Lf "$SDL_URL" -o "$tmp/sdl.tar.gz"
    tar -xzf "$tmp/sdl.tar.gz" -C "$tmp"
    mv "$tmp/SDL3-${SDL_VER}" app/jni/SDL
    rm -rf "$tmp"
    echo "  -> app/jni/SDL"
else
    echo "SDL already present at app/jni/SDL (skipping)"
fi

mkdir -p app/src/main/assets
cp ../assets/pararena2.dat app/src/main/assets/pararena2.dat
echo "  -> app/src/main/assets/pararena2.dat"

echo
echo "Done. Build with:"
echo "    ./gradlew assembleDebug     # -> app/build/outputs/apk/debug/app-debug.apk"
