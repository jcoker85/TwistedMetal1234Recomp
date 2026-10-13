#!/usr/bin/env bash
# Regenerate one game's recompiled C from your own disc image.
#
# Usage (from the repository root, after tools/setup_framework.sh):
#   tools/generate.sh <tm1|tm2|tm3|tm4> <path to .cue or .chd>
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
GAME="${1:?game id}"; DISC="${2:?disc image}"
G="$ROOT/games/$GAME"
[[ -f "$G/game.toml" ]] || { echo "error: no such game: $GAME" >&2; exit 1; }
PY="$(command -v python3 || command -v python)"
cd "$G"
if "$PY" "$ROOT/psxrecomp/psxrecomp_cli.py" generate --config game.toml --project-root . --disc "$DISC"; then
  exit 0
fi
# Twisted Metal III's boot EXE is padded on disc past the size its header
# declares, which psxrecomp-game rejects. The step above has already staged
# the EXE and regenerated the BIOS, so trim the padding and emit the game C.
EXE="disc/$(sed -n 's/^boot_exe *= *"\(.*\)"/\1/p' game.toml)"
"$PY" - "$EXE" <<'PY'
import struct, sys
p = sys.argv[1]; d = open(p, 'rb').read()
want = 2048 + struct.unpack_from('<I', d, 0x1C)[0]
if len(d) <= want or any(d[want:]):
    sys.exit("generate failed and the boot EXE has no zero padding to trim")
open(p, 'wb').write(d[:want])
print(f"trimmed {len(d) - want} padding bytes from {p}")
PY
"$ROOT/psxrecomp/recompiler/build/psxrecomp-game" --config game.toml
