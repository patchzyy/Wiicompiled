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

echo
echo "$passed passed, $failed failed"
[[ $failed -eq 0 ]]
