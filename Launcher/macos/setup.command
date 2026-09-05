#!/usr/bin/env bash
# Entry point bundled in WiiCompiled Setup.app. The package contains source and
# tools only; a user's own verified disc is extracted into Application Support.
set -euo pipefail

resources=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
workspace_source="$resources/workspace"
cmake_bin="$resources/tools/cmake/bin/cmake"
support_root="$HOME/Library/Application Support/WiiCompiled"
workspace="$support_root/BuildWorkspace"
products="$support_root/Products"

fail() { printf 'WiiCompiled Setup: %s\n' "$*" >&2; exit 1; }
notice() { /usr/bin/osascript -e "display dialog \"${1//\"/\\\"}\" buttons {\"OK\"} default button \"OK\" with icon caution" >/dev/null; }
usage() {
    cat <<'EOF'
Usage: setup.command --game IMAGE [--retro-dir DIR] [--install-location {user|applications}]

Without arguments this script opens file pickers. It is normally launched by
WiiCompiled Setup.app, not run directly.
EOF
}

game=""; retro_dir=""; install_location=applications
while (($#)); do
    case "$1" in
        --game) game=${2:-}; shift 2 ;;
        --retro-dir) retro_dir=${2:-}; shift 2 ;;
        --install-location) install_location=${2:-}; shift 2 ;;
        -h|--help) usage; exit 0 ;;
        *) fail "unknown option: $1" ;;
    esac
done
[[ "$install_location" == user || "$install_location" == applications ]] || fail '--install-location must be user or applications'

host_arch=$(uname -m)
case "$host_arch" in arm64|x86_64) ;; *) fail "unsupported macOS architecture: $host_arch" ;; esac
host_tools="$resources/tools/$host_arch"
nodtool="$host_tools/nodtool"
translator="$host_tools/Translator.Cli"
ninja_bin="$host_tools/ninja"

if [[ -z "$game" ]]; then
    game=$(/usr/bin/osascript <<'APPLESCRIPT'
set selectedFile to choose file with prompt "Choose your clean Mario Kart Wii PAL (RMCP01) disc image"
POSIX path of selectedFile
APPLESCRIPT
) || exit 0
    choice=$(/usr/bin/osascript -e 'button returned of (display dialog "Would you like to build Retro Rewind too?" buttons {"Base game only", "Choose Retro Rewind folder"} default button "Base game only")')
    if [[ "$choice" == 'Choose Retro Rewind folder' ]]; then
        retro_dir=$(/usr/bin/osascript <<'APPLESCRIPT'
set selectedFolder to choose folder with prompt "Choose the RetroRewind6 folder (or its parent folder)"
POSIX path of selectedFolder
APPLESCRIPT
) || exit 0
    fi
fi

[[ -x "$nodtool" ]] || fail 'the packaged nodtool is missing or not executable'
[[ -x "$translator" ]] || fail 'the packaged Translator.Cli is missing or not executable'
[[ -x "$cmake_bin" && -x "$ninja_bin" ]] || fail 'the packaged CMake or Ninja tool is missing'
if ! /usr/bin/xcode-select -p >/dev/null 2>&1; then
    notice 'Xcode Command Line Tools are required once to compile WiiCompiled. Click OK, complete the Apple installer, then run WiiCompiled Setup again.'
    /usr/bin/xcode-select --install || true
    exit 1
fi
for tool in "$nodtool" "$translator" "$cmake_bin" "$ninja_bin"; do
    /usr/bin/lipo "$tool" -verify_arch "$host_arch" >/dev/null 2>&1 || \
        fail "the packaged $(basename "$tool") does not support $host_arch"
done

mkdir -p "$support_root" "$products"
source_bundle_version="$workspace_source/.bundle-version"
workspace_bundle_version="$workspace/.bundle-version"
needs_workspace_refresh=0
if [[ ! -f "$workspace/projects/mkwii/recomp.yml" ]]; then
    needs_workspace_refresh=1
elif [[ -f "$source_bundle_version" ]] && [[ ! -f "$workspace_bundle_version" || "$(<"$source_bundle_version")" != "$(<"$workspace_bundle_version")" ]]; then
    needs_workspace_refresh=1
fi

if (( needs_workspace_refresh )); then
    printf 'Preparing the local build workspace...\n'
    if [[ ! -d "$workspace" ]]; then
        /usr/bin/ditto "$workspace_source" "$workspace"
    else
        # Refresh only packaged source inputs. Assets and the staged Retro
        # Rewind package belong to the user and stay in place.
        for source in aurora-main projects runtime translator Launcher; do
            /usr/bin/ditto "$workspace_source/$source" "$workspace/$source"
        done
        /usr/bin/ditto "$source_bundle_version" "$workspace_bundle_version"
        # A dependency provider can be cached in this directory, so make the
        # refreshed sources configure from a clean native build tree.
        rm -rf "$workspace/native-build-macos-arm64" "$workspace/native-build-macos-x86_64"
    fi
fi

profile=base
build_args=(--workspace "$workspace" --game "$game" --nodtool "$nodtool" --output-dir "$products")
if [[ -n "$retro_dir" ]]; then
    profile=both
    # Online play needs the shared Retro-WFC payload. Keep it in the per-user
    # support directory rather than the packaged app or build workspace, then
    # verify its pinned signature before publishing it into the local cache.
    retro_wfc_dir="$support_root/RetroWfcPayload"
    retro_wfc_payload="$retro_wfc_dir/binary/payload.RMCPD00.bin"
    if [[ -f "$retro_wfc_payload" ]] && ! "$translator" validate-retro-wfc-payload --directory "$retro_wfc_dir"; then
        printf 'Discarding an invalid cached Retro-WFC payload...\n' >&2
        rm -f "$retro_wfc_payload"
    fi
    if [[ ! -f "$retro_wfc_payload" ]]; then
        printf 'Downloading the Retro-WFC payload needed for online play...\n'
        mkdir -p "$retro_wfc_dir"
        payload_stage=$(mktemp -d "$retro_wfc_dir/.payload-download.XXXXXX")
        temporary_payload="$payload_stage/binary/payload.RMCPD00.bin"
        mkdir -p "$(dirname "$temporary_payload")"
        trap 'rm -rf "$payload_stage"' EXIT
        /usr/bin/curl --fail --silent --show-error --connect-timeout 10 --max-time 30 \
            --retry 1 --output "$temporary_payload" \
            'http://nas.play.rwfc.net/payload?g=RMCPD00' || fail 'could not download the Retro-WFC payload needed for online play'
        "$translator" validate-retro-wfc-payload --directory "$payload_stage" || \
            fail 'downloaded Retro-WFC payload failed signature validation'
        mkdir -p "$retro_wfc_dir/binary"
        mv "$temporary_payload" "$retro_wfc_payload"
        rmdir "$payload_stage/binary" "$payload_stage"
        trap - EXIT
    fi
    build_args+=(--profile both --base-output-dir "$products" --retro-rewind-package-dir "$retro_dir" --retro-wfc-offline-dir "$retro_wfc_dir")
fi
"$workspace/Launcher/local-build-macos.command" "${build_args[@]}" --profile "$profile" --cmake "$cmake_bin" --ninja "$ninja_bin" --translator-bin "$translator"

config="$support_root/Config.toml"
toml_string() { printf '%s' "$1" | sed -e 's/\\\\/\\\\\\\\/g' -e 's/"/\\\\"/g'; }
set_path() {
    local key=$1 value=$2 encoded line temporary
    encoded=$(toml_string "$value"); line="$key = \"$encoded\""; temporary="$config.tmp"
    touch "$config"
    if grep -q '^[[:space:]]*\[paths\][[:space:]]*$' "$config"; then
        awk -v key="$key" -v line="$line" '
            /^[[:space:]]*\[paths\][[:space:]]*$/ { print; print line; inside = 1; next }
            inside && /^[[:space:]]*\[/ { inside = 0 }
            inside && $0 ~ "^[[:space:]]*" key "[[:space:]]*=" { next }
            { print }
        ' "$config" > "$temporary"
    else
        { cat "$config"; printf '\n[paths]\n%s\n' "$line"; } > "$temporary"
    fi
    mv "$temporary" "$config"
}
set_path dvd_root "$workspace/Assets/DATA"
[[ -n "$retro_dir" ]] && set_path retro_rewind_root "$retro_dir"

destination="$HOME/Applications"
if [[ "$install_location" == applications ]]; then destination=/Applications; fi
install_app() {
    local app=$1
    [[ -d "$products/$app" ]] || return 0
    if [[ "$destination" == /Applications ]]; then
        command="mkdir -p /Applications && rm -rf '/Applications/$app' && ditto '$products/$app' '/Applications/$app'"
        /usr/bin/osascript -e "do shell script \"$command\" with administrator privileges"
    else
        mkdir -p "$destination"; rm -rf "$destination/$app"; /usr/bin/ditto "$products/$app" "$destination/$app"
    fi
}
install_app WiiCompiled.app
[[ "$profile" == both ]] && install_app RetroRewind.app
notice "Installation complete. Your apps are in $destination."
