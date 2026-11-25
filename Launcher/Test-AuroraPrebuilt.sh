#!/usr/bin/env bash
set -euo pipefail

launcher=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
scratch=$(mktemp -d)
trap 'rm -rf "$scratch"' EXIT
cd "$scratch"
mkdir -p 'workspace/aurora-main/include' package
cd workspace/aurora-main
printf 'project(aurora)\n' > CMakeLists.txt
printf 'header\n' > 'include/a header.h'
# Manifest format used by existing Linux packages.
fingerprint=$(
    for path in CMakeLists.txt 'include/a header.h'; do
        printf '%s %s\n' "$path" "$(sha256sum < "$path" | cut -d ' ' -f 1)"
    done | sha256sum | cut -d ' ' -f 1
)
mkdir -p .git extern build
touch .git/config extern/dependency build/object
cd ../..
if command -v shasum >/dev/null 2>&1; then
    mkdir fallback-tools
    for tool in shasum awk find sort xargs; do
        ln -s "$(command -v "$tool")" "fallback-tools/$tool"
    done
    (
        PATH="$PWD/fallback-tools"
        source "$launcher/build-cache.sh"
        [[ $(mkw_fingerprint_tree workspace/aurora-main extern build) == "$fingerprint" ]]
    )
fi
touch package/native_prebuilt.cmake
args=(--workspace workspace --output-dir output --translator-bin unused
    --cc true --cxx true --ninja true --x86-cpu-profile v2 --native-prebuilt-dir package)
expect_error() {
    local expected=$1 output
    if output=$(bash "$launcher/local-build.sh" "${args[@]}" 2>&1); then
        echo "unexpected build success" >&2
        exit 1
    fi
    if [[ "$output" != *"$expected"* ]]; then
        printf 'expected %s, got %s\n' "$expected" "$output" >&2
        exit 1
    fi
}
expect_error 'Native prebuilt provenance is missing'
printf '{"AuroraSourceFingerprint":"%s"}\n' "$fingerprint" > package/provenance.json
expect_error 'Extracted main.dol'
mv workspace 'workspace[old]'
args[1]='workspace[old]'
expect_error 'Extracted main.dol'
printf 'edited\n' >> 'workspace[old]/aurora-main/include/a header.h'
expect_error 'does not match the Aurora sources'
printf 'header\n' > 'workspace[old]/aurora-main/include/a header.h'
for invalid in '{}' 'null' 'not json' '{"AuroraSourceFingerprint":false}' \
    "{\"nested\":{\"AuroraSourceFingerprint\":\"$fingerprint\"}}"; do
    printf '%s\n' "$invalid" > package/provenance.json
    expect_error 'does not match the Aurora sources'
done

mkdir -p producer/{Launcher,aurora-main,runtime/{third_party,cmake},tools/bin}
cp "$launcher"/{Prepare-NativePrebuilt.sh,build-cache.sh} producer/Launcher/
touch producer/runtime/cmake/MbedTLSPin.cmake producer/tools/bin/{clang,clang++,clang-22}
producer_args=(--arch aarch64 --print-fingerprint-only --toolchain-dir "$PWD/producer/tools"
    --cmake-bin /bin/true --ninja-bin /bin/true)
if output=$(bash producer/Launcher/Prepare-NativePrebuilt.sh "${producer_args[@]}" 2>&1); then
    echo 'accepted an empty aurora checkout' >&2
    exit 1
fi
[[ "$output" == *'aurora-main/CMakeLists.txt'* ]]
cp 'workspace[old]/aurora-main/CMakeLists.txt' producer/aurora-main/
output=$(bash producer/Launcher/Prepare-NativePrebuilt.sh "${producer_args[@]}")
[[ "$output" =~ aurora_fingerprint=[0-9a-f]{64} ]]
