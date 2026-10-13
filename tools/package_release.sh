#!/usr/bin/env bash
# Stage a ready-to-run release from a finished build and zip it.
#
# The result is what a player downloads: the menu, the four game programs and
# the files each reads beside itself. It holds no disc images, BIOS dumps,
# saves, cover art, settings from this machine, sources or generated C. The
# player runs the menu, which downloads the cover art, and the first Play on
# each game opens its launcher to choose their own disc image.
#
# Usage (from the repository root, after `cmake --build build`):
#   tools/package_release.sh [--build-dir build] [--version 1.0.0]
# Output: dist/TwistedMetal1234Recomp-<version>/ and the same name + .zip
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT"
BUILD=build
VERSION=""
while [[ $# -gt 0 ]]; do
  case "$1" in
    --build-dir) BUILD="${2:?}"; shift 2 ;;
    --version) VERSION="${2:?}"; shift 2 ;;
    *) echo "error: unknown argument $1" >&2; exit 1 ;;
  esac
done
[[ -n "$VERSION" ]] || VERSION="$(git describe --tags --always 2>/dev/null || echo dev)"
NAME="TwistedMetal1234Recomp-$VERSION"
STAGE="dist/$NAME"
[[ -f "$BUILD/TwistedMetal1234Recomp.exe" && -f "$BUILD/games.txt" ]] ||
  { echo "error: $BUILD has no built menu; run cmake --build $BUILD first" >&2; exit 1; }

rm -rf "$STAGE" "dist/$NAME.zip"
mkdir -p "$STAGE/licenses"
cp "$BUILD/TwistedMetal1234Recomp.exe" "$BUILD/games.txt" "$STAGE/"
# The shipped display defaults, never this machine's own settings.
cp hub/default-settings.toml "$STAGE/shared-settings.toml"

count=0
while IFS='|' read -r _title _subtitle exe _cover; do
  [[ -z "${exe:-}" || "${_title:0:1}" == "#" ]] && continue
  exe="${exe%$'\r'}"
  game="${exe%%/*}"
  src="$BUILD/$game"
  [[ -f "$BUILD/$exe" ]] || { echo "error: $BUILD/$exe is missing" >&2; exit 1; }
  for need in assets/fonts assets/img bios/openbios.bin bios/OpenBIOS.LICENSE game.toml mods/bundled; do
    [[ -e "$src/$need" ]] || { echo "error: $src/$need is missing; rebuild $game" >&2; exit 1; }
  done
  mkdir -p "$STAGE/$game/bios" "$STAGE/$game/mods"
  cp "$BUILD/$exe" "$STAGE/$game/"
  cp "$src/game.toml" "$STAGE/$game/"
  for opt in game_options.toml psx_game_version.txt; do
    [[ -f "$src/$opt" ]] && cp "$src/$opt" "$STAGE/$game/"
  done
  for dll in "$src"/*.dll; do [[ -e "$dll" ]] && cp "$dll" "$STAGE/$game/"; done
  cp -r "$src/assets" "$STAGE/$game/assets"
  cp "$src/bios/openbios.bin" "$src/bios/OpenBIOS.LICENSE" "$STAGE/$game/bios/"
  cp -r "$src/mods/bundled" "$STAGE/$game/mods/bundled"
  count=$((count + 1))
done < "$BUILD/games.txt"
[[ $count -gt 0 ]] || { echo "error: $BUILD/games.txt lists no games" >&2; exit 1; }

cp LICENSE "$STAGE/licenses/TwistedMetal1234Recomp-LICENSE"
cp psxrecomp/LICENSE "$STAGE/licenses/psxrecomp-LICENSE"
cp recomp-ui/LICENSE "$STAGE/licenses/recomp-ui-LICENSE"
[[ -d psxrecomp/runtime/licenses ]] && cp -r psxrecomp/runtime/licenses/. "$STAGE/licenses/"
[[ -f psxrecomp/THIRD_PARTY_ATTRIBUTION.md ]] && cp psxrecomp/THIRD_PARTY_ATTRIBUTION.md "$STAGE/licenses/"

cat > "$STAGE/START_HERE.txt" <<EOF
TwistedMetal1234Recomp $VERSION

Twisted Metal, Twisted Metal 2, Twisted Metal III and Twisted Metal 4 as
native Windows programs. Unofficial fan project; non-commercial use only.

You need your own disc image of each game you want to play (.chd, or .cue
with its .bin). Nothing from the games is included here.

Supported discs (USA): Twisted Metal SCUS-94304 and Twisted Metal 2
SCUS-94306, retail release; Twisted Metal III SCUS-94249 Rev 1 (not the first
pressing); Twisted Metal 4 SCUS-94560 Rev 1. Other versions are accepted by
the launcher but are not expected to run correctly.

1. Run TwistedMetal1234Recomp.exe. The cover art downloads in the background
   the first time.
2. Press PLAY on a game. The first time, its launcher opens: choose your disc
   image there, then start the game. After that PLAY goes straight in.
3. SETTINGS opens a game's launcher at any time: display (aspect ratio, FPS
   Mode, Smooth Motion), audio, controls, memory cards. Display, audio and
   control settings are shared by all four games.

The games run on the bundled open-source OpenBIOS (bios/OpenBIOS.LICENSE).
The game programs contain a machine-translated build of each game's own code.
Licenses are in the licenses folder.
EOF

# Nothing personal or copyrighted may ride along.
bad="$(find "$STAGE" -type f \( -iname '*.chd' -o -iname '*.cue' -o -iname '*.iso' -o -iname '*.mcd' -o -iname '*.mcr' \
      -o -iname 'settings.toml' -o -iname 'state.toml' -o -iname 'keybinds.ini' -o -iname 'input.ini' \
      -o -iname 'SCPH*' -o -iname '*.c' -o -iname '*.png' -path '*/covers/*' \) 2>/dev/null || true)"
[[ -z "$bad" ]] || { echo "error: the package would include files it must not:" >&2; echo "$bad" >&2; exit 1; }
bins="$(find "$STAGE" -type f -iname '*.bin' ! -path '*/bios/openbios.bin' 2>/dev/null || true)"
[[ -z "$bins" ]] || { echo "error: unexpected .bin files in the package:" >&2; echo "$bins" >&2; exit 1; }

PY="$(command -v python3 || command -v python)"
"$PY" - "$STAGE" "dist/$NAME.zip" <<'PY'
import os, sys, zipfile
stage, out = sys.argv[1], sys.argv[2]
base = os.path.dirname(stage)
with zipfile.ZipFile(out, 'w', zipfile.ZIP_DEFLATED, compresslevel=9) as z:
    for d, _, files in os.walk(stage):
        for f in sorted(files):
            p = os.path.join(d, f)
            z.write(p, os.path.relpath(p, base))
print(f"{out}: {os.path.getsize(out) / 1048576:.1f} MB")
PY
echo "staged $count game(s) in $STAGE"
