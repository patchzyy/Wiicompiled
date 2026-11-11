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
readonly DRIRC_SRC="@drirc@/etc/drirc"
readonly REPO_SRC="@repoSrc@"
readonly VULKAN_LIB="@vulkan-loader@/lib"

readonly WS_ROOT="${XDG_DATA_HOME:-$HOME/.local/share}/WiiCompiled"
readonly WORKSPACE="$WS_ROOT/workspace"
readonly PACK_DIR="$WORKSPACE/PulsarPacks/completed/RetroRewind/RetroRewind6"
readonly WFC_OFFLINE="$WORKSPACE/RetroWfc"
readonly INSTALL_DIR="$WS_ROOT/Install/RetroRewind"
readonly BUILD_LOCK_DIR="$WS_ROOT/.retro-rewind-build.lock"
readonly RR_VERSION_URL="https://update.rwfc.net/RetroRewind/RetroRewindVersion.txt"
readonly RR_INSTALL_URL_FILE="https://update.rwfc.net/RetroRewind/RetroRewindInstall.txt"
readonly WFC_PAYLOAD_URL="https://rwfc.net/api/wfc/payload?g=RMCPD00"
readonly WFC_PAYLOAD_MAX_BYTES=$((16 * 1024 * 1024))

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

with_workspace_lock() (
    local pid printed=0
    mkdir -p "$WS_ROOT"
    while ! mkdir "$BUILD_LOCK_DIR" 2>/dev/null; do
        if [ -r "$BUILD_LOCK_DIR/pid" ]; then
            pid=$(cat "$BUILD_LOCK_DIR/pid" 2>/dev/null || true)
            if [ -n "$pid" ] && ! kill -0 "$pid" 2>/dev/null; then
                rm -rf "$BUILD_LOCK_DIR"
                continue
            fi
        fi
        if [ "$printed" -eq 0 ]; then
            log "Waiting for another Retro Rewind build in this workspace"
            printed=1
        fi
        sleep 2
    done
    printf '%s\n' "$$" > "$BUILD_LOCK_DIR/pid"
    trap 'rm -rf "$BUILD_LOCK_DIR"' EXIT
    "$@"
)

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

install_rr() (
    local version url tmp exdir rrdir xml rizip
    version=$(latest_version) || die "cannot reach the Retro Rewind CDN"
    [ -n "$version" ] || die "CDN returned no version"
    url=$(latest_install_url) || die "cannot resolve the full-pack URL"
    [ -n "$url" ] || die "empty full-pack URL"

    log "Downloading Retro Rewind $version"
    tmp=$(mktemp -d)
    trap 'rm -rf "$tmp" "${stage:-}" "${backup_pack:-}" "${backup_riivolution:-}"' EXIT
    curl -fL --progress-bar --connect-timeout 20 --speed-limit 1024 --speed-time 60 \
        "$url" -o "$tmp/rr.zip"

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
    [ -f "$rrdir/Binaries/Code.pul" ] || die "RetroRewind6/Binaries/Code.pul is missing from the pack zip"
    xml=$(find "$exdir" -iname 'RetroRewind6.xml' -type f | head -n 1)
    [ -n "$xml" ] || die "no RetroRewind6.xml in the pack zip"
    rizip=$(find "$exdir" -type d -iname 'riivolution' | head -n 1)

    pack_parent="$WORKSPACE/PulsarPacks/completed/RetroRewind"
    mkdir -p "$pack_parent"
    stage=$(mktemp -d "$pack_parent/.install.XXXXXX")
    backup_pack=$(mktemp -d "$pack_parent/.RetroRewind6.backup.XXXXXX")
    rmdir "$backup_pack"
    backup_riivolution=$(mktemp -d "$pack_parent/.riivolution.backup.XXXXXX")
    rmdir "$backup_riivolution"

    cp -r "$rrdir" "$stage/RetroRewind6"
    mkdir -p "$stage/RetroRewind6/xml"
    cp "$xml" "$stage/RetroRewind6/xml/RetroRewind6.xml"
    if [ -n "$rizip" ]; then
        cp -r "$rizip" "$stage/riivolution"
    fi
    printf '%s' "$version" > "$stage/RetroRewind6/version.txt"

    [ -f "$stage/RetroRewind6/Binaries/Code.pul" ] || die "staged Retro Rewind Code.pul is missing"
    [ -f "$stage/RetroRewind6/xml/RetroRewind6.xml" ] || die "staged Retro Rewind XML is missing"

    if [ -e "$PACK_DIR" ]; then
        mv "$PACK_DIR" "$backup_pack"
    fi
    if [ -e "$pack_parent/riivolution" ]; then
        mv "$pack_parent/riivolution" "$backup_riivolution"
    fi
    if ! mv "$stage/RetroRewind6" "$PACK_DIR"; then
        [ ! -e "$backup_pack" ] || mv "$backup_pack" "$PACK_DIR"
        [ ! -e "$backup_riivolution" ] || mv "$backup_riivolution" "$pack_parent/riivolution"
        die "failed to install staged Retro Rewind pack"
    fi
    if [ -e "$stage/riivolution" ] && ! mv "$stage/riivolution" "$pack_parent/riivolution"; then
        rm -rf "$PACK_DIR"
        [ ! -e "$backup_pack" ] || mv "$backup_pack" "$PACK_DIR"
        [ ! -e "$backup_riivolution" ] || mv "$backup_riivolution" "$pack_parent/riivolution"
        die "failed to install staged Retro Rewind riivolution data"
    fi
    rm -rf "$backup_pack" "$backup_riivolution" "$stage"
    chmod -R u+w "$WORKSPACE/PulsarPacks"
)

validate_retro_wfc_payload() (
    local payload=$1 size declared_size tmp pubkey sig signed
    tmp=$(mktemp -d)
    trap 'rm -rf "$tmp"' EXIT
    pubkey="$tmp/retro-wfc-payload.pem"
    sig="$tmp/payload.sig"
    signed="$tmp/payload.signed"

    size=$(wc -c < "$payload" | tr -d '[:space:]')
    [ "$size" -le "$WFC_PAYLOAD_MAX_BYTES" ] || die "Retro-WFC payload is unexpectedly large"
    [ "$size" -ge 304 ] || die "Retro-WFC payload has an invalid header"
    head -c 12 "$payload" | cmp -s - <(printf 'WWFC/Payload') ||
        die "Retro-WFC payload has an invalid header"
    declared_size=$(od -An -N4 -j12 -tu4 --endian=big "$payload" | tr -d '[:space:]')
    [ "$declared_size" = "$size" ] ||
        die "Retro-WFC payload declares $declared_size bytes but contains $size"

    cat > "$pubkey" <<'EOF'
-----BEGIN PUBLIC KEY-----
MIIBIjANBgkqhkiG9w0BAQEFAAOCAQ8AMIIBCgKCAQEA5ubOQW81BCLL4mw2pn66
YT3dzSfXmv0HfcxZPlMZ6qYIApNAADOHbT29/aEsFfRqyOT1tAxW57X2fpFkfWGM
uZnAQVgbhtEDvXcj/OrAOtOtUTS/YRzUfcUnACWWgh6UHJRwk4/qByOKhHZzI+Sm
EL2ZZGXlnQTa5P69kVyW/Ac55OgYMAgp148/InXh8/vSUH8b3nTySlKF5hAHuVml
g7SCDXXsp2aAhm7+XXlZC4LDV3t5YVWJlTDjBblLTO70QoZEtxnfPYVAyViPW7At
g9OTglXRoeBz00CBY/+TphWiEGoDkjo5eq1qKeu0MDHtBt4VdcjuK1Rnj6BZ4CX0
VQIDAQAB
-----END PUBLIC KEY-----
EOF
    dd if="$payload" of="$sig" bs=1 skip=16 count=256 status=none
    dd if="$payload" of="$signed" bs=1 skip=272 status=none
    openssl dgst -sha256 -verify "$pubkey" -signature "$sig" "$signed" >/dev/null ||
        die "Retro-WFC payload is not signed by the pinned Retro-WFC signing key"
)

install_retro_wfc_payload() (
    local pack_parent stage backup_payload payload http_code
    pack_parent="$WORKSPACE"
    mkdir -p "$pack_parent"
    stage=$(mktemp -d "$pack_parent/.retro-wfc.XXXXXX")
    backup_payload=$(mktemp -d "$pack_parent/.RetroWfc.backup.XXXXXX")
    rmdir "$backup_payload"
    trap 'rm -rf "$stage" "${backup_payload:-}"' EXIT

    log "Downloading Retro-WFC payload"
    mkdir -p "$stage/binary"
    payload="$stage/binary/payload.RMCPD00.bin"
    if ! http_code=$(curl -fS --progress-bar --proto '=https' --tlsv1.2 \
        --connect-timeout 20 --speed-limit 1024 --speed-time 60 \
        --max-filesize "$WFC_PAYLOAD_MAX_BYTES" --write-out '%{http_code}' \
        "$WFC_PAYLOAD_URL" -o "$payload"); then
        die "failed to download Retro-WFC payload"
    fi
    [ "$http_code" = 200 ] || die "Retro-WFC payload endpoint returned HTTP $http_code"
    [ -s "$payload" ] || die "Retro-WFC payload download was empty"
    validate_retro_wfc_payload "$payload"

    if [ -e "$WFC_OFFLINE" ]; then
        mv "$WFC_OFFLINE" "$backup_payload"
    fi
    if ! mv "$stage" "$WFC_OFFLINE"; then
        [ ! -e "$backup_payload" ] || mv "$backup_payload" "$WFC_OFFLINE"
        die "failed to install staged Retro-WFC payload"
    fi
    rm -rf "$backup_payload"
)

build_rr() {
    log "Translating and compiling (this can take a while; upstream caching applies)"
    install_retro_wfc_payload
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
    local key=$1 value=$2 file="$WS_ROOT/Config.toml" tmp
    mkdir -p "$WS_ROOT"
    if [ -f "$file" ]; then
        tmp=$(mktemp "$file.XXXXXX")
        awk -v key="$key" -v value="$value" '
            BEGIN {
                in_paths = 0
                found_paths = 0
                found_key = 0
                inserted = 0
                entry = key " = \"" value "\""
                key_re = "^[[:space:]]*" key "[[:space:]]*="
            }
            /^\[[^]]+\][[:space:]]*$/ {
                if (in_paths && !found_key && !inserted) {
                    print entry
                    inserted = 1
                }
                in_paths = ($0 ~ /^\[paths\][[:space:]]*$/)
                found_paths = found_paths || in_paths
                print
                next
            }
            in_paths && $0 ~ key_re {
                if (!found_key) {
                    print entry
                    found_key = 1
                }
                next
            }
            { print }
            END {
                if (!found_paths) {
                    print ""
                    print "[paths]"
                    print entry
                } else if (in_paths && !found_key && !inserted) {
                    print entry
                }
            }
        ' "$file" > "$tmp"
        mv "$tmp" "$file"
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

update_rr() {
    local latest current
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
}

reinstall_rr() {
    ensure_workspace
    install_rr
    build_rr
    log "reinstalled Retro Rewind $(installed_version)"
}

install_initial_rr() {
    log "no Retro Rewind install found; installing"
    ensure_workspace
    install_rr
    build_rr
    log "installed Retro Rewind $(installed_version)"
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
        with_workspace_lock update_rr
        launch_rr
        ;;
    reinstall)
        with_workspace_lock reinstall_rr
        launch_rr
        ;;
    launch)
        if [ -z "$(installed_version)" ] || [ ! -f "$INSTALL_DIR/RetroRewind" ]; then
            with_workspace_lock install_initial_rr
        fi
        maybe_hint_update
        launch_rr
        ;;
    *)
        die "usage: retro-rewind [launch|update|reinstall|check]"
        ;;
esac
