#!/bin/bash
# Run from Terminal or double-click this file.
# Optional overrides:
#   UE4SS_GAME=/path/to/GameExecutable ./launch-palworld.command
#   UE4SS_CONTAINER_DATA=/path/to/Container/Data ./launch-palworld.command

set -euo pipefail

PACKAGE_DIR="$(cd "$(dirname "$0")" && pwd)"
DYLIB="$PACKAGE_DIR/libUE4SS.dylib"
RUNTIME_SRC="$PACKAGE_DIR/UE4SS"

GAME="${UE4SS_GAME:-/Applications/Palworld.app/Contents/MacOS/Palworld}"
CONTAINER_DATA="${UE4SS_CONTAINER_DATA:-$HOME/Library/Containers/com.pocketpair.palworld.mac/Data}"
RUNTIME_DST="$CONTAINER_DATA/UE4SS"
LOG_DIR="${UE4SS_LOG_DIR:-$HOME/Library/Caches/ue4ss-mac}"
LOG="$LOG_DIR/palworld-launch.log"

fail() {
    echo "[FAIL] $*" >&2
    exit 1
}

warn() {
    echo "[WARN] $*" >&2
}

info() {
    echo "[OK] $*"
}

echo "== UE4SS macOS preflight =="

[ "$(uname -s)" = "Darwin" ] || fail "This launcher only supports macOS."
[ "$(uname -m)" = "arm64" ] || fail "This package is built for Apple Silicon arm64."

[ -f "$DYLIB" ] || fail "Missing dylib: $DYLIB"
[ -d "$RUNTIME_SRC" ] || fail "Missing runtime folder: $RUNTIME_SRC"
[ -f "$RUNTIME_SRC/UE4SS-settings.ini" ] || fail "Missing settings: $RUNTIME_SRC/UE4SS-settings.ini"
[ -d "$RUNTIME_SRC/Mods" ] || fail "Missing Mods folder: $RUNTIME_SRC/Mods"
[ -f "$RUNTIME_SRC/Mods/mods.txt" ] || fail "Missing mods.txt: $RUNTIME_SRC/Mods/mods.txt"
[ -x "$GAME" ] || fail "Missing game executable: $GAME. Set UE4SS_GAME=/path/to/GameExecutable to override."
[ -d "$CONTAINER_DATA" ] || fail "Missing sandbox container: $CONTAINER_DATA. Run the game once normally, then try again."

if ! file "$DYLIB" | grep -q "arm64"; then
    fail "Dylib is not arm64: $DYLIB"
fi

if ! file "$GAME" | grep -q "arm64"; then
    fail "Game executable is not arm64: $GAME"
fi

if command -v otool >/dev/null 2>&1; then
    if otool -L "$DYLIB" | grep -q "not found"; then
        otool -L "$DYLIB"
        fail "Dylib has missing dynamic dependencies."
    fi
else
    warn "otool not found; skipping dylib dependency check."
fi

if command -v codesign >/dev/null 2>&1; then
    codesign -s - --force --timestamp=none "$DYLIB" >/dev/null
    info "Ad-hoc signed dylib."
else
    warn "codesign not found; dyld may reject the dylib."
fi

mkdir -p "$RUNTIME_DST/Mods"
mkdir -p "$LOG_DIR"

cp "$RUNTIME_SRC/UE4SS-settings.ini" "$RUNTIME_DST/UE4SS-settings.ini"
cp "$RUNTIME_SRC/Mods/mods.txt" "$RUNTIME_DST/Mods/mods.txt"
find "$RUNTIME_SRC/Mods" -mindepth 1 -maxdepth 1 -type d -exec cp -R {} "$RUNTIME_DST/Mods/" \;

info "UE4SS runtime staged to: $RUNTIME_DST"
info "Injecting dylib: $DYLIB"
info "Launching: $GAME"
info "Log: $LOG"

set +e
DYLD_PRINT_LIBRARIES=1 DYLD_INSERT_LIBRARIES="$DYLIB" "$GAME" > "$LOG" 2>&1
STATUS=$?
set -e

echo "Game exited with status $STATUS"
exit "$STATUS"
