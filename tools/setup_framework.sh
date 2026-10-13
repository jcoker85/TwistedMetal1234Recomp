#!/usr/bin/env bash
# Prepare the shared framework checkouts: apply this repo's patches and build
# the recompiler emitters (plus the CHD reader the tools use).
#
# A patch is applied to the submodule its file name starts with, in name order:
#   patches/psxrecomp-*.patch  -> psxrecomp/
#   patches/recomp-ui-*.patch  -> recomp-ui/
# Safe to run again: a submodule that already carries exactly these patches is
# left alone. One with other local changes stops the script.
#
# Usage (from the repository root):
#   tools/setup_framework.sh [--toolchain-dir DIR]
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT"

apply_all() {   # <checkout dir> <submodule name>
  local dir="$1" sub="$2" p
  for p in "$ROOT"/patches/"$sub"-*.patch; do
    [[ -e "$p" ]] || continue
    git -C "$dir" apply "$p" || { echo "error: $(basename "$p") does not apply to $sub $(git -C "$dir" rev-parse --short HEAD)" >&2; return 1; }
  done
}

for sub in psxrecomp recomp-ui; do
  [[ -e "$sub/.git" ]] || { echo "error: $sub is missing; run: git submodule update --init --recursive" >&2; exit 1; }
  if git -C "$sub" diff --quiet; then
    apply_all "$sub" "$sub"
    echo "patched $sub"
    continue
  fi
  # Already modified: accept it only if it is exactly this repo's patches.
  tmp="$(mktemp -d)"
  git -C "$sub" worktree add --quiet --detach "$tmp/tree" HEAD
  ok=0
  if apply_all "$tmp/tree" "$sub" 2>/dev/null &&
     diff -q <(git -C "$tmp/tree" diff) <(git -C "$sub" diff) >/dev/null; then ok=1; fi
  git -C "$sub" worktree remove --force "$tmp/tree"
  rm -rf "$tmp"
  if [[ $ok == 1 ]]; then
    echo "already patched: $sub"
  else
    echo "error: $sub has local changes that are not this repo's patches." >&2
    echo "       To start over: git -C $sub checkout . && tools/setup_framework.sh" >&2
    exit 1
  fi
done

bash psxrecomp/tools/ci/build_emitters.sh --framework psxrecomp --build-dir psxrecomp/recompiler/build "$@"
cmake --build psxrecomp/recompiler/build --target chdr
