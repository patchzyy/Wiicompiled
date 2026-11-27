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

# An interrupted earlier run can leave its stage or backup behind. Remove only those whose owning
# process is gone; backups named by older versions (no process id) are left alone. A backup whose
# run stopped before publishing everything is moved back first, so it is never lost.
stale_stage_pattern='^[.]extract-stage-([0-9]+)$'
stale_backup_pattern='^[.]previous-extraction-([0-9]+)-[0-9]{8}-[0-9]{6}$'
extraction_items="DATA .data-manifest main.dol StaticR.rel"
for stale in "$assets_dir"/.extract-stage-* "$assets_dir"/.previous-extraction-*; do
    [[ -e "$stale" || -L "$stale" ]] || continue
    stale_name=${stale##*/}
    if [[ "$stale_name" =~ $stale_stage_pattern || "$stale_name" =~ $stale_backup_pattern ]]; then
        stale_pid=${BASH_REMATCH[1]}
        if ! kill -0 "$stale_pid" 2>/dev/null; then
            if [[ "$stale_name" =~ $stale_backup_pattern && -d "$stale" && ! -L "$stale" ]]; then
                # The run died while moving items into the backup or publishing the stage. Items are
                # only ever in both places once publishing started, and then everything in Assets is
                # from the new disc: unless that set is complete, it is dropped so the old one comes
                # back whole instead of mixing two extractions.
                present=0 shared=0
                for item in $extraction_items; do
                    if [[ -e "$assets_dir/$item" || -L "$assets_dir/$item" ]]; then
                        present=$((present + 1))
                        [[ -e "$stale/$item" ]] && shared=$((shared + 1))
                    fi
                done
                if (( present < 4 )); then
                    if (( shared > 0 )); then
                        for item in $extraction_items; do rm -rf "${assets_dir:?}/$item"; done
                    fi
                    for item in $extraction_items; do
                        if [[ ! -e "$assets_dir/$item" && ! -L "$assets_dir/$item" && -e "$stale/$item" ]]; then
                            mv "$stale/$item" "$assets_dir/$item"
                        fi
                    done
                fi
            fi
            rm -rf "$stale"
        fi
    fi
done

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
   cmp -s "$assets_dir/main.dol" "$assets_dir/DATA/sys/main.dol" && \
   cmp -s "$assets_dir/StaticR.rel" "$assets_dir/DATA/files/rel/StaticR.rel" && \
   [[ "$(data_manifest "$assets_dir/DATA")" == "$(cat "$assets_dir/.data-manifest")" ]]; then
    printf 'MKWCBUILD:STEP:disc-ready Reusing the verified extracted game assets\n'
    exit 0
fi
# Extracted straight into a stage beside the destination, so the final replacement stays on one
# volume and the data partition is never copied a second time.
stage="$assets_dir/.extract-stage-$$"
cleanup() { rm -rf "$stage"; }
trap cleanup EXIT
rm -rf "$stage"
mkdir -p "$stage"

printf 'MKWCBUILD:STEP:extract-disc Extracting the user-owned disc image\n'
"$nodtool" extract "$game" "$stage/DATA"

dol="$stage/DATA/sys/main.dol"
rel="$stage/DATA/files/rel/StaticR.rel"
[[ -f "$dol" ]] || fail 'nodtool extraction did not contain sys/main.dol'
[[ -f "$rel" ]] || fail 'nodtool extraction did not contain files/rel/StaticR.rel'
dol_sha=$(sha256 "$dol")
rel_sha=$(sha256 "$rel")
[[ $dol_sha == "$expected_dol_sha256" ]] || fail "main.dol does not match the clean $game_id image (expected $expected_dol_sha256, got $dol_sha)"
[[ $rel_sha == "$expected_rel_sha256" ]] || fail "StaticR.rel does not match the clean $game_id image (expected $expected_rel_sha256, got $rel_sha)"

[[ -d "$stage/DATA/files" ]] || fail 'nodtool extraction did not contain the Wii files directory'

ditto "$dol" "$stage/main.dol"
ditto "$rel" "$stage/StaticR.rel"
data_manifest "$stage/DATA" > "$stage/.data-manifest"

backup="$assets_dir/.previous-extraction-$$-$(date +%Y%m%d-%H%M%S)"
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
