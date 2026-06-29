# UE4SS for Palworld — macOS Port (Apple Silicon / arm64)

*[한국어 README](README.ko.md)*

A native **macOS arm64** port of [RE-UE4SS](https://github.com/UE4SS-RE/RE-UE4SS), **targeted at
macOS-native Palworld** (App Store, Apple Silicon, UE5.1). It reuses the UE4SS "brain" (Unreal
Engine reflection + Lua scripting) and rewrites the "hands" — dylib injection, arm64 inline hooking,
and Mach-O symbol resolution — for Apple Silicon.

This is **not** WINE, Rosetta, or emulation: a native arm64 dylib is injected directly into the
native game process.

This loader is the runtime consumed by the **Palworld Mod Manager** app, which downloads it from
this repo's GitHub releases. See **Releases** below.

## Status

**Proof-of-capability, validated end-to-end on macOS-native Palworld** (App Store, arm64, UE5.1):
a Lua mod can **hook and call game `UFunction`s to change real game state** (demonstrated by an
auto-hatch / auto-receive mod). The full chain works on macOS: injection → bootstrap (6 engine
anchors) → reactive hooking → `UObject` listeners → Lua reflection → `ProcessEvent` /
`ProcessInternal` / `ExecuteInGameThread`.

### Scope & limitations (read this)

This is validated on **one sample (Palworld, n=1)**. It is **not yet a generic UE5.1 loader.**

- **Bootstrap is Palworld-overfit.** The engine anchors are resolved symbol-first with a
  Palworld-tuned AOB fallback, so other UE5.1 macOS games are not supported as-is. True
  genericization (scanner upgrade + new arm64 signatures + ≥2 samples) is a separate, larger effort.
- **Runtime member-offset table is only partially mac-verified.** Three structs
  (`UClass`, `UEnum`, `AGameModeBase`) were corrected for macOS via disassembly; the rest are Linux
  values that happen to match by ABI coincidence and are unverified on macOS.
- **GUI subsystem is disabled** (built with `--ue4ssUI=None`).
- **macOS key-input backend is not ported** → `RegisterKeyBind` is unavailable. Trigger Lua via
  event hooks instead.
- **Out of scope:** game discovery, a product-grade launcher/injector, mod install/enable/update,
  and any mod manager UI. This repository only keeps experimental and minimal packaging scripts.

## Requirements

- macOS on Apple Silicon (arm64)
- Xcode Command Line Tools
- [xmake](https://xmake.io/) (e.g. `brew install xmake`)
- capstone (`brew install capstone`)
- The target game (e.g. Palworld) for runtime testing

## Build

```sh
# Unit tests — no game required
make test

# Full UE4SS dylib (Shipping, arm64, GUI disabled)
xmake f -P . -p macosx -a arm64 -m Game__Shipping__Mac --ue4ssUI=None -y
xmake build -P . -j4 UE4SS
```

## Run (Palworld example)

```sh
./tools/stage-ue4ss-runtime.sh   # stage settings + mods into the sandbox container
./tools/launch-ue4ss.sh          # inject via DYLD_INSERT_LIBRARIES and launch
./tools/tail-log.sh              # stream the runtime log
```

Injection requires launching through the script (Spotlight / double-click will not inject).
UE4SS runtime data (logs, Mods, settings) lives in the game's sandbox container, not in this repo.

### Minimum launch package layout

The minimal pre-zip package is generated under `dist/UE4SS_mac/`:

```sh
./tools/package-ue4ss-mac.sh --zip
```

After the user receives that folder, running `launch-palworld.command` performs a basic preflight
(arm64 check, dylib/settings/mod files, Palworld sandbox container, dylib dependencies, ad-hoc
signing). If it passes, the script first copies the packaged settings/mod files into the Palworld
sandbox container, then injects only `libUE4SS.dylib` into the game process.

Package folder:

```text
dist/UE4SS_mac/
  launch-palworld.command
  libUE4SS.dylib
  UE4SS/
    UE4SS-settings.ini
    Mods/
      mods.txt
```

Runtime layout created in the Palworld container when the launcher runs:

```text
~/Library/Containers/com.pocketpair.palworld.mac/Data/UE4SS/
  UE4SS-settings.ini
  Mods/
    mods.txt
```

`launch-palworld.command` injects only the package's `libUE4SS.dylib`:

```sh
DYLD_INSERT_LIBRARIES="$PACKAGE_DIR/libUE4SS.dylib" \
  /Applications/Palworld.app/Contents/MacOS/Palworld
```

After the dylib is loaded inside the game process, UE4SS uses `$HOME/UE4SS` as its working
directory. In the Palworld sandbox environment, that resolves to the container `Data/UE4SS` path
above, where UE4SS reads `UE4SS-settings.ini` and `Mods/`. To include mods in the package, place
them under `dist/UE4SS_mac/UE4SS/Mods/<ModName>/`.

## Releases

The Palworld Mod Manager consumes this loader from GitHub releases. The release contract is fixed:

- Version source of truth: the `VERSION` file at the repo root (semver `X.Y.Z`).
- Each release is tagged `vX.Y.Z` and carries a single asset named **`UE4SS_mac.zip`** whose payload
  contains `UE4SS_mac/libUE4SS.dylib`.

Cut a release with:

```sh
./tools/release.sh --dry-run   # build + zip only, no publish
./tools/release.sh             # tag vX.Y.Z + publish UE4SS_mac.zip to GitHub
```

Bump `VERSION` before publishing; `release.sh` refuses to clobber an existing tag unless
`--replace` is passed.

## Mods

- **`PmmLuaProbe`** (included) — a minimal Lua-runtime smoke test. Proves the Lua VM loads and runs
  inside the injected dylib. Use it as a minimal example and an in-repo health check.
- **AutoHatch** (Palworld-specific demo) is kept in a **separate mods repo**, not here. For local
  real-game testing, point the staging script at your local copy:
  ```sh
  PMM_AUTOHATCH_DIR=/path/to/AutoHatch ./tools/stage-ue4ss-runtime.sh
  ```
  (Defaults to `../04_AutoHatch`; absent → skipped.)

For Lua API and mod authoring, see the bundled reference under `docs/lua-api/`. It documents the
UE4SS Lua API as of the vendored snapshot; the macOS port does **not** change the API surface, so it
applies as-is. See *Limitations* for mac-specific runtime exceptions (e.g. `RegisterKeyBind`).

## Tests / regression

```sh
./tools/regression-check.sh      # unit tests + on-binary symbol scan
```

## Layout

| Path | Contents |
|------|----------|
| `src/darwin/` | Mach-O memory access, module/segment parsing, AOB pattern scanning, logging |
| `src/hook/` | arm64 relocator (B/BL/thunk resolution) and inline-hook primitives |
| `src/entry.cpp` | injected dylib entry point |
| `UE4SS/` | upstream UE4SS core (reflection + Lua), built with the Darwin entry |
| `deps/first/` | `UEPseudo` and `patternsleuth` submodules |
| `tools/` | build/stage/launch/log scripts and AOB derivation helpers |
| `tests/` | C++ unit tests (AOB, hooking, relocation, memory, symbol scan) |

## License

MIT — a port of [RE-UE4SS](https://github.com/UE4SS-RE/RE-UE4SS) (MIT).
`UEPseudo` is a gated submodule (Epic EULA) and is **not** vendored here.
