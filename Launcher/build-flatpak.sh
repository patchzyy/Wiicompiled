#!/usr/bin/env bash
# Packages Launcher/WiiCompiled.Setup.Linux as a self-contained Flatpak bundle for local install:
# a single .flatpak file anyone can `flatpak install` with no git clone, no `dotnet` install, and
# no host build prerequisites of any kind. It is the recommended Linux distribution path; the
# AppImage (build-appimage.sh) remains the alternative for users who prefer a file to execute
# directly.
#
# The bundle carries the exact same payload as the AppImage - self-contained installer and
# translator binaries, bundled nodtool, the pruned clang/lld/cmake/ninja toolchain
# (prepare-portable-tools.sh), the precompiled aurora + third-party package
# (Prepare-NativePrebuilt.sh), and a workspace snapshot - but lays it out under a Flatpak /app
# and runs on the org.freedesktop.Sdk runtime. The key difference from the AppImage is *why* the
# SDK is used as the app runtime: a Flatpak only sees what its runtime + bundle provide, and
# local-build.sh compiles the translated game at install time. The AppImage leans on the host
# distro for the glibc/libstdc++/zlib dev files its bundled clang resolves at compile time; the
# Sandbox has no host distro, so the SDK runtime fills exactly that role (its /usr/include, crt
# objects, libz.so and libstdc++-devel are what the build resolves against). /app is read-only
# and stable across runs (unlike an AppImage's fresh /tmp/.mount_XXXXXX FUSE mount every launch),
# so the entrypoint below - the Flatpak analogue of AppRun - copies the workspace snapshot out to
# a writable cache and references the toolchain/native-prebuilt at their stable /app paths
# directly, with no symlink indirection needed to keep CMake/Ninja's baked command lines stable.
#
# The sandbox's game-compile needs are what set the permissions: --filesystem=home so the user's
# game dump ISO and any --install-dir/--retro-dir paths resolve, --share=network for the
# Retro-WFC payload download, and the socket/device grants for launching the compiled game.
set -euo pipefail

script_dir=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
workspace=$(cd "$script_dir/.." && pwd)

app_id="io.github.skiletro.Wiicompiled"
runtime_branch="25.08"
output_dir="$workspace/Launcher/dist"
flatpak_override=""

while [[ $# -gt 0 ]]; do
    case "$1" in
        --output-dir) output_dir=$2; shift 2 ;;
        --app-id) app_id=$2; shift 2 ;;
        --runtime-branch) runtime_branch=$2; shift 2 ;;
        --flatpak) flatpak_override=$2; shift 2 ;;
        -h|--help)
            echo "Usage: build-flatpak.sh [--output-dir DIR] [--app-id ID] [--runtime-branch BRANCH] [--flatpak PATH]"
            exit 0
            ;;
        *) echo "build-flatpak.sh: unknown argument: $1" >&2; exit 1 ;;
    esac
done

# Same ELF-header userspace-architecture detection as build-appimage.sh (the kernel's `uname -m`
# can disagree with the actual userspace, e.g. an aarch64 kernel running 32-bit armhf userland) -
# the dotnet RID must match the binaries that will actually run inside the sandbox.
elf_exe=$(readlink -f "/proc/$$/exe")
elf_class=$(od -An -t u1 -j 4 -N 1 "$elf_exe" | tr -d ' ')
elf_machine_lo=$(od -An -t u1 -j 18 -N 1 "$elf_exe" | tr -d ' ')
elf_machine_hi=$(od -An -t u1 -j 19 -N 1 "$elf_exe" | tr -d ' ')
elf_machine=$(( elf_machine_hi * 256 + elf_machine_lo ))

case "$elf_class:$elf_machine" in
    2:62)
        dotnet_rid=linux-x64
        bundle_arch=x86_64
        ;;
    2:183)
        dotnet_rid=linux-arm64
        bundle_arch=aarch64
        ;;
    *)
        echo "build-flatpak.sh: unsupported userspace architecture (ELF class $elf_class, machine $elf_machine) - WiiCompiled requires a 64-bit x86_64 or aarch64 userland" >&2
        exit 1
        ;;
esac

runtime_ref="org.freedesktop.Sdk//$runtime_branch"
flatpak_bin=${flatpak_override:-flatpak}

if ! "$flatpak_bin" info "$runtime_ref" >/dev/null 2>&1; then
    echo "build-flatpak.sh: $runtime_ref is not installed. Install it with:" >&2
    echo "  flatpak remote-add --user --if-not-exists flathub https://flathub.org/repo/flathub.flatpakrepo" >&2
    echo "  flatpak install --user -y flathub $runtime_ref" >&2
    exit 1
fi

stagedir="$workspace/Launcher/artifacts/flatpak-build/$bundle_arch"
rm -rf "$stagedir"
mkdir -p "$stagedir/app/bin" "$stagedir/app/libexec" "$stagedir/app/workspace/Launcher" \
    "$stagedir/app/usr" "$stagedir/app/share/applications" \
    "$stagedir/app/share/icons/hicolor/512x512/apps"

echo "Publishing the installer (self-contained $dotnet_rid)..."
publish_tmp="$workspace/Launcher/artifacts/flatpak-build/publish-$bundle_arch"
rm -rf "$publish_tmp"
dotnet publish "$workspace/Launcher/WiiCompiled.Setup.Linux" -c Release -r "$dotnet_rid" \
    --self-contained -p:PublishSingleFile=true -p:EnableCompressionInSingleFile=true \
    -o "$publish_tmp"
cp "$publish_tmp/WiiCompiled.Setup.Linux" "$stagedir/app/libexec/wiicompiled-setup"
chmod +x "$stagedir/app/libexec/wiicompiled-setup"

echo "Publishing the translator (self-contained $dotnet_rid)..."
translator_publish_tmp="$workspace/Launcher/artifacts/flatpak-build/publish-translator-$bundle_arch"
rm -rf "$translator_publish_tmp"
dotnet publish "$workspace/translator/src/Translator.Cli" -c Release -r "$dotnet_rid" \
    --self-contained -p:PublishSingleFile=true -p:EnableCompressionInSingleFile=true \
    -o "$translator_publish_tmp"
cp "$translator_publish_tmp/Translator.Cli" "$stagedir/app/libexec/translator-cli"
chmod +x "$stagedir/app/libexec/translator-cli"

echo "Resolving nodtool..."
nodtool_path=$(dotnet run --project "$workspace/Launcher/WiiCompiled.Setup.Common.Cli" -c Release -- \
    --workspace "$workspace" | tail -n1)
cp "$nodtool_path" "$stagedir/app/libexec/nodtool"
chmod +x "$stagedir/app/libexec/nodtool"

echo "Preparing the portable clang/lld/cmake/ninja toolchain ($bundle_arch)..."
bash "$script_dir/prepare-portable-tools.sh" --arch "$bundle_arch"
toolchain_dir="$workspace/Launcher/artifacts/portable-tools/toolchain-$bundle_arch"
mkdir -p "$stagedir/app/usr/toolchain"
cp -a "$toolchain_dir"/. "$stagedir/app/usr/toolchain/"

# The official LLVM release's lld links three system host libs - libxml2, libicuuc, libicudata -
# whose SONAMEs the sandbox runtime does not provide (it ships libxml2 only as libxml2.so.16 and
# no ICU at all), so the bundle carries the copies the build host resolves and exposes them through
# LD_LIBRARY_PATH in the entrypoint.
lld_libs=$(ldd "$toolchain_dir/bin/lld" 2>/dev/null)
if printf '%s\n' "$lld_libs" | grep -q 'not found'; then
    echo "build-flatpak.sh: lld has dependencies the build host cannot resolve:" >&2
    printf '%s\n' "$lld_libs" | grep 'not found' >&2
    exit 1
fi
mkdir -p "$stagedir/app/usr/toolchain/lib"
while read -r soname libpath; do
    case "$soname" in
        libxml2.so.*|libicuuc.so.*|libicudata.so.*)
            cp -p "$libpath" "$stagedir/app/usr/toolchain/lib/$soname"
            ;;
    esac
done < <(printf '%s\n' "$lld_libs" | awk '$2 == "=>" { print $1, $3 }')
for libglob in "$stagedir/app/usr/toolchain/lib"/libxml2.so.* "$stagedir/app/usr/toolchain/lib"/libicuuc.so.* "$stagedir/app/usr/toolchain/lib"/libicudata.so.*; do
    if [[ ! -e "$libglob" ]]; then
        echo "build-flatpak.sh: expected bundled lld dependency missing: $libglob" >&2
        exit 1
    fi
done

native_prebuilt_dir="$workspace/Launcher/artifacts/native-prebuilt-$bundle_arch"
echo "Checking whether the precompiled aurora + third-party package ($bundle_arch) is current..."
current_fingerprint=$(bash "$script_dir/Prepare-NativePrebuilt.sh" --arch "$bundle_arch" --print-fingerprint-only)
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
    bash "$script_dir/Prepare-NativePrebuilt.sh" --arch "$bundle_arch"
fi
mkdir -p "$stagedir/app/native-prebuilt"
cp -a "$native_prebuilt_dir/." "$stagedir/app/native-prebuilt/"

echo "Staging the bundled workspace snapshot..."
for dir in runtime aurora-main projects; do
    cp -r "$workspace/$dir" "$stagedir/app/workspace/$dir"
done
find "$stagedir/app/workspace/aurora-main/extern" -mindepth 1 -maxdepth 1 -type d -exec rm -rf {} +
rm -rf "$stagedir/app/workspace/runtime/build"
cp "$workspace/Launcher/local-build.sh" "$stagedir/app/workspace/Launcher/local-build.sh"

# Same bundle-version stamping as build-appimage.sh: AppRun-analogue re-syncs the workspace into
# the writable cache only when this changes, so it must change whenever the bundled paths did.
if git -C "$workspace" rev-parse HEAD >/dev/null 2>&1; then
    version=$(git -C "$workspace" rev-parse HEAD)
    if [[ -n "$(git -C "$workspace" status --porcelain 2>/dev/null)" ]]; then
        version="$version-dirty-$(date -u +%s)"
    fi
    echo "$version" > "$stagedir/app/workspace/.bundle-version"
else
    date -u +%s > "$stagedir/app/workspace/.bundle-version"
fi

echo "Writing the Flatpak entrypoint (AppRun analogue)..."
cat > "$stagedir/app/bin/wiicompiled-setup" <<'ENTRYPOINT'
#!/bin/bash
set -euo pipefail
APP=/app
# lld in the bundled toolchain links host libs (libxml2, ICU) the sandbox runtime can't provide;
# expose the copies bundled at build time.
export LD_LIBRARY_PATH="$APP/usr/toolchain/lib${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
# /app is read-only and the workspace snapshot must be writable (local-build.sh writes
# generated/, native-build/, Assets/, ...), so it is copied out to the sandbox's writable data
# dir on first run and only re-synced when the bundled snapshot's version stamp changes - exactly
# AppRun's behavior, matching the comments in Launcher/build-appimage.sh.
CACHE="${XDG_DATA_HOME:-$HOME/.local/share}/WiiCompiled/workspace"
mkdir -p "$CACHE"
if [ ! -f "$CACHE/.bundle-version" ] || \
   [ "$(cat "$APP/workspace/.bundle-version")" != "$(cat "$CACHE/.bundle-version")" ]; then
    mkdir -p "$CACHE/Launcher"
    for dir in runtime aurora-main projects; do
        rm -rf "$CACHE/$dir"
        cp -r "$APP/workspace/$dir" "$CACHE/$dir"
    done
    cp "$APP/workspace/Launcher/local-build.sh" "$CACHE/Launcher/local-build.sh"
    cp "$APP/workspace/.bundle-version" "$CACHE/.bundle-version"
fi
# toolchain/ and native-prebuilt/ are NOT copied into the cache (they're large - ~500 MiB /
# ~90 MiB - and local-build.sh only ever reads from them). Unlike an AppImage, whose FUSE mount
# moves to a fresh /tmp/.mount_XXXXXX every launch (forcing the symlink workaround AppRun uses so
# CMake's baked-in command lines stay stable), a Flatpak's /app path is fixed for the lifetime of
# the installed bundle - so these are referenced directly here and the paths CMake records never
# change across runs.
exec "$APP/libexec/wiicompiled-setup" --workspace "$CACHE" \
    --translator-bin "$APP/libexec/translator-cli" \
    --disc-tool-bin "$APP/libexec/nodtool" \
    --cc "$APP/usr/toolchain/bin/clang" \
    --cxx "$APP/usr/toolchain/bin/clang++" \
    --fuse-ld lld \
    --cmake "$APP/usr/toolchain/bin/cmake" \
    --ninja "$APP/usr/toolchain/bin/ninja" \
    --native-prebuilt-dir "$APP/native-prebuilt" "$@"
ENTRYPOINT
chmod +x "$stagedir/app/bin/wiicompiled-setup"

echo "Writing desktop entry and icon..."
cat > "$stagedir/app/share/applications/$app_id.desktop" <<DESKTOP
[Desktop Entry]
Type=Application
Name=WiiCompiled Setup
Comment=Translate, compile, and launch Mario Kart Wii natively on Linux
Exec=wiicompiled-setup
Icon=$app_id
Categories=Game;
Terminal=true
DESKTOP

python3 - "$stagedir/app/share/icons/hicolor/512x512/apps/$app_id.png" <<'PY'
import struct
import sys
import zlib

path = sys.argv[1]


def chunk(tag: bytes, data: bytes) -> bytes:
    return struct.pack(">I", len(data)) + tag + data + struct.pack(">I", zlib.crc32(tag + data))


width = height = 512
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

build_dir="$workspace/Launcher/artifacts/flatpak-build/.build-$bundle_arch"
repo_dir="$workspace/Launcher/artifacts/flatpak-build/repo-$bundle_arch"
rm -rf "$build_dir" "$repo_dir"

echo "Initializing the Flatpak build dir on $runtime_ref..."
"$flatpak_bin" build-init "$build_dir" "$app_id" "$runtime_ref" "$runtime_ref" stable
cp -a "$stagedir/app/." "$build_dir/files/"

echo "Finalizing sandbox metadata..."
"$flatpak_bin" build-finish \
    --command=/app/bin/wiicompiled-setup \
    --filesystem=home \
    --share=network \
    --socket=wayland \
    --socket=x11 \
    --socket=pulseaudio \
    --device=dri \
    "$build_dir"

mkdir -p "$output_dir"
output_name="WiiCompiled-Setup-$bundle_arch.flatpak"
echo "Exporting to the local ostree repo and bundling..."
"$flatpak_bin" build-export --no-update-summary "$repo_dir" "$build_dir" stable
"$flatpak_bin" build-bundle --runtime-repo=https://flathub.org/repo/flathub.flatpakrepo \
    "$repo_dir" "$output_dir/$output_name" "$app_id" stable
echo "Built: $output_dir/$output_name"