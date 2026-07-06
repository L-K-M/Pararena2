#!/usr/bin/env bash
# Builds the Pararena 2 SDL port from the command line.
#
# Two modes:
#
#   • Default — an incremental Release build of the single host binary via the
#     shared lkm-build engine, revealed (or run) on success. Unchanged behaviour:
#
#       scripts/build.sh                 # incremental Release build → reveal
#       scripts/build.sh --clean         # wipe build/ first
#       scripts/build.sh --run -- --headless --cpu-demo   # …then run it
#
#   • --dist — build self-contained, distributable packages the way
#     .github/workflows/release.yml does: a double-clickable universal
#     Pararena2.app on macOS, and a static binary + asset pack in a tar.gz
#     (Linux) / zip (Windows) elsewhere. SDL3 is linked statically so every
#     archive is a complete, runnable game.
#
#       scripts/build.sh --dist                     # every achievable target → dist/
#       scripts/build.sh --dist --targets mac,linux # restrict the target set
#       scripts/build.sh --dist --check             # print the plan; build nothing
#       scripts/build.sh --dist --clean             # wipe the dist build dirs first
#
#     No single OS can natively build all three, so --dist is best-effort: it
#     builds each target whose toolchain is present and skips (never fails) the
#     rest. Per target:
#
#       macOS    native universal (.app) — macOS host only
#       Linux    native on a Linux host, else in a Docker container if docker exists
#       Windows  cross-compiled with the MinGW-w64 toolchain when it is installed
#       Android  Gradle assembleDebug .apk when a JDK + Android SDK/NDK are present
#
#     CI (release.yml) still builds every target on runners; --dist is the local
#     equivalent for whatever this machine can reach.
#
# Usage: scripts/build.sh [--clean] [--debug] [--run] [-- <args for the binary>]
#        scripts/build.sh --dist [--targets mac,linux,windows,android] [--check] [--clean]
# Shared engine: https://github.com/L-K-M/release-tool (this stub only sets config).
set -euo pipefail

export BUILD_APP_NAME="Pararena2"
export BUILD_KIND="cmake"
export BUILD_CMAKE_SOURCE_DIR="port"       # dir holding CMakeLists.txt (passed to cmake -S)
export BUILD_CMAKE_TARGET="pararena2"       # target to build / binary to reveal after a build
export BUILD_INVOKED_AS="scripts/build.sh"

# --- Default path: delegate to the shared engine unless --dist was requested -------
want_dist=false
for a in "$@"; do [[ "$a" == "--dist" ]] && want_dist=true; done
if ! $want_dist; then
  BIN="${LKM_BUILD_BIN:-lkm-build}"
  command -v "$BIN" >/dev/null 2>&1 || {
    echo "error: lkm-build not found — clone https://github.com/L-K-M/release-tool and run ./install.sh" >&2
    exit 1
  }
  exec "$BIN" "$@"
fi

# ==================================================================================
# --dist: local multi-target packaging (mirrors .github/workflows/release.yml)
# ==================================================================================
have() { command -v "$1" >/dev/null 2>&1; }
note() { echo "note: $*" >&2; }
die()  { echo "error: $*" >&2; exit 1; }

REPO_ROOT=$(git -C "$(dirname "$0")" rev-parse --show-toplevel 2>/dev/null) \
  || REPO_ROOT=$(cd "$(dirname "$0")/.." && pwd)
cd "$REPO_ROOT"

# --- Parse --dist flags ------------------------------------------------------------
TARGETS_CSV=""
DIST_CHECK=false
DIST_CLEAN=false
dist_args=("$@")
di=0
while [[ $di -lt ${#dist_args[@]} ]]; do
  a="${dist_args[$di]}"
  case "$a" in
    --dist)        ;;
    --targets)     di=$((di+1)); TARGETS_CSV="${dist_args[$di]:-}"
                   [[ -n "$TARGETS_CSV" ]] || die "--targets needs a value (e.g. mac,linux,windows)" ;;
    --targets=*)   TARGETS_CSV="${a#--targets=}" ;;
    --check)       DIST_CHECK=true ;;
    --clean)       DIST_CLEAN=true ;;
    *)             die "unknown --dist option '$a' (see the header of scripts/build.sh)" ;;
  esac
  di=$((di+1))
done

# Was <canonical target> asked for? Empty --targets means "all". Accept common aliases.
selected() {
  [[ -z "$TARGETS_CSV" ]] && return 0
  local t
  for t in $(echo "$TARGETS_CSV" | tr ',' ' '); do
    case "$t" in
      mac|macos|osx|darwin) [[ "$1" == mac ]]     && return 0 ;;
      linux)                [[ "$1" == linux ]]   && return 0 ;;
      win|windows)          [[ "$1" == windows ]] && return 0 ;;
      android|droid)        [[ "$1" == android ]] && return 0 ;;
      *) die "unknown target '$t' in --targets (use mac, linux, windows, android)" ;;
    esac
  done
  return 1
}

# --- Version: the exact v* tag on HEAD, else the short commit (matches release.yml) -
if V=$(git describe --exact-match --tags --match 'v*' HEAD 2>/dev/null); then
  VERSION="$V"
elif V=$(git rev-parse --short=7 HEAD 2>/dev/null); then
  VERSION="$V"
else
  VERSION="0.0.0-dev"
fi
PLIST_VERSION="${VERSION#v}"          # Info.plist wants the number without the leading v

HOST=$(uname -s)
DIST_DIR="dist"
BUILD_ROOT="port/build-dist"
SMOKE=(--headless --cpu-demo --frames 900 --fast 8)
# Force the self-contained SDL3: ignore any system/Homebrew SDL3 (a shared,
# host-arch-only dylib that breaks both static linking and the universal build)
# so the CMakeLists FetchContent fallback builds SDL3 from source, statically.
# A normal dev build (scripts/build.sh with no --dist) deliberately keeps using
# the fast system SDL3 — this override is dist-only.
SDL_BUNDLED=(-DCMAKE_DISABLE_FIND_PACKAGE_SDL3=ON -DSDL_SHARED=OFF -DSDL_STATIC=ON)

# --- Shared build/stage helpers ----------------------------------------------------
cmake_build() {                       # cmake_build <build-dir> [extra -D flags…]
  local bdir="$1"; shift
  cmake -S port -B "$bdir" -DCMAKE_BUILD_TYPE=Release "${SDL_BUNDLED[@]}" "$@"
  cmake --build "$bdir" --config Release -j
}

locate_bin() {                        # locate_bin <build-dir> [.exe] → prints path
  local bdir="$1" ext="${2:-}" c
  for c in "$bdir/pararena2$ext" "$bdir/Release/pararena2$ext"; do
    [[ -f "$c" ]] && { echo "$c"; return 0; }
  done
  return 1
}

stage_root() {                        # a fresh temp stage/ with pararena2/ + docs
  local s; s=$(mktemp -d)
  mkdir -p "$s/pararena2"
  cp port/README.md "$s/pararena2/README.md"
  cp LICENSE "$s/pararena2/"
  echo "$s"
}

# --- Per-target builders (each is a subshell so a failure can't abort the others) --
build_mac() (
  set -e
  local bdir="$BUILD_ROOT/macos"
  cmake_build "$bdir" -DCMAKE_OSX_ARCHITECTURES="arm64;x86_64" -DCMAKE_OSX_DEPLOYMENT_TARGET=11.0
  local bin dat; bin=$(locate_bin "$bdir") || die "pararena2 binary not found under $bdir"
  dat="$(dirname "$bin")/pararena2.dat"; [[ -f "$dat" ]] || die "asset pack missing beside $bin"
  lipo -info "$bin" | grep -q arm64  || die "binary is not universal (arm64 missing)"
  lipo -info "$bin" | grep -q x86_64 || die "binary is not universal (x86_64 missing)"
  "$bin" "${SMOKE[@]}"
  # Assemble a proper, standalone .app right in dist/ (not a temp dir) so it is
  # left behind as a double-clickable app: static universal binary in MacOS/,
  # asset pack in Resources/ (what SDL_GetBasePath() returns for a bundle),
  # version-stamped Info.plist, ad-hoc signature (identity "-") so a universal
  # app can launch on Apple Silicon.
  mkdir -p "$DIST_DIR"
  local app="$DIST_DIR/Pararena2.app"
  rm -rf "$app"
  mkdir -p "$app/Contents/MacOS" "$app/Contents/Resources"
  cp "$bin" "$app/Contents/MacOS/pararena2"
  cp "$dat" "$app/Contents/Resources/pararena2.dat"
  cp port/assets/icon/pararena.icns "$app/Contents/Resources/pararena.icns"
  sed "s/__VERSION__/$PLIST_VERSION/g" port/macos/Info.plist > "$app/Contents/Info.plist"
  plutil -lint "$app/Contents/Info.plist" >/dev/null
  codesign --force --sign - "$app"
  codesign --verify --strict "$app"
  # Distributable tarball (pararena2/Pararena2.app + docs), same layout as CI.
  # ditto copies the bundle preserving its signature; tar it, drop the temp dir.
  local s; s=$(stage_root)
  ditto "$app" "$s/pararena2/Pararena2.app"
  tar -czf "$DIST_DIR/pararena2-$VERSION-macos-universal.tar.gz" -C "$s" pararena2
  rm -rf "$s"
  echo "    macOS: app  → $app"
  # Reveal (and select) the app in Finder, like the default lkm-build path does.
  open -R "$app" >/dev/null 2>&1 || true
)

build_linux_native() (
  set -e
  local bdir="$BUILD_ROOT/linux"
  cmake_build "$bdir"
  local bin dat; bin=$(locate_bin "$bdir") || die "pararena2 binary not found under $bdir"
  dat="$(dirname "$bin")/pararena2.dat"; [[ -f "$dat" ]] || die "asset pack missing beside $bin"
  "$bin" "${SMOKE[@]}"
  local s; s=$(stage_root)
  cp "$bin" "$s/pararena2/"; cp "$dat" "$s/pararena2/"
  mkdir -p "$DIST_DIR"
  tar -czf "$DIST_DIR/pararena2-$VERSION-linux-x86_64.tar.gz" -C "$s" pararena2
  rm -rf "$s"
)

build_linux_docker() (
  set -e
  mkdir -p "$DIST_DIR"
  # Build inside a clean Ubuntu container (same recipe as release.yml's Linux job)
  # and stream the finished tarball out on stdout — all build noise goes to stderr,
  # and the read-only mount keeps the working tree untouched.
  docker run --rm -i -v "$REPO_ROOT:/src:ro" ubuntu:24.04 bash -s \
      > "$DIST_DIR/pararena2-$VERSION-linux-x86_64.tar.gz" <<'DOCKER'
set -e
export DEBIAN_FRONTEND=noninteractive
{
  apt-get update
  apt-get install -y build-essential cmake ninja-build python3 \
    libasound2-dev libpulse-dev libpipewire-0.3-dev libjack-dev \
    libx11-dev libxext-dev libxrandr-dev libxcursor-dev libxfixes-dev \
    libxi-dev libxss-dev libxtst-dev libxkbcommon-dev \
    libwayland-dev libdecor-0-dev libdrm-dev libgbm-dev \
    libgl1-mesa-dev libegl1-mesa-dev libdbus-1-dev libudev-dev libibus-1.0-dev
  cmake -S /src/port -B /bd -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_DISABLE_FIND_PACKAGE_SDL3=ON -DSDL_SHARED=OFF -DSDL_STATIC=ON
  cmake --build /bd -j
  /bd/pararena2 --headless --cpu-demo --frames 900 --fast 8
  mkdir -p /stage/pararena2
  cp /bd/pararena2 /bd/pararena2.dat /stage/pararena2/
  cp /src/port/README.md /stage/pararena2/README.md
  cp /src/LICENSE /stage/pararena2/
} >&2
tar -czf - -C /stage pararena2
DOCKER
)

build_windows_mingw() (
  set -e
  local bdir="$BUILD_ROOT/windows"
  local prefix; prefix=$(cd "$(dirname "$(command -v x86_64-w64-mingw32-gcc)")/.." && pwd)
  cmake_build "$bdir" \
    -DCMAKE_SYSTEM_NAME=Windows \
    -DCMAKE_C_COMPILER=x86_64-w64-mingw32-gcc \
    -DCMAKE_CXX_COMPILER=x86_64-w64-mingw32-g++ \
    -DCMAKE_RC_COMPILER=x86_64-w64-mingw32-windres \
    -DCMAKE_FIND_ROOT_PATH="$prefix/x86_64-w64-mingw32" \
    -DCMAKE_FIND_ROOT_PATH_MODE_PROGRAM=NEVER \
    -DCMAKE_FIND_ROOT_PATH_MODE_LIBRARY=ONLY \
    -DCMAKE_FIND_ROOT_PATH_MODE_INCLUDE=ONLY
  local bin dat; bin=$(locate_bin "$bdir" .exe) || die "pararena2.exe not found under $bdir"
  dat="$(dirname "$bin")/pararena2.dat"; [[ -f "$dat" ]] || die "asset pack missing beside $bin"
  # A Windows .exe can only be smoke-tested here if wine is around; keep it non-fatal.
  if   have wine64; then wine64 "$bin" "${SMOKE[@]}" || note "wine smoke test failed (ignored)"
  elif have wine;   then wine   "$bin" "${SMOKE[@]}" || note "wine smoke test failed (ignored)"
  fi
  local s out; s=$(stage_root)
  cp "$bin" "$s/pararena2/"; cp "$dat" "$s/pararena2/"
  mkdir -p "$DIST_DIR"; out="$REPO_ROOT/$DIST_DIR/pararena2-$VERSION-windows-x86_64.zip"; rm -f "$out"
  if   have zip;   then ( cd "$s" && zip -qr "$out" pararena2 )
  elif have 7z;    then ( cd "$s" && 7z a -bd "$out" pararena2 >/dev/null )
  elif have ditto; then ditto -c -k --sequesterRsrc --keepParent "$s/pararena2" "$out"
  else die "need zip, 7z, or ditto to build the Windows archive"; fi
  rm -rf "$s"
)

build_android() (
  set -e
  have java || die "java (JDK 17+) not found"
  [ -n "${ANDROID_HOME:-}${ANDROID_SDK_ROOT:-}" ] || die "set ANDROID_HOME (or ANDROID_SDK_ROOT) to your Android SDK"
  bash port/android/fetch-deps.sh >&2
  ( cd port/android && ./gradlew assembleDebug --no-daemon ) >&2
  local apk="port/android/app/build/outputs/apk/debug/app-debug.apk"
  [ -f "$apk" ] || die "APK not produced at $apk"
  mkdir -p "$DIST_DIR"
  cp "$apk" "$DIST_DIR/pararena2-$VERSION-android-arm64.apk"
)

# --- Orchestration + reporting -----------------------------------------------------
report=()
add() { report+=("$1|$2|$3"); }       # add <target> <status> <detail>

run_target() {                        # run_target <label> <artifact> <builder-fn>
  local label="$1" art="$2" fn="$3"
  echo "==> $label: building…"
  if "$fn" && [[ -s "$art" ]]; then
    add "$label" "built" "$art"; echo "    $label: built → $art"
  else
    add "$label" "FAILED" "build error (see output above)"; echo "    $label: FAILED"
  fi
}

echo "==> Pararena2 --dist  (version $VERSION, host $HOST)"
if $DIST_CLEAN && [[ -d "$BUILD_ROOT" ]]; then
  echo "==> Clean: wiping $BUILD_ROOT…"; rm -rf "$BUILD_ROOT"
fi

# macOS ----------------------------------------------------------------------------
if selected mac; then
  art="$DIST_DIR/pararena2-$VERSION-macos-universal.tar.gz"
  if [[ "$HOST" == "Darwin" ]]; then
    if $DIST_CHECK; then echo "==> macOS:   native universal .app  [would build]"; add macOS plan "standalone .app (revealed) + tar.gz"
    else run_target "macOS" "$art" build_mac; fi
  else
    add macOS skipped "a universal .app can only be built on a macOS host"
    echo "==> macOS:   [skipped — needs a macOS host]"
  fi
fi

# Linux ----------------------------------------------------------------------------
if selected linux; then
  art="$DIST_DIR/pararena2-$VERSION-linux-x86_64.tar.gz"
  if [[ "$HOST" == "Linux" ]]; then
    if $DIST_CHECK; then echo "==> Linux:   native static build    [would build]"; add Linux plan "native static → tar.gz"
    else run_target "Linux" "$art" build_linux_native; fi
  elif have docker; then
    if $DIST_CHECK; then echo "==> Linux:   Docker container build [would build]"; add Linux plan "docker (ubuntu:24.04) → tar.gz"
    else run_target "Linux" "$art" build_linux_docker; fi
  else
    add Linux skipped "not a Linux host and Docker not found — install Docker Desktop"
    echo "==> Linux:   [skipped — need a Linux host or Docker]"
  fi
fi

# Windows --------------------------------------------------------------------------
if selected windows; then
  art="$DIST_DIR/pararena2-$VERSION-windows-x86_64.zip"
  if have x86_64-w64-mingw32-gcc; then
    if $DIST_CHECK; then echo "==> Windows: MinGW-w64 cross-build  [would build]"; add Windows plan "mingw-w64 cross → zip"
    else run_target "Windows" "$art" build_windows_mingw; fi
  else
    add Windows skipped "MinGW-w64 not found — brew install mingw-w64 (macOS) / apt install mingw-w64 (Linux)"
    echo "==> Windows: [skipped — MinGW-w64 not found]"
  fi
fi

# Android --------------------------------------------------------------------------
if selected android; then
  art="$DIST_DIR/pararena2-$VERSION-android-arm64.apk"
  if have java && [ -n "${ANDROID_HOME:-}${ANDROID_SDK_ROOT:-}" ]; then
    if $DIST_CHECK; then echo "==> Android: Gradle assembleDebug   [would build]"; add Android plan "gradle → debug .apk (arm64)"
    else run_target "Android" "$art" build_android; fi
  else
    add Android skipped "need a JDK and ANDROID_HOME/ANDROID_SDK_ROOT (SDK + NDK 28.2.13676358)"
    echo "==> Android: [skipped — no JDK / Android SDK]"
  fi
fi

# Summary --------------------------------------------------------------------------
echo
echo "===================== dist summary ====================="
for r in ${report[@]+"${report[@]}"}; do
  IFS='|' read -r t s d <<<"$r"
  printf "  %-9s %-8s %s\n" "$t" "$s" "$d"
done
echo "========================================================"

if $DIST_CHECK; then
  echo "(--check: nothing was built)"
  exit 0
fi

echo "artifacts in $DIST_DIR/ (version $VERSION):"
found_any=false
# the standalone, double-clickable macOS app (version-independent name)
[[ -d "$DIST_DIR/Pararena2.app" ]] && { echo "  $DIST_DIR/Pararena2.app"; found_any=true; }
# the distributable archives
if ls "$DIST_DIR"/pararena2-"$VERSION"-* >/dev/null 2>&1; then
  for f in "$DIST_DIR"/pararena2-"$VERSION"-*; do echo "  $f"; done
  found_any=true
fi
$found_any || echo "  (none)"

# Non-zero exit only when a target we actually attempted failed to build.
rc=0
for r in ${report[@]+"${report[@]}"}; do
  IFS='|' read -r t s d <<<"$r"; [[ "$s" == "FAILED" ]] && rc=1
done
exit $rc
