#!/usr/bin/env bash
# Builds the Pararena 2 SDL port from the command line and reveals the binary on
# success. Incremental Release build by default; --clean wipes the build dir first.
# Thin stub for the shared lkm-build engine.
#
# The port is a CMake project under port/ — SDL3 is found via find_package or fetched
# and built automatically, so a fresh checkout builds with no manual dependency setup.
# It produces the self-contained `pararena2` binary with its asset pack (pararena2.dat)
# copied alongside; --run launches it (pass --headless etc. after -- to the game).
#
# Usage: scripts/build.sh [--clean] [--debug] [--run]
# Shared engine: https://github.com/L-K-M/release-tool (this stub only sets config).
set -euo pipefail

export BUILD_APP_NAME="Pararena2"
export BUILD_KIND="cmake"
export BUILD_CMAKE_SOURCE_DIR="port"       # dir holding CMakeLists.txt (passed to cmake -S)
export BUILD_CMAKE_TARGET="pararena2"       # target to build / binary to reveal after a build
export BUILD_INVOKED_AS="scripts/build.sh"

BIN="${LKM_BUILD_BIN:-lkm-build}"
command -v "$BIN" >/dev/null 2>&1 || {
  echo "error: lkm-build not found — clone https://github.com/L-K-M/release-tool and run ./install.sh" >&2
  exit 1
}
exec "$BIN" "$@"
