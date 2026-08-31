#!/usr/bin/env python3
"""Generate runtime/include/region/<region>.h from the guest-address identities the runtime uses.

The runtime spells every guest address by its PAL (RMCP01) identity (see region/guest_region.h).
This tool scans the runtime for those identities and writes, per region, the `#define MKW_G_<pal>
<region>` table the compiler and the translator both resolve them through, plus the region facts
(game code, TV format, SC area/game indices, product code).

  rmcp01: identity table.
  rmce01: code addresses through the community PAL->NTSC-U chunk table (mkw-sp port.py, MIT),
          cross-checked against the validated NTSC-U function map when present; data addresses
          from projects/mkwii-ntsc-u/data_addresses.txt (each line `<pal> <ntsc> <evidence>`), which
          records the disassembly evidence for every value. Any identity that neither source
          resolves aborts the generation - a region table is complete or it is not written.
"""
import os, re, sys, json, collections

ROOT = os.path.normpath(os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", ".."))
RUNTIME = os.path.join(ROOT, "runtime")
OUT_DIR = os.path.join(RUNTIME, "include", "region")

PAL_TEXT = [(0x80004000, 0x80006460), (0x800072C0, 0x80244DE0), (0x805103B4, 0x8088F400)]

# Console setting values, named rather than spelled as the integers they happen to be. The
# indices are the RVL SDK's own SCGetProductArea/SCGetProductGameRegion enums.
SC_AREA = {"JPN": 0, "USA": 1, "EUR": 2, "AUS": 3, "BRA": 4, "TWN": 5, "ROC": 6,
           "KOR": 7, "HKG": 8, "ASI": 9, "LTN": 10, "SAF": 11, "CHN": 12}
SC_GAME = {"JP": 0, "US": 1, "EU": 2, "KR": 3}
VI_TV_FORMAT = {"NTSC": 0, "PAL": 1}


class Region:
    """One region's constant facts plus where its ported evidence lives.

    `project` is the directory holding that region's MAP.txt (function map, ported and validated
    by tools/region/port_map.py) and data_addresses.txt (data globals, ported and confirmed by
    tools/region/port_data_addresses.py). PAL has neither: it is the identity region the runtime
    is written against, so its table maps every address to itself.
    """

    def __init__(self, header, game_id, letter, tv, area, game, product_code, arena_lo,
                 display, project=None):
        self.header, self.game_id, self.letter = header, game_id, letter
        self.tv, self.area, self.game = tv, area, game
        self.product_code, self.arena_lo = product_code, arena_lo
        self.display, self.project = display, project

    @property
    def game_code(self):
        return int.from_bytes(self.game_id[:4].encode("ascii"), "big")


REGIONS = [
    Region("rmcp01", "RMCP01", "P", "PAL", "EUR", "EU", "LEH", 0x80399180,
           "Mario Kart Wii PAL (RMCP01)"),
    Region("rmce01", "RMCE01", "E", "NTSC", "USA", "US", "LU", 0x80394E00,
           "Mario Kart Wii NTSC-U (RMCE01)", project="mkwii-ntsc-u"),
    Region("rmcj01", "RMCJ01", "J", "NTSC", "JPN", "JP", "LJ", 0x80398B00,
           "Mario Kart Wii NTSC-J (RMCJ01)", project="mkwii-ntsc-j"),
    Region("rmck01", "RMCK01", "K", "NTSC", "KOR", "KR", "LKM", 0x803871A0,
           "Mario Kart Wii NTSC-K (RMCK01)", project="mkwii-ntsc-k"),
]
REGIONS_BY_HEADER = {r.header: r for r in REGIONS}


TOKEN_PATTERNS = [
    re.compile(r"(?:PPC_NATIVE_OVERRIDE(?:_VOID)?|GX_FATAL_STUB)\s*\(\s*([0-9A-Fa-f]{8})\b"),
    re.compile(r"MKW_GADDR\s*\(\s*([0-9A-Fa-f]{8})\s*\)"),
    re.compile(r"MKW_GUEST_FUNC\s*\(\s*([0-9A-Fa-f]{8})\s*\)"),
]


def scan_tokens():
    """Every PAL identity token as it is spelled in the sources (case preserved)."""
    spellings = collections.defaultdict(set)
    for dp, _, fns in os.walk(RUNTIME):
        if "third_party" in dp or os.path.join("include", "region") in dp:
            continue
        for fn in fns:
            if not fn.endswith((".cpp", ".h", ".hpp", ".inc")):
                continue
            text = open(os.path.join(dp, fn), encoding="utf-8", errors="replace").read()
            for pat in TOKEN_PATTERNS:
                for m in pat.finditer(text):
                    spellings[int(m.group(1), 16)].add(m.group(1))
    return spellings


def is_text(addr):
    return any(a <= addr < b for a, b in PAL_TEXT)


def load_chunks(project):
    path = os.path.join(ROOT, "projects", project, "region_port.json")
    if os.path.exists(path):
        def num(v):
            return int(v, 16) if isinstance(v, str) else int(v)
        return [tuple(num(v) for v in c) for c in json.load(open(path))["chunks"]]
    sys.exit(f"missing {path} (produced by tools/region/port_map.py)")


def port_code(chunks, addr):
    for s, e, d in chunks:
        if s <= addr < e:
            return addr - s + d
    return None


def load_data_table(project):
    path = os.path.join(ROOT, "projects", project, "data_addresses.txt")
    table = {}
    if not os.path.exists(path):
        return table, path
    for line in open(path):
        line = line.split("#", 1)[0].strip()
        if not line:
            continue
        parts = line.split()
        if len(parts) < 2 or parts[1] == "?":
            continue
        table[int(parts[0], 16)] = int(parts[1], 16)
    return table, path


def load_map(path):
    entries = []
    for line in open(path):
        line = line.strip()
        if line and not line.startswith("#"):
            addr, _, name = line.partition(" ")
            entries.append((int(addr, 16), name.strip()))
    entries.sort()
    return entries


def load_validated_map(project):
    path = os.path.join(ROOT, "projects", project, "MAP.txt")
    if not os.path.exists(path):
        return None
    return load_map(path)


LABEL_NAME = re.compile(r"(_switch$|_caseD_|^0x)")


def containing_function(pal_entries, addr):
    """Nearest PAL map entry at or below addr that names a real function (not a jump-table
    label or an unnamed entry), and addr's offset inside it."""
    import bisect
    addrs = [a for a, _ in pal_entries]
    i = bisect.bisect_right(addrs, addr) - 1
    while i >= 0 and LABEL_NAME.search(pal_entries[i][1]):
        i -= 1
    if i < 0:
        return None, None, None
    start, name = pal_entries[i]
    return start, name, addr - start


def write_header(region, mapping, spellings, names, provenance):
    """Emit the region's header: the facts, then one MKW_G_/MKW_F_ pair per identity.

    Both forms are written out fully rather than assembled by token pasting at use time. The
    consumer macros are then a single paste each (region/guest_region.h), what a define expands
    to is visible in this file, and the PAL build keeps each identity's original spelling in the
    symbol name it generates (func_80044b30 stays lower case) without any special casing.
    """
    path = os.path.join(OUT_DIR, f"{region.header}.h")
    lines = [
        "// AUTO-GENERATED by tools/region/gen_region_headers.py - DO NOT EDIT.",
        f"// Guest address table for {region.display}: PAL identity -> {region.header} address.",
        f"// {provenance}",
        "#pragma once",
        "",
        f'#define MKW_REGION_GAME_ID "{region.game_id}"',
        f"#define MKW_REGION_LETTER '{region.letter}'",
        f'#define MKW_REGION_GAME_CODE 0x{region.game_code:08X}u  // "{region.game_id[:4]}"',
        f"#define MKW_REGION_VI_TV_FORMAT {VI_TV_FORMAT[region.tv]}u  // VI_{region.tv}",
        f"#define MKW_REGION_SC_AREA {SC_AREA[region.area]}u  // SC area {region.area}",
        f"#define MKW_REGION_SC_GAME_REGION {SC_GAME[region.game]}u  // SC game region {region.game}",
        f'#define MKW_REGION_SC_PRODUCT_CODE "{region.product_code}"',
        f"#define MKW_REGION_MEM1_ARENA_LO 0x{region.arena_lo:08X}u  // initial stack top / arena lo",
        "",
        "// PAL identity -> this region. MKW_G_ is the address, MKW_F_ the translated function's",
        "// C symbol; the trailing comment is the PAL map's name for it, for reading only.",
    ]
    for pal in sorted(mapping):
        target = mapping[pal]
        for spelling in sorted(spellings[pal]):
            token = spelling if target == pal else f"{target:08X}"
            comment = f"  // {names[pal]}" if names.get(pal) else ""
            lines.append(f"#define MKW_G_{spelling} 0x{token}u{comment}")
            lines.append(f"#define MKW_F_{spelling} func_{token}")
    lines.append("")
    os.makedirs(OUT_DIR, exist_ok=True)
    open(path, "w").write("\n".join(lines))
    return path


def main():
    spellings = scan_tokens()
    identities = sorted(spellings)
    pal_entries = load_map(os.path.join(ROOT, "projects", "mkwii", "MAP.txt"))
    pal_names = {a: n for a, n in pal_entries}
    provisional = "--provisional" in sys.argv
    only = [a for a in sys.argv[1:] if not a.startswith("-")]
    print(f"{len(identities)} guest-address identities in the runtime "
          f"({sum(1 for a in identities if is_text(a))} code, "
          f"{sum(1 for a in identities if not is_text(a))} data)")

    failures = 0
    for region in REGIONS:
        if only and region.header not in only:
            continue
        if region.project is None:
            # The identity region: every address maps to itself by definition.
            path = write_header(region, {a: a for a in identities}, spellings, pal_names,
                                "Identity table: the runtime is written against this executable.")
            print(f"wrote {path}")
            continue

        mapping, problems, provisional_data = port_region(region, identities, pal_entries,
                                                          provisional)
        if problems:
            failures += 1
            print(f"{region.header}: {len(problems)} identity/identities did not resolve:")
            for line in problems[:20]:
                print(line)
            print(f"  (no header written for {region.header})")
            continue
        note = (f"Code: PAL->{region.letter} chunk table validated against projects/"
                f"{region.project}/MAP.txt; data: projects/{region.project}/data_addresses.txt "
                f"(evidence recorded per entry).")
        if provisional_data:
            note = ("PROVISIONAL - " + str(len(provisional_data)) +
                    " data identities left at their PAL value and are wrong at run time. " + note)
        path = write_header(region, mapping, spellings, pal_names, note)
        print(f"wrote {path} ({len(mapping)} identities"
              + (f", {len(provisional_data)} provisional" if provisional_data else "") + ")")
    return 1 if failures else 0


def port_region(region, identities, pal_entries, provisional):
    """Resolve every identity for one region from that region's own ported evidence."""
    chunks = load_chunks(region.project)
    data_table, _ = load_data_table(region.project)
    validated_entries = load_validated_map(region.project)
    validated = {a for a, _ in validated_entries} if validated_entries is not None else None
    validated_by_name = {}
    if validated_entries is not None:
        for a, n in validated_entries:
            validated_by_name.setdefault(n, a)

    mapping, problems, provisional_data = {}, [], []
    for pal in identities:
        if is_text(pal):
            ported = port_code(chunks, pal)
            if ported is None:
                problems.append(f"  {pal:08X}: code address outside every port chunk")
                continue
            if validated is not None and ported not in validated:
                # Not a function entry: a jump-table label or a mid-function hook. It is right
                # exactly when it sits at the same offset inside a validated containing function.
                start, name, offset = containing_function(pal_entries, pal)
                target_start = validated_by_name.get(name) if name else None
                if target_start is None or target_start + offset != ported:
                    problems.append(
                        f"  {pal:08X} -> {ported:08X}: not a validated function entry"
                        f" (containing PAL function {name} not confirmed at the same offset)")
                    continue
            mapping[pal] = ported
        elif pal in data_table:
            mapping[pal] = data_table[pal]
        elif provisional:
            mapping[pal] = pal
            provisional_data.append(pal)
        else:
            problems.append(f"  {pal:08X}: data address absent from "
                            f"projects/{region.project}/data_addresses.txt")
    return mapping, problems, provisional_data


if __name__ == "__main__":
    sys.exit(main())
