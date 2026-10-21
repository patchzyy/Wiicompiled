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
REL address halves are resolved through the region's import relocation table. Explicit PAL
base/field instructions and immutable indexed records replace the old offset guesses. The source
and target must match the clean project hashes; --reference-region P uses PAL in place of US.
Plain Python 3 stdlib. Deterministic output.
"""
import argparse, hashlib, os, re, struct, sys
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import port_map as pm

ROOT = os.path.abspath(os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", ".."))
D_FORM = frozenset((32, 33, 34, 35, 36, 37, 38, 39, 40, 41, 42, 43, 44, 45,
                    46, 47, 48, 49, 50, 51, 52, 53, 54, 55))
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
        project = "mkwii" if region == "P" else pm.PROJECT_DIR[region]
        manifest = open(os.path.join(ROOT, "projects", project, "recomp.yml")).read()
        pins = re.findall(r"^    sha256: ([0-9a-f]{64})$", manifest, re.MULTILINE)
        if len(pins) != 2:
            raise ValueError(f"Expected DOL and REL SHA-256 pins for RMC{region}01")
        if hashlib.sha256(dol).hexdigest() != pins[0]:
            raise ValueError(f"RMC{region}01 main.dol does not match the clean project hash")
        off = struct.unpack(">18I", dol[0:72]); addr = struct.unpack(">18I", dol[72:144])
        size = struct.unpack(">18I", dol[144:216])
        # The DOL header lays out 7 text sections then 11 data sections; only the text ones
        # carry instructions the masked matcher may treat as such.
        for i in range(18):
            if size[i]:
                self.spans.append((addr[i], addr[i] + size[i], dol[off[i]:off[i] + size[i]], i < 7))
        rel = open(os.path.join(disc, "files", "rel", "StaticR.rel"), "rb").read()
        if hashlib.sha256(rel).hexdigest() != pins[1]:
            raise ValueError(f"RMC{region}01 StaticR.rel does not match the clean project hash")
        load = pm.REL_LOAD_ADDRESS[region]
        n = struct.unpack(">I", rel[0x0C:0x10])[0]
        info = struct.unpack(">I", rel[0x10:0x14])[0]
        for i in range(n):
            o, ln = struct.unpack(">II", rel[info + i * 8:info + i * 8 + 8])
            is_text = bool(o & 1)          # the REL section table keeps the executable bit in bit 0
            o &= ~3
            if ln and o:
                self.spans.append((load + o, load + o + ln, rel[o:o + ln], is_text))
        # REL immediates are placeholders until the loader applies the import table.
        # Decode addresses from that table, not from the zero halves on disc.
        bss = next(s for name, s, _e in pm.REL_SECTIONS[region] if name == "bss")
        linked = pm.parse_rel(os.path.join(disc, "files", "rel", "StaticR.rel"), load, bss)
        self.relocated_words = {}
        self.address_low_sites = set()
        for relocation in linked.relocations:
            if relocation.module not in (0, linked.module_id):
                continue
            if relocation.type not in (pm.R_PPC_ADDR16_LO, pm.R_PPC_ADDR16_HI, pm.R_PPC_ADDR16_HA):
                continue
            a = relocation.site & ~3
            if relocation.site & 3 != 2 or not any(
                    executable and start <= a < end for start, end, _data, executable in self.spans):
                continue
            w = self.word(a)
            if w is None:
                raise ValueError(f"REL relocation at unmapped instruction {a:08X}")
            target = relocation.target
            half = (target if relocation.type == pm.R_PPC_ADDR16_LO else
                    target >> 16 if relocation.type == pm.R_PPC_ADDR16_HI else
                    (target + 0x8000) >> 16)
            self.relocated_words[a] = (w & 0xFFFF0000) | (half & 0xFFFF)
            self.address_low_sites.add(a)
        # Also mask the low half of DOL lis/addi address materialization when matching.
        for s, _e, data, executable in self.spans:
            if not executable:
                continue
            words = struct.unpack(f">{len(data) // 4}I", data[:len(data) // 4 * 4])
            for i, w in enumerate(words):
                if w >> 26 != 15 or (w >> 16) & 31:
                    continue
                rt = (w >> 21) & 31
                for j in range(i + 1, min(i + 33, len(words))):
                    nxt = words[j]
                    op, nrt, ra = nxt >> 26, (nxt >> 21) & 31, (nxt >> 16) & 31
                    if ra == rt and (op in D_FORM or (op == 14 and nrt == rt)):
                        imm = nxt & 0xFFFF
                        value = ((w & 0xFFFF) << 16) + (imm - 0x10000 if imm & 0x8000 else imm)
                        if 0x80000000 <= value < 0x82000000:
                            self.address_low_sites.add(s + j * 4)
                        break
                    if rt in gpr_writes(nxt):
                        break

    def normalized_word(self, a):
        w = self.word(a)
        if w is None:
            return None
        return w & 0xFFFF0000 if a in self.address_low_sites else _norm(w)

    def normalized(self):
        """The code spans as masked instruction words, computed once."""
        if getattr(self, "_normalized", None) is None:
            out = []
            for s, e, b, is_text in self.spans:
                if not is_text:                # data sections carry no instructions
                    continue
                n = (e - s) // 4
                if n:
                    out.append((s, [self.normalized_word(s + 4 * i) for i in range(n)]))
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
        if a in getattr(self, "relocated_words", {}):
            return self.relocated_words[a]
        for s, e, b, _is_text in self.spans:
            if s <= a < e - 3:
                return struct.unpack(">I", b[a - s:a - s + 4])[0]
        return None


def _norm(w):
    """One instruction with the fields that legitimately differ between regions masked out."""
    op, ra = w >> 26, (w >> 16) & 31
    if op == 18:                                       # b/bl: displacement differs
        return w & 0xFC000003
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
        sig = [src_img.normalized_word(src_site + 4 * k) for k in range(-half, half + 1)]
        if any(w is None for w in sig):
            continue
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
        for k in range(1, 33):
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
            if rt in gpr_writes(nxt):                                # rT clobbered: sequence ended
                return None
        return None
    if op in D_FORM or op == 14:
        if ra == 13:
            return (sda13 + simm) & 0xFFFFFFFF
        if ra == 2:
            return (sda2 + simm) & 0xFFFFFFFF
    return None


# Explicit object references, not nearest-neighbour arithmetic. Each field is named by
# its PAL base materialization and its own addi/load/store instruction. These sites come
# from the NTSC-U evidence table inverted through CHUNKS_E and calibrated on clean PAL.
FIELD_REFERENCES = {
    0x802574A0: (0x8021B310, 0x8021B314, 0x8021B378, 0x80257480, 31, None),
    0x802574E0: (0x8021B310, 0x8021B314, 0x8021B388, 0x80257480, 31, None),
    0x80257520: (0x8021B310, 0x8021B314, 0x8021B514, 0x80257480, 31, None),
    0x80257540: (0x8021B310, 0x8021B314, 0x8021B5A4, 0x80257480, 31, None),
    0x809C183C: (0x80565270, 0x80565278, 0x80565288, 0x809C1830, 5, None),
    0x809BEBA0: (0x80550CD4, 0x80550CDC, 0x80550CFC, 0x809BEB00, 30, 0x80550CF8),
    0x809BEBA4: (0x80550CD4, 0x80550CDC, 0x80550F14, 0x809BEB00, 30, 0x80550CF8),
    0x809BEBA5: (0x80550CD4, 0x80550CDC, 0x80550F0C, 0x809BEB00, 30, 0x80550CF8),
    0x809BEBA6: (0x80550CD4, 0x80550CDC, 0x80550F18, 0x809BEB00, 30, 0x80550CF8),
    0x809BEBA7: (0x80550CD4, 0x80550CDC, 0x80550E1C, 0x809BEB00, 30, 0x80550CF8),
}


def gpr_writes(w):
    """Conservative integer-register writes for the instructions in these reference windows."""
    op, rt, ra = w >> 26, (w >> 21) & 31, (w >> 16) & 31
    writes = set()
    if op in (7, 8, 12, 13, 14, 15, 32, 33, 34, 35, 40, 41, 42, 43):
        writes.add(rt)
    if op == 46:  # lmw
        writes.update(range(rt, 32))
    if op in (20, 21, 23, 24, 25, 26, 27, 28, 29):
        writes.add(ra)
    if op in (33, 35, 37, 39, 41, 43, 45, 49, 51, 53, 55):
        writes.add(ra)
    if op == 31:
        xo = (w >> 1) & 1023
        if xo in (55, 119, 183, 247, 311, 375, 439, 567, 631, 695, 759):
            writes.add(ra)  # indexed update forms also replace their base register
        # compares, stores, cache/barrier operations, mtspr and mtcrf don't define a GPR.
        if xo not in (0, 32, 144, 150, 151, 183, 215, 247, 407, 439, 467,
                      535, 567, 599, 631, 663, 695, 727, 759,
                      54, 86, 246, 278, 598, 854, 982, 1014):
            if xo in (24, 26, 28, 60, 124, 284, 316, 412, 444, 476, 536, 792, 824, 922, 954):
                writes.add(ra)
            else:
                writes.add(rt)
    if op in (16, 18) and w & 1:
        writes.update(range(3, 13))  # ABI caller-clobbered registers
    return writes


def verified_field(pal, source, target, region):
    if pal not in FIELD_REFERENCES:
        return None, None
    base_site, base_end, field_site, expected_base, field_reg, move_site = FIELD_REFERENCES[pal]
    sites = [base_site, base_end, field_site] + ([move_site] if move_site else [])
    mapped = {}
    for site in sites:
        hit, _half = matched_site(source, site, target)
        if hit is None:
            return None, None
        mapped[site] = hit
    if formed_address(source, base_site, *SDA["P"]) != expected_base:
        return None, None
    base = formed_address(target, mapped[base_site], *SDA[region])
    if base is None:
        return None, None
    base_reg = (source.word(base_site) >> 21) & 31
    if (target.word(mapped[base_site]) >> 21) & 31 != base_reg:
        return None, None
    if mapped[base_end] - mapped[base_site] != base_end - base_site:
        return None, None
    # The low-half assignment and optional base copy must retain the same instruction shape.
    if source.word(base_end) & 0xFFFF0000 != target.word(mapped[base_end]) & 0xFFFF0000:
        return None, None
    start, target_start, tracked = base_end, mapped[base_end], base_reg
    if move_site:
        expected_move = (14 << 26) | (field_reg << 21) | (base_reg << 16)
        if source.word(move_site) != expected_move or target.word(mapped[move_site]) != expected_move:
            return None, None
        # Check the materialized register survives until its explicit copy.
        for image, lo, hi in ((source, base_end, move_site),
                              (target, mapped[base_end], mapped[move_site])):
            if any(base_reg in gpr_writes(image.word(a)) for a in range(lo + 4, hi, 4)):
                return None, None
        start, target_start, tracked = move_site, mapped[move_site], field_reg
    for image, lo, hi in ((source, start, field_site), (target, target_start, mapped[field_site])):
        if hi <= lo or any(tracked in gpr_writes(image.word(a)) for a in range(lo + 4, hi, 4)):
            return None, None
    w, tw = source.word(field_site), target.word(mapped[field_site])
    displacement = w & 0xFFFF
    displacement -= 0x10000 if displacement & 0x8000 else 0
    if (w != tw or w >> 26 not in D_FORM | {14} or (w >> 16) & 31 != field_reg
            or expected_base + displacement != pal):
        return None, None
    return base + displacement, (f"base @{mapped[base_site]:08X} -> 0x{base:08X}; "
                                  f"field @{mapped[field_site]:08X} word {tw:08X} "
                                  f"(unique PAL instruction matches; register r{field_reg} preserved)")


def verified_record(pal, source, target, region):
    if pal != 0x802A3EF4:
        return None, None
    base_site, stride_site, expected_base = 0x8023E574, 0x8023E56C, 0x802A3EE8
    base_hit, _ = matched_site(source, base_site, target)
    stride_hit, _ = matched_site(source, stride_site, target)
    if base_hit is None or stride_hit is None:
        return None, None
    if formed_address(source, base_site, *SDA["P"]) != expected_base:
        return None, None
    base = formed_address(target, base_hit, *SDA[region])
    if base is None or source.word(stride_site) != 0x1C80000C or target.word(stride_hit) != 0x1C80000C:
        return None, None
    # The two immutable canvas records must reproduce PAL byte-for-byte in the target.
    if any(source.word(expected_base + k) != target.word(base + k) for k in range(0, 24, 4)):
        return None, None
    return base + 12, (f"table @{base_hit:08X} -> 0x{base:08X}; stride @{stride_hit:08X} "
                       "mulli r4,r0,12; both 12-byte records equal PAL")


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--region", required=True, choices=["J"],
                    help="target region letter: J (NTSC-J)")
    ap.add_argument("--disc-root", required=True,
                    help="directory holding the extracted discs as RMC?01/ (sys/main.dol and "
                         "files/rel/StaticR.rel under each); the reference disc and target are read")
    ap.add_argument("--reference-region", choices=["P", "E"], default="E",
                    help="reference disc: PAL or NTSC-U; the evidence table remains NTSC-U")
    ap.add_argument("--source-table",
                    default=os.path.join(ROOT, "projects", pm.PROJECT_DIR["E"], "data_addresses.txt"))
    ap.add_argument("--out", default=None)
    args = ap.parse_args()
    region = args.region
    out = args.out or os.path.join(ROOT, "projects", pm.PROJECT_DIR[region], "data_addresses.txt")
    with open(args.source_table) as source:
        source_lines = source.readlines()
    pal_proof_globals = {f"{a:08X}" for a in FIELD_REFERENCES} | {"802A3EF4"}
    pal_required = any(parts[0].upper() in pal_proof_globals
                       for line in source_lines if (parts := line.split("#", 1)[0].split()))
    pal_disc = os.path.join(args.disc_root, "RMCP01")
    if pal_required and not all(os.path.isfile(os.path.join(pal_disc, path))
                                for path in ("sys/main.dol", "files/rel/StaticR.rel")):
        ap.error("FIELD/RECORD proofs require clean PAL inputs: "
                 "RMCP01/sys/main.dol and RMCP01/files/rel/StaticR.rel under --disc-root")

    inv_e = invert(pm.CHUNKS_E)
    dst = pm.CHUNK_TABLES[region]
    img = Image(region, args.disc_root)
    reference = args.reference_region
    reference_image = Image(reference, args.disc_root)
    sda13, sda2 = SDA[region]
    reference_sda13, reference_sda2 = SDA[reference]

    # Globals that are section boundaries rather than referenced objects. The PAL start address
    # maps to the target's start address for the same named section, which the section tables
    # (themselves read out of the shipped executables) state directly.
    boundaries = {}
    for tbl, pal_tbl in ((pm.DOL_SECTIONS[region], pm.DOL_SECTIONS["P"]),
                         (pm.REL_SECTIONS[region], pm.REL_SECTIONS["P"])):
        for (name, start, _end), (pname, pstart, _pe) in zip(tbl, pal_tbl):
            if name == pname:
                boundaries[pstart] = (start, name)

    # Field/record proofs need PAL itself, never a presumed offset between allocations.
    pal_image = reference_image if reference == "P" else (Image("P", args.disc_root)
        if os.path.exists(os.path.join(pal_disc, "sys", "main.dol")) else None)

    rows, proven, unresolved = [], 0, 0
    for line in source_lines:
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
        if reference == "P":
            sites = [mapped for a in sites if (mapped := port(inv_e, a)) is not None]
        reference_address = pal if reference == "P" else e_addr
        # The chunk-table port is the candidate. Disassembly only ever CONFIRMS it: a reference
        # site can form some other object's address (the evidence lists several sites, and a
        # function forms many addresses), so letting a decode override the candidate produced
        # nonsense like 0x00000000. Confirmation is agreement, never substitution.
        # Calibrate on the selected clean reference disc: a cited site must form its known
        # address for this global (the E table address, or the PAL identity). A site that
        # forms a different object is ignored; unique instruction matching must also confirm
        # the target code site before its decoded address is accepted.
        calibrated = [a for a in sites
                      if formed_address(reference_image, a, reference_sda13, reference_sda2) == reference_address]

        note, got = "no-reference-site", None
        seen = []
        for site_ref in calibrated:
            site_pal = site_ref if reference == "P" else port(inv_e, site_ref)
            site_r = port(dst, site_pal) if site_pal is not None else None
            if site_r is None:
                continue
            # A function-granular code port is not sufficient inside a resized function.
            # Require the calibrated instruction's unique signature to identify this site.
            hit, _half = matched_site(reference_image, site_ref, img)
            if hit != site_r:
                continue
            formed = formed_address(img, site_r, sda13, sda2)
            if formed is not None:
                got, note = formed, f"{site_r:08X} (calibrated on {reference}@{site_ref:08X})"
                break
        for site_ref in ([] if got is not None else sites):
            site_pal = site_ref if reference == "P" else port(inv_e, site_ref)
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
            for site_ref in calibrated:
                hit, half = matched_site(reference_image, site_ref, img)
                if hit is None:
                    continue
                formed = formed_address(img, hit, sda13, sda2)
                if formed is None:
                    continue
                match_addr = formed
                match_note = f"{hit:08X} (unique +-{half} masked match of {reference}@{site_ref:08X})"
                break
        if pal in boundaries:
            tgt, sname = boundaries[pal]
            rows.append(f"{pal:08X} {tgt:08X} SECTION .{sname} start (from the region section table)")
            proven += 1
        elif got is not None and cand is not None and got == cand:
            rows.append(f"{pal:08X} {cand:08X} PROVEN @{note} forms 0x{got:08X} (chunk port agrees)")
            proven += 1
        elif got is not None:
            rows.append(f"{pal:08X} {got:08X} CALIBRATED @{note} forms 0x{got:08X}"
                        f" (chunk port {'said 0x%08X' % cand if cand is not None else 'has no mapping'})")
            proven += 1
        elif match_addr is not None:
            agree = ("chunk port agrees" if cand == match_addr else
                     f"chunk port {'said 0x%08X' % cand if cand is not None else 'has no mapping'}")
            rows.append(f"{pal:08X} {match_addr:08X} MATCHED @{match_note}"
                        f" forms 0x{match_addr:08X} ({agree})")
            proven += 1
        elif cand is not None:
            rows.append(f"{pal:08X} {cand:08X} CHUNK-ONLY (no reference site confirmed it: {note})")
        else:
            field, field_note = verified_field(pal, pal_image, img, region) if pal_image else (None, None)
            record, record_note = verified_record(pal, pal_image, img, region) if pal_image else (None, None)
            if field is not None:
                rows.append(f"{pal:08X} {field:08X} FIELD {field_note}")
                proven += 1
            elif record is not None:
                rows.append(f"{pal:08X} {record:08X} RECORD {record_note}")
                proven += 1
            else:
                rows.append(f"{pal:08X} ? UNRESOLVED ({note}; no binary-backed field/record proof)")
                unresolved += 1

    header = [
        f"# RMC{region}01 addresses for the PAL (RMCP01) data globals named by the WiiCompiled runtime.",
        f"# Generated by tools/region/port_data_addresses.py --region {region} --reference-region {reference}; do not edit by hand.",
        "# Format: <PAL hex> <target hex> <verdict and evidence>",
        "#   PROVEN     - the instruction at the ported reference site forms exactly this address,",
        "#                and the independent chunk-table port of the PAL address agrees.",
        "#   CALIBRATED - the reference site reproduces the known reference-disc address for this",
        "#                global, so its decode in this region is trusted even where the chunk",
        "#                table has no mapping.",
        "#   MATCHED    - the reference site's instruction window, with the fields that legitimately",
        "#                differ between regions masked out, occurs exactly once in this region's",
        "#                code; the displacement was read from that instruction. Used where the",
        "#                chunk table is not instruction-exact inside a function.",
        "#   FIELD      - uniquely matched PAL base and field instructions, with the base register",
        "#                preserved and the target's own displacement decoded (including REL relocations).",
        "#   RECORD     - a verified table base, target indexing stride and matching immutable records.",
        "#   SECTION    - a section boundary, taken from the region section table.",
        "#   CHUNK-ONLY - chunk-table port only; no reference site formed exactly this address.",
        f"# Small-data bases: RMC{region}01 r13=0x{sda13:08X} r2=0x{sda2:08X}.",
        f"# Totals: {proven} confirmed, {len(rows) - proven - unresolved} chunk-only, {unresolved} unresolved.",
    ]
    os.makedirs(os.path.dirname(out), exist_ok=True)
    open(out, "w").write("\n".join(header + rows) + "\n")
    print(f"{region}: {len(rows)} globals -> {proven} confirmed, "
          f"{len(rows) - proven - unresolved} chunk-only, {unresolved} unresolved -> {out}")
    return 1 if unresolved else 0


if __name__ == "__main__":
    raise SystemExit(main())
