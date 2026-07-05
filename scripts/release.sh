#!/usr/bin/env bash
# Cuts a release: tags the current commit "v<version>" and, with --push, pushes the
# branch + tag — which triggers .github/workflows/release.yml to build self-contained
# packages for Linux, macOS (universal), and Windows and publish them as a GitHub
# Release. Pararena 2 keeps NO committed version number: the release workflow derives
# the released version straight from the tag (GITHUB_REF_NAME), so the tag is the single
# source of truth. That makes this a `tag-only` release — a version argument is required,
# nothing in the working tree is edited or committed, and only the annotated tag is made.
#
#   scripts/release.sh 1.0.0          # create annotated tag v1.0.0 on HEAD
#   scripts/release.sh 1.0.0 --push   # …also push the commit + tag (CI then publishes)
#   scripts/release.sh --check        # print resolved config + latest tag; mutate nothing
#
# Usage: scripts/release.sh X.Y[.Z][-pre] [--push]
# Shared engine: https://github.com/L-K-M/release-tool (this stub only sets config).
set -euo pipefail

export RELEASE_APP_NAME="Pararena2"
export RELEASE_KIND="tag-only"
# The release workflow accepts tags like v1.0 or v1.0.0 (optionally -beta.1), so allow
# the two-part form too rather than tag-only's strict X.Y.Z default.
export RELEASE_VERSION_REGEX='^[0-9]+\.[0-9]+(\.[0-9]+)?(-[0-9A-Za-z.]+)?$'
export RELEASE_CI_NOTE="CI (release.yml) will now build and package self-contained Linux, macOS, and Windows archives and publish the GitHub Release for <tag>."
export RELEASE_INVOKED_AS="scripts/release.sh"

BIN="${LKM_RELEASE_BIN:-lkm-release}"
command -v "$BIN" >/dev/null 2>&1 || {
  echo "error: lkm-release not found — clone https://github.com/L-K-M/release-tool and run ./install.sh" >&2
  exit 1
}
exec "$BIN" "$@"
