#!/usr/bin/env bash
# Turn one locally compiled macOS product into a self-contained .app bundle.
set -euo pipefail

fail() { printf 'publish-app.command: error: %s\n' "$*" >&2; exit 1; }
usage() {
    cat <<'EOF'
Usage: publish-app.command --build-dir DIR --product {WiiCompiled|RetroRewind} --output-dir DIR [options]

Copies a locally built product and its runtime assets into OUTPUT-DIR/<product>.app.
It bundles non-system dylibs, rewrites their install names, and ad-hoc signs the
result. This is suitable for local use; a release must replace ad-hoc signing
with the project's Developer ID signing and notarization process.

  --architecture {arm64|x86_64}       Required architecture of the compiled product (default: host)
  --minimum-system-version VERSION    App bundle minimum macOS version (default: 12.0)
EOF
}

build_dir=""; product=""; output_dir=""; architecture=$(uname -m); minimum_system_version=12.0
while (($#)); do
    case "$1" in
        --build-dir) build_dir=${2:-}; shift 2 ;;
        --product) product=${2:-}; shift 2 ;;
        --output-dir) output_dir=${2:-}; shift 2 ;;
        --architecture) architecture=${2:-}; shift 2 ;;
        --minimum-system-version) minimum_system_version=${2:-}; shift 2 ;;
        -h|--help) usage; exit 0 ;;
        *) fail "unknown option: $1" ;;
    esac
done
[[ "$product" == WiiCompiled || "$product" == RetroRewind ]] || fail '--product must be WiiCompiled or RetroRewind'
[[ "$architecture" == arm64 || "$architecture" == x86_64 ]] || fail '--architecture must be arm64 or x86_64'
[[ "$minimum_system_version" =~ ^[0-9]+(\.[0-9]+){1,2}$ ]] || fail '--minimum-system-version must contain two or three period-separated integers'
for tool in codesign ditto install_name_tool lipo otool; do command -v "$tool" >/dev/null || fail "required macOS tool is unavailable: $tool"; done
[[ -x "$build_dir/$product" ]] || fail "missing compiled product: $build_dir/$product"
lipo "$build_dir/$product" -verify_arch "$architecture" || fail "compiled product is not $architecture: $build_dir/$product"
for asset in dsp_coef.bin initial_pipeline_cache.db wii_bootstrap; do [[ -e "$build_dir/$asset" ]] || fail "missing runtime asset: $build_dir/$asset"; done

app="$output_dir/$product.app"
macos="$app/Contents/MacOS"
frameworks="$app/Contents/Frameworks"
resources="$app/Contents/Resources"
rm -rf "$app"
mkdir -p "$macos" "$frameworks" "$resources"
cat > "$app/Contents/Info.plist" <<EOF
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0"><dict>
  <key>CFBundleDevelopmentRegion</key><string>en</string>
  <key>CFBundleExecutable</key><string>$product</string>
  <key>CFBundleIdentifier</key><string>org.wiicompiled.$product</string>
  <key>CFBundleInfoDictionaryVersion</key><string>6.0</string>
  <key>CFBundleName</key><string>$product</string>
  <key>CFBundlePackageType</key><string>APPL</string>
  <key>CFBundleShortVersionString</key><string>0.1.0</string>
  <key>CFBundleVersion</key><string>1</string>
  <key>LSMinimumSystemVersion</key><string>$minimum_system_version</string>
  <key>NSHighResolutionCapable</key><true/>
</dict></plist>
EOF
ditto "$build_dir/$product" "$macos/$product"
for asset in dsp_coef.bin initial_pipeline_cache.db wii_bootstrap; do
    ditto "$build_dir/$asset" "$resources/$asset"
    ln -s "../Resources/$asset" "$macos/$asset"
done

# Resolve a non-system dependency from the build product's rpaths. This covers
# both traditional Homebrew dylibs and the vendored dylibs CMake emits under
# the local build directory for a cross-architecture build.
dependency_path() {
    local current=$1 dependency=$2 name rpath candidate
    case "$dependency" in
        /opt/homebrew/*|/usr/local/*)
            [[ -f "$dependency" ]] && { printf '%s\n' "$dependency"; return 0; }
            ;;
        @rpath/*)
            name=${dependency##*/}
            while IFS= read -r rpath; do
                case "$rpath" in
                    @loader_path/*) rpath="$(dirname "$current")/${rpath#@loader_path/}" ;;
                    @executable_path/*) rpath="$macos/${rpath#@executable_path/}" ;;
                esac
                candidate="$rpath/$name"
                [[ -f "$candidate" ]] && { printf '%s\n' "$candidate"; return 0; }
            done < <(otool -l "$current" | awk '/LC_RPATH/{rpath = 1; next} rpath && /path / { print $2; rpath = 0 }')
            ;;
        @loader_path/*)
            candidate="$(dirname "$current")/${dependency#@loader_path/}"
            [[ -f "$candidate" ]] && { printf '%s\n' "$candidate"; return 0; }
            ;;
    esac
    return 1
}

# Build a closure of non-system dylibs. System libraries remain system
# references, while every resolved dependency is copied beside the executable.
queue=("$macos/$product")
while ((${#queue[@]})); do
    current=${queue[0]}
    queue=("${queue[@]:1}")
    while IFS= read -r dependency; do
        dependency_path=$(dependency_path "$current" "$dependency") || continue
        name=$(basename "$dependency")
        if [[ ! -f "$frameworks/$name" ]]; then
            ditto "$dependency_path" "$frameworks/$name"
            install_name_tool -id "@rpath/$name" "$frameworks/$name"
            queue+=("$frameworks/$name")
        fi
    done < <(otool -L "$current" | tail -n +2 | awk '{print $1}')
done
while IFS= read -r binary; do
    while IFS= read -r old; do
        name=$(basename "$old")
        [[ -f "$frameworks/$name" ]] || continue
        if [[ "$binary" == "$macos/$product" ]]; then
            install_name_tool -change "$old" "@executable_path/../Frameworks/$name" "$binary"
        else
            install_name_tool -change "$old" "@loader_path/$name" "$binary"
        fi
    done < <(otool -L "$binary" | tail -n +2 | awk '{print $1}')
done < <(find "$frameworks" -type f -print; printf '%s\n' "$macos/$product")

find "$frameworks" -type f -exec codesign --force --sign - {} +
codesign --force --deep --sign - "$app"
codesign --verify --deep --strict "$app"
printf 'MKWCBUILD:APP=%s\n' "$app"
