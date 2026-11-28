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

# Linux package provenance uses sorted relative paths followed by their file digests.
mkw_fingerprint_tree() (
    set -o pipefail
    cd -- "$1" || exit 1
    shift
    local args=(. -name .git -prune -o) dir record digest
    for dir in "$@"; do args+=(-path "./$dir" -prune -o); done
    find "${args[@]}" -type f -print0 | LC_ALL=C sort -z |
        if [[ ${mkw_sha256_cmd[0]} == sha256sum ]]; then
            xargs -0 -r sha256sum -z -- | while IFS= read -r -d '' record; do
                # sha256sum -z emits the digest, two spaces, then the ./ path.
                printf '%s %s\n' "${record:68}" "${record:0:64}"
            done
        else
            while IFS= read -r -d '' record; do
                digest=$(mkw_sha256 < "$record") || exit 1
                printf '%s %s\n' "${record#./}" "$digest"
            done
        fi | mkw_sha256
)

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

# ---------------------------------------------------------------------------------------------
# Sync manifests. A packaged source tree ships $mkw_sync_manifest_name, written at package time by
# mkw_write_sync_manifest:
#   mkw-sync-manifest-v1
#   <sha256>\t<size>\t<relative path>      (one line per regular file, never mtimes)
#   end\t<entry count>
# After a successful mkw_sync_tree the destination keeps $mkw_sync_state_name, recording what that
# sync left behind:
#   mkw-sync-state-v1
#   written\t<epoch seconds after the last copy>
#   dirs\t<dir>\t<dir>...
#   <sha256>\t<size>\t<mtime>\t<relative path>
#   end\t<entry count>
# A file is only trusted without cmp when its shipped hash and size equal the recorded ones and
# its current size and mtime still equal the recorded ones, with that mtime older than the record
# (a later edit in the same second stays uncertain). Anything missing, malformed or unverifiable
# falls back to comparing every file.
# ---------------------------------------------------------------------------------------------
mkw_sync_manifest_name=.mkw-sync-manifest
mkw_sync_state_name=.mkw-sync-state
mkw_tab=$(printf '\t')
mkw_stat_mode=""

# Picks a stat that prints "<size>\t<mtime>\t<name>": GNU/busybox stat -c on Linux, the system BSD
# stat on macOS (a Homebrew GNU stat earlier on PATH must not decide the format). $1 = scratch dir.
mkw_sync_detect_stat() {
    local probe="$1/stat-probe" line expected
    [[ -n "$mkw_stat_mode" ]] && return 0
    printf 'abc' > "$probe" || return 1
    expected="^3${mkw_tab}[0-9][0-9]*\$"
    if [[ "$(uname -s 2>/dev/null)" == Darwin ]]; then
        if line=$(/usr/bin/stat -f "%z${mkw_tab}%m" "$probe" 2>/dev/null) && [[ $line =~ $expected ]]; then
            mkw_stat_mode=bsd
        fi
    elif line=$(stat -c "%s${mkw_tab}%Y" "$probe" 2>/dev/null) && [[ $line =~ $expected ]]; then
        mkw_stat_mode=gnu
    fi
    rm -f "$probe"
    [[ -n "$mkw_stat_mode" ]]
}

# Prints "<size>\t<mtime>\t<path>" for each NUL-separated path on stdin.
mkw_sync_stat_files() {
    case "$mkw_stat_mode" in
        gnu) xargs -0 stat -c "%s${mkw_tab}%Y${mkw_tab}%n" ;;
        bsd) xargs -0 /usr/bin/stat -f "%z${mkw_tab}%m${mkw_tab}%N" ;;
        *) return 1 ;;
    esac
}

# Writes $2 with "<size>\t<mtime>\t<dir/relative path>" for every regular file under each named
# directory of $1. Fails unless every listed file produced exactly one line. $1 = root, $2 = out.
mkw_sync_stat_tree() {
    local base=$1 out=$2 dir names lines; shift 2
    : > "$out.list" || return 1
    for dir in "$@"; do
        [[ -d "$base/$dir" ]] || continue
        (cd "$base" && find "$dir" -type f -print0) >> "$out.list" 2>/dev/null || return 1
    done
    : > "$out" || return 1
    if [[ -s "$out.list" ]]; then
        (cd "$base" && mkw_sync_stat_files) < "$out.list" > "$out" 2>/dev/null || return 1
    fi
    names=$(tr -cd '\000' < "$out.list" | wc -c) || return 1
    lines=$(wc -l < "$out") || return 1
    (( names == lines ))
}

# Writes $1/$2 (default $mkw_sync_manifest_name) for every regular file under $1 except itself.
# Paths holding a tab, newline or backslash cannot be represented, so no manifest is written and
# a sync of that tree keeps comparing every file.
mkw_write_sync_manifest() (
    set -o pipefail
    root=$1
    name=${2:-$mkw_sync_manifest_name}
    cd -- "$root" || exit 1
    rm -f -- "$name"
    work=$(mktemp -d "${TMPDIR:-/tmp}/mkw-manifest.XXXXXX") || exit 1
    trap 'rm -rf "$work"' EXIT
    mkw_sync_detect_stat "$work" || exit 1
    find . -type f ! -path "./$name" -print0 | LC_ALL=C sort -z > "$work/list" || exit 1
    names=$(tr -cd '\000' < "$work/list" | wc -c) || exit 1
    if (( names > 0 )); then
        tr '\000' '\n' < "$work/list" > "$work/lines" || exit 1
        (( $(wc -l < "$work/lines") == names )) || exit 1
        if LC_ALL=C grep -q "[$mkw_tab\\\\]" "$work/lines"; then exit 1; fi
        xargs -0 "${mkw_sha256_cmd[@]}" < "$work/list" > "$work/hashes" || exit 1
        mkw_sync_stat_files < "$work/list" > "$work/sizes" || exit 1
    else
        : > "$work/hashes"; : > "$work/sizes"
    fi
    MKW_NAMES=$names awk '
        BEGIN { FS = "\t" }
        FNR == 1 { file++ }
        file == 1 {
            # "  " in text mode, " *" where the tool defaults to binary mode.
            if (length($0) < 68 || (substr($0, 65, 2) != "  " && substr($0, 65, 2) != " *")) { bad = 1; next }
            path = substr($0, 67); sub(/^\.\//, "", path); sha[path] = substr($0, 1, 64); next
        }
        {
            path = substr($0, length($1) + length($2) + 3); sub(/^\.\//, "", path)
            if (!(path in sha) || $1 !~ /^[0-9][0-9]*$/) { bad = 1; next }
            line[++n] = sha[path] "\t" $1 "\t" path
        }
        END {
            if (bad || n != ENVIRON["MKW_NAMES"] + 0) exit 1
            print "mkw-sync-manifest-v1"
            for (i = 1; i <= n; i++) print line[i]
            print "end\t" n
        }
    ' "$work/hashes" "$work/sizes" > "$work/manifest" || exit 1
    # An empty hashes file never reaches FNR == 1, so an empty tree is only valid with no files.
    if (( names == 0 )); then printf 'mkw-sync-manifest-v1\nend\t0\n' > "$work/manifest" || exit 1; fi
    cp "$work/manifest" "$name.tmp.$$" && mv -f "$name.tmp.$$" "$name" || { rm -f "$name.tmp.$$"; exit 1; }
)

# Plans (MKW_SYNC_MODE=plan) or records (MKW_SYNC_MODE=state) a sync from sections on stdin:
# @M the shipped manifest, @C the source tree's stat, @S the previous sync state (plan only),
# @D the destination tree's stat. MKW_SYNC_DIRS lists the synced directories, one per line.
# plan prints "T\t<path>" (trusted unchanged) or "U\t<path>" (needs cmp/copy) for every shipped
# file; state prints a complete sync state. Any inconsistency exits non-zero.
mkw_sync_awk='
function hex64(s) { return length(s) == 64 && s !~ /[^0-9a-f]/ }
function count(s) { return s != "" && s !~ /[^0-9]/ }
function chosen(p,   i) {
    for (i = 1; i <= ndirs; i++) if (dirs[i] != "" && index(p, dirs[i] "/") == 1) return 1
    return 0
}
function rest(n,   i, skip) {
    skip = 0
    for (i = 1; i <= n; i++) skip += length($i) + 1
    return substr($0, skip + 1)
}
BEGIN {
    FS = "\t"; mode = ENVIRON["MKW_SYNC_MODE"]
    ndirs = split(ENVIRON["MKW_SYNC_DIRS"], dirs, "\n")
    if (mode != "plan" && mode != "state") bad = 1
}
/^@[MCSD]$/ { section = substr($0, 2); line = 0; seen[section]++; next }
{ line++ }
section == "M" {
    if (line == 1) { if ($0 != "mkw-sync-manifest-v1") bad = 1; next }
    if (mend) { bad = 1; next }
    if ($1 == "end") { if (NF != 2 || !count($2) || $2 + 0 != mtotal) bad = 1; mend = 1; next }
    if (NF < 3 || !hex64($1) || !count($2)) { bad = 1; next }
    path = rest(2); mtotal++
    if (path == "" || !chosen(path)) next
    if (path in msha) { bad = 1; next }
    msha[path] = $1; msize[path] = $2; order[++mn] = path
    next
}
section == "C" {
    if (NF < 3 || !count($1) || !count($2)) { bad = 1; next }
    path = rest(2)
    if (!(path in msha) || msize[path] != $1 || (path in cseen)) { bad = 1; next }
    cseen[path] = 1; cn++
    next
}
section == "S" {
    if (line == 1) { if ($0 != "mkw-sync-state-v1") bad = 1; next }
    if (line == 2) { if ($1 != "written" || NF != 2 || !count($2)) bad = 1; else written = $2 + 0; next }
    if (line == 3) { if ($1 != "dirs") bad = 1; next }
    if (send) { bad = 1; next }
    if ($1 == "end") { if (NF != 2 || !count($2) || $2 + 0 != stotal) bad = 1; send = 1; next }
    if (NF < 4 || !hex64($1) || !count($2) || !count($3)) { bad = 1; next }
    path = rest(3); stotal++
    if (path in ssha) { bad = 1; next }
    ssha[path] = $1; ssize[path] = $2; smtime[path] = $3
    next
}
section == "D" {
    if (NF < 3 || !count($1) || !count($2)) { bad = 1; next }
    path = rest(2)
    if (path in dsize) { bad = 1; next }
    dsize[path] = $1; dmtime[path] = $2
    next
}
{ bad = 1 }
END {
    if (bad || seen["M"] != 1 || !mend || seen["C"] != 1 || cn != mn || seen["D"] != 1) exit 1
    if (mode == "plan") {
        if (seen["S"] != 1 || !send) exit 1
        for (i = 1; i <= mn; i++) {
            path = order[i]
            if ((path in ssha) && ssha[path] == msha[path] && ssize[path] == msize[path] &&
                (path in dsize) && dsize[path] == ssize[path] && dmtime[path] == smtime[path] &&
                smtime[path] + 0 < written)
                print "T\t" path
            else
                print "U\t" path
        }
        exit 0
    }
    if (seen["S"]) exit 1
    written = ENVIRON["MKW_SYNC_WRITTEN"]
    if (!count(written)) exit 1
    dirline = "dirs"
    for (i = 1; i <= ndirs; i++) if (dirs[i] != "") dirline = dirline "\t" dirs[i]
    for (i = 1; i <= mn; i++) if (!(order[i] in dsize) || dsize[order[i]] != msize[order[i]]) exit 1
    print "mkw-sync-state-v1"
    print "written\t" written
    print dirline
    for (i = 1; i <= mn; i++) print msha[order[i]] "\t" msize[order[i]] "\t" dmtime[order[i]] "\t" order[i]
    print "end\t" mn
}'

# Checks that $1 ships a manifest matching its tree (same files, same sizes, nothing but regular
# files and directories) and leaves the tree's stat in $2/src.stat. $1 = source, $2 = scratch.
mkw_sync_check_source() {
    local source=$1 work=$2 dir; shift 2
    [[ -f "$source/$mkw_sync_manifest_name" ]] || return 1
    mkw_sync_detect_stat "$work" || return 1
    : > "$work/odd" || return 1
    for dir in "$@"; do
        [[ -e "$source/$dir" || -L "$source/$dir" ]] || continue
        (cd "$source" && find "$dir" ! -type f ! -type d -print) >> "$work/odd" 2>/dev/null || return 1
    done
    [[ ! -s "$work/odd" ]] || return 1
    mkw_sync_stat_tree "$source" "$work/src.stat" "$@"
}

# Writes $work/plan for a sync of the named directories. $4 = directories, one per line.
mkw_sync_plan() {
    local source=$1 destination=$2 work=$3 dirs=$4; shift 4
    [[ -f "$work/state" ]] || return 1
    mkw_sync_check_source "$source" "$work" "$@" || return 1
    mkw_sync_stat_tree "$destination" "$work/dst.stat" "$@" || return 1
    {
        printf '@M\n'; cat "$source/$mkw_sync_manifest_name"
        printf '@C\n'; cat "$work/src.stat"
        printf '@S\n'; cat "$work/state"
        printf '@D\n'; cat "$work/dst.stat"
    } > "$work/plan.in" || return 1
    MKW_SYNC_MODE=plan MKW_SYNC_DIRS=$dirs awk "$mkw_sync_awk" "$work/plan.in" > "$work/plan" 2>/dev/null
}

# Atomically writes $2's sync state from $1's manifest and $2's current files. Must only run after
# a complete, successful sync. $3 = scratch dir, $4 = directories, one per line.
mkw_sync_write_state() {
    local source=$1 destination=$2 work=$3 dirs=$4 written tmp; shift 4
    tmp="$destination/$mkw_sync_state_name.tmp.$$"
    mkw_sync_check_source "$source" "$work" "$@" || return 1
    written=$(date +%s) || return 1
    [[ "$written" =~ ^[0-9]+$ ]] || return 1
    mkw_sync_stat_tree "$destination" "$work/final.stat" "$@" || return 1
    {
        printf '@M\n'; cat "$source/$mkw_sync_manifest_name"
        printf '@C\n'; cat "$work/src.stat"
        printf '@D\n'; cat "$work/final.stat"
    } > "$work/state.in" || return 1
    if MKW_SYNC_MODE=state MKW_SYNC_WRITTEN=$written MKW_SYNC_DIRS=$dirs awk "$mkw_sync_awk" "$work/state.in" > "$tmp" 2>/dev/null &&
       mv -f "$tmp" "$destination/$mkw_sync_state_name"; then
        return 0
    fi
    rm -f "$tmp"
    return 1
}

# Records the sync state of a destination that was just populated by a complete copy of $1 (for
# example ditto of the whole packaged workspace). Best effort: without a state the next sync
# compares every file. $1 = source, $2 = destination, remaining = synced directories.
mkw_sync_record_state() {
    local source=$1 destination=$2 dir dirs="" work status=1; shift 2
    rm -f "$destination/$mkw_sync_state_name" || return 1
    for dir in "$@"; do dirs="$dirs$dir"$'\n'; done
    work=$(mktemp -d "${TMPDIR:-/tmp}/mkw-sync.XXXXXX" 2>/dev/null) || return 1
    if [[ "$work" == /* && -d "$work" ]] && mkw_sync_write_state "$source" "$destination" "$work" "$dirs" "$@"; then
        status=0
    fi
    rm -rf "$work"
    return "$status"
}

# Copies one file of a sync unless the destination already holds identical bytes. Updates the
# caller's changed/unchanged/failed counters.
mkw_sync_file() {
    if [[ -f "$2" ]] && cmp -s "$1" "$2"; then
        unchanged=$((unchanged + 1))
    else
        rm -rf "$2"
        cp "$1" "$2"
        [[ $? -eq 0 ]] || failed=1
        changed=$((changed + 1))
    fi
}

# Mirrors each named directory from $1 into $2. Unchanged files are left untouched so their
# timestamps (which clang's PCH validation and Ninja compare) survive; changed and new files are
# copied fresh and files no longer shipped are removed. With a shipped manifest and the state of
# the previous sync, only files that may differ are compared (see the format notes above).
mkw_sync_tree() {
    local source=$1 destination=$2; shift 2
    local dir relative kind changed=0 unchanged=0 removed=0 failed=0 fast=0 planned=0 visited=0
    local work="" dirs="" state="$destination/$mkw_sync_state_name"
    for dir in "$@"; do dirs="$dirs$dir"$'\n'; done
    work=$(mktemp -d "${TMPDIR:-/tmp}/mkw-sync.XXXXXX" 2>/dev/null) || work=""
    [[ "$work" == /* && -d "$work" ]] || work=""
    if [[ -n "$work" && -f "$state" ]]; then
        cp "$state" "$work/state" 2>/dev/null || rm -f "$work/state"
    fi
    # Invalidated before anything is touched: an interrupted sync must never leave a state that
    # vouches for files it was rewriting.
    rm -f "$state"
    if [[ -n "$work" ]] && mkw_sync_plan "$source" "$destination" "$work" "$dirs" "$@"; then
        fast=1
        planned=$(wc -l < "$work/plan") || fast=0
    fi
    for dir in "$@"; do
        if [[ ! -d "$source/$dir" ]]; then
            rm -rf "$destination/$dir"
            continue
        fi
        if [[ -e "$destination/$dir" && ! -d "$destination/$dir" ]]; then
            rm -f "$destination/$dir"
        fi
        mkdir -p "$destination/$dir"
        if (( fast )); then
            while IFS= read -r -d '' relative; do
                relative=${relative#./}
                [[ -d "$destination/$dir/$relative" ]] || { rm -f "$destination/$dir/$relative"; mkdir -p "$destination/$dir/$relative"; }
            done < <(cd "$source/$dir" && find . -mindepth 1 -type d -print0)
            while IFS="$mkw_tab" read -r kind relative; do
                case "$relative" in
                    "$dir"/*) ;;
                    *) continue ;;
                esac
                visited=$((visited + 1))
                if [[ "$kind" == T ]]; then
                    unchanged=$((unchanged + 1))
                else
                    relative=${relative#"$dir"/}
                    mkw_sync_file "$source/$dir/$relative" "$destination/$dir/$relative"
                fi
            done < "$work/plan"
        else
            while IFS= read -r -d '' relative; do
                relative=${relative#./}
                if [[ -d "$source/$dir/$relative" ]]; then
                    [[ -d "$destination/$dir/$relative" ]] || { rm -f "$destination/$dir/$relative"; mkdir -p "$destination/$dir/$relative"; }
                else
                    mkw_sync_file "$source/$dir/$relative" "$destination/$dir/$relative"
                fi
            done < <(cd "$source/$dir" && find . -mindepth 1 -print0)
        fi
        while IFS= read -r -d '' relative; do
            relative=${relative#./}
            if [[ ! -e "$source/$dir/$relative" && ( -e "$destination/$dir/$relative" || -L "$destination/$dir/$relative" ) ]]; then
                [[ -d "$destination/$dir/$relative" ]] || removed=$((removed + 1))
                rm -rf "$destination/$dir/$relative"
            fi
        done < <(cd "$destination/$dir" && find . -mindepth 1 -depth -print0)
    done
    # Every planned file must have been visited; otherwise the plan was not fully applied.
    (( ! fast || visited == planned )) || failed=1
    if [[ -n "$work" ]]; then
        if (( ! failed )); then
            mkw_sync_write_state "$source" "$destination" "$work" "$dirs" "$@" || true
        fi
        rm -rf "$work"
    fi
    printf 'Workspace refreshed: %d changed, %d unchanged, %d removed file(s).\n' "$changed" "$unchanged" "$removed"
}
