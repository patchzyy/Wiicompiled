#!/usr/bin/env bash
# Packages Launcher/WiiCompiled.Setup.Linux as a self-contained AppImage: a single file Wheel
# Wizard (or anyone else) can fetch and execute with no git clone, no `dotnet` install, and no
# `dolphin-tool` package required at all. The installer and translator are published as
# self-contained binaries, and `nodtool` (a prebuilt MIT/Apache-2.0 CLI from encounter/nod, see
# NodToolProvider.cs) is downloaded and bundled too - AppRun passes --translator-bin and
# --disc-tool-bin so local-build.sh/DiscTool.cs skip their from-source/download fallbacks entirely.
# A pruned native clang/lld/cmake/ninja toolchain (see prepare-portable-tools.sh) is bundled the
# same way - AppRun passes --cc/--cxx/--fuse-ld/--cmake/--ninja so local-build.sh never has to find
# a system compiler, CMake, or Ninja. It still shells out to system pkg-config and Vulkan headers,
# matching Launcher/local-build.sh's own remaining prerequisites. A precompiled aurora +
# third-party package (see Prepare-NativePrebuilt.sh) is bundled the same way too - AppRun passes
# --native-prebuilt-dir so local-build.sh never compiles aurora-main from source at all.
#
# An AppImage mounts read-only, but local-build.sh writes generated/, native-build/, Assets/, etc.
# into the workspace it's given. So AppRun (written below) copies the bundled workspace snapshot
# out to a writable cache directory on first run, and only ever re-syncs the bundled directories
# (runtime/, aurora-main/, projects/, local-build.sh) on a later run whose bundled version changed
# - generated/native-build/Assets/PulsarPacks live only in that writable cache and are never
# touched by the sync, so local-build.sh's own incremental caching survives across runs and across
# AppImage updates. translator/ isn't part of this snapshot at all: it's published as its own
# self-contained binary (usr/bin/translator-cli) below and never needs a writable copy. Neither
# native-prebuilt/ nor the toolchain are copied into the cache either - both are large
# (~90 MiB / ~500 MiB) and local-build.sh only ever reads from them - but AppRun does point
# $CACHE/toolchain and $CACHE/native-prebuilt symlinks at the current mount on every single launch
# (see AppRun's own comment): an AppImage's FUSE mount is at a fresh random /tmp/.mount_XXXXXX
# every run, and CMake bakes whatever compiler/tool path it's given directly into each
# build.ninja rule's command line, so referencing $HERE straight would change that command line -
# and Ninja reruns any rule whose command line changed - forcing a full rebuild on every single
# launch even though the compiler itself never actually changed. The symlink keeps the path
# string CMake/Ninja see identical across runs while what it resolves to tracks the current mount
# underneath.
set -euo pipefail

script_dir=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
workspace=$(cd "$script_dir/.." && pwd)

# `uname -m` reports the *kernel's* architecture, which can differ from userspace - an aarch64
# kernel can run a 32-bit armhf userland (as shipped by 32-bit Raspberry Pi OS), same as an x86_64
# kernel can run an i686 one. What matters here is which userspace binaries (dotnet, appimagetool)
# will actually run, so this reads the ELF header of this script's own running bash interpreter -
# real userspace - rather than trusting the kernel's self-report. /proc/$$/exe (not /proc/self/exe:
# that would resolve inside the readlink subprocess below, to readlink itself, not to bash) is this
# shell's own PID. EI_CLASS (byte 4: 1=32-bit, 2=64-bit) and e_machine (bytes 18-19: 3=EM_386,
# 40=EM_ARM, 62=EM_X86_64, 183=EM_AARCH64) are read as plain little-endian bytes, which every
# real-world x86/ARM Linux userland uses; ELF's big-endian encoding is a non-issue here since no
# Linux distro ships a big-endian x86 or ARM userland.
elf_exe=$(readlink -f "/proc/$$/exe")
elf_class=$(od -An -t u1 -j 4 -N 1 "$elf_exe" | tr -d ' ')
elf_machine_lo=$(od -An -t u1 -j 18 -N 1 "$elf_exe" | tr -d ' ')
elf_machine_hi=$(od -An -t u1 -j 19 -N 1 "$elf_exe" | tr -d ' ')
elf_machine=$(( elf_machine_hi * 256 + elf_machine_lo ))

# Mirrors the host-architecture detection NodToolProvider.cs already does (RuntimeInformation.
# OSArchitecture) so this script's own dotnet RID and appimagetool selection agree with the
# nodtool binary that same code path resolves below. local-build.sh needs no such mapping itself:
# it just drives the native CMake configure, which already accepts x86_64 or aarch64 natively
# (see runtime/CMakeLists.txt's CMAKE_SYSTEM_PROCESSOR check).
case "$elf_class:$elf_machine" in
    2:62)
        dotnet_rid=linux-x64
        appimagetool_arch=x86_64
        ;;
    2:183)
        dotnet_rid=linux-arm64
        appimagetool_arch=aarch64
        ;;
    *)
        echo "build-appimage.sh: unsupported userspace architecture (ELF class $elf_class, machine $elf_machine) - WiiCompiled requires a 64-bit x86_64 or aarch64 userland" >&2
        exit 1
        ;;
esac

output_dir="$workspace/Launcher/dist"
appimagetool_override=""

while [[ $# -gt 0 ]]; do
    case "$1" in
        --output-dir) output_dir=$2; shift 2 ;;
        --appimagetool) appimagetool_override=$2; shift 2 ;;
        -h|--help)
            echo "Usage: build-appimage.sh [--output-dir DIR] [--appimagetool PATH]"
            exit 0
            ;;
        *) echo "build-appimage.sh: unknown argument: $1" >&2; exit 1 ;;
    esac
done

appdir="$workspace/Launcher/artifacts/appimage-build/AppDir"
rm -rf "$appdir"
mkdir -p "$appdir/usr/bin" "$appdir/workspace/Launcher"

echo "Publishing the installer (self-contained $dotnet_rid)..."
publish_tmp="$workspace/Launcher/artifacts/appimage-build/publish"
rm -rf "$publish_tmp"
dotnet publish "$workspace/Launcher/WiiCompiled.Setup.Linux" -c Release -r "$dotnet_rid" \
    --self-contained -p:PublishSingleFile=true -p:EnableCompressionInSingleFile=true \
    -o "$publish_tmp"
cp "$publish_tmp/WiiCompiled.Setup.Linux" "$appdir/usr/bin/wiicompiled-setup"
chmod +x "$appdir/usr/bin/wiicompiled-setup"

# Published as a self-contained binary too, so an AppImage user never needs a `dotnet` SDK on
# PATH at all - local-build.sh is told about it via --translator-bin and skips its own
# dotnet-build-from-source step entirely (see local-build.sh's translator resolution branch).
echo "Publishing the translator (self-contained $dotnet_rid)..."
translator_publish_tmp="$workspace/Launcher/artifacts/appimage-build/publish-translator"
rm -rf "$translator_publish_tmp"
dotnet publish "$workspace/translator/src/Translator.Cli" -c Release -r "$dotnet_rid" \
    --self-contained -p:PublishSingleFile=true -p:EnableCompressionInSingleFile=true \
    -o "$translator_publish_tmp"
cp "$translator_publish_tmp/Translator.Cli" "$appdir/usr/bin/translator-cli"
chmod +x "$appdir/usr/bin/translator-cli"

# Resolved via the shared WiiCompiled.Setup.Common.Cli helper (also used by Build-Installer.ps1 on
# Windows) rather than a second curl/version-pin copy here: it downloads and caches the same way
# NodToolProvider.cs always does (Launcher/artifacts/nodtool), so there is exactly one place that
# knows the nodtool version/URL/platform-asset mapping.
echo "Resolving nodtool..."
nodtool_path=$(dotnet run --project "$workspace/Launcher/WiiCompiled.Setup.Common.Cli" -c Release -- \
    --workspace "$workspace" | tail -n1)
cp "$nodtool_path" "$appdir/usr/bin/nodtool"
chmod +x "$appdir/usr/bin/nodtool"

echo "Preparing the portable clang/lld/cmake/ninja toolchain ($appimagetool_arch)..."
bash "$script_dir/prepare-portable-tools.sh" --arch "$appimagetool_arch"
mkdir -p "$appdir/usr/toolchain"
cp -a "$workspace/Launcher/artifacts/portable-tools/toolchain-$appimagetool_arch"/. "$appdir/usr/toolchain/"

# Precompiled aurora + third-party package (see Prepare-NativePrebuilt.sh) so a user's own
# local-build.sh never has to compile aurora itself (~43% of local build CPU time). Re-harvesting
# recompiles the whole aurora/Crypto++ closure with the toolchain above, so this is skipped unless
# --print-fingerprint-only (a fast, build-free check) says the existing package no longer matches
# the current compiler/flags/aurora/third_party sources.
native_prebuilt_dir="$workspace/Launcher/artifacts/native-prebuilt-$appimagetool_arch"
echo "Checking whether the precompiled aurora + third-party package ($appimagetool_arch) is current..."
current_fingerprint=$(bash "$script_dir/Prepare-NativePrebuilt.sh" --arch "$appimagetool_arch" --print-fingerprint-only)
package_current=0
if [[ -f "$native_prebuilt_dir/provenance.json" ]]; then
    package_current=$(CURRENT_FINGERPRINT="$current_fingerprint" python3 - "$native_prebuilt_dir/provenance.json" <<'PY'
import json
import os
import sys

provenance = json.load(open(sys.argv[1], encoding="utf-8"))
current = dict(line.split("=", 1) for line in os.environ["CURRENT_FINGERPRINT"].splitlines() if line)
fields = {
    "compiler_sha256": "CompilerSha256",
    "flag_fingerprint": "FlagFingerprint",
    "aurora_fingerprint": "AuroraSourceFingerprint",
    "third_party_fingerprint": "ThirdPartySourceFingerprint",
}
print(1 if all(provenance.get(v) == current.get(k) for k, v in fields.items()) else 0)
PY
    )
fi
if [[ "$package_current" == "1" ]]; then
    echo "Native prebuilt package is current; reusing $native_prebuilt_dir"
else
    echo "Native prebuilt package is missing or stale; harvesting a fresh one (compiles aurora once, can take a while)..."
    bash "$script_dir/Prepare-NativePrebuilt.sh" --arch "$appimagetool_arch"
fi
mkdir -p "$appdir/native-prebuilt"
cp -a "$native_prebuilt_dir/." "$appdir/native-prebuilt/"

echo "Staging the bundled workspace snapshot..."
for dir in runtime aurora-main projects; do
    cp -r "$workspace/$dir" "$appdir/workspace/$dir"
done
# Mirrors Build-Installer.ps1's own staging exclusions exactly: aurora-main/extern/CMakeLists.txt
# is the real FetchContent driver and must ship, but any already-fetched dependency *subdirectory*
# a developer's local checkout accumulated under extern/ is stale/large build output, not a
# release input - only directories inside extern/ are stripped, never the file itself. runtime/build
# is a plain developer build directory.
find "$appdir/workspace/aurora-main/extern" -mindepth 1 -maxdepth 1 -type d -exec rm -rf {} +
rm -rf "$appdir/workspace/runtime/build"
cp "$workspace/Launcher/local-build.sh" "$appdir/workspace/Launcher/local-build.sh"

# AppRun re-syncs runtime/aurora-main/projects/local-build.sh into the writable cache only when
# this changes, so it must change whenever any of those bundled paths actually did - a bare commit
# hash gets this wrong for an uncommitted change (verified directly: rebuilding after editing
# local-build.sh with no commit produced the same hash as the stale cache, so AppRun kept serving
# the old script and failed on a flag that didn't exist yet). `git status --porcelain` catches both
# modified tracked files and new untracked ones; appending a fresh timestamp when it's non-empty
# guarantees this never matches a previous build's stamp, forcing a resync every time the tree is
# dirty. A clean tree (a real tagged release) keeps the stable commit-hash behavior, so identical
# reruns of the same release AppImage don't resync needlessly.
if git -C "$workspace" rev-parse HEAD >/dev/null 2>&1; then
    version=$(git -C "$workspace" rev-parse HEAD)
    if [[ -n "$(git -C "$workspace" status --porcelain 2>/dev/null)" ]]; then
        version="$version-dirty-$(date -u +%s)"
    fi
    echo "$version" > "$appdir/workspace/.bundle-version"
else
    date -u +%s > "$appdir/workspace/.bundle-version"
fi

echo "Writing AppRun..."
cat > "$appdir/AppRun" <<'APPRUN'
#!/bin/bash
set -euo pipefail
HERE="$(dirname "$(readlink -f "$0")")"
CACHE="${XDG_DATA_HOME:-$HOME/.local/share}/WiiCompiled/workspace"
mkdir -p "$CACHE"
if [ ! -f "$CACHE/.bundle-version" ] || \
   [ "$(cat "$HERE/workspace/.bundle-version")" != "$(cat "$CACHE/.bundle-version")" ]; then
    mkdir -p "$CACHE/Launcher"
    for dir in runtime aurora-main projects; do
        rm -rf "$CACHE/$dir"
        cp -r "$HERE/workspace/$dir" "$CACHE/$dir"
    done
    cp "$HERE/workspace/Launcher/local-build.sh" "$CACHE/Launcher/local-build.sh"
    cp "$HERE/workspace/.bundle-version" "$CACHE/.bundle-version"
fi
# toolchain/ and native-prebuilt/ are NOT copied into the cache (they're large - ~500 MiB /
# ~90 MiB - and local-build.sh only ever reads from them): $CACHE/toolchain and
# $CACHE/native-prebuilt are symlinks re-pointed at the current mount on every single launch
# (unconditionally, not gated on .bundle-version above, since the mount path itself - unlike the
# bundled content - changes every run regardless). CMake bakes a compiler/tool path directly into
# each build.ninja rule's command line and Ninja reruns any rule whose command line changed since
# the last build (verified directly) - an AppImage's FUSE mount is at a fresh random
# /tmp/.mount_XXXXXX every launch, so referencing $HERE straight would change that command line,
# and therefore force a full rebuild, on every single run even though the compiler itself never
# actually changed. A symlink keeps the *path string* CMake/Ninja see identical across runs while
# what it resolves to tracks the current mount underneath (verified directly: CMake records
# whatever path it's given as-is - including a symlink - without resolving it first).
[ -L "$CACHE/toolchain" ] || rm -rf "$CACHE/toolchain"
[ -L "$CACHE/native-prebuilt" ] || rm -rf "$CACHE/native-prebuilt"
ln -sfn "$HERE/usr/toolchain" "$CACHE/toolchain"
ln -sfn "$HERE/native-prebuilt" "$CACHE/native-prebuilt"
exec "$HERE/usr/bin/wiicompiled-setup" --workspace "$CACHE" \
    --translator-bin "$HERE/usr/bin/translator-cli" \
    --disc-tool-bin "$HERE/usr/bin/nodtool" \
    --cc "$CACHE/toolchain/bin/clang" \
    --cxx "$CACHE/toolchain/bin/clang++" \
    --fuse-ld lld \
    --cmake "$CACHE/toolchain/bin/cmake" \
    --ninja "$CACHE/toolchain/bin/ninja" \
    --native-prebuilt-dir "$CACHE/native-prebuilt" "$@"
APPRUN
chmod +x "$appdir/AppRun"

echo "Writing desktop entry and icon..."
cat > "$appdir/wiicompiled-setup.desktop" <<'DESKTOP'
[Desktop Entry]
Type=Application
Name=WiiCompiled Setup
Comment=Translate, compile, and launch Mario Kart Wii natively on Linux
Exec=AppRun
Icon=wiicompiled-setup
Categories=Game;
Terminal=true
DESKTOP

# No WiiCompiled logo/icon asset exists anywhere in this repo yet. appimagetool refuses to package
# without one, so this is a minimal solid-color placeholder - a one-line swap for real branding
# later (just replace this generated file with a real wiicompiled-setup.png before packaging).
python3 - "$appdir/wiicompiled-setup.png" <<'PY'
import struct
import sys
import zlib

path = sys.argv[1]


def chunk(tag: bytes, data: bytes) -> bytes:
    return struct.pack(">I", len(data)) + tag + data + struct.pack(">I", zlib.crc32(tag + data))


width = height = 256
row = b"\x00" + bytes([0x3A, 0x5F, 0x8F, 0xFF]) * width  # filter byte + opaque blue-grey pixels
raw = row * height
ihdr = struct.pack(">IIBBBBB", width, height, 8, 6, 0, 0, 0)
idat = zlib.compress(raw, 9)

with open(path, "wb") as handle:
    handle.write(b"\x89PNG\r\n\x1a\n")
    handle.write(chunk(b"IHDR", ihdr))
    handle.write(chunk(b"IDAT", idat))
    handle.write(chunk(b"IEND", b""))
PY

echo "Resolving appimagetool..."
appimagetool="$appimagetool_override"
if [[ -z "$appimagetool" ]]; then
    # Cache path is arch-tagged so a workspace shared or synced across an x86_64 and an aarch64
    # machine never picks up the wrong architecture's cached binary.
    appimagetool="$workspace/Launcher/artifacts/appimagetool-$appimagetool_arch"
    if [[ ! -x "$appimagetool" ]]; then
        echo "Downloading appimagetool ($appimagetool_arch)..."
        mkdir -p "$(dirname "$appimagetool")"
        curl -fsSL "https://github.com/AppImage/appimagetool/releases/download/continuous/appimagetool-$appimagetool_arch.AppImage" \
            -o "$appimagetool"
        chmod +x "$appimagetool"
    fi
fi

mkdir -p "$output_dir"
echo "Packaging..."
# appimagetool detects the target architecture from the first ELF executable it finds in the
# AppDir; AppRun here is a shell script, not ELF, so ARCH must be set explicitly.
output_name="WiiCompiled-Setup-$appimagetool_arch.AppImage"
ARCH="$appimagetool_arch" "$appimagetool" "$appdir" "$output_dir/$output_name"
echo "Built: $output_dir/$output_name"
