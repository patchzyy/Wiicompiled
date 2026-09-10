#!/usr/bin/env bash
# Builds the redistributable precompiled aurora + third-party package for native Linux: aurora
# (~43% of local build CPU time per Prepare-NativePrebuilt.ps1) and vendored Crypto++ are identical
# for every user under the pinned toolchain prepare-portable-tools.sh bundles, so this configures
# runtime/ against that toolchain, builds just that closure, and harvests the archives plus a
# generated CMake description into an output package - the Linux counterpart to
# Launcher/Prepare-NativePrebuilt.ps1, consumed by the same platform-agnostic
# runtime/cmake/NativePrebuilt.cmake either script's package works with unmodified.
#
# Not a byte-for-byte port of the Windows script: Linux needs no offline pinned dependency cache.
# Every FetchContent dependency in aurora-main/extern/CMakeLists.txt is a fixed-version URL already
# (verified directly), and aurora-main/CMakeLists.txt's own _default_linkage is "static" on any
# non-Windows platform. Windows pins Launcher/artifacts/dependencies because its installer ships
# pre-fetched sources to end users for a fully offline build; this harvest only ever runs on a
# maintainer's own machine (network access needed once, here, not shipped to anyone), so a plain
# networked configure is exactly as reproducible - what actually makes the harvested archives safe
# to link into any consumer's own build is the pinned compiler below, not an offline source cache.
set -euo pipefail

script_dir=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
workspace=$(cd "$script_dir/.." && pwd)

arch=""
output_dir=""
stage_dir="$workspace/build/native-prebuilt-stage"
keep_stage=0
reuse_stage=0
parallel=0
print_fingerprint_only=0

usage() {
    cat <<'EOF'
Usage: Prepare-NativePrebuilt.sh --arch {x86_64|aarch64} [options]

  --arch ARCH             Target architecture; selects the matching bundled toolchain (required)
  --output-dir DIR        Where the package is written (default: Launcher/artifacts/native-prebuilt-ARCH)
  --stage-dir DIR         Staging build directory (default: build/native-prebuilt-stage)
  --keep-stage            Do not delete the staging build directory afterward
  --reuse-stage           Reuse an existing staging build directory (maintainer iteration aid: a
                          re-harvest does not recompile aurora from scratch)
  --parallel N            Ninja build parallelism (default: nproc)
  --print-fingerprint-only  Print the four provenance inputs (compiler_sha256, flag_fingerprint,
                          aurora_fingerprint, third_party_fingerprint) as "key=value" lines and
                          exit, without configuring/building/harvesting anything - lets a caller
                          (build-appimage.sh) decide whether an existing package is still current
                          without paying for a full aurora rebuild just to find out.
EOF
}

while [[ $# -gt 0 ]]; do
    case "$1" in
        --arch) arch=$2; shift 2 ;;
        --output-dir) output_dir=$2; shift 2 ;;
        --stage-dir) stage_dir=$2; shift 2 ;;
        --keep-stage) keep_stage=1; shift ;;
        --reuse-stage) reuse_stage=1; shift ;;
        --parallel) parallel=$2; shift 2 ;;
        --print-fingerprint-only) print_fingerprint_only=1; shift ;;
        -h|--help) usage; exit 0 ;;
        *) echo "Prepare-NativePrebuilt.sh: unknown argument: $1" >&2; exit 1 ;;
    esac
done

case "$arch" in
    x86_64|aarch64) ;;
    *) echo "Prepare-NativePrebuilt.sh: --arch must be x86_64 or aarch64" >&2; usage; exit 1 ;;
esac

fail() { echo "Prepare-NativePrebuilt.sh: error: $*" >&2; exit 1; }
assert_file() { [[ -f "$1" ]] || fail "$2 is missing: $1"; }
assert_dir() { [[ -d "$1" ]] || fail "$2 is missing: $1"; }
sha256_of() { sha256sum "$1" | awk '{print $1}'; }
normalize() { readlink -f "$1"; }

[[ -n "$output_dir" ]] || output_dir="$script_dir/artifacts/native-prebuilt-$arch"
[[ "$stage_dir" = /* ]] || stage_dir="$workspace/$stage_dir"

toolchain_dir="$script_dir/artifacts/portable-tools/toolchain-$arch"
cc="$toolchain_dir/bin/clang"
cxx="$toolchain_dir/bin/clang++"
cmake_bin="$toolchain_dir/bin/cmake"
ninja_bin="$toolchain_dir/bin/ninja"
runtime_source="$workspace/runtime"
aurora_source="$workspace/aurora-main"

assert_file "$cmake_bin" "Portable CMake (run prepare-portable-tools.sh --arch $arch first)"
assert_file "$ninja_bin" "Portable Ninja"
assert_file "$cc" "Portable C compiler"
assert_file "$cxx" "Portable C++ compiler"
assert_dir "$aurora_source" "aurora-main source tree"
clang_binary=$(normalize "$toolchain_dir/bin/clang-22")
assert_file "$clang_binary" "Portable clang driver binary"

(( parallel > 0 )) || parallel=$(nproc)

fingerprint_tree() {
    # $1 = root dir, remaining args = top-level subdirectory names to exclude
    local root=$1; shift
    root=$(normalize "$root")
    [[ -d "$root" ]] || return 0
    local find_args=("$root")
    local ex
    for ex in "$@"; do find_args+=(-path "$root/$ex" -prune -o); done
    find_args+=(-type f -print)
    find "${find_args[@]}" | LC_ALL=C sort | while IFS= read -r abs; do
        printf '%s %s\n' "${abs#$root/}" "$(sha256_of "$abs")"
    done | sha256sum | awk '{print $1}'
}

# The fixed (path-independent) half of the configure command line - identical for every
# harvest, which is what lets one package be valid regardless of where it was built.
# -DBUILD_SHARED_LIBS=OFF: aurora-main/extern/CMakeLists.txt derives its own _USE_SHARED from
# whether BUILD_SHARED_LIBS is *defined at all* (not its value), so leaving it unset would default
# zlib/libpng to shared. -DAURORA_SDL3_PROVIDER=vendor: without it, "auto" resolves to "system" on
# any machine that happens to have an SDL3 dev package installed. -DCMAKE_DISABLE_FIND_PACKAGE_*:
# absl/PNG/Freetype each call find_package() unconditionally, with no provider flag to gate them
# (unlike SDL3/Dawn) - verified directly that this machine's system libpng-dev/freetype-dev get
# linked in shared instead of the pinned vendored source otherwise. Disabling find_package for
# these three forces the same FetchContent-vendored, statically-built result regardless of what a
# given maintainer's machine happens to have installed. ZLIB is deliberately NOT disabled the same
# way: libpng's own vendored CMakeLists.txt calls find_package(ZLIB REQUIRED) internally even when
# zlib came from aurora's own FetchContent (extern/CMakeLists.txt writes a redirect config for
# exactly this), and CMAKE_DISABLE_FIND_PACKAGE_ZLIB errors out on any REQUIRED call site outright
# (verified directly) - it cannot be scoped to only aurora's own initial, non-required check.
# Dawn stays a prebuilt package regardless (Linux x86_64/aarch64 always auto-resolve to "package" -
# see AuroraDawnProvider.cmake).
fixed_configure_flags=(
    -DCMAKE_BUILD_TYPE=Release
    -DBUILD_SHARED_LIBS=OFF
    -DAURORA_SDL3_PROVIDER=vendor
    -DCMAKE_DISABLE_FIND_PACKAGE_absl=ON
    -DCMAKE_DISABLE_FIND_PACKAGE_PNG=ON
    -DCMAKE_DISABLE_FIND_PACKAGE_Freetype=ON
    # Freetype's own vendored CMakeLists.txt separately probes for system BZip2 (optional
    # bzip2-compressed-font support aurora-main never asked for) regardless of the Freetype
    # find_package disable above, since that only stops aurora's own outer find_package(Freetype)
    # from picking up a system Freetype - it does not reach into the FetchContent-built copy's own
    # internal find_package(BZip2) call. FT_DISABLE_BZIP2 is Freetype's own documented flag for
    # exactly this (verified in its CMakeLists.txt), unlike the ZLIB/libpng situation where no such
    # source-level flag exists.
    -DFT_DISABLE_BZIP2=ON
    -DCMAKE_POLICY_DEFAULT_CMP0168=NEW
)
flag_fingerprint=$(printf '%s\n' "${fixed_configure_flags[@]}" | sha256sum | awk '{print $1}')

# extern/ is excluded because the payload ships that tree separately (aurora-main/extern is bundled
# whole by build-appimage.sh); build/ is a plain developer build directory.
aurora_fingerprint=$(fingerprint_tree "$aurora_source" extern build)
[[ -n "$aurora_fingerprint" ]] || fail "The aurora source tree could not be fingerprinted: $aurora_source"

# The harvested Crypto++ archive is consumed against this tree's headers, so it is fingerprinted
# for the same reason as aurora above. No exclusions: unlike aurora's extern/, nothing under
# runtime/third_party is shipped separately.
third_party_fingerprint=$(fingerprint_tree "$runtime_source/third_party")
[[ -n "$third_party_fingerprint" ]] || fail "The vendored third-party tree could not be fingerprinted: $runtime_source/third_party"

compiler_sha256=$(sha256_of "$clang_binary")

if [[ "$print_fingerprint_only" -eq 1 ]]; then
    printf 'compiler_sha256=%s\n' "$compiler_sha256"
    printf 'flag_fingerprint=%s\n' "$flag_fingerprint"
    printf 'aurora_fingerprint=%s\n' "$aurora_fingerprint"
    printf 'third_party_fingerprint=%s\n' "$third_party_fingerprint"
    exit 0
fi

# The package must never contain a stale mixture of two builds.
rm -rf "$output_dir"
mkdir -p "$output_dir/lib" "$output_dir/bin" "$output_dir/include"

# The staging build has to be configured without the package present, otherwise the runtime would
# consume the very package this script is producing.
if [[ "$reuse_stage" -eq 0 ]]; then rm -rf "$stage_dir"; fi
export_dir="$stage_dir/export"
mkdir -p "$export_dir"

# fixed_configure_flags/flag_fingerprint were already computed above (needed before the
# --print-fingerprint-only early exit).
echo "Prepare-NativePrebuilt.sh: configuring the aurora/third-party staging build..."
"$cmake_bin" -S "$runtime_source" -B "$stage_dir" -G Ninja \
    "${fixed_configure_flags[@]}" \
    -DCMAKE_C_COMPILER="$cc" -DCMAKE_CXX_COMPILER="$cxx" \
    -DCMAKE_MAKE_PROGRAM="$ninja_bin" \
    -DCMAKE_EXE_LINKER_FLAGS="-fuse-ld=lld" \
    -DMKW_NATIVE_PREBUILT_EXPORT_DIR="$export_dir"

# mkw_np_probe links exactly what mkw_runtime_common and the public products link, so building it
# compiles the whole redistributable closure and nothing else - and a successful link proves the
# harvested archives are complete.
echo "Prepare-NativePrebuilt.sh: building the aurora/third-party closure..."
"$cmake_bin" --build "$stage_dir" --target mkw_np_probe --parallel "$parallel"

# ---------------------------------------------------------------------------
# Read the description CMake wrote, plus the resolved command lines Ninja holds.
# ---------------------------------------------------------------------------

meta_path="$export_dir/meta.txt"
assert_file "$meta_path" "Native prebuilt export metadata"
get_meta() { awk -F= -v k="$1" '$0 ~ "^" k "=" { sub(/^[^=]*=/, ""); print; exit }' "$meta_path"; }

build_ninja="$stage_dir/build.ninja"
assert_file "$build_ninja" "Generated build.ninja"

extract_ninja_vars() {
    # $1 = ninja file, $2 = exact rule name. Prints "KEY\tVALUE" for every "  KEY = VALUE" line
    # following the first "build ...: RULE ..." statement that uses that rule.
    awk -v rule="$2" '
        BEGIN { in_stmt = 0 }
        /^build / {
            if (in_stmt) exit
            colon = index($0, ": ")
            if (colon == 0) next
            rest = substr($0, colon + 2)
            sp = index(rest, " ")
            stmt_rule = (sp > 0) ? substr(rest, 1, sp - 1) : rest
            if (stmt_rule == rule) in_stmt = 1
            next
        }
        in_stmt {
            if (substr($0, 1, 2) != "  ") exit
            line = substr($0, 3)
            eq = index(line, " = ")
            if (eq == 0) exit
            print substr(line, 1, eq - 1) "\t" substr(line, eq + 3)
        }
    ' "$1"
}

declare -A probe_compile=() control_compile=() probe_link=() control_link=()
load_vars() {
    local -n dest=$1
    local k v
    while IFS=$'\t' read -r k v; do dest["$k"]=$v; done < <(extract_ninja_vars "$build_ninja" "$2")
}
load_vars probe_compile   "CXX_COMPILER__mkw_np_probe_unscanned_Release"
load_vars control_compile "CXX_COMPILER__mkw_np_probe_control_unscanned_Release"
load_vars probe_link      "CXX_EXECUTABLE_LINKER__mkw_np_probe_Release"
load_vars control_link    "CXX_EXECUTABLE_LINKER__mkw_np_probe_control_Release"
[[ -n "${probe_compile[INCLUDES]+x}" ]] || fail "No Ninja statement used rule CXX_COMPILER__mkw_np_probe_unscanned_Release."
[[ -n "${probe_link[LINK_LIBRARIES]+x}" ]] || fail "No Ninja statement used rule CXX_EXECUTABLE_LINKER__mkw_np_probe_Release."

split_list() {
    # $1 = string value, $2 = name of the array to fill (nameref)
    local -n out=$2
    out=()
    [[ -n "$1" ]] || return 0
    read -ra out <<< "$1"
}

subtract_multiset() {
    # $1 = "all" array name, $2 = "remove" array name, $3 = result array name (all namerefs)
    local -n all_ref=$1 remove_ref=$2 result_ref=$3
    local -A pending=()
    local item
    for item in "${remove_ref[@]}"; do
        pending["$item"]=$(( ${pending["$item"]:-0} + 1 ))
    done
    result_ref=()
    for item in "${all_ref[@]}"; do
        if [[ "${pending["$item"]:-0}" -gt 0 ]]; then
            pending["$item"]=$(( pending["$item"] - 1 ))
            continue
        fi
        result_ref+=("$item")
    done
}

split_list "${probe_compile[INCLUDES]:-}" _probe_includes
split_list "${control_compile[INCLUDES]:-}" _control_includes
subtract_multiset _probe_includes _control_includes include_arguments

split_list "${probe_compile[DEFINES]:-}" _probe_defines
split_list "${control_compile[DEFINES]:-}" _control_defines
subtract_multiset _probe_defines _control_defines define_arguments

split_list "${probe_compile[FLAGS]:-}" _probe_flags
split_list "${control_compile[FLAGS]:-}" _control_flags
subtract_multiset _probe_flags _control_flags compile_options_raw

split_list "${probe_link[LINK_LIBRARIES]:-}" _probe_link
split_list "${control_link[LINK_LIBRARIES]:-}" _control_link
subtract_multiset _probe_link _control_link link_items

[[ ${#include_arguments[@]} -gt 0 ]] || fail "The aurora compile interface is empty; the probe did not link aurora."
[[ ${#link_items[@]} -gt 0 ]] || fail "The aurora link interface is empty; the probe did not link aurora."

# ---------------------------------------------------------------------------
# Harvest the built libraries.
# ---------------------------------------------------------------------------

binary_root=$(normalize "$stage_dir")
aurora_root=$(normalize "$(get_meta aurora_dir)")
runtime_root=$(normalize "$(get_meta runtime_dir)")

generated_include_index=0
copy_harvested() {
    # $1 = source path, $2 = subdirectory under the package. Echoes "subdir/name".
    local source=$1 subdir=$2
    local name destination
    name=$(basename "$source")
    destination="$output_dir/$subdir/$name"
    [[ ! -e "$destination" ]] || fail "Two harvested files collide on the name $name: $source"
    cp "$source" "$destination"
    printf '%s/%s' "$subdir" "$name"
}
declare -A generated_include_tokens=()
convert_to_token() {
    # $1 = absolute path, $2 = 'include' or another kind. Sets $TOKEN_RESULT - NOT echoed/command-
    # substituted: this needs to mutate generated_include_tokens/generated_include_index, and a
    # $(...) call runs in a subshell, silently discarding that mutation once it returns (verified
    # directly: every build-tree include directory collided into the same "generated01" until this
    # was written this way instead).
    local path kind=$2
    path=$(normalize "$1")
    case "$path" in
        "$aurora_root"/*) TOKEN_RESULT="@AURORA@/${path#$aurora_root/}"; return ;;
        "$runtime_root"/*) TOKEN_RESULT="@RUNTIME@/${path#$runtime_root/}"; return ;;
        "$binary_root"/*)
            [[ "$kind" == include ]] || fail "Unhandled build-tree artifact: $path"
            if [[ -n "${generated_include_tokens[$path]:-}" ]]; then
                TOKEN_RESULT=${generated_include_tokens[$path]}
                return
            fi
            generated_include_index=$((generated_include_index + 1))
            local name destination
            name=$(printf 'generated%02d' "$generated_include_index")
            destination="$output_dir/include/$name"
            mkdir -p "$destination"
            cp -a "$path/." "$destination/"
            TOKEN_RESULT="@PKG@/include/$name"
            generated_include_tokens[$path]=$TOKEN_RESULT
            return ;;
        *) fail "Path escapes every shippable root ($kind): $path" ;;
    esac
}

declare -A linker_file_to_reference=()
shared_import_targets=() shared_import_runtime=() shared_import_implib=()
targets_txt="$export_dir/targets.txt"
assert_file "$targets_txt" "Native prebuilt export targets list"
while IFS='|' read -r name type file linkerfile; do
    [[ -n "$name" ]] || continue
    [[ -f "$linkerfile" ]] || continue  # not part of the closure mkw_np_probe pulled in
    linkerfile=$(normalize "$linkerfile")
    relative_linker=$(copy_harvested "$linkerfile" lib)
    if [[ "$type" == "SHARED_LIBRARY" ]]; then
        file_abs=$(normalize "$file")
        # Unlike Windows (a .dll + a separate .dll.a import library), a Linux shared object's
        # TARGET_FILE and TARGET_LINKER_FILE are the same path - one file, harvested once.
        if [[ "$file_abs" == "$linkerfile" ]]; then
            relative_runtime=$relative_linker
        else
            [[ -f "$file_abs" ]] || fail "Shared library $name has no runtime file: $file_abs"
            relative_runtime=$(copy_harvested "$file_abs" bin)
        fi
        shared_import_targets+=("mkw_np::$name")
        shared_import_runtime+=("$relative_runtime")
        shared_import_implib+=("$relative_linker")
        linker_file_to_reference["$linkerfile"]="mkw_np::$name"
    else
        linker_file_to_reference["$linkerfile"]="@PKG@/$relative_linker"
    fi
done < "$targets_txt"
# Unlike Windows (SDL/zlib/libpng ship as DLLs by default), everything here was forced static above
# and Dawn's own Linux package (verified directly) ships libwebgpu_dawn.a, also static - so zero
# shared imports is the expected, normal outcome, not a failure.
echo "Prepare-NativePrebuilt.sh: harvested ${#linker_file_to_reference[@]} archive(s) (${#shared_import_targets[@]} shared)"

dawn_linker_file="" dawn_runtime_file=""
dawn_txt="$export_dir/dawn.txt"
if [[ -s "$dawn_txt" ]]; then
    IFS='|' read -r _ dawn_runtime_file dawn_linker_file < "$dawn_txt"
    dawn_runtime_file=$(normalize "$dawn_runtime_file")
    dawn_linker_file=$(normalize "$dawn_linker_file")
fi
[[ -n "$dawn_linker_file" ]] && linker_file_to_reference["$dawn_linker_file"]="dawn::webgpu_dawn"

# ---------------------------------------------------------------------------
# Rewrite the compile/link interface into workspace-relative tokens.
# ---------------------------------------------------------------------------

package_include_dirs=()
include_count=${#include_arguments[@]}
i=0
while (( i < include_count )); do
    entry=${include_arguments[i]}
    case "$entry" in
        -I*) convert_to_token "${entry#-I}" include; package_include_dirs+=("$TOKEN_RESULT") ;;
        -isystem)
            i=$((i + 1))
            (( i < include_count )) || fail "Trailing -isystem with no argument"
            convert_to_token "${include_arguments[i]}" include; package_include_dirs+=("$TOKEN_RESULT") ;;
        -isystem*) convert_to_token "${entry#-isystem}" include; package_include_dirs+=("$TOKEN_RESULT") ;;
        *) fail "Unexpected include argument: $entry" ;;
    esac
    i=$((i + 1))
done

package_definitions=()
for entry in "${define_arguments[@]}"; do
    case "$entry" in
        # Ninja records a quoted define's value with its quotes backslash-escaped (verified
        # directly: IMGUI_USER_CONFIG=\"aurora/imgui_config.h\" in build.ninja) regardless of
        # platform - format_cmake_block below re-escapes for CMake's own string syntax, so the
        # literal backslash has to come out here first or the result is double-escaped and the
        # define expands to literal backslash-quote characters instead of a quoted string.
        -D*) package_definitions+=("${entry#-D}") ;;
        *) fail "Unexpected define argument: $entry" ;;
    esac
done
package_definitions=("${package_definitions[@]//\\\"/\"}")

# fmt's `-include cstdlib` (and anything like it) is recorded from a C++ compile line, so re-scope
# it to C++. The runtime also assembles generated .S blobs through the same targets, and a forced
# C++ header include would break them.
package_compile_options=()
for entry in "${compile_options_raw[@]}"; do
    package_compile_options+=("\$<\$<COMPILE_LANGUAGE:CXX>:$entry>")
done

package_link_items=()
for item in "${link_items[@]}"; do
    case "$item" in
        -*) package_link_items+=("$item"); continue ;;
    esac
    if [[ "$item" = /* ]]; then item_abs=$(normalize "$item"); else item_abs=$(normalize "$stage_dir/$item"); fi
    ref=${linker_file_to_reference["$item_abs"]:-}
    if [[ -n "$ref" ]]; then
        package_link_items+=("$ref")
        continue
    fi
    case "$item_abs" in
        */libz.so*)
            # The one unavoidable system reference: libpng's own vendored CMakeLists.txt calls
            # find_package(ZLIB REQUIRED) internally even when zlib came from aurora's own
            # FetchContent (extern/CMakeLists.txt writes a redirect config for exactly this case),
            # and CMAKE_DISABLE_FIND_PACKAGE_ZLIB errors out on any REQUIRED call site outright
            # (verified directly), so it cannot be forced to vendor/static the way absl/PNG/Freetype
            # are above. zlib is as close to a universal baseline as a Linux shared library gets
            # (glibc-adjacent - practically every distro has it already), so this is recorded as a
            # portable `-lz` link flag instead of the harvesting machine's absolute path.
            package_link_items+=("-lz")
            continue ;;
    esac
    fail "Link item is neither a system library nor a harvested archive: $item"
done

# ---------------------------------------------------------------------------
# Emit the consumer-facing CMake description.
# ---------------------------------------------------------------------------

format_cmake_block() {
    # $1 = CMake variable name, remaining args = list items
    local name=$1; shift
    if [[ $# -eq 0 ]]; then
        printf 'set(%s "")\n' "$name"
        return
    fi
    printf 'set(%s\n' "$name"
    local item escaped
    for item in "$@"; do
        escaped=${item//\\/\\\\}
        escaped=${escaped//\"/\\\"}
        printf '    "%s"\n' "$escaped"
    done
    printf ')\n'
}

dawn_config_dir_meta=$(get_meta dawn_config_dir)
dawn_config_token=""
if [[ -n "$dawn_config_dir_meta" ]]; then
    dawn_config_dir_abs=$(normalize "$dawn_config_dir_meta")
    case "$dawn_config_dir_abs" in
        "$binary_root"/*)
            # Dawn's find_package() config directory (lib/cmake/Dawn under its own fetched package
            # root) cannot go through the generic single-directory copy other build-tree artifacts
            # use: DawnConfig.cmake/DawnTargets.cmake derive _IMPORT_PREFIX/PACKAGE_PREFIX_DIR from
            # their OWN file location by walking up three parent directories (verified directly), so
            # the whole package root - include/, lib/, lib/cmake/Dawn/ together - must be copied as
            # one self-contained unit for that relative navigation to still resolve once moved.
            dawn_package_root=$(normalize "$dawn_config_dir_abs/../../..")
            dawn_config_relative=${dawn_config_dir_abs#$dawn_package_root/}
            mkdir -p "$output_dir/include/dawn_package"
            cp -a "$dawn_package_root/." "$output_dir/include/dawn_package/"
            dawn_config_token="@PKG@/include/dawn_package/$dawn_config_relative"
            ;;
        *) convert_to_token "$dawn_config_dir_abs" include; dawn_config_token=$TOKEN_RESULT ;;
    esac
fi

generated_cmake="$output_dir/native_prebuilt.cmake"
{
    echo "# Generated by Launcher/Prepare-NativePrebuilt.sh - do not edit."
    echo "#"
    echo "# Describes the precompiled aurora + third-party archives that replace a"
    echo "# from-source aurora-main build. Every path is a token resolved against the"
    echo "# consuming workspace, because the source trees still ship next to this"
    echo "# package and the runtime compiles against their headers."
    echo ""
    echo 'if(NOT MKW_NP_PACKAGE_DIR OR NOT MKW_NP_AURORA_DIR OR NOT MKW_NP_RUNTIME_DIR OR NOT MKW_NP_DEPS_DIR)'
    echo '    message(FATAL_ERROR "native_prebuilt.cmake must be included by runtime/cmake/NativePrebuilt.cmake")'
    echo 'endif()'
    echo ""
    format_cmake_block MKW_NP_INCLUDE_DIRECTORIES "${package_include_dirs[@]}"
    format_cmake_block MKW_NP_COMPILE_DEFINITIONS "${package_definitions[@]}"
    format_cmake_block MKW_NP_COMPILE_OPTIONS "${package_compile_options[@]}"
    format_cmake_block MKW_NP_LINK_LIBRARIES "${package_link_items[@]}"
    format_cmake_block MKW_NP_AURORA_TARGETS "aurora::gx" "aurora::pad" "aurora::si" "aurora::vi" "aurora::mtx"
    echo ""
    printf 'set(MKW_NP_DAWN_CONFIG_DIR "%s")\n' "$dawn_config_token"
    printf 'set(MKW_NP_DAWN_VERSION "%s")\n' "$(get_meta aurora_dawn_version)"
    echo ""
    echo "# Shared third-party libraries keep imported SHARED targets so that"
    echo '# $<TARGET_RUNTIME_DLLS> still copies them next to the game executable.'
    for idx in "${!shared_import_targets[@]}"; do
        printf 'add_library(%s SHARED IMPORTED GLOBAL)\n' "${shared_import_targets[$idx]}"
        printf 'set_target_properties(%s PROPERTIES\n' "${shared_import_targets[$idx]}"
        printf '    IMPORTED_LOCATION "${MKW_NP_PACKAGE_DIR}/%s"\n' "${shared_import_runtime[$idx]}"
        printf '    IMPORTED_IMPLIB "${MKW_NP_PACKAGE_DIR}/%s")\n' "${shared_import_implib[$idx]}"
    done
} > "$generated_cmake"

# ---------------------------------------------------------------------------
# Provenance: the pinned toolchain is what makes a precompiled archive interchangeable with
# locally compiled objects (bit-for-bit the compiler and flag set the user's machine will use);
# LocalBuild.ps1's Windows equivalent re-checks both before consuming a package, and a Linux
# consumer-side check (once local-build.sh gains --native-prebuilt-dir) should do the same.
# compiler_sha256/flag_fingerprint/aurora_fingerprint/third_party_fingerprint were already computed
# above (needed before the --print-fingerprint-only early exit).
# ---------------------------------------------------------------------------

dawn_runtime_sha256=""
[[ -n "$dawn_runtime_file" && -f "$dawn_runtime_file" ]] && dawn_runtime_sha256=$(sha256_of "$dawn_runtime_file")
[[ -n "$dawn_runtime_sha256" ]] || fail "The Dawn runtime could not be hashed: $dawn_runtime_file"

compiler_version=$(get_meta cxx_compiler_version)
sdl3_target=$(get_meta aurora_sdl3_target)
dawn_version=$(get_meta aurora_dawn_version)
harvested_count=${#linker_file_to_reference[@]}

python3 - "$output_dir" "$compiler_sha256" "$compiler_version" "$flag_fingerprint" \
    "$dawn_version" "$dawn_runtime_sha256" "$aurora_fingerprint" "$third_party_fingerprint" \
    "$sdl3_target" "$harvested_count" <<'PY'
import hashlib, json, os, sys, datetime

(output_dir, compiler_sha256, compiler_version, flag_fingerprint, dawn_version,
 dawn_runtime_sha256, aurora_fingerprint, third_party_fingerprint, sdl3_target,
 harvested_count) = sys.argv[1:]

contents = []
for root, dirs, files in os.walk(output_dir):
    dirs.sort()
    for name in sorted(files):
        if name == "provenance.json":
            continue
        path = os.path.join(root, name)
        rel = os.path.relpath(path, output_dir).replace(os.sep, "/")
        with open(path, "rb") as fh:
            digest = hashlib.sha256(fh.read()).hexdigest()
        contents.append({"Path": rel, "Bytes": os.path.getsize(path), "Sha256": digest})
contents.sort(key=lambda c: c["Path"])

provenance = {
    "SchemaVersion": 1,
    "BuiltUtc": datetime.datetime.now(datetime.timezone.utc).strftime("%Y-%m-%dT%H:%M:%S.%f")[:-3] + "Z",
    "CompilerSha256": compiler_sha256,
    "CompilerVersion": compiler_version,
    "FlagFingerprint": flag_fingerprint,
    "DawnVersion": dawn_version,
    "DawnRuntimeSha256": dawn_runtime_sha256,
    "AuroraSourceFingerprint": aurora_fingerprint,
    "ThirdPartySourceFingerprint": third_party_fingerprint,
    "Sdl3Target": sdl3_target,
    "HarvestedLibraryCount": int(harvested_count),
    "Contents": contents,
}
with open(os.path.join(output_dir, "provenance.json"), "w", encoding="utf-8") as fh:
    json.dump(provenance, fh, indent=2)
    fh.write("\n")

total_bytes = sum(c["Bytes"] for c in contents)
print(f"  files: {len(contents)}; size: {total_bytes / (1024 * 1024):.1f} MiB")
PY

if [[ "$keep_stage" -eq 0 ]]; then rm -rf "$stage_dir"; fi

echo ""
echo "Native prebuilt package: $output_dir"
echo "  archives: $(find "$output_dir/lib" -maxdepth 1 -type f | wc -l); shared imports: ${#shared_import_targets[@]}"
echo "  flag fingerprint: $flag_fingerprint"
