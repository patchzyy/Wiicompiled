#!/usr/bin/env python3
"""Classify changed repository paths for the pull-request workflow."""

from __future__ import annotations

import argparse
import os
import subprocess
from pathlib import PurePosixPath
from typing import Iterable


NATIVE_PREFIXES = (
    "translator/",
    "runtime/",
    "aurora-main/",
    "projects/",
)
NATIVE_FILES = {
    "Launcher/NativeBuildFlags.ps1",
    "Launcher/Prepare-Dependencies.ps1",
    "Launcher/Prepare-PortableTools.ps1",
    "Launcher/Test-Recompilation.ps1",
    "Launcher/local-build.sh",
    "Launcher/local-build-macos.command",
}
FORCE_ALL_PREFIXES = (".github/",)
FORCE_ALL_FILES = {"global.json", "AGENTS.md"}
PROJECT_SUFFIXES = (".sln", ".csproj", ".props", ".targets")


def classify(paths: Iterable[str], force_all: bool = False) -> dict[str, bool]:
    normalized = set()
    for raw_path in paths:
        path = PurePosixPath(raw_path.strip().replace("\\", "/")).as_posix()
        if path.startswith("./"):
            path = path[2:]
        if path and path != ".":
            normalized.add(path)
    result = {"native": force_all, "aurora": force_all}

    for path in normalized:
        if (
            path in FORCE_ALL_FILES
            or path.startswith(FORCE_ALL_PREFIXES)
            or path.endswith(PROJECT_SUFFIXES)
        ):
            result.update(native=True, aurora=True)
            continue

        if path.startswith("aurora-main/"):
            result.update(native=True, aurora=True)
        elif path.startswith(NATIVE_PREFIXES) or path in NATIVE_FILES:
            result["native"] = True

    return result


def write_github_output(values: dict[str, bool], output_path: str) -> None:
    with open(output_path, "a", encoding="utf-8") as output:
        for key, value in values.items():
            output.write(f"{key}={'true' if value else 'false'}\n")


def git_changed_paths(base: str, head: str) -> list[str]:
    if not base or set(base) == {"0"} or not head:
        raise ValueError("A real base and head revision are required")
    completed = subprocess.run(
        ["git", "diff", "--name-only", "-z", base, head],
        check=True,
        capture_output=True,
    )
    return [path.decode("utf-8") for path in completed.stdout.split(b"\0") if path]


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("paths", nargs="*")
    parser.add_argument("--all", action="store_true", dest="force_all")
    parser.add_argument("--base")
    parser.add_argument("--head")
    parser.add_argument("--github-output", default=os.environ.get("GITHUB_OUTPUT"))
    args = parser.parse_args()

    paths = list(args.paths)
    force_all = args.force_all
    if args.base or args.head:
        try:
            paths.extend(git_changed_paths(args.base or "", args.head or ""))
        except (ValueError, subprocess.CalledProcessError) as error:
            print(f"Could not calculate a reliable diff ({error}); enabling every category.")
            force_all = True

    values = classify(paths, force_all)
    print("changed_paths=" + (",".join(sorted(paths)) if paths else "(none)"))
    if args.github_output:
        write_github_output(values, args.github_output)
    for key, value in values.items():
        print(f"{key}={'true' if value else 'false'}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
