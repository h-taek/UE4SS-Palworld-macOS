#!/usr/bin/env bash
# Cut a GitHub release the manager (03_PalworldModManager) can consume.
#
# Contract (see docs/spec/02): public repo, tag vX.Y.Z, asset UE4SS_mac.zip
# whose payload contains UE4SS_mac/libUE4SS.dylib. Version SSOT = ./VERSION.
#
# Usage:
#   tools/release.sh [--dry-run] [--replace] [--notes-file PATH] [--dylib PATH]
#
#   --dry-run      build + zip only; do not tag or publish.
#   --replace      if the release/tag already exists, delete and recreate it.
#   --notes-file   markdown notes for the release (default: auto one-liner).
#   --dylib        override the dylib path passed to the packager.

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(dirname "$SCRIPT_DIR")"
cd "$PROJECT_ROOT"

DRY_RUN=0
REPLACE=0
NOTES_FILE=""
DYLIB_ARG=()

while [ "$#" -gt 0 ]; do
    case "$1" in
        --dry-run) DRY_RUN=1; shift ;;
        --replace) REPLACE=1; shift ;;
        --notes-file)
            [ "$#" -ge 2 ] || { echo "Missing value for --notes-file" >&2; exit 2; }
            NOTES_FILE="$2"; shift 2 ;;
        --dylib)
            [ "$#" -ge 2 ] || { echo "Missing value for --dylib" >&2; exit 2; }
            DYLIB_ARG=(--dylib "$2"); shift 2 ;;
        -h|--help)
            sed -n '2,14p' "$0"; exit 0 ;;
        *) echo "Unknown argument: $1" >&2; exit 2 ;;
    esac
done

[ -f VERSION ] || { echo "Missing VERSION file at repo root" >&2; exit 1; }
VERSION="$(tr -d '[:space:]' < VERSION)"
case "$VERSION" in
    [0-9]*.[0-9]*.[0-9]*) : ;;
    *) echo "VERSION must be semver X.Y.Z, got: '$VERSION'" >&2; exit 1 ;;
esac
TAG="v$VERSION"

# Build the package + zip. Asset name is fixed to UE4SS_mac.zip by the packager.
# (${arr[@]:+...} guards empty-array expansion under set -u on bash 3.2.)
"$SCRIPT_DIR/package-ue4ss-mac.sh" --zip ${DYLIB_ARG[@]:+"${DYLIB_ARG[@]}"}
ZIP="$PROJECT_ROOT/dist/UE4SS_mac.zip"
[ -f "$ZIP" ] || { echo "Packager did not produce $ZIP" >&2; exit 1; }
echo "Asset ready: $ZIP ($(du -h "$ZIP" | cut -f1))"

if [ "$DRY_RUN" -eq 1 ]; then
    echo "DRY-RUN: would publish $TAG with asset UE4SS_mac.zip. Stopping."
    exit 0
fi

command -v gh >/dev/null 2>&1 || { echo "gh CLI not found" >&2; exit 1; }
gh auth status >/dev/null 2>&1 || { echo "gh not authenticated" >&2; exit 1; }

NOTES_ARGS=()
if [ -n "$NOTES_FILE" ]; then
    NOTES_ARGS=(--notes-file "$NOTES_FILE")
else
    NOTES_ARGS=(--notes "macOS (Apple Silicon) UE4SS loader for Palworld — $TAG. Asset: UE4SS_mac.zip (UE4SS_mac/libUE4SS.dylib).")
fi

# Idempotency guard: refuse to clobber an existing release unless --replace.
if gh release view "$TAG" >/dev/null 2>&1; then
    if [ "$REPLACE" -eq 1 ]; then
        echo "Release $TAG exists; --replace given, deleting it."
        gh release delete "$TAG" --yes --cleanup-tag
    else
        echo "Release $TAG already exists. Bump VERSION or pass --replace." >&2
        exit 1
    fi
fi

# Pin the tag to the exact local commit and push it (carries needed objects).
# We push the tag FIRST so `gh release create` attaches to the existing remote
# tag. Passing --target here instead makes gh try to create the tag ref via the
# API, which fails with a misleading "workflow scope" error — so we don't.
if ! git rev-parse -q --verify "refs/tags/$TAG" >/dev/null; then
    git tag -a "$TAG" -m "UE4SS-Palworld-macOS $TAG"
fi
git push origin "$TAG"

gh release create "$TAG" "$ZIP#UE4SS_mac.zip" \
    --title "$TAG" \
    "${NOTES_ARGS[@]}"

echo "Published $TAG"
gh release view "$TAG"
