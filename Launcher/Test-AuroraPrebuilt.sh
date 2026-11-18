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
printf 'edited\n' >> 'workspace/aurora-main/include/a header.h'
expect_error 'does not match the Aurora sources'
printf 'header\n' > 'workspace/aurora-main/include/a header.h'
for invalid in '{}' 'null' 'not json' '{"AuroraSourceFingerprint":false}' \
    "{\"nested\":{\"AuroraSourceFingerprint\":\"$fingerprint\"}}"; do
    printf '%s\n' "$invalid" > package/provenance.json
    expect_error 'does not match the Aurora sources'
done
