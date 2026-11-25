#!/bin/bash
# Header of WiiCompiled-Setup-macos.run, the self-extracting setup that frontends such as Wheel Wizard
# download like the Linux AppImage. build-setup-pkg.command fills in the version and appends the
# Setup.app Resources as a gzipped tar after the marker line. The bundle is unpacked once per version
# into Application Support, then its wiicompiled-setup entry point runs from there.
set -euo pipefail
version='@VERSION@'
if [[ $# -eq 1 && $1 == --version ]]; then printf '%s\n' "$version"; exit 0; fi

setup_root="$HOME/Library/Application Support/WiiCompiled/Setup"
bundle="$setup_root/$version"
if [[ ! -x "$bundle/wiicompiled-setup" ]]; then
    mkdir -p "$setup_root"
    staging=$(mktemp -d "$setup_root/.unpack.XXXXXX")
    trap 'rm -rf "$staging"' EXIT
    payload_line=$(awk '/^__WIICOMPILED_PAYLOAD__$/ { print NR + 1; exit }' "$0")
    tail -n +"$payload_line" "$0" | /usr/bin/tar -xzf - -C "$staging" \
        || { printf 'WiiCompiled setup: could not unpack %s; the download may be incomplete\n' "$0" >&2; exit 1; }
    # Not safe against two first runs racing; frontends already run setup operations one at a time.
    rm -rf "$bundle"
    mv "$staging" "$bundle"
    trap - EXIT
    # Older versions are never run again once this one is unpacked.
    find "$setup_root" -mindepth 1 -maxdepth 1 ! -name "$version" ! -name '.unpack.*' -exec rm -rf {} +
fi
exec "$bundle/wiicompiled-setup" "$@"
__WIICOMPILED_PAYLOAD__
