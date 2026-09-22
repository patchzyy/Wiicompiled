#!/usr/bin/env python3
"""Enforce the stable branch-protection contract for conditional CI jobs."""

from __future__ import annotations

import argparse
import os
from pathlib import Path


def validate(changes: str, managed: str, native: str, native_required: bool) -> list[str]:
    failures = []
    if changes != "success":
        failures.append(f"change classification finished as {changes}")
    if managed != "success":
        failures.append(f"managed matrix finished as {managed}")
    expected_native = "success" if native_required else "skipped"
    if native != expected_native:
        failures.append(f"native matrix finished as {native}; expected {expected_native}")
    return failures


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--changes", required=True)
    parser.add_argument("--managed", required=True)
    parser.add_argument("--native", required=True)
    parser.add_argument("--native-required", choices=("true", "false"), required=True)
    args = parser.parse_args()

    native_required = args.native_required == "true"
    failures = validate(args.changes, args.managed, args.native, native_required)
    rows = [
        "## Required checks",
        "",
        "| Job | Result | Required |",
        "| --- | --- | --- |",
        f"| Change classification | {args.changes} | yes |",
        f"| Managed matrix | {args.managed} | yes |",
        f"| Native synthetic matrix | {args.native} | {'yes' if native_required else 'no'} |",
        "",
    ]
    if failures:
        rows.extend(["Failures:", *[f"- {failure}" for failure in failures], ""])
    rendered = "\n".join(rows)
    print(rendered)
    if summary := os.environ.get("GITHUB_STEP_SUMMARY"):
        with Path(summary).open("a", encoding="utf-8") as output:
            output.write(rendered)
    return 1 if failures else 0


if __name__ == "__main__":
    raise SystemExit(main())
