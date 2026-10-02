#!/usr/bin/env bash
# Build the distributable macOS arm64 setup CLI and a clean source workspace.
set -euo pipefail

fail() { printf 'package-macos-setup.sh: error: %s\n' "$*" >&2; exit 1; }
usage() {
    cat <<'EOF'
Usage: package-macos-setup.sh [--output-dir DIR]

Creates WiiCompiled-Setup-macos-arm64.zip. Requires an Apple Silicon Mac and
.NET 8. The archive contains no game dump, translated game, or Retro Rewind data.
EOF
}

script_dir=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
repo_root=$(cd "$script_dir/.." && pwd)
output_dir="$repo_root/Launcher/dist"
while (($#)); do
    case "$1" in
        --output-dir) output_dir=${2:-}; shift 2 ;;
        -h|--help) usage; exit 0 ;;
        *) fail "unknown argument: $1" ;;
    esac
done

[[ $(uname -s) == Darwin ]] || fail 'packaging must run on macOS'
[[ $(uname -m) == arm64 ]] || fail 'packaging requires an Apple Silicon arm64 runner'
command -v dotnet >/dev/null || fail '.NET 8 SDK is required'
command -v git >/dev/null || fail 'git is required'
command -v ditto >/dev/null || fail 'ditto is required'

version=$(sed -n 's/.*<Version>\([^<]*\)<\/Version>.*/\1/p' \
    "$repo_root/Launcher/Directory.Build.props" | head -n1)
[[ "$version" =~ ^[0-9]+(\.[0-9]+){0,2}$ ]] || fail 'could not read a valid setup version'
git -C "$repo_root" diff --quiet HEAD -- projects runtime aurora-main \
    Launcher/local-build-macos.command Launcher/macos/extract-disc.command Launcher/macos/publish-app.command ||
    fail 'tracked packaged sources have changes not present in HEAD; commit them before packaging'
source_revision=$(git -C "$repo_root" rev-parse HEAD)

stage=$(mktemp -d "${TMPDIR:-/tmp}/wiicompiled-setup-macos.XXXXXX")
trap 'rm -rf "$stage"' EXIT
package="$stage/WiiCompiled-Setup-macos-arm64"
mkdir -p "$package/tools" "$package/workspace"

echo "Publishing WiiCompiled Setup $version for osx-arm64..."
dotnet publish "$repo_root/Launcher/WiiCompiled.Setup.Linux" -c Release -r osx-arm64 \
    --self-contained true -p:PublishSingleFile=true -p:EnableCompressionInSingleFile=true \
    -o "$stage/setup"
cp "$stage/setup/WiiCompiled.Setup.Linux" "$package/WiiCompiled-Setup-macos-arm64"
chmod 755 "$package/WiiCompiled-Setup-macos-arm64"

echo "Publishing the self-contained translator..."
dotnet publish "$repo_root/translator/src/Translator.Cli" -c Release -r osx-arm64 \
    --self-contained true -p:PublishSingleFile=true -p:EnableCompressionInSingleFile=true \
    -o "$stage/translator"
cp "$stage/translator/Translator.Cli" "$package/tools/Translator.Cli"
chmod 755 "$package/tools/Translator.Cli"

echo "Resolving the pinned nodtool release..."
nodtool=$(dotnet run --project "$repo_root/Launcher/WiiCompiled.Setup.Common.Cli" -c Release \
    --verbosity quiet -- --workspace "$repo_root" | tail -n1)
[[ -f "$nodtool" ]] || fail "nodtool resolver did not produce a binary: $nodtool"
cp "$nodtool" "$package/tools/nodtool"
chmod 755 "$package/tools/nodtool"

echo "Staging tracked, game-code-free build sources..."
git -C "$repo_root" archive --format=tar HEAD projects/mkwii runtime aurora-main |
    tar -xf - -C "$package/workspace"
git -C "$repo_root" archive --format=tar HEAD \
    Launcher/local-build-macos.command \
    Launcher/macos/extract-disc.command \
    Launcher/macos/publish-app.command |
    tar -xf - -C "$package/workspace"
printf '%s+%s\n' "$version" "$source_revision" > "$package/workspace/.setup-source-version"
chmod 755 "$package/workspace/Launcher/local-build-macos.command" \
    "$package/workspace/Launcher/macos/extract-disc.command" \
    "$package/workspace/Launcher/macos/publish-app.command"
cp "$repo_root/LICENSE" "$package/LICENSE"
cp "$repo_root/THIRD-PARTY-NOTICES.md" "$package/THIRD-PARTY-NOTICES.md"
cat > "$package/README.txt" <<EOF
WiiCompiled Setup $version - macOS Apple Silicon

Requires macOS 14 or later, Apple Silicon, Xcode Command Line Tools, CMake,
and Ninja. Install CMake and Ninja with Homebrew: brew install cmake ninja

This setup CLI extracts and translates your own PAL Mario Kart Wii disc locally.
No game dump, translated game, or Retro Rewind data is included.

Run from Terminal:
  ./WiiCompiled-Setup-macos-arm64 --version
  ./WiiCompiled-Setup-macos-arm64 --silent --game /path/to/your/RMCP01.iso
  ./WiiCompiled-Setup-macos-arm64 --check-products
  ./WiiCompiled-Setup-macos-arm64 --launch-base
  ./WiiCompiled-Setup-macos-arm64 --repair-products
  ./WiiCompiled-Setup-macos-arm64 --uninstall

Retro Rewind:
  ./WiiCompiled-Setup-macos-arm64 --silent --game /path/to/your/RMCP01.iso \\
    --retro-dir /path/to/RetroRewind6 --download-retro-wfc-payload

The release archive is not signed or notarized. When macOS marks files in the
downloaded archive as quarantined, verify the download and remove that attribute
from this extracted setup folder before running it:
  xattr -dr com.apple.quarantine .
EOF

"$package/WiiCompiled-Setup-macos-arm64" --version | grep -Fx "$version" >/dev/null ||
    fail 'packaged setup --version smoke test failed'
mkdir -p "$stage/home"
check_output=$(HOME="$stage/home" "$package/WiiCompiled-Setup-macos-arm64" --check-products --progress-json)
grep -Fq '"type":"products"' <<< "$check_output" &&
    grep -Fq '"type":"result","success":true' <<< "$check_output" ||
    fail 'packaged check-products smoke test failed'
if HOME="$stage/home" "$package/WiiCompiled-Setup-macos-arm64" --silent --progress-json \
    > "$stage/install-smoke.log" 2>&1; then
    fail 'install-without-disc smoke test unexpectedly succeeded'
fi
grep -Fq '"type":"result","success":false' "$stage/install-smoke.log" &&
    grep -Fq 'No --game ISO was given' "$stage/install-smoke.log" ||
    fail 'packaged install CLI contract smoke test failed'
[[ -x "$package/tools/Translator.Cli" && -x "$package/tools/nodtool" ]] ||
    fail 'a bundled command-line tool is not executable'
if find "$package" \( -type d -name Assets -o -type f \( -name main.dol -o -name StaticR.rel -o -name '*.iso' -o -name '*.wbfs' \) \) -print -quit |
    grep -q .; then
    fail 'staging unexpectedly contains proprietary game data'
fi

mkdir -p "$output_dir"
output_dir=$(cd "$output_dir" && pwd)
output="$output_dir/WiiCompiled-Setup-macos-arm64.zip"
rm -f "$output"
ditto -c -k --norsrc --noqtn --keepParent "$package" "$output"
[[ -s "$output" ]] || fail 'archive was not created'
echo "Created unsigned macOS arm64 setup: $output"
