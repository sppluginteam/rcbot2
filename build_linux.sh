#!/usr/bin/env bash
# RCBot2 DoD:S Linux build: x86 + x86_64, then stage a release package.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
NO_ARCHIVE=0
[[ "${1:-}" == "--no-archive" ]] && NO_ARCHIVE=1

find_dep() {
  local env_name="$1"; shift
  if [[ -n "${!env_name:-}" && -d "${!env_name}" ]]; then printf '%s\n' "${!env_name}"; return; fi
  local candidate
  for candidate in "$@"; do
    if [[ -d "$candidate" ]]; then printf '%s\n' "$candidate"; return; fi
  done
  printf '%s\n' ""
}

SM_PATH="$(find_dep SM_PATH "$ROOT/deps/sourcemod" "$ROOT/sourcemod" "$ROOT/../sourcemod")"
MMS_PATH="$(find_dep MMS_PATH "$ROOT/deps/mmsource" "$ROOT/mmsource" "$ROOT/../mmsource")"
HL2SDK_ROOT="$(find_dep HL2SDK_ROOT "$ROOT/deps" "$ROOT" "$ROOT/..")"

[[ -f "$SM_PATH/core/logic/ExtensionSys.cpp" ]] || { echo "SourceMod not found; set SM_PATH" >&2; exit 1; }
[[ -f "$MMS_PATH/core/metamod_plugins.cpp" ]] || { echo "Metamod:Source not found; set MMS_PATH" >&2; exit 1; }
[[ -d "$HL2SDK_ROOT/hl2sdk-dods/public" ]] || { echo "hl2sdk-dods not found; set HL2SDK_ROOT" >&2; exit 1; }
python3 -c 'import ambuild2' 2>/dev/null || python3 -m pip install --user ambuild

build_one() {
  local arch="$1" dir="$ROOT/build_$1"
  rm -rf "$dir"; mkdir -p "$dir"; cd "$dir"
  python3 "$ROOT/configure.py" --sm-path "$SM_PATH" --mms-path "$MMS_PATH" \
    --hl2sdk-root "$HL2SDK_ROOT" --sdks=dods --targets="$arch" --enable-optimize
  ambuild
}

build_one x86
build_one x86_64

DIST="$ROOT/dist"; rm -rf "$DIST"; mkdir -p "$DIST"
cp -a "$ROOT/build_x86/package/." "$DIST/"
mkdir -p "$DIST/addons/rcbot2/bin/x64"
cp -a "$ROOT/build_x86_64/package/addons/rcbot2/bin/x64/." "$DIST/addons/rcbot2/bin/x64/"

if [[ "$NO_ARCHIVE" -eq 0 ]]; then
  tar -czf "$ROOT/RCBot2-dods-linux.tar.gz" -C "$DIST" addons
fi
echo "Built: $DIST"
