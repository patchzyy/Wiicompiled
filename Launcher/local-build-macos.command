#!/usr/bin/env bash
# Native macOS build automation: optionally extract -> translate -> compile -> publish .app.
set -euo pipefail

script_dir=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
default_workspace=$(cd "$script_dir/.." && pwd)
# shellcheck source=build-cache.sh
source "$script_dir/build-cache.sh"

fail() { printf 'local-build-macos.command: error: %s\n' "$*" >&2; exit 1; }
step() { printf 'MKWCBUILD:STEP:%s %s\n' "$1" "$2"; }
assert_file() { [[ -f "$1" ]] || fail "$2 is missing: $1"; }

usage() {
    cat <<'EOF'
Usage: local-build-macos.command --output-dir DIR [options]

  --workspace DIR                 Repository root (default: this script's parent directory)
  --profile {base|retro-rewind|both}  Product to build (default: base)
  --output-dir DIR                Output .app directory (required; Retro Rewind for both)
  --base-output-dir DIR           Base .app directory (required with --profile both)
  --game IMAGE --nodtool PATH     Extract and verify a clean supported disc image first
  --retro-rewind-package-dir DIR  RetroRewind6 directory (required for Retro Rewind)
  --retro-wfc-offline-dir DIR     Directory containing the release's binary/payload.<game>.bin
  --skip-retro-wfc-payload        Build Retro Rewind without the shared Retro-WFC payload
  --force-clean-build             Delete local generated and current-architecture native build caches
  --parallel N                    Pin translation and build parallelism
  --cmake PATH --ninja PATH       Override build tools
  --dotnet PATH                   Override dotnet
  --translator-bin PATH           Use a self-contained Translator.Cli executable
  --setup-version VERSION         Version recorded in the runtime log (default: <Version> in
                                  Launcher/Directory.Build.props when present)
EOF
}

workspace="$default_workspace"; profile=base; output_dir=""; base_output_dir=""
game=""; nodtool=""; retro_root=""; retro_wfc=""; skip_retro_wfc=0; force_clean=0
parallel=0; cmake_bin=cmake; ninja_bin=ninja; dotnet_bin=dotnet; translator_bin=""
setup_version=""
while (($#)); do
    case "$1" in
        --workspace) workspace=${2:-}; shift 2 ;;
        --profile) profile=${2:-}; shift 2 ;;
        --output-dir) output_dir=${2:-}; shift 2 ;;
        --base-output-dir) base_output_dir=${2:-}; shift 2 ;;
        --game) game=${2:-}; shift 2 ;;
        --nodtool) nodtool=${2:-}; shift 2 ;;
        --retro-rewind-package-dir) retro_root=${2:-}; shift 2 ;;
        --retro-wfc-offline-dir) retro_wfc=${2:-}; shift 2 ;;
        --skip-retro-wfc-payload) skip_retro_wfc=1; shift ;;
        --force-clean-build) force_clean=1; shift ;;
        --parallel) parallel=${2:-}; shift 2 ;;
        --cmake) cmake_bin=${2:-}; shift 2 ;;
        --ninja) ninja_bin=${2:-}; shift 2 ;;
        --dotnet) dotnet_bin=${2:-}; shift 2 ;;
        --translator-bin) translator_bin=${2:-}; shift 2 ;;
        --setup-version) setup_version=${2:-}; shift 2 ;;
        -h|--help) usage; exit 0 ;;
        *) fail "unknown option: $1" ;;
    esac
done

[[ $(uname -s) == Darwin ]] || fail 'this build script is for macOS only'
macos_arch=$(uname -m)
case "$macos_arch" in arm64|x86_64) ;; *) fail "unsupported macOS architecture: $macos_arch" ;; esac
macos_deployment_target=12.0
workspace=$(cd "$workspace" && pwd)
if [[ -z "$setup_version" && -f "$workspace/Launcher/Directory.Build.props" ]]; then
    setup_version=$(sed -nE 's/.*<Version>([^<]+)<\/Version>.*/\1/p' "$workspace/Launcher/Directory.Build.props" | head -n 1)
fi
[[ "$setup_version" =~ ^[0-9A-Za-z.+-]*$ ]] || fail '--setup-version contains unsupported characters'
[[ -n "$output_dir" ]] || fail '--output-dir is required'
case "$profile" in base|retro-rewind|both) ;; *) fail '--profile must be base, retro-rewind, or both' ;; esac
builds_retro=0; [[ "$profile" != base ]] && builds_retro=1
if [[ "$profile" == both && -z "$base_output_dir" ]]; then fail '--base-output-dir is required with --profile both'; fi
if [[ "$profile" != both && -n "$base_output_dir" ]]; then fail '--base-output-dir is valid only with --profile both'; fi
if [[ -n "$game" || -n "$nodtool" ]]; then [[ -n "$game" && -n "$nodtool" ]] || fail '--game and --nodtool must be supplied together'; fi
if (( builds_retro )); then
    [[ -n "$retro_root" ]] || fail '--retro-rewind-package-dir is required for Retro Rewind'
    [[ -n "$retro_wfc" ]] && (( skip_retro_wfc )) && fail 'choose only one Retro-WFC mode'
    [[ -n "$retro_wfc" || $skip_retro_wfc -eq 1 ]] || fail 'choose a Retro-WFC payload directory or --skip-retro-wfc-payload'
fi
# Apple clang must match the SDK headers; another clang earlier on PATH may not.
cc_bin=$(xcrun -f clang 2>/dev/null) || fail 'Apple clang not found; install the Xcode Command Line Tools (xcode-select --install)'
cxx_bin=$(xcrun -f clang++ 2>/dev/null) || fail 'Apple clang++ not found; install the Xcode Command Line Tools (xcode-select --install)'
for tool in "$cmake_bin" "$ninja_bin" "$cc_bin" "$cxx_bin" shasum; do command -v "$tool" >/dev/null || fail "required tool not found: $tool"; done

assets="$workspace/Assets"; generated="$workspace/generated"
functions="$generated/functions"; metadata="$generated/base_translation_output.json"; manifest_dir="$workspace/build/base"
manifest="$manifest_dir/mkwii_base_manifest.json"; shards="$generated/build_shards"
native_build="$workspace/native-build-macos-$macos_arch"
x86_cpu_profile=v3
if [[ "$macos_arch" == x86_64 ]]; then
    native_macros=$("$cc_bin" -march=native -dM -E -x c /dev/null 2>/dev/null || true)
    for macro in __AVX__ __AVX2__ __BMI__ __BMI2__ __F16C__ __FMA__ __LZCNT__ __MOVBE__; do
        if ! grep -q "^#define $macro " <<<"$native_macros"; then
            x86_cpu_profile=v2
            printf 'MKWCBUILD: This CPU does not support x86-64-v3; building for x86-64-v2\n'
            break
        fi
    done
fi
if [[ -n "$game" ]]; then "$script_dir/macos/extract-disc.command" --workspace "$workspace" --game "$game" --assets-dir "$assets" --nodtool "$nodtool"; fi
assert_file "$assets/main.dol" 'extracted main.dol'; assert_file "$assets/StaticR.rel" 'extracted StaticR.rel'
# The translation project is the one pinned to the extracted main.dol, which decides the region.
project=$(mkw_resolve_project "$workspace" "$assets") || fail 'the extracted game cannot be translated'
region=$(mkw_project_pin "$project" region); [[ -n "$region" ]] || fail "could not find the region of $project"

if (( force_clean )); then
    step force-clean 'Discarding translation and native build caches'
    rm -rf "$generated" "$manifest_dir" "$native_build"
fi

if [[ -n "$translator_bin" ]]; then
    assert_file "$translator_bin" 'Translator.Cli executable'
    translator() { "$translator_bin" "$@"; }
else
    command -v "$dotnet_bin" >/dev/null || fail "required tool not found: $dotnet_bin"
    translator_dll="$workspace/translator/src/Translator.Cli/bin/Release/net8.0/Translator.Cli.dll"
    if [[ ! -f "$translator_dll" ]]; then
        step build-translator 'Building the translator'
        "$dotnet_bin" build "$workspace/translator/src/Translator.Cli/Translator.Cli.csproj" -c Release
    fi
    translator() { "$dotnet_bin" "$translator_dll" "$@"; }
fi

entry_point=$(awk '/^translation:/{inside=1} inside && /^[[:space:]]*-[[:space:]]*0[xX][0-9a-fA-F]+[[:space:]]*$/{gsub(/^[[:space:]]*-[[:space:]]*/, ""); print; exit}' "$project")
[[ -n "$entry_point" ]] || fail 'could not find the translation entry point'
cpu=$(sysctl -n hw.ncpu); mem_gib=$(( $(sysctl -n hw.memsize) / 1024 / 1024 / 1024 )); (( mem_gib < 1 )) && mem_gib=1
if (( parallel > 0 )); then translator_threads=$parallel; translated_jobs=$parallel; global_jobs=$parallel
else translator_threads=$(( cpu < 16 ? cpu : 16 )); translated_jobs=$(( cpu < mem_gib / 2 ? cpu : mem_gib / 2 )); (( translated_jobs < 1 )) && translated_jobs=1; global_jobs=$cpu; fi

if (( builds_retro )); then
    retro_root=$(cd "$retro_root" && pwd)
    if [[ ! -f "$retro_root/Binaries/Code.pul" && -f "$retro_root/RetroRewind6/Binaries/Code.pul" ]]; then
        retro_root="$retro_root/RetroRewind6"
    fi
    assert_file "$retro_root/Binaries/Code.pul" 'Retro Rewind Code.pul'
    stage="$workspace/PulsarPacks/completed/RetroRewind/RetroRewind6/Binaries"
    mkdir -p "$stage"
    if [[ ! "$retro_root/Binaries/Code.pul" -ef "$stage/Code.pul" ]]; then
        cp -f "$retro_root/Binaries/Code.pul" "$stage/Code.pul"
    fi
fi

translation_provenance="$generated/translation-provenance.json"
translation_fingerprint=$(mkw_translation_identity "$workspace" "${translator_bin:-$(dirname "${translator_dll:-}")}" "$script_dir/local-build-macos.command")
reuse_base=0
if [[ "$(mkw_read_identity "$translation_provenance" TranslationFingerprint)" == "$translation_fingerprint" && -f "$metadata" && -f "$manifest" ]]; then
    reuse_base=1
fi
if (( reuse_base && builds_retro )); then
    # A base tree that never saw this Code.pul would bake vanilla code into the modded product.
    pul_sha=$(mkw_sha256 "$retro_root/Binaries/Code.pul")
    if ! grep -q "\"codePulSha256\":\"$pul_sha\"" "$metadata" && \
       ! translator check-base-mod-awareness --project "$project" --profile retro-rewind --translation-output-metadata "$metadata" --code-pul "$retro_root/Binaries/Code.pul"; then
        reuse_base=0
    fi
fi
if (( reuse_base )); then
    step reuse-base-translation 'Reusing the completed base translation'
else
    rm -f "$translation_provenance"; mkdir -p "$functions" "$manifest_dir"
    step translate-base 'Translating the user-owned base game'
    translator translate-recursive "$entry_point" --project "$project" --outdir "$functions" --output-metadata "$metadata" --production-source-bundle "$generated/base_translation_sources.bin" --no-function-files --prune-stale --threads "$translator_threads"
    step emit-base-manifest 'Creating the local base translation manifest'
    translator emit-base-manifest --project "$project" --out "$manifest_dir" --functions-dir "$functions" --translation-output-metadata "$metadata" --region "$region"
    mkw_write_identity "$translation_provenance" TranslationFingerprint "$translation_fingerprint"
fi
if (( builds_retro )); then
    mod_out="$workspace/build/mods/retro_rewind_full_cpp"; args=(translate-mod --project "$project" --profile retro-rewind --base-manifest "$manifest" --base-translation-output-metadata "$metadata" --code-pul "$retro_root/Binaries/Code.pul" --mod-root "$retro_root" --mod-name 'Retro Rewind' --region "$region" --out "$mod_out" --prefer-cached-inputs --emit-cpp --threads "$translator_threads")
    if (( skip_retro_wfc )); then args+=(--skip-retro-wfc); else payload_file=$(mkw_retro_wfc_payload_file "$project") || fail "$project pins no Retro-WFC payload"; offline_payload="$retro_wfc/$payload_file"; assert_file "$offline_payload" 'Offline Retro-WFC payload'; args+=(--retro-wfc-payload "$offline_payload"); fi
    step translate-mod 'Translating Retro Rewind'; translator "${args[@]}"
fi
step generate-data-init 'Generating local game data initialization'; translator generate-data-init --project "$project"
args=(emit-build-shards --project "$project" --base-metadata "$metadata" --base-functions-dir "$functions" --native-source-dir "$workspace/runtime/src" --out "$shards")
if (( builds_retro )); then args+=(--resolved-profile "$mod_out/resolved_dispatch_profile.json" --retro-cpp-dir "$mod_out/cpp"); fi
step emit-build-shards 'Preparing native build shards'; translator "${args[@]}"

toolchain_provenance="$native_build/toolchain-provenance.json"
toolchain_fingerprint=$(printf '%s\n' "$(mkw_toolchain_identity "$workspace" "$cc_bin" "$cxx_bin" "$cmake_bin" "$ninja_bin")" "$cc_bin" "$cxx_bin" | mkw_sha256)
if [[ -d "$native_build" && "$(mkw_read_identity "$toolchain_provenance" ToolchainFingerprint)" != "$toolchain_fingerprint" ]]; then
    printf 'MKWCBUILD: The toolchain or a pinned dependency changed; rebuilding the native build from scratch\n'
    rm -rf "$native_build"
fi
step configure-native 'Configuring the native toolchain'
# Use Aurora's pinned SDL3 source on macOS.  A system SDL3 can be older than
# Aurora's required API even when find_package() succeeds.
"$cmake_bin" -S "$workspace/runtime" -B "$native_build" -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_C_COMPILER="$cc_bin" -DCMAKE_CXX_COMPILER="$cxx_bin" -DCMAKE_MAKE_PROGRAM="$ninja_bin" -DCMAKE_OSX_ARCHITECTURES="$macos_arch" -DCMAKE_OSX_DEPLOYMENT_TARGET="$macos_deployment_target" -DMKW_TRANSLATED_COMPILE_JOBS="$translated_jobs" -DMKW_X86_CPU_PROFILE="$x86_cpu_profile" -DAURORA_SDL3_PROVIDER=vendor -DMKW_SETUP_VERSION="$setup_version"
mkw_write_identity "$toolchain_provenance" ToolchainFingerprint "$toolchain_fingerprint"
targets=(); [[ "$profile" != retro-rewind ]] && targets+=(WiiCompiled); [[ "$profile" != base ]] && targets+=(RetroRewind)
step compile "Compiling ${targets[*]} locally"; "$cmake_bin" --build "$native_build" --target "${targets[@]}" --parallel "$global_jobs"
if [[ "$profile" != retro-rewind ]]; then "$script_dir/macos/publish-app.command" --build-dir "$native_build" --product WiiCompiled --output-dir "${base_output_dir:-$output_dir}" --architecture "$macos_arch" --minimum-system-version "$macos_deployment_target"; fi
if (( builds_retro )); then "$script_dir/macos/publish-app.command" --build-dir "$native_build" --product RetroRewind --output-dir "$output_dir" --architecture "$macos_arch" --minimum-system-version "$macos_deployment_target"; fi
printf 'MKWCBUILD:OUTPUT=%s\n' "$output_dir"
