#!/usr/bin/env python3
"""Write a compact Cobertura report to the GitHub job summary."""

from __future__ import annotations

import argparse
import os
import xml.etree.ElementTree as ET
from pathlib import Path


def percent(value: str | None) -> str:
    return f"{float(value or 0) * 100:.2f}%"


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("coverage", type=Path)
    parser.add_argument("--summary", type=Path, default=None)
    args = parser.parse_args()

    root = ET.parse(args.coverage).getroot()
    lines = [
        "## Translator coverage",
        "",
        f"Overall: **{percent(root.get('line-rate'))} lines**, "
        f"**{percent(root.get('branch-rate'))} branches**.",
        "",
        "| Assembly | Lines | Branches |",
        "| --- | ---: | ---: |",
    ]
    packages = root.find("packages")
    if packages is not None:
        for package in sorted(packages, key=lambda item: item.get("name", "")):
            lines.append(
                f"| `{package.get('name', 'unknown')}` | "
                f"{percent(package.get('line-rate'))} | {percent(package.get('branch-rate'))} |"
            )
    lines.append("")
    rendered = "\n".join(lines)
    print(rendered)

    summary = args.summary or (
        Path(os.environ["GITHUB_STEP_SUMMARY"]) if "GITHUB_STEP_SUMMARY" in os.environ else None
    )
    if summary:
        with summary.open("a", encoding="utf-8") as output:
            output.write(rendered)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
