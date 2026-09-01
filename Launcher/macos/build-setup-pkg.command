#!/usr/bin/env bash
# Maintainer release builder. It packages setup/source/tooling only -- never a
# translated executable, extracted DATA tree, disc image, or Retro Rewind data.
set -euo pipefail

fail() { printf 'build-setup-pkg.command: error: %s\n' "$*" >&2; exit 1; }
# Release payloads must not inherit Finder metadata, resource forks, or a
# downloaded-file quarantine bit from a maintainer's working volume.
copy_clean() { DITTONORSRC=1 ditto --norsrc --noqtn "$@"; }
usage() {
    cat <<'EOF'
Usage: build-setup-pkg.command --nodtool-arm64 PATH --nodtool-x86_64 PATH --translator-arm64 PATH --translator-x86_64 PATH --cmake-root DIR --ninja-arm64 PATH --ninja-x86_64 PATH --output PKG [options]

Creates a game-code-free WiiCompiled Setup.pkg. The supplied tools must be
maintainer-verified, redistributable macOS artifacts for both arm64 and
x86_64. The setup package selects native tools for its host while the game is
compiled locally for that host architecture. CMake must be universal2. The
resulting pkg is unsigned unless
--installer-identity is supplied; releases should sign and notarize it with a
Developer ID Installer certificate.

  --workspace DIR             Repository root (default: script's grandparent)
  --version VERSION           Bundle/package version (default: 0.1.0)
  --installer-identity NAME   Developer ID Installer identity for productbuild
EOF
}

script_dir=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
workspace=$(cd "$script_dir/../.." && pwd); nodtool_arm64=""; nodtool_x86_64=""; translator_arm64=""; translator_x86_64=""; cmake_root=""; ninja_arm64=""; ninja_x86_64=""; output=""; version=0.1.0; identity=""
while (($#)); do
    case "$1" in
        --workspace) workspace=${2:-}; shift 2 ;;
        --nodtool-arm64) nodtool_arm64=${2:-}; shift 2 ;;
        --nodtool-x86_64) nodtool_x86_64=${2:-}; shift 2 ;;
        --translator-arm64) translator_arm64=${2:-}; shift 2 ;;
        --translator-x86_64) translator_x86_64=${2:-}; shift 2 ;;
        --cmake-root) cmake_root=${2:-}; shift 2 ;;
        --ninja-arm64) ninja_arm64=${2:-}; shift 2 ;;
        --ninja-x86_64) ninja_x86_64=${2:-}; shift 2 ;;
        --output) output=${2:-}; shift 2 ;;
        --version) version=${2:-}; shift 2 ;;
        --installer-identity) identity=${2:-}; shift 2 ;;
        -h|--help) usage; exit 0 ;;
        *) fail "unknown option: $1" ;;
    esac
done
version=${version#v}
[[ "$version" =~ ^[0-9]+(\.[0-9]+){0,2}$ ]] || fail '--version must contain one to three period-separated integers'
IFS=. read -r version_major version_minor version_patch <<< "$version"
short_version="$version_major.${version_minor:-0}.${version_patch:-0}"
for tool in pkgbuild productbuild ditto codesign lipo; do command -v "$tool" >/dev/null || fail "required macOS tool unavailable: $tool"; done
for tool_path in "$nodtool_arm64" "$nodtool_x86_64" "$translator_arm64" "$translator_x86_64" "$ninja_arm64" "$ninja_x86_64"; do [[ -x "$tool_path" ]] || fail 'each architecture-specific tool must name an executable'; done
[[ -x "$cmake_root/bin/cmake" ]] || fail '--cmake-root must contain bin/cmake'
require_arch() {
    local artifact=$1 arch=$2 label=$3
    lipo "$artifact" -verify_arch "$arch" >/dev/null 2>&1 || fail "$label must contain a $arch slice: $artifact"
}
require_arch "$nodtool_arm64" arm64 '--nodtool-arm64'; require_arch "$nodtool_x86_64" x86_64 '--nodtool-x86_64'
require_arch "$translator_arm64" arm64 '--translator-arm64'; require_arch "$translator_x86_64" x86_64 '--translator-x86_64'
require_arch "$ninja_arm64" arm64 '--ninja-arm64'; require_arch "$ninja_x86_64" x86_64 '--ninja-x86_64'
lipo "$cmake_root/bin/cmake" -verify_arch arm64 x86_64 >/dev/null 2>&1 || fail '--cmake-root/bin/cmake must be universal2'

# Slice checks above prevent accidental cross-architecture packaging. Exercise
# each supplied executable as well: an incorrectly bundled runtime can have a
# valid Mach-O header but still fail before the setup app can use it. Apple
# Silicon maintainers validate Intel tools through Rosetta when it is present.
host_arch=$(uname -m)
run_for_arch() {
    local arch=$1 label=$2
    shift 2
    if [[ "$arch" == "$host_arch" ]]; then
        "$@" >/dev/null || fail "$label did not run successfully"
    elif [[ "$host_arch" == arm64 && "$arch" == x86_64 ]] && /usr/bin/arch -x86_64 /usr/bin/true >/dev/null 2>&1; then
        /usr/bin/arch -x86_64 "$@" >/dev/null || fail "$label did not run successfully under Rosetta"
    else
        # Intel hosts cannot execute arm64 binaries. The slice remains checked
        # above; CI or an Apple Silicon maintainer must execute that tool set.
        printf 'build-setup-pkg.command: warning: unable to execute %s on %s; architecture slice was verified, but run it in %s CI before release\n' \
            "$label" "$host_arch" "$arch" >&2
    fi
}
for arch in arm64 x86_64; do
    if [[ "$arch" == arm64 ]]; then
        nodtool=$nodtool_arm64; translator=$translator_arm64; ninja=$ninja_arm64
    else
        nodtool=$nodtool_x86_64; translator=$translator_x86_64; ninja=$ninja_x86_64
    fi
    run_for_arch "$arch" "--nodtool-$arch" "$nodtool" --version
    run_for_arch "$arch" "--translator-$arch" "$translator" --help
    run_for_arch "$arch" "--ninja-$arch" "$ninja" --version
done
run_for_arch "$host_arch" '--cmake-root/bin/cmake' "$cmake_root/bin/cmake" --version
workspace=$(cd "$workspace" && pwd); output=$(cd "$(dirname "$output")" && pwd)/$(basename "$output")
stage=$(mktemp -d "${TMPDIR:-/tmp}/wiicompiled-pkg.XXXXXX")
trap 'rm -rf "$stage"' EXIT
app="$stage/root/Applications/WiiCompiled Setup.app"
resources="$app/Contents/Resources"
mkdir -p "$app/Contents/MacOS" "$resources/tools"
cat > "$app/Contents/Info.plist" <<EOF
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0"><dict>
<key>CFBundleExecutable</key><string>WiiCompiledSetup</string>
<key>CFBundleIdentifier</key><string>org.wiicompiled.setup</string>
<key>CFBundleName</key><string>WiiCompiled Setup</string>
<key>CFBundlePackageType</key><string>APPL</string>
<key>CFBundleShortVersionString</key><string>$short_version</string>
<key>CFBundleVersion</key><string>$version</string>
<key>LSMinimumSystemVersion</key><string>12.0</string>
</dict></plist>
EOF
cat > "$app/Contents/MacOS/WiiCompiledSetup" <<'EOF'
#!/usr/bin/env bash
resources="$(cd "$(dirname "$0")/../Resources" && pwd)"

# Finder launches an app with no terminal attached. The setup work deliberately
# writes human-readable build progress to stdout, so run its .command entry
# point in Terminal instead of discarding that output behind an inert app icon.
exec /usr/bin/osascript - "$resources/setup.command" "$@" <<'APPLESCRIPT'
on run argv
    set commandLine to quoted form of (item 1 of argv)
    if (count of argv) > 1 then
        repeat with argumentIndex from 2 to (count of argv)
            set commandLine to commandLine & " " & quoted form of (item argumentIndex of argv)
        end repeat
    end if
    tell application "Terminal"
        activate
        do script commandLine
    end tell
end run
APPLESCRIPT
EOF
chmod +x "$app/Contents/MacOS/WiiCompiledSetup"
copy_clean "$script_dir/setup.command" "$resources/setup.command"; chmod +x "$resources/setup.command"
# Copy only the build inputs. This deliberately avoids a maintainer's ignored
# output directories, local disc extraction, and developer-only packaging.
mkdir -p "$resources/workspace"
for source in aurora-main projects runtime translator; do
    [[ -d "$workspace/$source" ]] || fail "required workspace directory is missing: $source"
    copy_clean "$workspace/$source" "$resources/workspace/$source"
done
mkdir -p "$resources/workspace/Launcher/macos"
copy_clean "$workspace/Launcher/local-build-macos.command" "$resources/workspace/Launcher/local-build-macos.command"
copy_clean "$workspace/Launcher/macos/extract-disc.command" "$resources/workspace/Launcher/macos/extract-disc.command"
copy_clean "$workspace/Launcher/macos/publish-app.command" "$resources/workspace/Launcher/macos/publish-app.command"
chmod +x "$resources/workspace/Launcher/local-build-macos.command" "$resources/workspace/Launcher/macos/"*.command
mkdir -p "$resources/tools/cmake" "$resources/tools/arm64" "$resources/tools/x86_64"
copy_clean "$nodtool_arm64" "$resources/tools/arm64/nodtool"; chmod +x "$resources/tools/arm64/nodtool"
copy_clean "$nodtool_x86_64" "$resources/tools/x86_64/nodtool"; chmod +x "$resources/tools/x86_64/nodtool"
copy_clean "$translator_arm64" "$resources/tools/arm64/Translator.Cli"; chmod +x "$resources/tools/arm64/Translator.Cli"
copy_clean "$translator_x86_64" "$resources/tools/x86_64/Translator.Cli"; chmod +x "$resources/tools/x86_64/Translator.Cli"
copy_clean "$cmake_root" "$resources/tools/cmake"
copy_clean "$ninja_arm64" "$resources/tools/arm64/ninja"; chmod +x "$resources/tools/arm64/ninja"
copy_clean "$ninja_x86_64" "$resources/tools/x86_64/ninja"; chmod +x "$resources/tools/x86_64/ninja"
copy_clean "$workspace/LICENSE" "$resources/LICENSE"
copy_clean "$workspace/THIRD-PARTY-NOTICES.md" "$resources/THIRD-PARTY-NOTICES.md"
codesign --force --deep --sign - "$app"
pkg="$stage/WiiCompiled-Setup-unsigned.pkg"
DITTONORSRC=1 COPYFILE_DISABLE=1 pkgbuild --root "$stage/root" --identifier org.wiicompiled.setup --version "$version" --install-location / "$pkg"
if [[ -n "$identity" ]]; then productbuild --sign "$identity" --package "$pkg" "$output"; else ditto "$pkg" "$output"; fi
printf 'Created game-code-free package: %s\n' "$output"
