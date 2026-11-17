#!/usr/bin/env bash
# Parser tests for Prepare-NativePrebuilt.sh
# (https://github.com/patchzyy/Wiicompiled/issues/274).
#
# Rejection cases must fail during argument parsing, before any filesystem check, configure, or
# build side effect. Acceptance cases always run with --print-fingerprint-only so that even on a
# maintainer machine with the portable toolchain present the script exits at the fingerprint
# print instead of harvesting a package; on machines without the toolchain the run still fails
# later at the portable-tools assertion, which is not a parser error.
#
# No network, no toolchain, and no write access outside this checkout's build paths is needed:
# every command line under test reaches its expected outcome before the first side effect.
set -uo pipefail

script_under_test=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)/Prepare-NativePrebuilt.sh

passed=0
failed=0

# expect_reject <description> <expected option name> <args...>
# The script must exit nonzero and its error must both name the affected option and complain
# about the value; a bare set -u unbound-variable crash or an unnamed failure does not count.
expect_reject() {
    local description=$1 expected_option=$2
    shift 2
    local output status
    output=$("$script_under_test" "$@" 2>&1)
    status=$?
    if [[ $status -eq 0 ]]; then
        echo "FAIL: $description: expected rejection, exited 0"
        failed=$((failed + 1))
        return
    fi
    if [[ "$output" != *"$expected_option"* || "$output" != *"requires a value"* ]]; then
        echo "FAIL: $description: error does not name $expected_option with a value complaint:"
        echo "$output"
        failed=$((failed + 1))
        return
    fi
    echo "PASS: $description"
    passed=$((passed + 1))
}

# expect_parse_ok <description> <args...>
# Parsing must accept the command line. The run may still fail afterward (missing portable
# toolchain), but never with one of the parser errors.
expect_parse_ok() {
    local description=$1
    shift
    local output status
    output=$("$script_under_test" "$@" 2>&1)
    status=$?
    if [[ "$output" == *"requires a value"* || "$output" == *"unknown argument"* \
          || "$output" == *"must be x86_64 or aarch64"* ]]; then
        echo "FAIL: $description: rejected a valid command line:"
        echo "$output"
        failed=$((failed + 1))
        return
    fi
    echo "PASS: $description (exit $status)"
    passed=$((passed + 1))
}

# The case from the PR #270 review: --disconnected must not be eaten as the sysroot value while
# disconnected mode silently stays off.
expect_reject "--sysroot swallowing --disconnected" "--sysroot" \
    --arch x86_64 --sysroot --disconnected

# A following flag is not a valid value for any value-taking option.
for option in --arch --output-dir --stage-dir --parallel --toolchain-dir \
              --ninja-bin --cmake-bin --llvm-dir --sysroot; do
    expect_reject "$option with an option-like value" "$option" \
        --arch x86_64 "$option" --keep-stage
done

# A trailing value-less option must fail naming the option, not on set -u's unbound variable.
expect_reject "--arch without a value" "--arch" --arch
expect_reject "--parallel without a value" "--parallel" --arch x86_64 --parallel
expect_reject "--sysroot without a value" "--sysroot" --arch x86_64 --sysroot

# An explicitly empty value is as useless as a missing one.
expect_reject "--arch with an empty value" "--arch" --arch ""
expect_reject "--output-dir with an empty value" "--output-dir" --arch x86_64 --output-dir ""
expect_reject "--sysroot swallowing -h" "--sysroot" --arch x86_64 --sysroot -h

# Valid command lines and flag-only options keep working.
expect_parse_ok "full valid option set parses" \
    --arch x86_64 --parallel 4 --keep-stage --reuse-stage --disconnected --print-fingerprint-only
expect_parse_ok "explicit paths parse" \
    --arch aarch64 --output-dir /tmp/np-out --stage-dir /tmp/np-stage \
    --toolchain-dir /tmp/np-tools --ninja-bin /tmp/np-ninja --cmake-bin /tmp/np-cmake \
    --llvm-dir /tmp/np-llvm --sysroot /tmp/np-sysroot --print-fingerprint-only
expect_parse_ok "flag-only options parse" \
    --arch x86_64 --keep-stage --reuse-stage --disconnected --print-fingerprint-only

# -h stays a clean exit-0 usage print.
help_output=$("$script_under_test" -h 2>&1)
help_status=$?
if [[ $help_status -eq 0 && "$help_output" == *"Usage:"* ]]; then
    echo "PASS: -h prints usage and exits 0"
    passed=$((passed + 1))
else
    echo "FAIL: -h no longer prints usage with exit 0:"
    echo "$help_output"
    failed=$((failed + 1))
fi

# ---------------------------------------------------------------------------
# The same guard on the other Launcher shell entry points. Only rejection paths are
# exercised: unlike Prepare-NativePrebuilt.sh these scripts have no parse-only mode, so an
# accepted command line would start real work. build-appimage.sh is edited but not driven
# here: its pre-parser ELF architecture probe exits on a non-Linux userspace before the
# argument parser is reached.
# ---------------------------------------------------------------------------

# expect_value_reject <script> <option> <description> [value...]
# Runs "$script" "$option" <value...>. With no value arguments this is the trailing-option
# case, with --help it is an option-like value, with "" an empty value.
expect_value_reject() {
    local script=$1 option=$2 description=$3
    shift 3
    local output status
    output=$("$script" "$option" "$@" 2>&1)
    status=$?
    if [[ $status -eq 0 ]]; then
        echo "FAIL: $description: expected rejection, exited 0"
        failed=$((failed + 1))
        return
    fi
    if [[ "$output" != *"$option"* || "$output" != *"requires a value"* ]]; then
        echo "FAIL: $description: error does not name $option with a value complaint:"
        echo "$output"
        failed=$((failed + 1))
        return
    fi
    echo "PASS: $description"
    passed=$((passed + 1))
}

launcher_dir=$(cd "$(dirname "$script_under_test")" && pwd)

value_options_of() {
    case $1 in
        prepare-portable-tools.sh)
            echo "--arch --destination" ;;
        local-build.sh)
            echo "--workspace --profile --output-dir --base-output-dir --retro-rewind-package-dir --retro-wfc-offline-dir --parallel --cc --cxx --fuse-ld --cmake --ninja --dotnet --translator-dll --translator-bin --native-prebuilt-dir --sysroot --setup-version --x86-cpu-profile" ;;
        local-build-macos.command)
            echo "--workspace --profile --output-dir --base-output-dir --game --nodtool --retro-rewind-package-dir --retro-wfc-offline-dir --parallel --cmake --ninja --dotnet --translator-bin --setup-version" ;;
    esac
}

for script_name in prepare-portable-tools.sh local-build.sh local-build-macos.command; do
    script=$launcher_dir/$script_name
    first_option=""
    for option in $(value_options_of "$script_name"); do
        [[ -z "$first_option" ]] && first_option=$option
        expect_value_reject "$script" "$option" "$script_name: $option with an option-like value" --help
    done
    expect_value_reject "$script" "$first_option" "$script_name: $first_option swallowing -h" -h
    expect_value_reject "$script" "$first_option" "$script_name: $first_option with an empty value" ""
    expect_value_reject "$script" "$first_option" "$script_name: trailing $first_option without a value"
done

echo
echo "$passed passed, $failed failed"
[[ $failed -eq 0 ]]
