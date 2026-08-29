#!@bash@/bin/bash
# Impure Retro Rewind lifecycle for the WiiCompiled flake: install, update,
# build and launch in the user's workspace, mirroring the upstream AppImage
# flow. The pack updates too often to pin in Nix, so everything after the
# base-game build happens here with store-provided tools.
#
#   retro-rewind          ensure installed (auto-install on first run), launch
#   retro-rewind update   check the CDN, apply the newest pack, rebuild, launch
#   retro-rewind check    report only; never builds or downloads
set -euo pipefail

readonly DATA_TREE="@datatree@"
readonly TRANSLATOR="@translator@/bin/Translator.Cli"
readonly WFC_OFFLINE="@wfcOffline@"
readonly DRIRC_SRC="@launcher@/etc/drirc"
readonly REPO_SRC="@repoSrc@"
readonly VULKAN_LIB="@vulkan-loader@/lib"

readonly WS_ROOT="${XDG_DATA_HOME:-$HOME/.local/share}/WiiCompiled"
readonly WORKSPACE="$WS_ROOT/workspace"
readonly PACK_DIR="$WORKSPACE/PulsarPacks/completed/RetroRewind/RetroRewind6"
readonly INSTALL_DIR="$WS_ROOT/Install/RetroRewind"
readonly RR_VERSION_URL="https://update.rwfc.net/RetroRewind/RetroRewindVersion.txt"
readonly RR_INSTALL_URL_FILE="https://update.rwfc.net/RetroRewind/RetroRewindInstall.txt"

log() { printf 'retro-rewind: %s\n' "$*"; }
die() { printf 'retro-rewind: error: %s\n' "$*" >&2; exit 1; }

readonly FLAKE_DIR="$PWD"
[ -f "$FLAKE_DIR/flake.nix" ] || die "run from the WiiCompiled flake checkout (./flake.nix is needed for the build shell)"

installed_version() {
    if [ -f "$PACK_DIR/version.txt" ]; then
        cat "$PACK_DIR/version.txt"
    else
        echo ""
    fi
}

latest_version() {
    curl -fsSL --max-time 5 "$RR_VERSION_URL" | awk 'NF {v=$1} END {print v}'
}

latest_install_url() {
    curl -fsSL --max-time 5 "$RR_INSTALL_URL_FILE" | tr -d '[:space:]'
}

ensure_workspace() {
    mkdir -p "$WS_ROOT"
    stamp="$WORKSPACE/.bundle-version"
    if [ ! -f "$stamp" ] || [ "$(cat "$stamp")" != "$REPO_SRC" ]; then
        log "Syncing workspace sources from the flake snapshot"
        mkdir -p "$WORKSPACE/Launcher"
        rm -rf "$WORKSPACE/runtime" "$WORKSPACE/aurora-main" "$WORKSPACE/projects"
        cp -r "$REPO_SRC/runtime" "$REPO_SRC/aurora-main" "$REPO_SRC/projects" "$WORKSPACE/"
        cp "$REPO_SRC/Launcher/local-build.sh" "$WORKSPACE/Launcher/"
        chmod -R u+w "$WORKSPACE"
        printf '%s' "$REPO_SRC" > "$stamp"
    fi

    # The workspace translation needs the user-owned disc data; copy it out of
    # the store tree so local-build.sh's fingerprint checks see real files.
    mkdir -p "$WORKSPACE/Assets"
    cp -f "$DATA_TREE/sys/main.dol" "$WORKSPACE/Assets/main.dol"
    cp -f "$DATA_TREE/files/rel/StaticR.rel" "$WORKSPACE/Assets/StaticR.rel"
}

install_rr() {
    local version url tmp exdir rrdir xml rizip
    version=$(latest_version) || die "cannot reach the Retro Rewind CDN"
    [ -n "$version" ] || die "CDN returned no version"
    url=$(latest_install_url) || die "cannot resolve the full-pack URL"
    [ -n "$url" ] || die "empty full-pack URL"

    log "Downloading Retro Rewind $version"
    tmp=$(mktemp -d)
    trap 'rm -rf "$tmp"' EXIT
    curl -fL --progress-bar "$url" -o "$tmp/rr.zip"

    log "Extracting"
    exdir="$tmp/extracted"
    mkdir -p "$exdir"
    unzip -q "$tmp/rr.zip" -d "$exdir"

    # Normalize into the virtual-SD layout the pack's Riivolution xml and
    # projects/mkwii/recomp.yml both assume:
    #
    #   completed/RetroRewind/          <- virtual SD root
    #   ├── RetroRewind6/               <- pack folder (overlayRoot)
    #   │   ├── Binaries/Code.pul       (translator: profile code_pul)
    #   │   ├── xml/RetroRewind6.xml    (profile riivolution pin)
    #   │   └── Tracks/ Assets/ ...     (content the xml externals reference
    #   │                                 as /RetroRewind6/...)
    #   └── riivolution/                (xml + save data, referenced as
    #                                     /riivolution/...)
    rrdir=$(find "$exdir" -type d -name RetroRewind6 | head -n 1)
    [ -n "$rrdir" ] || die "no RetroRewind6 directory in the pack zip"
    xml=$(find "$exdir" -iname 'RetroRewind6.xml' -type f | head -n 1)
    [ -n "$xml" ] || die "no RetroRewind6.xml in the pack zip"
    rizip=$(find "$exdir" -type d -iname 'riivolution' | head -n 1)

    pack_parent="$WORKSPACE/PulsarPacks/completed/RetroRewind"
    mkdir -p "$pack_parent"
    rm -rf "$PACK_DIR" "$pack_parent/riivolution"
    cp -r "$rrdir" "$PACK_DIR"
    mkdir -p "$PACK_DIR/xml"
    cp "$xml" "$PACK_DIR/xml/RetroRewind6.xml"
    if [ -n "$rizip" ]; then
        cp -r "$rizip" "$pack_parent/riivolution"
    fi
    printf '%s' "$version" > "$PACK_DIR/version.txt"
    chmod -R u+w "$WORKSPACE/PulsarPacks"
}

build_rr() {
    log "Translating and compiling (this can take a while; upstream caching applies)"
    (cd "$FLAKE_DIR" && nix develop .#retro-rewind-build -c bash "$WORKSPACE/Launcher/local-build.sh" \
        --workspace "$WORKSPACE" \
        --profile retro-rewind \
        --output-dir "$INSTALL_DIR" \
        --retro-rewind-package-dir "$PACK_DIR" \
        --retro-wfc-offline-dir "$WFC_OFFLINE" \
        --translator-bin "$TRANSLATOR")
}

# $1 = key, $2 = value. Heals an uncommented key under [paths] without
# clobbering any other user settings, mirroring the pure launcher.
ensure_path_key() {
    local key=$1 value=$2 file="$WS_ROOT/Config.toml"
    if [ -f "$file" ] && grep -q "^[[:space:]]*$key" "$file"; then
        sed -i "s|^[[:space:]]*$key = .*|$key = \"$value\"|" "$file"
    elif [ -f "$file" ] && grep -q '^\[paths\]' "$file"; then
        sed -i "/^\[paths\]/a $key = \"$value\"" "$file"
    elif [ -f "$file" ]; then
        printf '\n[paths]\n%s = "%s"\n' "$key" "$value" >> "$file"
    else
        printf '[paths]\n%s = "%s"\n' "$key" "$value" > "$file"
    fi
}

launch_rr() {
    local exe="$INSTALL_DIR/RetroRewind"
    [ -f "$exe" ] || die "Retro Rewind is not installed; run 'retro-rewind update'"

    ensure_path_key dvd_root "$DATA_TREE"
    ensure_path_key retro_rewind_root "$PACK_DIR"

    export LD_LIBRARY_PATH="$VULKAN_LIB${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
    export XDG_CONFIG_HOME="$WS_ROOT/xdg-config"
    mkdir -p "$XDG_CONFIG_HOME"
    if [ ! -f "$XDG_CONFIG_HOME/drirc" ]; then
        cp "$DRIRC_SRC" "$XDG_CONFIG_HOME/drirc"
    fi
    exec "$exe"
}

maybe_hint_update() {
    local latest current
    latest=$(latest_version 2>/dev/null) || return 0
    current=$(installed_version)
    [ -n "$current" ] || return 0
    if [ "$latest" != "$current" ]; then
        log "Retro Rewind $latest is available ($current installed); run 'retro-rewind update'"
    fi
}

case "${1:-launch}" in
    check)
        latest=$(latest_version) || die "cannot reach the Retro Rewind CDN"
        current=$(installed_version)
        if [ -z "$current" ]; then
            log "not installed; latest on the CDN is $latest (run 'retro-rewind update' to install)"
        elif [ "$latest" = "$current" ]; then
            log "up to date ($current)"
        else
            log "update available: $current -> $latest (run 'retro-rewind update')"
        fi
        ;;
    update)
        ensure_workspace
        latest=$(latest_version) || die "cannot reach the Retro Rewind CDN"
        current=$(installed_version)
        if [ "$latest" != "$current" ]; then
            install_rr
        else
            log "pack already latest ($current)"
        fi
        # Always rebuild: incremental (cmake config + no-op ninja when
        # nothing changed), and it picks up toolchain/flake changes that a
        # version match alone would hide.
        build_rr
        log "Retro Rewind $(installed_version) ready"
        launch_rr
        ;;
    reinstall)
        ensure_workspace
        install_rr
        build_rr
        log "reinstalled Retro Rewind $(installed_version)"
        launch_rr
        ;;
    launch)
        if [ -z "$(installed_version)" ] || [ ! -f "$INSTALL_DIR/RetroRewind" ]; then
            log "no Retro Rewind install found; installing"
            ensure_workspace
            install_rr
            build_rr
            log "installed Retro Rewind $(installed_version)"
        fi
        maybe_hint_update
        launch_rr
        ;;
    *)
        die "usage: retro-rewind [launch|update|reinstall|check]"
        ;;
esac
