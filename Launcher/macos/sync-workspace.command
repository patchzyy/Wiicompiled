#!/usr/bin/env bash
# Copies the packaged build workspace into a writable per-user location, or refreshes its source
# inputs when the package version changed. Shared by setup.command (Setup.pkg) and the
# wiicompiled-setup entry point (WiiCompiled-Setup-macos.run).
set -euo pipefail

[[ $# -eq 2 ]] || { printf 'Usage: sync-workspace.command PACKAGED-WORKSPACE USER-WORKSPACE\n' >&2; exit 1; }
workspace_source=$1; workspace=$2

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
            rm -rf "$workspace/$source"
            /usr/bin/ditto "$workspace_source/$source" "$workspace/$source"
        done
        /usr/bin/ditto "$source_bundle_version" "$workspace_bundle_version"
        # A dependency provider can be cached in this directory, so make the
        # refreshed sources configure from a clean native build tree.
        rm -rf "$workspace/native-build-macos-arm64" "$workspace/native-build-macos-x86_64"
    fi
fi
