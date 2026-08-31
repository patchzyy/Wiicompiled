#!/usr/bin/env python3
"""Disassemble a range of a region's DOL (or StaticR.rel) at a guest address with llvm-mc.

Usage: disasm.py --region E|J|K|P <main.dol> <hexaddr> [words] [--rel StaticR.rel]

One instruction per line with its guest address. r13/r2-relative accesses are annotated with the
absolute address they form in that region (the small-data bases come from port_map.py), and
lis/addi(ori) pairs with the address they materialise, so a data global can be located in another
region's executable and cited as evidence in data_addresses.txt.

Needs llvm-mc on PATH (or in $LLVM_MC). Plain Python 3 stdlib otherwise.
"""
import argparse, os, re, shutil, struct, subprocess, sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import port_map as pm

D_FORM = {32, 33, 34, 35, 36, 37, 38, 39, 40, 41, 44, 45, 48, 49, 50, 51, 52, 53, 54, 55, 56, 57, 60, 61}


def parse_dol(path):
    d = open(path, "rb").read()
    offs = struct.unpack(">18I", d[0:72]); addrs = struct.unpack(">18I", d[72:144]); sizes = struct.unpack(">18I", d[144:216])
    return d, [(addrs[i], offs[i], sizes[i]) for i in range(18) if sizes[i]]


def parse_rel(path, base):
    d = open(path, "rb").read()
    num = struct.unpack(">I", d[0x0C:0x10])[0]; stab = struct.unpack(">I", d[0x10:0x14])[0]
    secs = []
    for i in range(num):
        off, size = struct.unpack(">II", d[stab + 8 * i:stab + 8 * i + 8])
        off &= ~1
        if off and size:
            secs.append((base + off, off, size))
    return d, secs


def read_words(img, secs, addr, n):
    for va, fo, sz in secs:
        if va <= addr < va + sz:
            avail = min(n * 4, va + sz - addr)
            return img[fo + (addr - va):fo + (addr - va) + avail]
    raise SystemExit(f"address {addr:#x} is not in any section")


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--region", required=True, choices=sorted(pm.SDA_BASES))
    ap.add_argument("dol")
    ap.add_argument("addr", type=lambda v: int(v, 16))
    ap.add_argument("words", nargs="?", type=int, default=48)
    ap.add_argument("--rel", help="StaticR.rel, for addresses at or above its load address")
    a = ap.parse_args()
    mc = os.environ.get("LLVM_MC") or shutil.which("llvm-mc")
    if not mc:
        raise SystemExit("llvm-mc not found: put it on PATH or set LLVM_MC")
    sda13, sda2 = pm.SDA_BASES[a.region]
    rel_base = pm.REL_LOAD_ADDRESS[a.region]

    img, secs = parse_dol(a.dol)
    if a.addr >= rel_base and a.rel:
        img, secs = parse_rel(a.rel, rel_base)
    b = read_words(img, secs, a.addr, a.words)
    # One word per line keeps the decoder aligned even when data words sit between instructions:
    # llvm-mc resynchronises per input line, and an undecodable word simply yields no output.
    hexs = "\n".join(" ".join(f"0x{x:02x}" for x in b[i:i + 4]) for i in range(0, len(b) - len(b) % 4, 4))
    out = subprocess.run([mc, "--disassemble", "-triple=powerpc-unknown-linux-gnu", "-mcpu=750"],
                         input=hexs, capture_output=True, text=True)
    # llvm-mc prints a '.text' header once, then one line per decoded word; errors go to stderr
    # with the offending line number, so rebuild the per-word list from those line numbers.
    decoded = [l.strip() for l in out.stdout.splitlines() if l.strip() and not l.strip().startswith(".")]
    bad = {int(m.group(1)) - 1 for m in (re.search(r"<stdin>:(\d+):", l) for l in out.stderr.splitlines()) if m}
    lines, k = [], 0
    for i in range(len(b) // 4):
        if i in bad:
            lines.append("??")
        else:
            lines.append(decoded[k] if k < len(decoded) else "??"); k += 1

    pc, hi = a.addr, {}
    for i in range(0, len(b) - len(b) % 4, 4):
        w = struct.unpack(">I", b[i:i + 4])[0]
        ins = lines[i // 4] if i // 4 < len(lines) else "??"
        op, rt, ra, imm = w >> 26, (w >> 21) & 31, (w >> 16) & 31, w & 0xFFFF
        simm = imm - 0x10000 if imm & 0x8000 else imm
        note = ""
        if op in D_FORM and ra in (2, 13):
            base = sda13 if ra == 13 else sda2
            note = f"   ; r{ra}{simm:+#x} -> {base + simm:#x}"
        if op == 15 and ra == 0:
            hi[rt] = imm << 16; note = f"   ; lis r{rt}"
        elif op == 14 and ra in hi:
            note = f"   ; addi -> {hi[ra] + simm:#x}"
        elif op == 24 and rt in hi:
            note = f"   ; ori -> {hi[rt] | imm:#x}"
        elif op in D_FORM and ra in hi:
            note = f"   ; [lis-based] -> {hi[ra] + simm:#x}"
        print(f"{pc:08x}  {w:08x}  {ins}{note}")
        pc += 4


if __name__ == "__main__":
    main()
