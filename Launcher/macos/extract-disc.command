#!/usr/bin/env bash
# Extract a user-owned Mario Kart Wii PAL (RMCP01) disc image for a local build.
# This script deliberately contains no game data and is intended for the macOS
# setup application and for maintainers testing that setup path.
set -euo pipefail

readonly EXPECTED_DOL_SHA256=80d18895b39c63bd80f457398bfcbb91b7d16ac116a41a88967e954080155b05
readonly EXPECTED_REL_SHA256=16d9d146112541fefea701ecb5bc1a496f9d50e4a752fbb5b6778e7c6399f67d

fail() { printf 'extract-disc.command: error: %s\n' "$*" >&2; exit 1; }
sha256() { shasum -a 256 "$1" | awk '{ print $1 }'; }

usage() {
    cat <<'EOF'
Usage: extract-disc.command --game IMAGE --assets-dir DIR --nodtool PATH

Extracts a user-owned Mario Kart Wii PAL RMCP01 image into DIR. The final
layout is DIR/main.dol, DIR/StaticR.rel, and DIR/DATA. Existing data is left
untouched unless the complete new extraction passes both content hash checks.
EOF
}

game=""
assets_dir=""
nodtool=""
while (($#)); do
    case "$1" in
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

# Sorted "<size> <path>" of every extracted file; recorded at extraction, compared on reuse.
data_manifest() { (cd "$1" && find . -type f -print0 | LC_ALL=C sort -z | xargs -0 stat -f '%z %N'); }

if [[ -d "$assets_dir/DATA/files" && -f "$assets_dir/.data-manifest" && -f "$assets_dir/main.dol" && -f "$assets_dir/StaticR.rel" ]] && \
   [[ $(sha256 "$assets_dir/main.dol") == "$EXPECTED_DOL_SHA256" && $(sha256 "$assets_dir/StaticR.rel") == "$EXPECTED_REL_SHA256" ]] && \
   [[ "$(data_manifest "$assets_dir/DATA")" == "$(cat "$assets_dir/.data-manifest")" ]]; then
    printf 'MKWCBUILD:STEP:disc-ready Reusing the verified extracted game assets\n'
    exit 0
fi
scratch=$(mktemp -d "${TMPDIR:-/tmp}/wiicompiled-disc.XXXXXX")
cleanup() { rm -rf "$scratch"; }
trap cleanup EXIT

printf 'MKWCBUILD:STEP:validate-disc Checking the selected disc image\n'
"$nodtool" info "$game" >/dev/null
printf 'MKWCBUILD:STEP:extract-disc Extracting the user-owned disc image\n'
"$nodtool" extract "$game" "$scratch/extracted"

dol=$(find "$scratch/extracted" -type f -path '*/sys/main.dol' -print -quit)
rel=$(find "$scratch/extracted" -type f -path '*/files/rel/StaticR.rel' -print -quit)
[[ -n "$dol" ]] || fail 'nodtool extraction did not contain sys/main.dol'
[[ -n "$rel" ]] || fail 'nodtool extraction did not contain files/rel/StaticR.rel'
dol_sha=$(sha256 "$dol")
rel_sha=$(sha256 "$rel")
[[ $dol_sha == "$EXPECTED_DOL_SHA256" ]] || fail "main.dol does not match the clean PAL RMCP01 image (expected $EXPECTED_DOL_SHA256, got $dol_sha)"
[[ $rel_sha == "$EXPECTED_REL_SHA256" ]] || fail "StaticR.rel does not match the clean PAL RMCP01 image (expected $EXPECTED_REL_SHA256, got $rel_sha)"

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

printf 'MKWCBUILD:STEP:disc-ready Verified and extracted clean PAL RMCP01 game assets\n'
