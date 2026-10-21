#!/usr/bin/env python3
"""Generate runtime/include/guest_abi/guest_address_table.inc and runtime/include/guest_function_imports.inc.

The runtime names every guest address by its PAL (RMCP01) address. This scans the runtime for
those identities and writes one row per identity with its NTSC-U and NTSC-J address:

    MKW_GUEST_ADDRESS(<PAL>, <NTSC-U>, <NTSC-J>)

Identities the runtime calls through MKW_GUEST_FUNC also get a MKW_GUEST_FUNCTION_IMPORT row.

Code addresses go through each region's chunk table (projects/<region>/region_port.json) and must
be a function entry in that region's validated MAP.txt, or sit at the same offset inside the same
validated function. Data addresses come from projects/<region>/data_addresses.txt. An identity
that does not resolve aborts the generation.

Usage: python -I tools/region/gen_guest_address_table.py
"""
import bisect
import json
import os
import re
import sys

ROOT = os.path.normpath(os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", ".."))
RUNTIME = os.path.join(ROOT, "runtime")
OUT = os.path.join(RUNTIME, "include", "guest_abi", "guest_address_table.inc")
IMPORTS_OUT = os.path.join(RUNTIME, "include", "guest_function_imports.inc")

PAL_TEXT = [(0x80004000, 0x80006460), (0x800072C0, 0x80244DE0), (0x805103B4, 0x8088F400)]

# Column order is the GuestRegion::Id order in guest_abi/guest_region.h.
REGIONS = ["mkwii-ntsc-u", "mkwii-ntsc-j"]

HEX = r"([0-9A-Fa-f]{8})(?![0-9A-Fa-f])"
TOKEN_PATTERNS = [re.compile(p) for p in (
    r"PPC_NATIVE_OVERRIDE(?:_VOID)?\s*\(\s*" + HEX,
    r"GX_FATAL_STUB\s*\(\s*" + HEX,
    r"MKW_GADDR\s*\(\s*" + HEX,
    r"MKW_GADDR_CT\s*\(\s*" + HEX,
    r"MKW_GUEST_FUNC\s*\(\s*" + HEX,
    r"MKW_KNOWN_TYPED_NATIVE\s*\(\s*" + HEX,
    r"MKW_KNOWN_NATIVE_CPU_CALL\s*\(\s*" + HEX,
    r"REGISTER_NATIVE_FUNCTION(?:_AS)?\s*\(\s*0x" + HEX,
    r"REGISTER_TRANSLATED_FUNCTION\s*\(\s*0x" + HEX,
    r"GuestRegion::(?:Is|Address)<0x" + HEX,
)]
IMPORT_PATTERN = re.compile(r"MKW_GUEST_FUNC\s*\(\s*" + HEX)
COMMENTS = re.compile(r"//[^\n]*|/\*.*?\*/", re.S)
LABEL_NAME = re.compile(r"(_switch$|_caseD_|^0x)")


def scan_identities():
    found, imports = set(), set()
    for directory, _, files in os.walk(RUNTIME):
        if directory.startswith(os.path.join(RUNTIME, "third_party")):
            continue
        for name in files:
            path = os.path.join(directory, name)
            if not name.endswith((".cpp", ".h", ".hpp", ".inc")) or path in (OUT, IMPORTS_OUT):
                continue
            with open(path, encoding="utf-8", errors="replace") as f:
                text = COMMENTS.sub("", f.read())
            for pattern in TOKEN_PATTERNS:
                found.update(int(m.group(1), 16) for m in pattern.finditer(text))
            imports.update(int(m.group(1), 16) for m in IMPORT_PATTERN.finditer(text))
    return sorted(found), sorted(imports)


def is_text(address):
    return any(start <= address < end for start, end in PAL_TEXT)


def load_map(path):
    entries = []
    with open(path, encoding="utf-8") as f:
        for line in f:
            line = line.strip()
            if line and not line.startswith("#"):
                address, _, name = line.partition(" ")
                entries.append((int(address, 16), name.strip()))
    entries.sort()
    return entries


def load_chunks(project):
    with open(os.path.join(ROOT, "projects", project, "region_port.json"), encoding="utf-8") as f:
        return [tuple(int(v, 16) for v in chunk) for chunk in json.load(f)["chunks"]]


def load_data_table(project):
    table = {}
    with open(os.path.join(ROOT, "projects", project, "data_addresses.txt"), encoding="utf-8") as f:
        for line in f:
            parts = line.split("#", 1)[0].split()
            if len(parts) >= 2 and parts[1] != "?":
                table[int(parts[0], 16)] = int(parts[1], 16)
    return table


def containing_function(entries, addresses, address):
    i = bisect.bisect_right(addresses, address) - 1
    while i >= 0 and LABEL_NAME.search(entries[i][1]):
        i -= 1
    if i < 0:
        return None, None
    return entries[i][1], address - entries[i][0]


def port_region(project, identities, pal_entries):
    chunks = load_chunks(project)
    data = load_data_table(project)
    target_entries = load_map(os.path.join(ROOT, "projects", project, "MAP.txt"))
    target_starts = {a for a, _ in target_entries}
    target_by_name = {}
    for address, name in target_entries:
        target_by_name.setdefault(name, address)
    pal_addresses = [a for a, _ in pal_entries]

    mapping, problems = {}, []
    for pal in identities:
        if not is_text(pal):
            if pal in data:
                mapping[pal] = data[pal]
            else:
                problems.append(f"{pal:08X}: data address missing from projects/{project}/data_addresses.txt")
            continue
        ported = next((pal - s + d for s, e, d in chunks if s <= pal < e), None)
        if ported is None:
            problems.append(f"{pal:08X}: code address outside every chunk")
            continue
        if ported not in target_starts:
            name, offset = containing_function(pal_entries, pal_addresses, pal)
            start = target_by_name.get(name) if name else None
            if start is None or start + offset != ported:
                problems.append(f"{pal:08X} -> {ported:08X}: not a validated entry of {project}/MAP.txt "
                                f"(containing function {name} not confirmed at the same offset)")
                continue
        mapping[pal] = ported
    return mapping, problems


def write(path, macro, header, rows, columns, names):
    lines = ["// AUTO-GENERATED by tools/region/gen_guest_address_table.py - DO NOT EDIT."] + header
    for pal in rows:
        row = ", ".join([f"{pal:08X}"] + [f"{column[pal]:08X}" for column in columns])
        name = names.get(pal)
        lines.append(f"{macro}({row})" + (f"  // {name}" if name and not name.startswith("0x") else ""))
    with open(path, "w", encoding="utf-8", newline="\n") as f:
        f.write("\n".join(lines) + "\n")


def main():
    identities, imports = scan_identities()
    pal_entries = load_map(os.path.join(ROOT, "projects", "mkwii", "MAP.txt"))
    names = dict(pal_entries)
    columns, failed = [], False
    for project in REGIONS:
        mapping, problems = port_region(project, identities, pal_entries)
        if problems:
            failed = True
            print(f"{project}: {len(problems)} identities did not resolve:")
            for problem in problems:
                print("  " + problem)
        columns.append(mapping)
    if failed:
        return 1

    write(OUT, "MKW_GUEST_ADDRESS", [
        "// PAL identity -> address in each region: MKW_GUEST_ADDRESS(PAL, NTSC-U, NTSC-J).",
        "// Sorted by PAL address; guest_abi/guest_region.h relies on that.",
    ], identities, columns, names)
    write(IMPORTS_OUT, "MKW_GUEST_FUNCTION_IMPORT", [
        "// Base translations the runtime calls directly: MKW_GUEST_FUNCTION_IMPORT(PAL, NTSC-U, NTSC-J).",
    ], imports, columns, names)
    code = sum(1 for a in identities if is_text(a))
    print(f"wrote {len(identities)} identities ({code} code, {len(identities) - code} data), "
          f"{len(imports)} function imports")
    return 0


if __name__ == "__main__":
    sys.exit(main())
