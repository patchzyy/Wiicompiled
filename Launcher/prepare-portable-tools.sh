#!/usr/bin/env bash
# Prepares a self-contained native-Linux build toolchain bundled into the AppImage by
# build-appimage.sh, so a user needs no `clang`/`cmake`/`ninja` of their own to build the
# translated game (mirrors why Windows bundles llvm-mingw + CMake + Ninja via
# Prepare-PortableTools.ps1 - this is that script's Linux counterpart, for the same reason).
#
# clang/lld/llvm-ar: pruned from the official llvm.org GitHub release tarball (NOT llvm-mingw -
# that project targets Windows/mingw, never native Linux) down to just what's needed to compile
# and link: clang, lld, llvm-ar, the clang resource dir (builtin headers + compiler-rt), and
# libc++/libc++abi/libunwind (so the toolchain never has to fall back to the host's system
# libstdc++ headers). The raw release is ~1.9 GiB per arch (every LLVM backend, mlir, flang, lldb,
# docs, tests); pruned it is ~500 MiB uncompressed / ~100 MiB compressed, verified against a real
# build of this project. The release binaries are dynamically linked against a few libraries of
# the Ubuntu the LLVM project builds on (ICU, for lld), so those are bundled into lib/ too - see
# bundle_private_deps below - and every executable is verified to resolve nothing but the glibc
# baseline from the host.
#
# cmake: pruned from the official Kitware GitHub release tarball down to bin/cmake (not
# ccmake/cmake-gui/cpack/ctest, which local-build.sh never invokes) plus the Modules/Templates
# CMake needs at runtime (found relative to bin/cmake via CMAKE_ROOT auto-detection - Help/doc/man/
# the desktop-integration files under share/ are documentation/GUI-only and dropped). Verified with
# a real configure+build using the pruned cmake+ninja+clang together.
#
# ninja: the official ninja-build GitHub release zip, used as-is - it is already a single small
# (~130 KiB compressed) static-ish binary with nothing to prune.
set -euo pipefail

script_dir=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
workspace=$(cd "$script_dir/.." && pwd)

llvm_version=22.1.8
cmake_version=4.3.3
ninja_version=1.13.2
destination="$script_dir/artifacts/portable-tools"
arch=""

usage() {
    cat <<'EOF'
Usage: prepare-portable-tools.sh --arch {x86_64|aarch64} [--destination DIR]

  --arch ARCH          Target architecture (required)
  --destination DIR     Where the toolchain is written, as DIR/toolchain-ARCH
                        (default: Launcher/artifacts/portable-tools)
EOF
}

while [[ $# -gt 0 ]]; do
    case "$1" in
        --arch) arch=$2; shift 2 ;;
        --destination) destination=$2; shift 2 ;;
        -h|--help) usage; exit 0 ;;
        *) echo "prepare-portable-tools.sh: unknown argument: $1" >&2; exit 1 ;;
    esac
done

case "$arch" in
    x86_64) llvm_release_arch=X64; target_triple=x86_64-unknown-linux-gnu
            llvm_release_sha256=df0e1ecf16caf3489a272a5eea4eec9b0d82878f6477fa309504f918a0006384
            cmake_release_arch=x86_64
            cmake_sha256=927b2368a946c37269c3a66225ab00544e756459cdd0b5d0da438694fb9ff802
            ninja_asset=ninja-linux.zip
            ninja_sha256=5749cbc4e668273514150a80e387a957f933c6ed3f5f11e03fb30955e2bbead6 ;;
    aarch64) llvm_release_arch=ARM64; target_triple=aarch64-unknown-linux-gnu
            llvm_release_sha256=805efad2bb91cb4967fa569e0881d10c0f69c04461cf671cccbae19f547acc34
            cmake_release_arch=aarch64
            cmake_sha256=9ea38356dbd3e32e51029a3e09a0f2f8e117ef4fbcaad7a21ffb36409bbd5cb4
            ninja_asset=ninja-linux-aarch64.zip
            ninja_sha256=fd2cacc8050a7f12a16a2e48f9e06fca5c14fc4c2bee2babb67b58be17a607fc ;;
    *) echo "prepare-portable-tools.sh: --arch must be x86_64 or aarch64" >&2; usage; exit 1 ;;
esac

destination=$(mkdir -p "$destination" && cd "$destination" && pwd)
toolchain_dir="$destination/toolchain-$arch"
downloads="$script_dir/artifacts/downloads"
mkdir -p "$downloads"

sha256_of() { sha256sum "$1" | awk '{print $1}'; }

download_verified() {
    # $1 = destination path, $2 = URL, $3 = expected sha256
    local dest=$1 url=$2 expected=$3
    if [[ -f "$dest" ]] && [[ "$(sha256_of "$dest")" == "$expected" ]]; then return; fi
    echo "prepare-portable-tools.sh: downloading $(basename "$dest")..."
    local tmp="$dest.partial"
    rm -f "$tmp"
    curl -fL --progress-bar -o "$tmp" "$url"
    local actual
    actual=$(sha256_of "$tmp")
    if [[ "$actual" != "$expected" ]]; then
        echo "prepare-portable-tools.sh: $(basename "$dest") hash mismatch: expected $expected, got $actual" >&2
        rm -f "$tmp"
        exit 1
    fi
    mv "$tmp" "$dest"
}

if [[ -x "$toolchain_dir/bin/clang" && -x "$toolchain_dir/bin/ninja" && -x "$toolchain_dir/bin/cmake" ]]; then
    echo "prepare-portable-tools.sh: reusing existing toolchain at $toolchain_dir"
    exit 0
fi

work="$destination/.building-toolchain-$arch"
rm -rf "$work"
mkdir -p "$work/bin" "$work/lib/$target_triple" "$work/include/$target_triple/c++/v1"

# --- clang/lld/llvm-ar, pruned from the official LLVM release ---

llvm_archive_name="LLVM-$llvm_version-Linux-$llvm_release_arch.tar.xz"
llvm_archive="$downloads/$llvm_archive_name"
download_verified "$llvm_archive" \
    "https://github.com/llvm/llvm-project/releases/download/llvmorg-$llvm_version/$llvm_archive_name" \
    "$llvm_release_sha256"

extract_root="$script_dir/artifacts/.extract-clang-$arch"
rm -rf "$extract_root"
mkdir -p "$extract_root"
echo "prepare-portable-tools.sh: extracting $llvm_archive_name (this is the full ~1.9 GiB release; only a fraction is kept)..."
tar -xf "$llvm_archive" -C "$extract_root"
src="$extract_root/LLVM-$llvm_version-Linux-$llvm_release_arch"
[[ -d "$src" ]] || { echo "prepare-portable-tools.sh: unexpected archive layout, expected $src" >&2; exit 1; }

echo "prepare-portable-tools.sh: pruning to the minimal compile+link toolchain..."

# clang: the real driver executable plus the clang/clang++ symlinks CMake/local-build.sh invoke.
# Stripped: debug symbols are dead weight for a bundled compiler nobody will debug.
cp -a "$src/bin/clang-22" "$work/bin/"
strip "$work/bin/clang-22"
ln -s clang-22 "$work/bin/clang"
ln -s clang "$work/bin/clang++"

# lld: linked via -fuse-ld=lld, which clang resolves by looking for ld.lld next to itself first -
# see local-build.sh's --fuse-ld option.
cp -a "$src/bin/lld" "$work/bin/"
strip "$work/bin/lld"
ln -s lld "$work/bin/ld.lld"

# The llvm.org release binaries are built on Ubuntu 22.04, and lld is dynamically linked against
# that distro's ICU (libicui18n/libicuuc/libicudata .so.70) - which neither the LLVM tarball nor
# this bundle shipped. Every other distro carries a different ICU major (Arch/CachyOS 78, Fedora
# and Debian 13 76, Ubuntu 24.04 74) and no compat package, so there `ld.lld` could not even
# start ("error while loading shared libraries: libicui18n.so.70") and CMake's "Check for working
# C compiler" aborted the whole install. The smoke test below never caught it because it runs on
# the very Ubuntu 22.04 runner that has ICU 70 in /usr/lib. lld's own RUNPATH is already
# $ORIGIN/../lib, so bundling the libraries there is all it takes: no rpath patching and no
# LD_LIBRARY_PATH juggling in AppRun.
#
# Driven by ldd rather than a hardcoded ICU list so that an LLVM bump that links something else
# (or ICU 74 once the LLVM builders move to 24.04) is picked up, and so that a dependency the
# build host cannot resolve fails here instead of on a user's machine. Applied to every bundled
# executable; the verification at the end then proves each one resolves everything outside the
# glibc baseline from inside the bundle.
baseline_sonames='^(ld-linux-[^ ]*|libc|libm|libdl|libpthread|librt|libgcc_s|libstdc\+\+|libz|liblzma)\.so(\.|$)'
bundle_private_deps() {
    # $1 = executable under $work/bin. Each shared library it needs beyond the baseline is copied
    # (symlink-dereferenced, under its soname) into $work/lib, where the LLVM binaries' RUNPATH
    # looks first.
    local exe=$1 soname arrow path rest
    while read -r soname arrow path rest; do
        [[ "$arrow" == "=>" ]] || continue            # vdso / the ELF interpreter line
        soname=${soname##*/}                          # some ldd's print the interpreter as a path
        [[ "$soname" =~ $baseline_sonames ]] && continue
        if [[ "$path" != /* || ! -f "$path" ]]; then
            echo "prepare-portable-tools.sh: $(basename "$exe") needs $soname, which this host cannot resolve" >&2
            exit 1
        fi
        [[ -e "$work/lib/$soname" ]] || cp -L "$path" "$work/lib/$soname"
    done < <(ldd "$exe")
}
bundle_private_deps "$work/bin/lld"

# llvm-ar/llvm-ranlib: CMake's archiver for the many static libraries this project builds
# (aurora, Crypto++, SDL3, Dawn's dependency closure, the translated game shards).
cp -a "$src/bin/llvm-ar" "$work/bin/"
strip "$work/bin/llvm-ar"
ln -s llvm-ar "$work/bin/llvm-ranlib"

bundle_private_deps "$work/bin/clang-23"
bundle_private_deps "$work/bin/llvm-ar"

# Clang's resource directory: builtin headers (stddef.h, immintrin.h, ...) and compiler-rt
# (builtins, sanitizer runtimes). `clang -print-resource-dir` must find this at lib/clang/<ver>/.
cp -a "$src/lib/clang" "$work/lib/"

# libc++/libc++abi/libunwind: so this toolchain never has to fall back to whatever libstdc++ the
# host distro happens to have installed. Not the default yet (local-build.sh still resolves the
# system libstdc++ unless -stdlib=libc++ is passed), but bundled so that option exists.
cp -a "$src/include/c++" "$work/include/"
cp -a "$src/include/$target_triple/c++/v1/__config_site" "$work/include/$target_triple/c++/v1/"
cp -a "$src/lib/$target_triple"/libc++.a "$src/lib/$target_triple"/libc++abi.a "$src/lib/$target_triple"/libunwind.a "$work/lib/$target_triple/"
cp -a "$src/lib/$target_triple"/libc++.so* "$src/lib/$target_triple"/libc++abi.so* "$src/lib/$target_triple"/libunwind.so* "$work/lib/$target_triple/"

rm -rf "$extract_root"

# --- cmake, pruned from the official Kitware release ---

cmake_share_version=${cmake_version%.*}
cmake_archive_name="cmake-$cmake_version-linux-$cmake_release_arch.tar.gz"
cmake_archive="$downloads/$cmake_archive_name"
download_verified "$cmake_archive" \
    "https://github.com/Kitware/CMake/releases/download/v$cmake_version/$cmake_archive_name" \
    "$cmake_sha256"

cmake_extract_root="$script_dir/artifacts/.extract-cmake-$arch"
rm -rf "$cmake_extract_root"
mkdir -p "$cmake_extract_root"
echo "prepare-portable-tools.sh: extracting $cmake_archive_name..."
tar -xzf "$cmake_archive" -C "$cmake_extract_root"
cmake_src="$cmake_extract_root/cmake-$cmake_version-linux-$cmake_release_arch"
[[ -d "$cmake_src" ]] || { echo "prepare-portable-tools.sh: unexpected archive layout, expected $cmake_src" >&2; exit 1; }

mkdir -p "$work/share/cmake-$cmake_share_version"
cp -a "$cmake_src/bin/cmake" "$work/bin/"
cp -a "$cmake_src/share/cmake-$cmake_share_version/Modules" "$cmake_src/share/cmake-$cmake_share_version/Templates" \
    "$work/share/cmake-$cmake_share_version/"
rm -rf "$cmake_extract_root"
bundle_private_deps "$work/bin/cmake"

# --- ninja, used as-is ---

ninja_archive="$downloads/ninja-$ninja_version-$arch.zip"
download_verified "$ninja_archive" \
    "https://github.com/ninja-build/ninja/releases/download/v$ninja_version/$ninja_asset" \
    "$ninja_sha256"
echo "prepare-portable-tools.sh: staging ninja $ninja_version..."
unzip -oq "$ninja_archive" -d "$work/bin"
chmod +x "$work/bin/ninja"
bundle_private_deps "$work/bin/ninja"

cat > "$work/LICENSE.txt" <<EOF
Portable build tools bundled by WiiCompiled

clang/lld/llvm-ar $llvm_version (pruned from the official LLVM release for Linux/$llvm_release_arch)
  https://github.com/llvm/llvm-project/releases/tag/llvmorg-$llvm_version
  Apache License v2.0 with LLVM Exceptions:
  https://github.com/llvm/llvm-project/blob/llvmorg-$llvm_version/LICENSE.TXT

ICU (libicui18n, libicuuc, libicudata under lib/), the shared libraries lld from that release
depends on, taken from the Ubuntu 22.04 libicu package that built it
  https://icu.unicode.org/
  Unicode License:
  https://github.com/unicode-org/icu/blob/main/LICENSE

CMake $cmake_version
  https://github.com/Kitware/CMake
  BSD 3-Clause License

Ninja $ninja_version
  https://github.com/ninja-build/ninja
  Apache License 2.0
EOF

echo "prepare-portable-tools.sh: checking that the toolchain carries its own shared libraries..."
# Every bundled executable must resolve all its non-baseline shared libraries from inside the
# bundle. This host has them all installed, so a "not found" from ldd could never fire here; the
# check is on where each library resolves *from*, not on whether it resolves at all. This is the
# check that would have caught the unbundled ICU 70 that lld shipped with for a while.
for exe in clang-23 lld llvm-ar cmake ninja; do
    while read -r soname arrow path rest; do
        [[ "$arrow" == "=>" ]] || continue
        soname=${soname##*/}
        [[ "$soname" =~ $baseline_sonames ]] && continue
        case "$path" in
            "$work"/*) ;;
            *) echo "prepare-portable-tools.sh: $exe resolves $soname from ${path:-nowhere} instead of the bundle - it would break on any host without it" >&2
               exit 1 ;;
        esac
    done < <(ldd "$work/bin/$exe")
done

echo "prepare-portable-tools.sh: smoke-testing the toolchain..."
smoke_dir=$(mktemp -d)
trap 'rm -rf "$smoke_dir"' EXIT
cat > "$smoke_dir/t.cpp" <<'EOF'
#include <vector>
#include <cstdio>
int main() {
    std::vector<int> v{1, 2, 3};
    int sum = 0;
    for (int x : v) sum += x;
    return sum == 6 ? 0 : 1;
}
EOF
"$work/bin/clang++" -std=c++20 -fuse-ld=lld "$smoke_dir/t.cpp" -o "$smoke_dir/t"
"$smoke_dir/t"

# Also exercised together through CMake+Ninja, exactly how local-build.sh drives them - a plain
# clang++ invocation above would not catch a broken CMAKE_ROOT (Modules/Templates) or a Ninja that
# can't find the compiler.
cat > "$smoke_dir/CMakeLists.txt" <<'EOF'
cmake_minimum_required(VERSION 3.16)
project(smoke CXX)
add_executable(smoke t.cpp)
EOF
"$work/bin/cmake" -S "$smoke_dir" -B "$smoke_dir/build" -G Ninja \
    -DCMAKE_MAKE_PROGRAM="$work/bin/ninja" -DCMAKE_CXX_COMPILER="$work/bin/clang++" >/dev/null
"$work/bin/cmake" --build "$smoke_dir/build" >/dev/null
"$smoke_dir/build/smoke"

rm -rf "$smoke_dir"
trap - EXIT

mv "$work" "$toolchain_dir"
echo "prepare-portable-tools.sh: toolchain ready at $toolchain_dir ($(du -sh "$toolchain_dir" | cut -f1))"
