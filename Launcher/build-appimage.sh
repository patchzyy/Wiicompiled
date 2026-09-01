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
# matching Launcher/local-build.sh's own remaining prerequisites.
#
# An AppImage mounts read-only, but local-build.sh writes generated/, native-build/, Assets/, etc.
# into the workspace it's given. So AppRun (written below) copies the bundled workspace snapshot
# out to a writable cache directory on first run, and only ever re-syncs the bundled directories
# (runtime/, aurora-main/, projects/, local-build.sh) on a later run whose bundled version changed
# - generated/native-build/Assets/PulsarPacks live only in that writable cache and are never
# touched by the sync, so local-build.sh's own incremental caching survives across runs and across
# AppImage updates. translator/ isn't part of this snapshot at all: it's published as its own
# self-contained binary (usr/bin/translator-cli) below and never needs a writable copy.
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

if git -C "$workspace" rev-parse HEAD >/dev/null 2>&1; then
    git -C "$workspace" rev-parse HEAD > "$appdir/workspace/.bundle-version"
else
    date -u +%s > "$appdir/workspace/.bundle-version"
fi

echo "Writing AppRun..."
cat > "$appdir/AppRun" <<'APPRUN'
#!/bin/bash
set -euo pipefail
HERE="$(dirname "$(readlink -f "$0")")"
CACHE="${XDG_DATA_HOME:-$HOME/.local/share}/WiiCompiled/workspace"
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
exec "$HERE/usr/bin/wiicompiled-setup" --workspace "$CACHE" \
    --translator-bin "$HERE/usr/bin/translator-cli" \
    --disc-tool-bin "$HERE/usr/bin/nodtool" \
    --cc "$HERE/usr/toolchain/bin/clang" \
    --cxx "$HERE/usr/toolchain/bin/clang++" \
    --fuse-ld lld \
    --cmake "$HERE/usr/toolchain/bin/cmake" \
    --ninja "$HERE/usr/toolchain/bin/ninja" "$@"
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
