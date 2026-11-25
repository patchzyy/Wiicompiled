#!/usr/bin/env bash
# Extract a user-owned Mario Kart Wii disc image of a supported release for a local build.
# This script deliberately contains no game data and is intended for the macOS
# setup application and for maintainers testing that setup path.
set -euo pipefail

script_dir=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
# shellcheck source=../build-cache.sh
source "$script_dir/../build-cache.sh"

fail() { printf 'extract-disc.command: error: %s\n' "$*" >&2; exit 1; }
sha256() { shasum -a 256 "$1" | awk '{ print $1 }'; }

usage() {
    cat <<'EOF'
Usage: extract-disc.command --game IMAGE --assets-dir DIR --nodtool PATH [--workspace DIR]

Extracts a user-owned Mario Kart Wii image into DIR. The image must be a release
a projects/*/recomp.yml in the workspace (default: this script's repository)
pins. The final layout is DIR/main.dol, DIR/StaticR.rel, and DIR/DATA. Existing
data is left untouched unless the complete new extraction passes both content
hash checks.
EOF
}

game=""
assets_dir=""
nodtool=""
workspace=$(cd "$script_dir/../.." && pwd)
while (($#)); do
    case "$1" in
        --workspace) workspace=${2:-}; shift 2 ;;
        --game) game=${2:-}; shift 2 ;;
        --assets-dir) assets_dir=${2:-}; shift 2 ;;
        --nodtool) nodtool=${2:-}; shift 2 ;;
        -h|--help) usage; exit 0 ;;
        *) fail "unknown option: $1" ;;
    esac
done

[[ -f "$game" ]] || fail "disc image does not exist: $game"
[[ -n "$assets_dir" ]] || fail "--assets-dir is required"
[[ -x "$nodtool" ]] || fail "nodtool is not executable: $nodtool"

assets_dir=$(mkdir -p "$assets_dir" && cd "$assets_dir" && pwd)

# The disc's game ID selects the release, so an existing extraction is only reused for the same one.
printf 'MKWCBUILD:STEP:validate-disc Checking the selected disc image\n'
disc_info=$("$nodtool" info "$game") || fail 'nodtool could not read the disc image'
game_id=$(awk '/^Game ID: / { print $3; exit }' <<<"$disc_info")
[[ -n "$game_id" ]] || fail 'nodtool did not report the disc game ID'
project=$(mkw_project_for_game "$workspace" "$game_id") || fail "disc $game_id is not a supported Mario Kart Wii release"
expected_dol_sha256=$(mkw_project_pin "$project" dol)
expected_rel_sha256=$(mkw_project_pin "$project" rel)
[[ -n "$expected_dol_sha256" && -n "$expected_rel_sha256" ]] || fail "$project does not pin the clean $game_id digests"

# Sorted "<size> <path>" of every extracted file; recorded at extraction, compared on reuse.
data_manifest() { (cd "$1" && find . -type f -print0 | LC_ALL=C sort -z | xargs -0 stat -f '%z %N'); }

if [[ -d "$assets_dir/DATA/files" && -f "$assets_dir/.data-manifest" && -f "$assets_dir/main.dol" && -f "$assets_dir/StaticR.rel" ]] && \
   [[ $(sha256 "$assets_dir/main.dol") == "$expected_dol_sha256" && $(sha256 "$assets_dir/StaticR.rel") == "$expected_rel_sha256" ]] && \
   [[ "$(data_manifest "$assets_dir/DATA")" == "$(cat "$assets_dir/.data-manifest")" ]]; then
    printf 'MKWCBUILD:STEP:disc-ready Reusing the verified extracted game assets\n'
    exit 0
fi
scratch=$(mktemp -d "${TMPDIR:-/tmp}/wiicompiled-disc.XXXXXX")
cleanup() { rm -rf "$scratch"; }
trap cleanup EXIT

printf 'MKWCBUILD:STEP:extract-disc Extracting the user-owned disc image\n'
"$nodtool" extract "$game" "$scratch/extracted"

dol=$(find "$scratch/extracted" -type f -path '*/sys/main.dol' -print -quit)
rel=$(find "$scratch/extracted" -type f -path '*/files/rel/StaticR.rel' -print -quit)
[[ -n "$dol" ]] || fail 'nodtool extraction did not contain sys/main.dol'
[[ -n "$rel" ]] || fail 'nodtool extraction did not contain files/rel/StaticR.rel'
dol_sha=$(sha256 "$dol")
rel_sha=$(sha256 "$rel")
[[ $dol_sha == "$expected_dol_sha256" ]] || fail "main.dol does not match the clean $game_id image (expected $expected_dol_sha256, got $dol_sha)"
[[ $rel_sha == "$expected_rel_sha256" ]] || fail "StaticR.rel does not match the clean $game_id image (expected $expected_rel_sha256, got $rel_sha)"

data_root=$(dirname "$(dirname "$dol")")
[[ -d "$data_root/files" ]] || fail 'nodtool extraction did not contain the Wii files directory'

# Stage beside the destination so the final replacement stays on one volume.
stage="$assets_dir/.extract-stage-$$"
rm -rf "$stage"
mkdir -p "$stage"
ditto "$data_root" "$stage/DATA"
ditto "$dol" "$stage/main.dol"
ditto "$rel" "$stage/StaticR.rel"
data_manifest "$stage/DATA" > "$stage/.data-manifest"

backup="$assets_dir/.previous-extraction-$(date +%Y%m%d-%H%M%S)"
if [[ -e "$assets_dir/DATA" || -e "$assets_dir/.data-manifest" || -e "$assets_dir/main.dol" || -e "$assets_dir/StaticR.rel" ]]; then
    mkdir -p "$backup"
    for item in DATA .data-manifest main.dol StaticR.rel; do
        [[ -e "$assets_dir/$item" ]] && mv "$assets_dir/$item" "$backup/$item"
    done
fi
mv "$stage/DATA" "$assets_dir/DATA"
mv "$stage/.data-manifest" "$assets_dir/.data-manifest"
mv "$stage/main.dol" "$assets_dir/main.dol"
mv "$stage/StaticR.rel" "$assets_dir/StaticR.rel"
rmdir "$stage"
rm -rf "$backup"

printf 'MKWCBUILD:STEP:disc-ready Verified and extracted clean %s game assets\n' "$game_id"
