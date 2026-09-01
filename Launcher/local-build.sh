#!/usr/bin/env bash
# Linux build automation: translate -> emit build shards -> configure -> compile -> publish.
#
# This is the native-Linux counterpart to Launcher/LocalBuild.ps1. It is a from-scratch parallel
# implementation, not a port of NativeBuildFlags.ps1: that file's canonical flags and
# prebuilt-package fingerprinting exist only for the Windows/mingw toolchain (a precompiled
# aurora/third-party package, offline pinned dependencies) that this script does not build.
# Linux always builds aurora from source, letting its own CMake auto-detect Vulkan + vendor
# SDL3/Dawn via FetchContent - the same configuration already verified working by hand.
set -euo pipefail

# ---------------------------------------------------------------------------
# Helpers
# ---------------------------------------------------------------------------

log_step() {
    # $1 = machine-readable step id, $2 = human sentence. Mirrors LocalBuild.ps1's
    # Write-MkwBuildStep: the id is a stable marker a future installer could parse from the log,
    # the sentence is for the human reading the terminal.
    printf 'MKWCBUILD:STEP:%s %s\n' "$1" "$2"
}

fail() {
    echo "local-build.sh: error: $*" >&2
    exit 1
}

assert_file() {
    [[ -f "$1" ]] || fail "$2 is missing: $1"
}

assert_dir() {
    [[ -d "$1" ]] || fail "$2 is missing: $1"
}

require_command() {
    command -v "$1" >/dev/null 2>&1 || fail "required tool '$1' was not found on PATH (override with --$2)"
}

sha256_of() {
    sha256sum "$1" | awk '{print $1}'
}

# ---------------------------------------------------------------------------
# Argument parsing
# ---------------------------------------------------------------------------

script_dir=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
workspace=$(cd "$script_dir/.." && pwd)
profile=base
output_dir=""
base_output_dir=""
retro_rewind_package_dir=""
retro_wfc_offline_dir=""
skip_retro_wfc_payload=0
force_clean_build=0
parallel_override=0
cc_override=""
cxx_override=""
cmake_override=""
ninja_override=""
dotnet_override=""
translator_dll_override=""
translator_bin_override=""
fuse_ld_override=""

usage() {
    cat <<'EOF'
Usage: local-build.sh --output-dir DIR [options]

  --workspace DIR                 Repository root (default: this script's parent directory)
  --profile {base|retro-rewind|both}   Build profile (default: base)
  --output-dir DIR                Where the built product is published (required)
  --base-output-dir DIR           Second output directory; required with --profile both
  --retro-rewind-package-dir DIR  Retro Rewind source tree (default: PulsarPacks/completed/RetroRewind/RetroRewind6)
  --retro-wfc-offline-dir DIR     Offline Retro-WFC payload directory
  --skip-retro-wfc-payload        Build Retro Rewind without a Retro-WFC payload
  --force-clean-build             Discard every translation/build cache first
  --parallel N                    Pin translator threads, translated-shard job pool, and Ninja parallelism to N
  --cc PATH / --cxx PATH          C/C++ compiler (default: cc/c++ on PATH)
  --fuse-ld NAME_OR_PATH          Linker passed to clang as -fuse-ld=NAME_OR_PATH (default: clang's own default linker)
  --cmake PATH / --ninja PATH     Build tools (default: on PATH)
  --dotnet PATH                   dotnet executable (default: on PATH)
  --translator-dll PATH           Pre-built Translator.Cli.dll (skips building the translator; still needs --dotnet to run it)
  --translator-bin PATH           Self-contained Translator.Cli executable (skips building AND needs no dotnet at all)
EOF
}

while [[ $# -gt 0 ]]; do
    case "$1" in
        --workspace) workspace=$(cd "$2" && pwd); shift 2 ;;
        --profile) profile=$2; shift 2 ;;
        --output-dir) output_dir=$2; shift 2 ;;
        --base-output-dir) base_output_dir=$2; shift 2 ;;
        --retro-rewind-package-dir) retro_rewind_package_dir=$2; shift 2 ;;
        --retro-wfc-offline-dir) retro_wfc_offline_dir=$2; shift 2 ;;
        --skip-retro-wfc-payload) skip_retro_wfc_payload=1; shift ;;
        --force-clean-build) force_clean_build=1; shift ;;
        --parallel) parallel_override=$2; shift 2 ;;
        --cc) cc_override=$2; shift 2 ;;
        --cxx) cxx_override=$2; shift 2 ;;
        --fuse-ld) fuse_ld_override=$2; shift 2 ;;
        --cmake) cmake_override=$2; shift 2 ;;
        --ninja) ninja_override=$2; shift 2 ;;
        --dotnet) dotnet_override=$2; shift 2 ;;
        --translator-dll) translator_dll_override=$2; shift 2 ;;
        --translator-bin) translator_bin_override=$2; shift 2 ;;
        -h|--help) usage; exit 0 ;;
        *) fail "unknown argument: $1" ;;
    esac
done

[[ -n "$output_dir" ]] || { usage; fail "--output-dir is required"; }
case "$profile" in
    base|retro-rewind|both) ;;
    *) fail "--profile must be base, retro-rewind, or both" ;;
esac

builds_retro=0
[[ "$profile" == "retro-rewind" || "$profile" == "both" ]] && builds_retro=1
has_offline_retro_wfc=0
[[ -n "$retro_wfc_offline_dir" ]] && has_offline_retro_wfc=1

if [[ "$builds_retro" -eq 0 ]]; then
    if [[ "$has_offline_retro_wfc" -eq 1 || "$skip_retro_wfc_payload" -eq 1 ]]; then
        fail "Retro-WFC payload options are valid only for a Retro Rewind build."
    fi
    if [[ -n "$retro_rewind_package_dir" ]]; then
        fail "--retro-rewind-package-dir is valid only for a Retro Rewind build."
    fi
else
    if [[ "$has_offline_retro_wfc" -eq "$skip_retro_wfc_payload" ]]; then
        fail "Choose exactly one Retro-WFC mode: --retro-wfc-offline-dir or --skip-retro-wfc-payload."
    fi
fi
if [[ "$profile" == "both" && -z "$base_output_dir" ]]; then
    fail "--base-output-dir is required with --profile both; --output-dir receives the Retro Rewind product."
fi
if [[ "$profile" != "both" && -n "$base_output_dir" ]]; then
    fail "--base-output-dir is valid only with --profile both."
fi

# ---------------------------------------------------------------------------
# Tool resolution and prerequisite checks
# ---------------------------------------------------------------------------

dotnet_bin=${dotnet_override:-dotnet}
cmake_bin=${cmake_override:-cmake}
ninja_bin=${ninja_override:-ninja}
cc_bin=${cc_override:-clang}
cxx_bin=${cxx_override:-clang++}

# A self-contained --translator-bin needs no dotnet at all (it bundles its own runtime); dotnet is
# only required when the translator has to be built from source or run as a plain .dll.
if [[ -z "$translator_bin_override" ]]; then
    require_command "$dotnet_bin" dotnet
fi
require_command "$cmake_bin" cmake
require_command "$ninja_bin" ninja
require_command "$cc_bin" cc
require_command "$cxx_bin" cxx

project=$workspace/projects/mkwii/recomp.yml
assets=$workspace/Assets
generated=$workspace/generated
functions=$generated/functions
base_metadata=$generated/base_translation_output.json
base_manifest_dir=$workspace/build/base
base_manifest=$base_manifest_dir/mkwii_base_manifest.json
shards=$generated/build_shards
build=$workspace/native-build
translation_provenance=$generated/translation-provenance.json
toolchain_provenance=$build/toolchain-provenance.json
retro_root=${retro_rewind_package_dir:-$workspace/PulsarPacks/completed/RetroRewind/RetroRewind6}

assert_file "$project" "Translation project"
assert_file "$assets/main.dol" "Extracted main.dol (see translator/README.md - owning the game is required)"
assert_file "$assets/StaticR.rel" "Extracted StaticR.rel (see translator/README.md - owning the game is required)"

# Literal line matching against the manifest's fixed shape, not a YAML dependency - the same
# approach NativeBuildFlags.ps1's Get-MkwProjectPins uses on Windows, kept here only for the one
# field this script actually needs from the manifest.
entry_point=$(awk '
    /^translation:/ { in_translation = 1 }
    in_translation && /^[[:space:]]*-[[:space:]]*0[xX][0-9a-fA-F]+[[:space:]]*$/ {
        gsub(/^[[:space:]]*-[[:space:]]*/, ""); gsub(/[[:space:]]*$/, ""); print; exit
    }
' "$project")
[[ -n "$entry_point" ]] || fail "Could not find a translation entry point in $project"

translator_bin=$translator_bin_override
translator_dll=$translator_dll_override
if [[ -n "$translator_bin" ]]; then
    assert_file "$translator_bin" "Translator.Cli executable"
    translator() { "$translator_bin" "$@"; }
else
    if [[ -z "$translator_dll" ]]; then
        translator_dll=$workspace/translator/src/Translator.Cli/bin/Release/net8.0/Translator.Cli.dll
        log_step build-translator "Building the translator"
        "$dotnet_bin" build "$workspace/translator/src/Translator.Cli/Translator.Cli.csproj" -c Release
    fi
    assert_file "$translator_dll" "Translator.Cli.dll"
    translator() { "$dotnet_bin" "$translator_dll" "$@"; }
fi

# ---------------------------------------------------------------------------
# Parallelism: three independent knobs, same reasoning as LocalBuild.ps1 -
# translator_threads (translation's own worker threads), translated_jobs (the real RAM guard,
# capping concurrent compiles of memory-hungry translated TUs via Ninja's MKW_TRANSLATED_COMPILE_JOBS
# pool), global_jobs (Ninja's overall parallelism). --parallel pins all three.
# ---------------------------------------------------------------------------

cpu_count=$(nproc)
mem_gib=$(( $(awk '/^MemTotal:/{print $2}' /proc/meminfo) / 1024 / 1024 ))
(( mem_gib < 1 )) && mem_gib=1

if (( parallel_override > 0 )); then
    translator_threads=$parallel_override
    translated_jobs=$parallel_override
    global_jobs=$parallel_override
else
    translator_threads=$(( cpu_count < 16 ? cpu_count : 16 ))
    (( translator_threads < 1 )) && translator_threads=1
    mem_based_cap=$(( mem_gib / 2 ))
    (( mem_based_cap < 1 )) && mem_based_cap=1
    translated_jobs=$(( cpu_count < mem_based_cap ? cpu_count : mem_based_cap ))
    (( translated_jobs < 1 )) && translated_jobs=1
    global_jobs=$(( translated_jobs > cpu_count ? translated_jobs : cpu_count ))
fi

# ---------------------------------------------------------------------------
# Translation cache: this script is the only owner of the reuse decision (unlike LocalBuild.ps1,
# which is handed caller-computed fingerprints by the Windows installer - there is no Linux
# installer yet to supply anything). Hash the game inputs the translation actually depends on;
# a match plus every expected output file present means the previous translation is still good.
# ---------------------------------------------------------------------------

if (( force_clean_build )); then
    log_step force-clean "A clean build was requested; discarding every translation and build cache"
    rm -rf "$generated" "$base_manifest_dir" "$build"
fi

translation_fingerprint=$(cat "$assets/main.dol" "$assets/StaticR.rel" "$project" | sha256sum | awk '{print $1}')
reuse_base=0
if [[ -f "$translation_provenance" ]]; then
    recorded=$(grep -o '"TranslationFingerprint" *: *"[^"]*"' "$translation_provenance" 2>/dev/null | sed 's/.*"\([0-9a-f]*\)"$/\1/' || true)
    if [[ "$recorded" == "$translation_fingerprint" && -f "$base_metadata" && -f "$base_manifest" ]]; then
        reuse_base=1
    fi
fi

if (( builds_retro )); then
    # The translator discovers the mod through the project file's workspace-relative profile
    # paths, and both the base and mod leg block leaf inlining at every address the profile
    # patches - so the selected Code.pul must sit at the profile's mod_root before either leg runs.
    source_pul=$retro_root/Binaries/Code.pul
    assert_file "$source_pul" "Retro Rewind Code.pul"
    staged_binaries=$workspace/PulsarPacks/completed/RetroRewind/RetroRewind6/Binaries
    mkdir -p "$staged_binaries"
    staged_pul=$staged_binaries/Code.pul
    if [[ "$(cd "$(dirname "$source_pul")" && pwd)/$(basename "$source_pul")" != "$(cd "$(dirname "$staged_pul")" && pwd)/$(basename "$staged_pul")" ]]; then
        cp -f "$source_pul" "$staged_pul"
    fi
fi

if (( reuse_base )) && (( builds_retro )); then
    # A base tree that never saw this Code.pul would silently bake vanilla code into the modded
    # product - check-base-mod-awareness fails closed (anything but exit 0 forces a retranslation).
    retro_code_pul=$retro_root/Binaries/Code.pul
    assert_file "$retro_code_pul" "Retro Rewind Code.pul"
    pul_sha=$(sha256_of "$retro_code_pul")
    if ! grep -q "\"codePulSha256\":\"$pul_sha\"" "$base_metadata"; then
        if ! translator check-base-mod-awareness --project "$project" --profile retro-rewind \
            --translation-output-metadata "$base_metadata" --code-pul "$retro_code_pul"; then
            log_step retranslate-base "The base translation is stale; retranslating the base game for the new Code.pul"
            reuse_base=0
        fi
    fi
fi

if (( reuse_base )); then
    log_step reuse-base-translation "Reusing the completed base translation"
else
    rm -f "$translation_provenance"
    mkdir -p "$generated" "$base_manifest_dir"

    log_step translate-base "Translating the user-owned base game"
    translator translate-recursive "$entry_point" --project "$project" \
        --outdir "$functions" --output-metadata "$base_metadata" \
        --production-source-bundle "$generated/base_translation_sources.bin" \
        --no-function-files --prune-stale --threads "$translator_threads"

    log_step emit-base-manifest "Creating the local base translation manifest"
    translator emit-base-manifest --project "$project" --out "$base_manifest_dir" \
        --functions-dir "$functions" --translation-output-metadata "$base_metadata" --region P

    printf '{"SchemaVersion":1,"TranslationFingerprint":"%s"}' "$translation_fingerprint" \
        > "$translation_provenance"
fi

if (( builds_retro )); then
    code_pul=$retro_root/Binaries/Code.pul
    assert_file "$code_pul" "Retro Rewind Code.pul"
    retro_out=$workspace/build/mods/retro_rewind_full_cpp
    translate_mod_args=(translate-mod --project "$project" --profile retro-rewind
        --base-manifest "$base_manifest" --base-translation-output-metadata "$base_metadata"
        --code-pul "$code_pul" --mod-root "$retro_root" --mod-name "Retro Rewind"
        --region P --out "$retro_out" --prefer-cached-inputs --emit-cpp
        --threads "$translator_threads")
    if (( skip_retro_wfc_payload )); then
        translate_mod_args+=(--skip-retro-wfc)
    else
        offline_payload=$retro_wfc_offline_dir/binary/payload.RMCPD00.bin
        assert_file "$offline_payload" "Offline Retro-WFC shared payload"
        translate_mod_args+=(--retro-wfc-payload "$offline_payload")
    fi
    log_step translate-mod "Translating the selected Retro Rewind Code.pul"
    translator "${translate_mod_args[@]}"
fi

log_step generate-data-init "Generating local game data initialization"
translator generate-data-init --project "$project"

shard_args=(emit-build-shards --project "$project" --base-metadata "$base_metadata"
    --base-functions-dir "$functions" --native-source-dir "$workspace/runtime/src" --out "$shards")
if (( builds_retro )); then
    retro_out=$workspace/build/mods/retro_rewind_full_cpp
    shard_args+=(--resolved-profile "$retro_out/resolved_dispatch_profile.json"
        --retro-cpp-dir "$retro_out/cpp")
fi
log_step emit-build-shards "Preparing local native build shards"
translator "${shard_args[@]}"

# ---------------------------------------------------------------------------
# Native configure + build. Deliberately not passing -DAURORA_DAWN_PROVIDER=package or
# -DFETCHCONTENT_FULLY_DISCONNECTED=ON: those exist for the Windows prebuilt-package/offline-
# dependencies workflow this script does not build. aurora's own CMake auto-detects Linux and
# picks Vulkan + vendors SDL3/Dawn via FetchContent, exactly as already verified working by hand.
# ---------------------------------------------------------------------------

keep_native_build=0
if [[ -f "$build/CMakeCache.txt" ]]; then
    expected_home=$workspace/runtime
    cache_home=$(grep '^CMAKE_HOME_DIRECTORY:INTERNAL=' "$build/CMakeCache.txt" | cut -d= -f2- || true)
    if [[ -n "$cache_home" && "$(cd "$cache_home" 2>/dev/null && pwd)" == "$expected_home" ]]; then
        keep_native_build=1
    fi
fi
if [[ -d "$build" && "$keep_native_build" -eq 0 ]]; then
    echo "MKWCBUILD: The native build cache does not belong to this workspace path; rebuilding from scratch"
    rm -rf "$build"
elif [[ "$keep_native_build" -eq 1 ]]; then
    echo "MKWCBUILD: Reusing the incremental native build directory"
fi

configure_args=(-S "$workspace/runtime" -B "$build" -G Ninja
    -DCMAKE_BUILD_TYPE=Release
    -DCMAKE_C_COMPILER="$cc_bin" -DCMAKE_CXX_COMPILER="$cxx_bin"
    -DCMAKE_MAKE_PROGRAM="$ninja_bin"
    -DMKW_TRANSLATED_COMPILE_JOBS="$translated_jobs")
if [[ -n "$fuse_ld_override" ]]; then
    configure_args+=(-DCMAKE_EXE_LINKER_FLAGS="-fuse-ld=$fuse_ld_override")
fi

log_step configure-native "Configuring the native toolchain"
"$cmake_bin" "${configure_args[@]}"

case "$profile" in
    base) targets=(WiiCompiled) ;;
    retro-rewind) targets=(RetroRewind) ;;
    both) targets=(WiiCompiled RetroRewind) ;;
esac
build_args=(--build "$build")
for target in "${targets[@]}"; do build_args+=(--target "$target"); done
build_args+=(--parallel "$global_jobs")
log_step compile "Compiling ${targets[*]} locally"
"$cmake_bin" "${build_args[@]}"

# ---------------------------------------------------------------------------
# Publish: the Linux build statically links SDL3/Dawn/etc (verified this session), so unlike
# LocalBuild.ps1's DLL-copying dance there is nothing to copy beside the binary except the
# runtime's own first-run assets.
# ---------------------------------------------------------------------------

dol_sha=$(sha256_of "$assets/main.dol")
rel_sha=$(sha256_of "$assets/StaticR.rel")
compiler_version=$("$cxx_bin" --version | head -1)

publish_built_product() {
    local target=$1 destination=$2 provenance_profile=$3
    mkdir -p "$destination"
    local exe=$build/$target
    assert_file "$exe" "Locally compiled game executable"
    cp -f "$exe" "$destination/$target"
    for name in dsp_coef.bin initial_pipeline_cache.db; do
        [[ -f "$build/$name" ]] && cp -f "$build/$name" "$destination/"
    done
    [[ -d "$build/wii_bootstrap" ]] && cp -rf "$build/wii_bootstrap" "$destination/"

    local is_retro=0 code_pul_sha=null
    if [[ "$provenance_profile" == "retro-rewind" ]]; then
        is_retro=1
        code_pul_sha=\"$(sha256_of "$retro_root/Binaries/Code.pul")\"
    fi
    local built_utc
    built_utc=$(date -u +%Y-%m-%dT%H:%M:%SZ)
    cat > "$destination/local-build.json" <<JSON
{
  "SchemaVersion": 1,
  "Profile": "$provenance_profile",
  "BuiltUtc": "$built_utc",
  "DolSha256": "$dol_sha",
  "RelSha256": "$rel_sha",
  "CodePulSha256": $code_pul_sha,
  "Compiler": "$compiler_version"
}
JSON
}

case "$profile" in
    both)
        publish_built_product WiiCompiled "$base_output_dir" base
        publish_built_product RetroRewind "$output_dir" retro-rewind
        ;;
    retro-rewind)
        publish_built_product RetroRewind "$output_dir" retro-rewind
        ;;
    base)
        publish_built_product WiiCompiled "$output_dir" base
        ;;
esac

echo "MKWCBUILD:OUTPUT=$output_dir"
