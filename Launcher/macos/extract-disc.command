#!/usr/bin/env bash
# Extract a user-owned Mario Kart Wii disc image (PAL, NTSC-U, NTSC-J, or NTSC-K) for a local build.
# This script deliberately contains no game data and is intended for the macOS
# setup application and for maintainers testing that setup path.
set -euo pipefail

fail() { printf 'extract-disc.command: error: %s\n' "$*" >&2; exit 1; }
sha256() { shasum -a 256 "$1" | awk '{ print $1 }'; }

expected_dol_sha256() {
    case "$1" in
        RMCP01) echo "80d18895b39c63bd80f457398bfcbb91b7d16ac116a41a88967e954080155b05" ;;
        RMCE01) echo "d2beec1b1645fcd134efe9e7e63774b546667764ed8d431029daccd725995694" ;;
        RMCJ01) echo "1b9621ef7c5d97dada103e50e5389730e67f3c2545dda592edd4b5843655af91" ;;
        RMCK01) echo "3098a1e9259b4915e32a4ccb5a1f124823f2a0914db29cd2476e10bdb01a77da" ;;
        *) echo "" ;;
    esac
}

expected_rel_sha256() {
    case "$1" in
        RMCP01) echo "16d9d146112541fefea701ecb5bc1a496f9d50e4a752fbb5b6778e7c6399f67d" ;;
        RMCE01) echo "1168107f8fdef27a356df76036db55afe4fbf7752606dbac991726701133a617" ;;
        RMCJ01) echo "88539012d357a1420724e51dc7e351192ce696da4b0045994895518a3fad6fae" ;;
        RMCK01) echo "f441b08e4ccc2d64aadcac8429973f10ea6b1cc8f175500fc084983775f7b3e5" ;;
        *) echo "" ;;
    esac
}

usage() {
    cat <<'EOF'
Usage: extract-disc.command --game IMAGE --assets-dir DIR --nodtool PATH

Extracts a user-owned Mario Kart Wii disc image (RMCP01, RMCE01, RMCJ01, or
RMCK01) into DIR. The final layout is DIR/main.dol, DIR/StaticR.rel, and
DIR/DATA. Existing data is left untouched unless the complete new extraction
passes both content hash checks.
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
scratch=$(mktemp -d "${TMPDIR:-/tmp}/wiicompiled-disc.XXXXXX")
cleanup() { rm -rf "$scratch"; }
trap cleanup EXIT

printf 'MKWCBUILD:STEP:validate-disc Checking the selected disc image\n'
"$nodtool" info "$game" >/dev/null
printf 'MKWCBUILD:STEP:extract-disc Extracting the user-owned disc image\n'
"$nodtool" extract "$game" "$scratch/extracted"

dol=$(find "$scratch/extracted" -type f -path '*/sys/main.dol' -print -quit)
rel=$(find "$scratch/extracted" -type f -path '*/files/rel/StaticR.rel' -print -quit)
boot_bin=$(find "$scratch/extracted" -type f -path '*/sys/boot.bin' -print -quit)
[[ -n "$dol" ]] || fail 'nodtool extraction did not contain sys/main.dol'
[[ -n "$rel" ]] || fail 'nodtool extraction did not contain files/rel/StaticR.rel'

game_id=""
if [[ -n "$boot_bin" ]]; then
    game_id=$(head -c 6 "$boot_bin" 2>/dev/null || true)
fi
if [[ -z "$game_id" || -z "$(expected_dol_sha256 "$game_id")" ]]; then
    actual_dol_hash=$(sha256 "$dol")
    case "$actual_dol_hash" in
        80d18895b39c63bd80f457398bfcbb91b7d16ac116a41a88967e954080155b05) game_id="RMCP01" ;;
        d2beec1b1645fcd134efe9e7e63774b546667764ed8d431029daccd725995694) game_id="RMCE01" ;;
        1b9621ef7c5d97dada103e50e5389730e67f3c2545dda592edd4b5843655af91) game_id="RMCJ01" ;;
        3098a1e9259b4915e32a4ccb5a1f124823f2a0914db29cd2476e10bdb01a77da) game_id="RMCK01" ;;
        *) fail "disc image is not a supported clean Mario Kart Wii release (RMCP01, RMCE01, RMCJ01, RMCK01)" ;;
    esac
fi

expected_dol=$(expected_dol_sha256 "$game_id")
expected_rel=$(expected_rel_sha256 "$game_id")
[[ $(sha256 "$dol") == "$expected_dol" ]] || fail "disc main.dol sha256 does not match the clean $game_id revision"
[[ $(sha256 "$rel") == "$expected_rel" ]] || fail "disc StaticR.rel sha256 does not match the clean $game_id revision"

data_root=$(dirname "$(dirname "$dol")")
[[ -d "$data_root/files" ]] || fail 'nodtool extraction did not contain the Wii files directory'

# Stage beside the destination so the final replacement stays on one volume.
stage="$assets_dir/.extract-stage-$$"
rm -rf "$stage"
mkdir -p "$stage"
ditto "$data_root" "$stage/DATA"
ditto "$dol" "$stage/main.dol"
ditto "$rel" "$stage/StaticR.rel"

backup="$assets_dir/.previous-extraction-$(date +%Y%m%d-%H%M%S)"
if [[ -e "$assets_dir/DATA" || -e "$assets_dir/main.dol" || -e "$assets_dir/StaticR.rel" ]]; then
    mkdir -p "$backup"
    for item in DATA main.dol StaticR.rel; do
        [[ -e "$assets_dir/$item" ]] && mv "$assets_dir/$item" "$backup/$item"
    done
fi
mv "$stage/DATA" "$assets_dir/DATA"
mv "$stage/main.dol" "$assets_dir/main.dol"
mv "$stage/StaticR.rel" "$assets_dir/StaticR.rel"
rmdir "$stage"
rm -rf "$backup"

printf 'MKWCBUILD:STEP:disc-ready Verified and extracted clean %s game assets\n' "$game_id"
