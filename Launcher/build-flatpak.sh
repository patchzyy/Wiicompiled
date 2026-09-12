#!/usr/bin/env bash
# Builds the self-hosted WiiCompiled Flatpak bundle (single-file .flatpak asset).
#
# Everything the old versions staged by hand on the host - the dotnet publishes, the portable
# toolchain prune, the workspace snapshot, the icon/desktop/entrypoint - is now real module
# build-commands inside Launcher/flatpak/io.github.TeamWheelWizard.Wiicompiled.yml.in. What
# remains as host-side preparation is only what the manifest cannot reasonably build itself:
#
#   * the source tree staging: flatpak-builder's `dir` source has no exclude list, so a slim
#     copy of the repo (no .git/artifacts/bin/obj/build trees) is made for the module sources,
#   * the native-prebuilt harvest: compiled once per pinned toolchain/flag set by
#     Prepare-NativePrebuilt.sh and imported by the native-prebuilt module (fingerprint-guarded),
#   * the .bundle-version stamp the workspace-snapshot module injects.
#
# The app then builds from real, pinned-archive sources against org.freedesktop.Platform with the
# compile dev files bundled under /app/usr (see the dev-files module and the manifest header). On
# hosts where nested bwrap cannot run (e.g. inside a container), run it there with
# `--flatpak-builder`/`--flatpak` disabled - flatpak-builder itself always runs where bwrap works.
set -euo pipefail

script_dir=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
workspace=$(cd "$script_dir/.." && pwd)

app_id="io.github.TeamWheelWizard.Wiicompiled"
runtime_branch="25.08"
output_dir="$workspace/Launcher/dist"
flatpak_override=""
flatpak_builder_cmd="flatpak-builder"
arch_override=""

while [[ $# -gt 0 ]]; do
    case "$1" in
        --output-dir) output_dir=$2; shift 2 ;;
        --app-id) app_id=$2; shift 2 ;;
        --runtime-branch) runtime_branch=$2; shift 2 ;;
        --arch) arch_override=$2; shift 2 ;;
        --flatpak) flatpak_override=$2; shift 2 ;;
        --flatpak-builder) flatpak_builder_cmd=$2; shift 2 ;;
        -h|--help)
            echo "Usage: build-flatpak.sh [--output-dir DIR] [--app-id ID] [--runtime-branch BRANCH] [--arch ARCH] [--flatpak PATH] [--flatpak-builder PATH]" >&2
            exit 0
            ;;
        *) echo "build-flatpak.sh: unknown argument: $1" >&2; exit 1 ;;
    esac
done

# ELF-header userspace-architecture detection (the kernel's `uname -m` can disagree with the
# actual userspace, e.g. an aarch64 kernel running 32-bit armhf userland).
if [[ -n "$arch_override" ]]; then
    bundle_arch=$arch_override
else
    elf_exe=$(readlink -f "/proc/$$/exe")
    elf_class=$(od -An -t u1 -j 4 -N 1 "$elf_exe" | tr -d ' ')
    elf_machine=$(( $(od -An -t u1 -j 19 -N 1 "$elf_exe" | tr -d ' ') * 256 + $(od -An -t u1 -j 18 -N 1 "$elf_exe" | tr -d ' ') ))
    case "$elf_class:$elf_machine" in
        2:62) bundle_arch=x86_64 ;;
        2:183) bundle_arch=aarch64 ;;
        *) echo "build-flatpak.sh: unsupported userspace architecture (ELF class $elf_class, machine $elf_machine) - WiiCompiled requires a 64-bit x86_64 or aarch64 userland" >&2; exit 1 ;;
    esac
fi
[[ "$bundle_arch" == "x86_64" || "$bundle_arch" == "aarch64" ]] || { echo "build-flatpak.sh: --arch must be x86_64 or aarch64" >&2; exit 1; }

flatpak_bin=${flatpak_override:-flatpak}

# The provenance freshness check (and a fresh native-prebuilt harvest) needs python3. On hosts
# that lack it, fall back to what nix provides.
PYTHON3=""
if command -v python3 >/dev/null 2>&1; then
    PYTHON3=$(command -v python3)
elif command -v nix >/dev/null 2>&1; then
    echo "build-flatpak.sh: python3 not on PATH; using nix-shell to provide it..."
    PYTHON3=$(nix shell nixpkgs#python3 --command python3 -c 'import sys; print(sys.executable)')
fi
[[ -n "$PYTHON3" ]] || { echo "build-flatpak.sh: python3 is required for the native-prebuilt provenance check" >&2; exit 1; }

# The build needs the SDK (build sandbox), the Platform runtime (what the bundle installs on),
# and the dotnet8 sdk-extension (the dotnet-apps module's compiler). Require all three up front
# with one actionable message; flatpak-builder also auto-installs them if a remote is configured.
for ref in \
    "org.freedesktop.Platform//$runtime_branch" \
    "org.freedesktop.Sdk//$runtime_branch" \
    "org.freedesktop.Sdk.Extension.dotnet8//$runtime_branch"; do
    if ! "$flatpak_bin" info "$ref" >/dev/null 2>&1; then
        echo "build-flatpak.sh: $ref is not installed. Install it with:" >&2
        echo "  flatpak remote-add --user --if-not-exists flathub https://flathub.org/repo/flathub.flatpakrepo" >&2
        echo "  flatpak install --user -y flathub $ref" >&2
        exit 1
    fi
done

artifacts="$workspace/Launcher/artifacts/flatpak-build"
mkdir -p "$artifacts"

# --- source tree staging (slim copy for the `dir` module sources) -----------------------------

source_tree="$artifacts/source-tree"
echo "Staging the slim source tree (excluding .git, artifacts, bin/obj, build trees)..."
rm -rf "$source_tree"
mkdir -p "$source_tree"
tar -C "$workspace" \
    --exclude .git \
    --exclude .flatpak-builder \
    --exclude 'Launcher/artifacts' \
    --exclude 'Launcher/dist' \
    --exclude 'Launcher/bin' \
    --exclude 'Launcher/obj' \
    --exclude '*/bin' --exclude '*/obj' \
    --exclude '*/build' --exclude 'aurora-main/extern' \
    -cf - . | tar -C "$source_tree" -xf -

# --- bundle-version stamp ----------------------------------------------------------------------
# Changes whenever the bundled paths did; the entrypoint re-syncs the sandbox workspace only on a
# change. Injected into the workspace-snapshot module's build commands at render time.
version=""
if git -C "$workspace" rev-parse HEAD >/dev/null 2>&1; then
    version=$(git -C "$workspace" rev-parse HEAD)
    if [[ -n "$(git -C "$workspace" status --porcelain 2>/dev/null)" ]]; then
        version="$version-dirty-$(date -u +%s)"
    fi
else
    version=$(date -u +%s)
fi

# --- native-prebuilt harvest -------------------------------------------------------------------
native_prebuilt_dir="$workspace/Launcher/artifacts/native-prebuilt-$bundle_arch"
echo "Checking whether the precompiled aurora + third-party package ($bundle_arch) is current..."
current_fingerprint=$(bash "$script_dir/Prepare-NativePrebuilt.sh" --arch "$bundle_arch" --print-fingerprint-only)
package_current=0
if [[ -f "$native_prebuilt_dir/provenance.json" ]]; then
    package_current=$(CURRENT_FINGERPRINT="$current_fingerprint" "$PYTHON3" - "$native_prebuilt_dir/provenance.json" <<'PY'
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
    echo "Native prebuilt package is missing or stale; harvesting a fresh one (compiles aurora/third-party once, can take a while)..."
    bash "$script_dir/Prepare-NativePrebuilt.sh" --arch "$bundle_arch"
fi

# --- per-arch source pins ----------------------------------------------------------------------
llvm_release_arch=""; llvm_sha256=""
cmake_release_arch=""; cmake_sha256=""
ninja_asset=""; ninja_sha256=""
lld_deb_icu_url=""; lld_deb_icu_sha256=""
lld_deb_xml_url=""; lld_deb_xml_sha256=""
nodtool_url=""; nodtool_sha256=""
case "$bundle_arch" in
    x86_64)
        llvm_release_arch=X64
        llvm_sha256=df0e1ecf16caf3489a272a5eea4eec9b0d82878f6477fa309504f918a0006384
        cmake_release_arch=x86_64
        cmake_sha256=927b2368a946c37269c3a66225ab00544e756459cdd0b5d0da438694fb9ff802
        ninja_asset=ninja-linux.zip
        ninja_sha256=5749cbc4e668273514150a80e387a957f933c6ed3f5f11e03fb30955e2bbead6
        nodtool_url=https://github.com/encounter/nod/releases/download/v2.0.0-alpha.10/nodtool-linux-x86_64
        nodtool_sha256=f853b83268b9542faf0d28815bc3a23c090ae28ebf397e46f99cbb08122c8307
        lld_deb_icu_url=https://launchpad.net/ubuntu/+archive/primary/+files/libicu70_70.1-2_amd64.deb
        lld_deb_icu_sha256=58a154f6307289813da2276f900498ef536ae7c0522d2cf31a3c3c5cf62dfd9a
        lld_deb_xml_url=https://launchpad.net/ubuntu/+archive/primary/+files/libxml2_2.9.13+dfsg-1build1_amd64.deb
        lld_deb_xml_sha256=4831826d0e320f6715722716d9737c7e57a052d20e55dd03ff7444c6e502a3ff
        ;;
    aarch64)
        llvm_release_arch=ARM64
        llvm_sha256=805efad2bb91cb4967fa569e0881d10c0f69c04461cf671cccbae19f547acc34
        cmake_release_arch=aarch64
        cmake_sha256=9ea38356dbd3e32e51029a3e09a0f2f8e117ef4fbcaad7a21ffb36409bbd5cb4
        ninja_asset=ninja-linux-aarch64.zip
        ninja_sha256=fd2cacc8050a7f12a16a2e48f9e06fca5c14fc4c2bee2babb67b58be17a607fc
        nodtool_url=https://github.com/encounter/nod/releases/download/v2.0.0-alpha.10/nodtool-linux-aarch64
        nodtool_sha256=b0d94617ed2393334845669cc1f74f1e4fce0d6acb24e8be28c479689a1b907b
        lld_deb_icu_url=https://launchpad.net/ubuntu/+archive/primary/+files/libicu70_70.1-2_arm64.deb
        lld_deb_icu_sha256=ac68372cf4a976e6a206858fd9b28c68e49d37d650b9b8653270038a6e7bc174
        lld_deb_xml_url=https://launchpad.net/ubuntu/+archive/primary/+files/libxml2_2.9.13+dfsg-1build1_arm64.deb
        lld_deb_xml_sha256=85ec6ee7d1169c825beba2a30bbb3b37974b286d0284c69882de12b4eeb6c99b
        ;;
esac

# --- manifest render + build -------------------------------------------------------------------
manifest_template="$script_dir/flatpak/io.github.TeamWheelWizard.Wiicompiled.yml.in"
manifest_path="$artifacts/wiicompiled-$bundle_arch.yml"
sed -e "s|@RUNTIME_BRANCH@|$runtime_branch|g" \
    -e "s|@APP_ID@|$app_id|g" \
    -e "s|@SOURCE_TREE@|$source_tree|g" \
    -e "s|@BUNDLE_VERSION@|$version|g" \
    -e "s|@NATIVE_PREBUILT_DIR@|$native_prebuilt_dir|g" \
    -e "s|@LLVM_RELEASE_ARCH@|$llvm_release_arch|g" \
    -e "s|@LLVM_SHA256@|$llvm_sha256|g" \
    -e "s|@CMAKE_RELEASE_ARCH@|$cmake_release_arch|g" \
    -e "s|@CMAKE_SHA256@|$cmake_sha256|g" \
    -e "s|@NINJA_ASSET@|$ninja_asset|g" \
    -e "s|@NINJA_SHA256@|$ninja_sha256|g" \
    -e "s|@LLD_DEB_ICU_URL@|$lld_deb_icu_url|g" \
    -e "s|@LLD_DEB_ICU_SHA@|$lld_deb_icu_sha256|g" \
    -e "s|@LLD_DEB_XML_URL@|$lld_deb_xml_url|g" \
    -e "s|@LLD_DEB_XML_SHA@|$lld_deb_xml_sha256|g" \
    -e "s|@NODTOOL_URL@|$nodtool_url|g" \
    -e "s|@NODTOOL_SHA256@|$nodtool_sha256|g" \
    "$manifest_template" > "$manifest_path"

build_dir="$artifacts/.build-$bundle_arch"
repo_dir="$artifacts/repo-$bundle_arch"
rm -rf "$build_dir" "$repo_dir"

echo "Building $build_dir from the rendered manifest and exporting to the local repo..."
# --default-branch=stable: flatpak-builder otherwise exports the app on the
# 'master' branch, but build-bundle below looks up the ref on 'stable'.
# --disable-rofiles-fuse: keeps the build off FUSE (the file size heuristics that
# decide which files go through the rofiles store are irrelevant for the payload).
"$flatpak_builder_cmd" --repo="$repo_dir" --force-clean --arch="$bundle_arch" \
    --default-branch=stable --disable-rofiles-fuse \
    "$build_dir" "$manifest_path"

mkdir -p "$output_dir"
output_name="WiiCompiled-Setup-$bundle_arch.flatpak"
echo "Bundling the single-file asset from the exported repo..."
"$flatpak_bin" build-bundle --runtime-repo=https://flathub.org/repo/flathub.flatpakrepo \
    "$repo_dir" "$output_dir/$output_name" "$app_id" stable
echo "Built: $output_dir/$output_name"