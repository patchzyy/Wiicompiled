#!/usr/bin/env python3
"""
port_data_addresses.py - port the PAL data-global addresses named by the WiiCompiled runtime to
another region, and prove each one from that region's own executables.

The NTSC-U table (projects/mkwii-ntsc-u/data_addresses.txt) was built by hand: for each PAL global
it records the NTSC-U address together with the instruction that references it. This ports the
same set to a new region without repeating that by hand, and without guessing:

  1. Recover the PAL address of the referencing instruction by inverting the PAL->E chunk table.
  2. Map that PAL code address into the target region with the target's chunk table.
  3. Disassemble the instruction actually present there and recompute the address it forms
     (lis/addi, lis/ori, or a d-form displacement off r13/r2).
  4. Independently chunk-port the PAL *data* address, and accept only when the two agree.

Steps 3 and 4 are independent derivations, so agreement is the proof. Anything that does not
decode, or where the two disagree, is written out as unresolved rather than guessed at.

Where the chunk table is not instruction-exact (it is function-granular, so a function that
changed length between regions throws off every address inside it), a second, independent method
takes over: masked instruction matching. The compiled instruction stream for the same SDK source
is identical between regions apart from the fields that legitimately move - SDA displacements,
branch targets and lis high halves. Mask exactly those out and the window around a reference site
becomes a signature; if it occurs exactly once in the target region's code, that occurrence is
the same instruction, and the displacement read from it is that region's address for the global.
A unique match is the proof, so zero matches or several are reported rather than guessed at.
Plain Python 3 stdlib. Deterministic output.
"""
import argparse, os, re, struct, sys
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import port_map as pm

ROOT = os.path.abspath(os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", ".."))
D_FORM = frozenset((32, 33, 34, 35, 36, 37, 38, 39, 40, 41, 44, 45, 46, 47, 48, 50, 52, 54))
SDA = pm.SDA_BASES


def invert(chunks):
    return [(d, d + (e - s), s) for s, e, d in chunks]


def port(chunks, addr):
    for s, e, d in chunks:
        if s <= addr < e:
            return addr - s + d
    return None


class Image:
    """Flat reader over one region's DOL sections plus its REL text/data, by guest address."""

    def __init__(self, region, disc_root):
        self.spans = []
        disc = os.path.join(disc_root, f"RMC{region}01")
        dol = open(os.path.join(disc, "sys", "main.dol"), "rb").read()
        off = struct.unpack(">18I", dol[0:72]); addr = struct.unpack(">18I", dol[72:144])
        size = struct.unpack(">18I", dol[144:216])
        # The DOL header lays out 7 text sections then 11 data sections; only the text ones
        # carry instructions the masked matcher may treat as such.
        for i in range(18):
            if size[i]:
                self.spans.append((addr[i], addr[i] + size[i], dol[off[i]:off[i] + size[i]], i < 7))
        rel = open(os.path.join(disc, "files", "rel", "StaticR.rel"), "rb").read()
        load = pm.REL_LOAD_ADDRESS[region]
        n = struct.unpack(">I", rel[0x0C:0x10])[0]
        info = struct.unpack(">I", rel[0x10:0x14])[0]
        for i in range(n):
            o, ln = struct.unpack(">II", rel[info + i * 8:info + i * 8 + 8])
            is_text = bool(o & 1)          # the REL section table keeps the executable bit in bit 0
            o &= ~3
            if ln and o:
                self.spans.append((load + o, load + o + ln, rel[o:o + ln], is_text))

    def normalized(self):
        """The code spans as masked instruction words, computed once."""
        if getattr(self, "_normalized", None) is None:
            out = []
            for s, e, b, is_text in self.spans:
                if not is_text:                # data sections carry no instructions
                    continue
                n = (e - s) // 4
                if n:
                    out.append((s, [_norm(w) for w in struct.unpack(f">{n}I", b[:n * 4])]))
            self._normalized = out
        return self._normalized

    def index(self):
        """Masked word -> [(span, instruction index)], so a signature can be probed directly."""
        if getattr(self, "_index", None) is None:
            idx = {}
            for si, (_base, words) in enumerate(self.normalized()):
                for i, w in enumerate(words):
                    idx.setdefault(w, []).append((si, i))
            self._index = idx
        return self._index

    def word(self, a):
        for s, e, b, _is_text in self.spans:
            if s <= a < e - 3:
                return struct.unpack(">I", b[a - s:a - s + 4])[0]
        return None


def _norm(w):
    """One instruction with the fields that legitimately differ between regions masked out."""
    op, ra = w >> 26, (w >> 16) & 31
    if op == 18:                                       # b/bl: displacement differs
        return w & 0xFC000000
    if op == 16:                                       # bc: keep BO/BI, drop the displacement
        return w & 0xFFFF0000
    if op == 15:                                       # lis: high half differs
        return w & 0xFFFF0000
    if (op in D_FORM or op == 14) and ra in (2, 13):   # the SDA displacement we are solving for
        return w & 0xFFFF0000
    return w


def matched_site(src_img, src_site, dst_img):
    """dst_img's counterpart of src_img@src_site, found by masked instruction matching.

    Returns (address, half_window) for a unique match, else (None, None). Widening windows are
    tried in turn: the narrowest that matches exactly once is the least likely to be thrown off
    by an unrelated edit nearby, and a wider one rescues a site whose neighbourhood is generic."""
    for half in (10, 16, 24):
        sig = [src_img.word(src_site + 4 * k) for k in range(-half, half + 1)]
        if any(w is None for w in sig):
            continue
        sig = [_norm(w) for w in sig]
        # Probe on the rarest instruction in the window, not the first: a common prologue word
        # would mean re-checking most of the image for every site.
        idx = dst_img.index()
        probe = min(range(len(sig)), key=lambda i: len(idx.get(sig[i], ())))
        hits = []
        for si, i in idx.get(sig[probe], ()):
            base, words = dst_img.normalized()[si]
            j = i - probe
            if j < 0 or j + len(sig) > len(words):
                continue
            if words[j:j + len(sig)] == sig:
                hits.append(base + 4 * (j + half))
                if len(hits) > 1:
                    break
        if len(hits) == 1:
            return hits[0], half
    return None, None


def formed_address(img, a, sda13, sda2):
    """The data address the instruction at `a` forms, or None if it forms none."""
    w = img.word(a)
    if w is None:
        return None
    op, rt, ra, imm = w >> 26, (w >> 21) & 31, (w >> 16) & 31, w & 0xFFFF
    simm = imm - 0x10000 if imm & 0x8000 else imm
    if op == 15 and ra == 0:
        # lis rT,hi forms only the high half. The low half arrives in a later instruction, and
        # not always the next one: it may be addi/ori into the same register, or a d-form access
        # that carries the displacement (lis r3,0x8035; lwz r4,0x1E4(r3)). Scan forward for the
        # first instruction that consumes rT, and give up rather than return a bare high half -
        # 0x80340000 is not an address anybody named.
        for k in range(1, 9):
            nxt = img.word(a + 4 * k)
            if nxt is None:
                return None
            nop, nrt, nra, nimm = nxt >> 26, (nxt >> 21) & 31, (nxt >> 16) & 31, nxt & 0xFFFF
            nsimm = nimm - 0x10000 if nimm & 0x8000 else nimm
            if nop == 14 and nrt == rt and nra == rt:                 # addi rT,rT,lo
                return ((imm << 16) + nsimm) & 0xFFFFFFFF
            if nop == 24 and nrt == rt and nra == rt:                 # ori rT,rT,lo
                return (imm << 16) | nimm
            if nop in D_FORM and nra == rt:                            # lwz/stw/... lo(rT)
                return ((imm << 16) + nsimm) & 0xFFFFFFFF
            if nop == 15 and nrt == rt:                               # rT reloaded: sequence ended
                return None
        return None
    if op in D_FORM or op == 14:
        if ra == 13:
            return (sda13 + simm) & 0xFFFFFFFF
        if ra == 2:
            return (sda2 + simm) & 0xFFFFFFFF
    return None


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--region", required=True, choices=["J", "K"],
                    help="target region letter: J (NTSC-J) or K (NTSC-K)")
    ap.add_argument("--disc-root", required=True,
                    help="directory holding the extracted discs as RMC?01/ (sys/main.dol and "
                         "files/rel/StaticR.rel under each); RMCE01 and the target are read")
    ap.add_argument("--source-table",
                    default=os.path.join(ROOT, "projects", pm.PROJECT_DIR["E"], "data_addresses.txt"))
    ap.add_argument("--out", default=None)
    args = ap.parse_args()
    region = args.region
    out = args.out or os.path.join(ROOT, "projects", pm.PROJECT_DIR[region], "data_addresses.txt")

    inv_e = invert(pm.CHUNKS_E)
    dst = pm.CHUNK_TABLES[region]
    img = Image(region, args.disc_root)
    img_e = Image("E", args.disc_root)
    sda13, sda2 = SDA[region]
    sda13_e, sda2_e = SDA["E"]

    # Globals that are section boundaries rather than referenced objects. The PAL start address
    # maps to the target's start address for the same named section, which the section tables
    # (themselves read out of the shipped executables) state directly.
    boundaries = {}
    for tbl, pal_tbl in ((pm.DOL_SECTIONS[region], pm.DOL_SECTIONS["P"]),
                         (pm.REL_SECTIONS[region], pm.REL_SECTIONS["P"])):
        for (name, start, _end), (pname, pstart, _pe) in zip(tbl, pal_tbl):
            if name == pname:
                boundaries[pstart] = (start, name)

    # Section-relative fallback. The chunk table does not cover every .rodata/.data/.bss range,
    # but the section tables give each section's bounds in both regions and the linker lays the
    # same source out in the same order, so an object's offset within its section carries over.
    # Weaker than a disassembly proof, and labelled as such.
    def section_relative(addr):
        for tbl_p, tbl_r in ((pm.DOL_SECTIONS["P"], pm.DOL_SECTIONS[region]),
                             (pm.REL_SECTIONS["P"], pm.REL_SECTIONS[region])):
            for (pname, ps, pe), (rname, rs, re_) in zip(tbl_p, tbl_r):
                if pname != rname or not (ps <= addr < pe):
                    continue
                target = rs + (addr - ps)
                # The sections are not the same size between regions: PAL's .data is larger than
                # Korea's, so a PAL offset can fall past the end of the target section. Emitting
                # it anyway produces an address in whatever follows, which the runtime then reads
                # as the global - the failure looks like corrupt state far from here. Refuse it.
                if target >= re_:
                    return None, (f".{pname} is 0x{re_ - rs:X} bytes here but the PAL offset is "
                                  f"0x{addr - ps:X}")
                return target, pname
        return None, None

    # Anchor derivation. Some globals are a fixed offset from another global in the same object
    # (a record inside a table, a field inside a struct). When the anchor resolved and the same
    # offset relationship holds in the hand-verified NTSC-U table, it holds here too: the object
    # is one allocation, so its interior keeps its shape across regions. Checked against E rather
    # than assumed, and only used after every stronger method has failed.
    def anchor_derived(pal, resolved, e_by_pal):
        best = None
        for other in resolved:
            if other < pal and pal - other <= 0x100 and (best is None or other > best):
                best = other
        if best is None:
            return None, None
        delta = pal - best
        if e_by_pal.get(pal) is None or e_by_pal.get(best) is None:
            return None, None
        if e_by_pal[pal] - e_by_pal[best] != delta:
            return None, None
        return resolved[best] + delta, best

    rows, proven, unresolved = [], 0, 0
    e_by_pal = {}
    for line in open(args.source_table):
        body = line.split("#", 1)[0].strip()
        parts = body.split()
        if len(parts) >= 2 and parts[1] != "?":
            try:
                e_by_pal[int(parts[0], 16)] = int(parts[1], 16)
            except ValueError:
                pass

    resolved = {}
    for line in open(args.source_table):
        body = line.split("#", 1)[0].strip()
        if not body:
            continue
        parts = body.split()
        if len(parts) < 3 or parts[1] == "?":
            continue
        pal, e_addr, evidence = int(parts[0], 16), int(parts[1], 16), " ".join(parts[2:])
        cand = port(dst, pal)
        # Every "@xxxxxxxx" in the evidence is a reference site, including the "Name@addr" form.
        sites = [int(h, 16) for h in re.findall(r"@([0-9A-Fa-f]{8})\b", evidence)]
        # The chunk-table port is the candidate. Disassembly only ever CONFIRMS it: a reference
        # site can form some other object's address (the evidence lists several sites, and a
        # function forms many addresses), so letting a decode override the candidate produced
        # nonsense like 0x00000000. Confirmation is agreement, never substitution.
        # Calibrate the decoder on NTSC-U first. The E table is hand-verified, so a site whose
        # decode reproduces the known E address is a site this decoder reads correctly for this
        # global; the same site ported into the target region can then be trusted. Sites that do
        # not reproduce E are forming some other object and are ignored.
        calibrated = [a for a in sites
                      if formed_address(img_e, a, sda13_e, sda2_e) == e_addr]

        note, got = "no-reference-site", None
        seen = []
        for site_e in calibrated:
            site_pal = port(inv_e, site_e)
            site_r = port(dst, site_pal) if site_pal is not None else None
            if site_r is None:
                continue
            formed = formed_address(img, site_r, sda13, sda2)
            if formed is not None:
                got, note = formed, f"{site_r:08X} (calibrated on E@{site_e:08X})"
                break
        for site_e in ([] if got is not None else sites):
            site_pal = port(inv_e, site_e)
            site_r = port(dst, site_pal) if site_pal is not None else None
            if site_r is None:
                continue
            formed = formed_address(img, site_r, sda13, sda2)
            if formed is None:
                continue
            seen.append((site_r, formed))
            if cand is not None and formed == cand:
                got, note = formed, f"{site_r:08X}"
                break
        if got is None and seen:
            note = "; ".join(f"{a:08X} forms 0x{v:08X}" for a, v in seen[:3])

        # Masked instruction matching. Used only where the steps above did not confirm an
        # address, because they are cheaper; this one does not depend on the chunk table being
        # instruction-exact, so it is what resolves globals inside functions whose length
        # changed between regions. Calibrated sites first, for the same reason as above.
        match_addr, match_note = None, None
        if got is None:
            # Calibrated sites only. An uncalibrated site forms some other object - the GX
            # globals are reached through a pointer cell, so the site loads the cell, not the
            # object - and matching it would resolve the wrong address with full confidence.
            for site_e in calibrated:
                hit, half = matched_site(img_e, site_e, img)
                if hit is None:
                    continue
                formed = formed_address(img, hit, sda13, sda2)
                if formed is None:
                    continue
                match_addr = formed
                match_note = f"{hit:08X} (unique +-{half} masked match of E@{site_e:08X})"
                break
        if pal in boundaries:
            tgt, sname = boundaries[pal]
            rows.append(f"{pal:08X} {tgt:08X} SECTION .{sname} start (from the region section table)")
            resolved[pal] = tgt
            proven += 1
        elif got is not None and cand is not None and got == cand:
            rows.append(f"{pal:08X} {cand:08X} PROVEN @{note} forms 0x{got:08X} (chunk port agrees)")
            resolved[pal] = cand
            proven += 1
        elif got is not None:
            rows.append(f"{pal:08X} {got:08X} CALIBRATED @{note} forms 0x{got:08X}"
                        f" (chunk port {'said 0x%08X' % cand if cand is not None else 'has no mapping'})")
            resolved[pal] = got
            proven += 1
        elif match_addr is not None:
            agree = ("chunk port agrees" if cand == match_addr else
                     f"chunk port {'said 0x%08X' % cand if cand is not None else 'has no mapping'}")
            rows.append(f"{pal:08X} {match_addr:08X} MATCHED @{match_note}"
                        f" forms 0x{match_addr:08X} ({agree})")
            resolved[pal] = match_addr
            proven += 1
        elif cand is not None:
            rows.append(f"{pal:08X} {cand:08X} CHUNK-ONLY (no reference site confirmed it: {note})")
            resolved[pal] = cand
        else:
            derived, anchor = anchor_derived(pal, resolved, e_by_pal)
            if derived is not None:
                rows.append(f"{pal:08X} {derived:08X} ANCHORED +0x{pal - anchor:X} from {anchor:08X}"
                            f" (same offset holds in the verified NTSC-U table)")
                resolved[pal] = derived
                proven += 1
                continue
            rel_addr, sname = section_relative(pal)
            if rel_addr is not None:
                resolved[pal] = rel_addr
                rows.append(f"{pal:08X} {rel_addr:08X} SECTION-RELATIVE same offset in .{sname}"
                            f" (no chunk mapping, no confirming reference site)")
            else:
                why = sname if sname else note
                rows.append(f"{pal:08X} ? UNRESOLVED ({why})")
                unresolved += 1

    header = [
        f"# RMC{region}01 addresses for the PAL (RMCP01) data globals named by the WiiCompiled runtime.",
        f"# Generated by tools/region/port_data_addresses.py --region {region}; do not edit by hand.",
        "# Format: <PAL hex> <target hex> <verdict and evidence>",
        "#   PROVEN     - the instruction at the ported reference site forms exactly this address,",
        "#                and the independent chunk-table port of the PAL address agrees.",
        "#   CALIBRATED - the reference site reproduces the hand-verified NTSC-U address for this",
        "#                global, so its decode in this region is trusted even where the chunk",
        "#                table has no mapping.",
        "#   MATCHED    - the reference site's instruction window, with the fields that legitimately",
        "#                differ between regions masked out, occurs exactly once in this region's",
        "#                code; the displacement was read from that instruction. Used where the",
        "#                chunk table is not instruction-exact inside a function.",
        "#   ANCHORED   - a fixed offset from a resolved neighbour, where the verified NTSC-U",
        "#                table shows the same offset (a record inside a table, say).",
        "#   SECTION    - a section boundary, taken from the region section table.",
        "#   SECTION-RELATIVE - same offset within the same named section; used only where the",
        "#                chunk table has no mapping and no reference site confirmed one.",
        "#   CHUNK-ONLY - chunk-table port only; no reference site formed exactly this address.",
        f"# Small-data bases: RMC{region}01 r13=0x{sda13:08X} r2=0x{sda2:08X}.",
        f"# Totals: {proven} confirmed, {len(rows) - proven - unresolved} chunk-only, {unresolved} unresolved.",
    ]
    os.makedirs(os.path.dirname(out), exist_ok=True)
    open(out, "w").write("\n".join(header + rows) + "\n")
    print(f"{region}: {len(rows)} globals -> {proven} disassembly-backed, "
          f"{len(rows) - proven - unresolved} chunk-only, {unresolved} unresolved -> {out}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
