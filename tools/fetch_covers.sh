#!/usr/bin/env bash
# Download the four front covers the hub uses as backgrounds, from the
# libretro-thumbnails project, into hub/covers/<id>.png. They are not committed.
# Re-run CMake afterwards so they are copied beside the hub.
#
# Usage (from the repository root): tools/fetch_covers.sh
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT"
PY="$(command -v python3 || command -v python)"
mkdir -p hub/covers
fetch() {
  local id="$1" name="$2" tmp
  tmp="$(mktemp -d)"
  "$PY" psxrecomp/tools/new_project_layout/fetch_boxart.py --name "$name" \
      --out "$tmp/cover.tga" --png-out "hub/covers/$id.png" \
      --source-out "hub/covers/$id.source.txt" >/dev/null
  echo "$id: $name"
}
fetch tm1 "Twisted Metal (USA)"
fetch tm2 "Twisted Metal 2 (USA)"
fetch tm3 "Twisted Metal III (USA)"
fetch tm4 "Twisted Metal 4 (USA)"
