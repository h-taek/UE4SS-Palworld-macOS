#!/usr/bin/env bash
# Build the minimal Palworld launch package under dist/UE4SS_mac by default.

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(dirname "$SCRIPT_DIR")"

OUT_DIR="$PROJECT_ROOT/dist/UE4SS_mac"
DYLIB="$PROJECT_ROOT/Binaries/Game__Shipping__Mac/UE4SS/libUE4SS.dylib"
MAKE_ZIP=0

usage() {
    cat <<'USAGE'
Usage: tools/package-ue4ss-mac.sh [--dylib PATH] [--out-dir PATH] [--zip]

Creates:
  dist/UE4SS_mac/
    launch-palworld.command
    libUE4SS.dylib
    UE4SS/
      UE4SS-settings.ini
      Mods/
        mods.txt

Environment overrides:
  UE4SS_PACKAGE_DYLIB    default dylib path
  UE4SS_PACKAGE_OUT_DIR  default output directory
USAGE
}

DYLIB="${UE4SS_PACKAGE_DYLIB:-$DYLIB}"
OUT_DIR="${UE4SS_PACKAGE_OUT_DIR:-$OUT_DIR}"

while [ "$#" -gt 0 ]; do
    case "$1" in
        --dylib)
            [ "$#" -ge 2 ] || { echo "Missing value for --dylib" >&2; exit 2; }
            DYLIB="$2"
            shift 2
            ;;
        --out-dir)
            [ "$#" -ge 2 ] || { echo "Missing value for --out-dir" >&2; exit 2; }
            OUT_DIR="$2"
            shift 2
            ;;
        --zip)
            MAKE_ZIP=1
            shift
            ;;
        -h|--help)
            usage
            exit 0
            ;;
        *)
            echo "Unknown argument: $1" >&2
            usage >&2
            exit 2
            ;;
    esac
done

ASSETS_INI="$PROJECT_ROOT/assets/UE4SS-settings.ini"
ASSETS_MODS="$PROJECT_ROOT/assets/Mods"
LAUNCHER_TEMPLATE="$PROJECT_ROOT/tools/templates/launch-palworld.command"

[ -f "$DYLIB" ] || { echo "Missing dylib: $DYLIB" >&2; exit 1; }
[ -f "$ASSETS_INI" ] || { echo "Missing settings template: $ASSETS_INI" >&2; exit 1; }
[ -f "$LAUNCHER_TEMPLATE" ] || { echo "Missing launcher template: $LAUNCHER_TEMPLATE" >&2; exit 1; }

case "$OUT_DIR" in
    ""|"/"|"$PROJECT_ROOT"|"$PROJECT_ROOT/"|"$HOME"|"$HOME/")
        echo "Refusing unsafe output directory: $OUT_DIR" >&2
        exit 1
        ;;
esac

if ! file "$DYLIB" | grep -q "arm64"; then
    echo "Dylib is not arm64: $DYLIB" >&2
    exit 1
fi

rm -rf "$OUT_DIR"
mkdir -p "$OUT_DIR/UE4SS/Mods"

cp "$DYLIB" "$OUT_DIR/libUE4SS.dylib"
cp "$LAUNCHER_TEMPLATE" "$OUT_DIR/launch-palworld.command"
chmod +x "$OUT_DIR/launch-palworld.command"

# Render UE4SS' build-time template syntax into the plain ini expected at runtime.
awk '/\$\{if/{skip=1} skip==0{print} /\$\{endif\}/{skip=0}' "$ASSETS_INI" \
  | sed -e 's/= \$EnableHotReloadSystem/= 0/' \
        -e 's/= \$IgnoreEngineAndCoreUObject/= 1/' \
        -e 's/= \$ConsoleEnabled/= 1/' \
        -e 's/= \$GuiConsoleVisible/= 0/' \
        -e 's/= \$MaxMemoryUsageDuringAssetLoading/= 80/' \
        -e 's/= \$GUIUFunctionCaller/= 0/' \
  > "$OUT_DIR/UE4SS/UE4SS-settings.ini"

if grep -qE '\$\{|= \$[A-Za-z]' "$OUT_DIR/UE4SS/UE4SS-settings.ini"; then
    echo "Rendered settings still contain unresolved template variables:" >&2
    grep -nE '\$\{|= \$[A-Za-z]' "$OUT_DIR/UE4SS/UE4SS-settings.ini" >&2
    exit 1
fi

if [ -f "$ASSETS_MODS/mods.txt" ]; then
    cp "$ASSETS_MODS/mods.txt" "$OUT_DIR/UE4SS/Mods/mods.txt"
else
    touch "$OUT_DIR/UE4SS/Mods/mods.txt"
fi

if command -v codesign >/dev/null 2>&1; then
    codesign -s - --force --timestamp=none "$OUT_DIR/libUE4SS.dylib" >/dev/null
fi

if [ "$MAKE_ZIP" -eq 1 ]; then
    ZIP_PATH="$(dirname "$OUT_DIR")/$(basename "$OUT_DIR").zip"
    rm -f "$ZIP_PATH"
    (
        cd "$(dirname "$OUT_DIR")"
        zip -qry "$(basename "$ZIP_PATH")" "$(basename "$OUT_DIR")"
    )
    echo "WROTE $ZIP_PATH"
fi

echo "WROTE $OUT_DIR"
