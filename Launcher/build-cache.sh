# Shared incremental-build and release-selection logic for the Unix setups (Linux AppImage and
# macOS), sourced by local-build.sh, local-build-macos.command, macos/extract-disc.command, the
# AppImage's AppRun and macos/setup.command.
# Mirrors the Windows installer's translation/toolchain identities and timestamp preservation.
# Must stay compatible with macOS's bash 3.2.

if command -v sha256sum >/dev/null 2>&1; then
    mkw_sha256_cmd=(sha256sum)
else
    mkw_sha256_cmd=(shasum -a 256)
fi

mkw_sha256() {
    "${mkw_sha256_cmd[@]}" "$@" | awk '{print $1}'
}

# Prints "<sha>  <relative path>" for every file under each directory, relative to $1.
mkw_digest_tree() {
    local root=$1; shift
    (
        cd "$root" || exit 1
        local dir
        for dir in "$@"; do
            if [[ -d "$dir" ]]; then
                printf 'tree %s\n' "$dir"
                find "$dir" -type f -print0 | LC_ALL=C sort -z | xargs -0 "${mkw_sha256_cmd[@]}" /dev/null
            else
                printf 'absent %s\n' "$dir"
            fi
        done
    )
}

# Content-only digest of a file or directory, independent of where it lives (the AppImage mount
# path changes every launch).
mkw_digest_path() {
    if [[ -d "$1" ]]; then
        mkw_digest_tree "$1" . | mkw_sha256
    elif [[ -f "$1" ]]; then
        mkw_sha256 < "$1"
    else
        printf 'absent\n'
    fi
}

# Same digest as the translator's RuntimeNativeIndexFile.ComputeSourceDigest. $1 = runtime/src.
mkw_native_source_digest() {
    (
        cd "$1" || exit 1
        find . -type f \( -iname '*.cpp' -o -iname '*.h' -o -iname '*.hpp' \) -print0 | LC_ALL=C sort -z |
            xargs -0 "${mkw_sha256_cmd[@]}" /dev/null
    ) | grep -v '  /dev/null$' | sed 's|  \./|  |' | mkw_sha256
}

# The native registrations the translator reads from runtime/src. A current native-index.json
# stands in for the sources (minus its source digest), so runtime edits that leave the
# registrations alone keep the translation; a missing or stale index falls back to the sources.
# $1 = workspace.
mkw_native_index_identity() {
    local index="$1/runtime/native-index.json" recorded
    recorded=$(mkw_read_identity "$index" sourceDigest)
    if [[ -n "$recorded" && -d "$1/runtime/src" && "$recorded" == "$(mkw_native_source_digest "$1/runtime/src")" ]]; then
        printf 'native-index %s\n' "$(grep -v '^[[:space:]]*"sourceDigest"[[:space:]]*:' "$index" | mkw_sha256)"
    else
        mkw_digest_tree "$1" runtime/src
    fi
}

# Everything the base translation depends on: game inputs, project, native registrations, the
# translator itself and the build script that drives it.
# $1 = workspace, $2 = translator file or directory, $3 = build script.
mkw_translation_identity() {
    local workspace=$1 translator=$2 script=$3
    {
        printf 'mkw-translation-v2\n'
        printf 'dol %s\n' "$(mkw_digest_path "$workspace/Assets/main.dol")"
        printf 'rel %s\n' "$(mkw_digest_path "$workspace/Assets/StaticR.rel")"
        printf 'translator %s\n' "$(mkw_digest_path "$translator")"
        printf 'script %s\n' "$(mkw_digest_path "$script")"
        printf 'regions %s\n' "$(mkw_digest_path "$workspace/runtime/include/guest_abi/guest_versions.inc")"
        mkw_digest_tree "$workspace" projects
        mkw_native_index_identity "$workspace"
    } | mkw_sha256
}

# Inputs that make compiled objects unsafe to reuse: compiler/CMake/Ninja versions and the
# from-source dependency pins. Source changes are excluded; Ninja rebuilds exactly what they touch.
# $1 = workspace, remaining = tools (C compiler, C++ compiler, cmake, ninja).
mkw_toolchain_identity() {
    local workspace=$1; shift
    local tool
    {
        printf 'mkw-toolchain-v2\n'
        for tool in "$@"; do
            # First line only: clang appends its InstalledDir, which moves with the AppImage mount.
            printf '%s\n' "$("$tool" --version 2>/dev/null | head -n 1)"
        done
        mkw_digest_tree "$workspace" aurora-main/extern aurora-main/cmake
        printf 'aurora %s\n' "$(mkw_digest_path "$workspace/aurora-main/CMakeLists.txt")"
        printf 'mbedtls %s\n' "$(mkw_digest_path "$workspace/runtime/cmake/MbedTLSPin.cmake")"
    } | mkw_sha256
}

# Prints one fact a projects/*/recomp.yml pins: game_id, region, dol, rel or retro_wfc_payload.
# Literal line matching against the fixed shape, like NativeBuildFlags.ps1's Get-MkwProjectPins.
# $1 = recomp.yml, $2 = fact.
mkw_project_pin() {
    awk -v want="$2" '
        { sub(/\r$/, ""); sub(/#.*/, "") }
        /^[A-Za-z0-9_]+:/ { section = $1; sub(/:.*/, "", section); input = ""; next }
        section == "inputs" && /^  [A-Za-z0-9_]+:[[:space:]]*$/ { input = $1; sub(/:$/, "", input); next }
        section == "project" && $1 == (want ":") { print $2; exit }
        section == "inputs" && input == want && $1 == "sha256:" { print tolower($2); exit }
        want == "retro_wfc_payload" && $1 == "retro_wfc_payload:" { print $2; exit }
    ' "$1"
}

# Prints the recomp.yml of the supported release whose game id is $2. $1 = workspace.
mkw_project_for_game() {
    local project
    for project in "$1"/projects/*/recomp.yml; do
        if [[ -f "$project" && "$(mkw_project_pin "$project" game_id)" == "$2" ]]; then
            printf '%s\n' "$project"
            return 0
        fi
    done
    return 1
}

# Prints the recomp.yml pinned to $2/main.dol after checking StaticR.rel and any extracted disc
# header against it, like NativeBuildFlags.ps1's Resolve-MkwProjectForAssets.
# $1 = workspace, $2 = assets directory.
mkw_resolve_project() {
    local project dol_sha game_id disc_id
    dol_sha=$(mkw_sha256 "$2/main.dol")
    for project in "$1"/projects/*/recomp.yml; do
        [[ -f "$project" && "$(mkw_project_pin "$project" dol)" == "$dol_sha" ]] || continue
        game_id=$(mkw_project_pin "$project" game_id)
        if [[ "$(mkw_sha256 "$2/StaticR.rel")" != "$(mkw_project_pin "$project" rel)" ]]; then
            printf 'StaticR.rel does not match the clean %s release of main.dol.\n' "$game_id" >&2
            return 1
        fi
        if [[ -f "$2/DATA/sys/boot.bin" ]]; then
            disc_id=$(head -c 6 "$2/DATA/sys/boot.bin")
            if [[ "$disc_id" != "$game_id" ]]; then
                printf 'The extracted game data is %s but main.dol is %s; extract the game again.\n' "$disc_id" "$game_id" >&2
                return 1
            fi
        fi
        printf '%s\n' "$project"
        return 0
    done
    printf 'main.dol (sha256 %s) is not a supported clean Mario Kart Wii release.\n' "$dol_sha" >&2
    return 1
}

# The offline file name the translator expects for a release's Retro-WFC payload. $1 = recomp.yml.
mkw_retro_wfc_payload_file() {
    local uri
    uri=$(mkw_project_pin "$1" retro_wfc_payload)
    [[ -n "$uri" ]] || return 1
    printf 'binary/payload.%s.bin\n' "${uri##*g=}"
}

# $1 = JSON file, $2 = key. Prints the recorded string value, or nothing.
mkw_read_identity() {
    [[ -f "$1" ]] || return 0
    grep -o "\"$2\" *: *\"[^\"]*\"" "$1" 2>/dev/null | sed 's/.*"\([^"]*\)"$/\1/' || true
}

mkw_write_identity() {
    printf '{"SchemaVersion":1,"%s":"%s"}\n' "$2" "$3" > "$1"
}

# Mirrors each named directory from $1 into $2. Unchanged files are left untouched so their
# timestamps (which clang's PCH validation and Ninja compare) survive; changed and new files are
# copied fresh and files no longer shipped are removed.
mkw_sync_tree() {
    local source=$1 destination=$2; shift 2
    local dir relative changed=0 unchanged=0 removed=0
    for dir in "$@"; do
        if [[ ! -d "$source/$dir" ]]; then
            rm -rf "$destination/$dir"
            continue
        fi
        if [[ -e "$destination/$dir" && ! -d "$destination/$dir" ]]; then
            rm -f "$destination/$dir"
        fi
        mkdir -p "$destination/$dir"
        while IFS= read -r -d '' relative; do
            relative=${relative#./}
            if [[ -d "$source/$dir/$relative" ]]; then
                [[ -d "$destination/$dir/$relative" ]] || { rm -f "$destination/$dir/$relative"; mkdir -p "$destination/$dir/$relative"; }
            elif [[ -f "$destination/$dir/$relative" ]] && cmp -s "$source/$dir/$relative" "$destination/$dir/$relative"; then
                unchanged=$((unchanged + 1))
            else
                rm -rf "$destination/$dir/$relative"
                cp "$source/$dir/$relative" "$destination/$dir/$relative"
                changed=$((changed + 1))
            fi
        done < <(cd "$source/$dir" && find . -mindepth 1 -print0)
        while IFS= read -r -d '' relative; do
            relative=${relative#./}
            if [[ ! -e "$source/$dir/$relative" && ( -e "$destination/$dir/$relative" || -L "$destination/$dir/$relative" ) ]]; then
                [[ -d "$destination/$dir/$relative" ]] || removed=$((removed + 1))
                rm -rf "$destination/$dir/$relative"
            fi
        done < <(cd "$destination/$dir" && find . -mindepth 1 -depth -print0)
    done
    printf 'Workspace refreshed: %d changed, %d unchanged, %d removed file(s).\n' "$changed" "$unchanged" "$removed"
}
