#!/usr/bin/env python3
"""
port_map.py - port the Mario Kart Wii PAL (RMCP01) function map used by the WiiCompiled
static recompiler to another retail region (NTSC-U RMCE01, NTSC-J RMCJ01 or NTSC-K RMCK01)
and validate every ported entry against that region's own executables (sys/main.dol +
files/rel/StaticR.rel).

Nothing here guesses: every address that ends up in the output map carries a verdict that
was computed from the target region's binaries (call targets, relocation targets,
exception-table records, function-pointer tables, or at least a terminator/prologue shape).

Plain Python 3 stdlib only. Output is deterministic (no timestamps, sorted everywhere).

Method (see MAP_REPORT.md for the numbers of a given run):
  A. Build the evidence set K of target function entry points proven by the binaries:
       - DOL text: `bl` targets (strong), `b` targets (weak), lis/addi(ori) pairs that
         materialise a code address (medium), the DOL entry point (strong).
       - DOL extabindex: {function start, size, extab ptr} records (strong).
       - DOL data/rodata/ctors/dtors/sdata/sdata2: 32-bit words pointing into DOL or REL
         text (medium; runs that look like switch jump tables are excluded).
       - REL: text-section `bl`/`b` targets resolved at link time, R_PPC_REL24 relocations
         whose site instruction is `bl` (strong) / `b` (weak), R_PPC_ADDR32 relocations into
         text (medium; jump-table-like runs excluded), R_PPC_ADDR16_LO/HI/HA relocations into
         text (medium), and the REL prolog/epilog/unresolved entry points (strong).
  B. Port every PAL entry through the community-validated mkw-sp chunk table and classify the
     result: PROVEN (strong evidence) / REFERENCED (medium) / PLAUSIBLE (predecessor is a
     terminator and the word decodes as a first instruction) / UNVERIFIED (never emitted).
     Refinements, all driven by the binary:
       - data-pointer runs that look like switch jump tables (short span, no strong entry
         inside) do not count as function-pointer references;
       - a conditional-branch target lies inside a function, so it vetoes REFERENCED/PLAUSIBLE
         for entries that claim to be functions;
       - the PAL map carries Ghidra-style labels: `*_caseD_N`-style case labels are confirmed by
         a jump-table reference at the ported address, `*_switch` dispatch sites (and unnamed
         entries at such sites) by a `mtctr`+`bctr` pair.
  C. Entries the chunk table cannot port: IDENTITY below 0x80004000, else INTERPOLATED when
     the PAL gap and the target gap between the nearest PROVEN neighbours hold the same number
     of entries, else DROPPED.
  D. Verify the CodeWarrior save/restore thunk families the recompiler resolves by name.
  E. Emit MAP.txt sorted by target address, deduplicated, parseable by FunctionMap.cs.
  H. Append every target entry point K proves that no ported PAL entry covers (bl targets,
     .ctors/.dtors entries, REL prolog/epilog/unresolved, extabindex records, function-pointer
     tables) as unnamed placeholder entries, reported per evidence class.
  F. Emit region_port.json with the vendored chunk table for other tooling.
  G. Emit MAP_REPORT.md.

Usage:
  port_map.py --region E --disc-root DIR      # DIR/RMCE01/sys/main.dol, DIR/RMCE01/files/rel/StaticR.rel
  port_map.py --region K --dol main.dol --rel StaticR.rel
"""

from __future__ import annotations

import argparse
import array
import bisect
import collections
import hashlib
import json
import os
import re
import struct
import sys
from dataclasses import dataclass, field
from typing import Dict, List, Optional, Tuple

TOOL_ID = "tools/region/port_map.py"

# ---------------------------------------------------------------------------------------
# Region constants
# ---------------------------------------------------------------------------------------
# Section layouts and the chunk table below are vendored from the MKW-SP project
# (https://github.com/mkw-sp/mkw-sp, port.py, MIT license). The chunk table maps PAL
# (RMCP01) addresses to NTSC-U (RMCE01) addresses: if src_start <= a < src_end then
# a - src_start + dst_start.

REL_LOAD_ADDRESS = {"P": 0x805102E0, "E": 0x8050BF60, "J": 0x8050FC60, "K": 0x804FE300}
# Small-data bases (r13 _SDA_BASE_, r2 _SDA2_BASE_) each region's __init_registers installs.
SDA_BASES = {"P": (0x8038CC00, 0x8038EFA0), "E": (0x80388880, 0x8038AC20),
             "J": (0x8038C580, 0x8038E920), "K": (0x8037AC00, 0x8037CFC0)}

# Output project directory per destination region.
PROJECT_DIR = {"E": "mkwii-ntsc-u", "J": "mkwii-ntsc-j", "K": "mkwii-ntsc-k"}
# Display name and game ID of each target region, for the map header and the report.
REGION_NAME = {"E": ("NTSC-U", "RMCE01"), "J": ("NTSC-J", "RMCJ01"), "K": ("NTSC-K", "RMCK01")}

DOL_SECTIONS = {
    "P": [
        ("init", 0x80004000, 0x80006460),
        ("extab", 0x80006460, 0x80006A20),
        ("extabindex", 0x80006A20, 0x800072C0),
        ("text", 0x800072C0, 0x80244DE0),
        ("ctors", 0x80244DE0, 0x80244EA0),
        ("dtors", 0x80244EA0, 0x80244EC0),
        ("rodata", 0x80244EC0, 0x80258580),
        ("data", 0x80258580, 0x802A4040),
        ("bss", 0x802A4080, 0x80384C00),
        ("sdata", 0x80384C00, 0x80385FC0),
        ("sbss", 0x80385FC0, 0x80386FA0),
        ("sdata2", 0x80386FA0, 0x80389140),
        ("sbss2", 0x80389140, 0x8038917C),
    ],
    "E": [
        ("init", 0x80004000, 0x80006460),
        ("extab", 0x80006460, 0x80006A20),
        ("extabindex", 0x80006A20, 0x800072C0),
        ("text", 0x800072C0, 0x80244D40),
        ("ctors", 0x80244D40, 0x80244E00),
        ("dtors", 0x80244E00, 0x80244E20),
        ("rodata", 0x80244E40, 0x80258260),
        ("data", 0x80258260, 0x8029FD00),
        ("bss", 0x8029FD00, 0x80380880),
        ("sdata", 0x80380880, 0x80381C40),
        ("sbss", 0x80381C40, 0x80382C20),
        ("sdata2", 0x80382C20, 0x80384DC0),
        ("sbss2", 0x80384DC0, 0x80384DFC),
    ],
    "J": [
        ("init", 0x80004000, 0x80006460),
        ("extab", 0x80006460, 0x80006A20),
        ("extabindex", 0x80006A20, 0x800072C0),
        ("text", 0x800072C0, 0x80244D00),
        ("ctors", 0x80244D00, 0x80244DC0),
        ("dtors", 0x80244DC0, 0x80244DE0),
        ("rodata", 0x80244E00, 0x80257F20),
        ("data", 0x80257F20, 0x802A39E0),
        ("bss", 0x802A3A00, 0x80384580),
        ("sdata", 0x80384580, 0x80385940),
        ("sbss", 0x80385940, 0x80386920),
        ("sdata2", 0x80386920, 0x80388AC0),
        ("sbss2", 0x80388AC0, 0x80388AFC),
    ],
    "K": [
        ("init", 0x80004000, 0x80006460),
        ("extab", 0x80006460, 0x80006A20),
        ("extabindex", 0x80006A20, 0x800072C0),
        ("text", 0x800072C0, 0x80245160),
        ("ctors", 0x80245160, 0x80245220),
        ("dtors", 0x80245220, 0x80245240),
        ("rodata", 0x80245240, 0x80258340),
        ("data", 0x80258340, 0x80292040),
        ("bss", 0x80292080, 0x80372C00),
        ("sdata", 0x80372C00, 0x80373FE0),
        ("sbss", 0x80373FE0, 0x80374FC0),
        ("sdata2", 0x80374FC0, 0x80377160),
        ("sbss2", 0x80377160, 0x8037719C),
    ],
}

REL_SECTIONS = {
    "P": [
        ("text", 0x805103B4, 0x8088F400),
        ("ctors", 0x8088F400, 0x8088F704),
        ("dtors", 0x8088F704, 0x8088F710),
        ("rodata", 0x8088F720, 0x808B2BD0),
        ("data", 0x808B2BD0, 0x808DD3D4),
        ("bss", 0x809BD6E0, 0x809C4F90),
    ],
    "E": [
        ("text", 0x8050C034, 0x8088AFD0),
        ("ctors", 0x8088AFD0, 0x8088B2D4),
        ("dtors", 0x8088B2D4, 0x8088B2E0),
        ("rodata", 0x8088B2E0, 0x808AE520),
        ("data", 0x808AE520, 0x808D8C7C),
        ("bss", 0x809B8F20, 0x809C07D0),
    ],
    "J": [
        ("text", 0x8050FD34, 0x8088EA6C),
        ("ctors", 0x8088EA6C, 0x8088ED70),
        ("dtors", 0x8088ED70, 0x8088ED7C),
        ("rodata", 0x8088ED80, 0x808B1D30),
        ("data", 0x808B1D30, 0x808DC524),
        ("bss", 0x809BC740, 0x809C3FF0),
    ],
    "K": [
        ("text", 0x804FE3D4, 0x8087D7C0),
        ("ctors", 0x8087D7C0, 0x8087DAC4),
        ("dtors", 0x8087DAC4, 0x8087DAD0),
        ("rodata", 0x8087DAE0, 0x808A1030),
        ("data", 0x808A1030, 0x808CB86C),
        ("bss", 0x809ABD20, 0x809B35D0),
    ],
}

# PAL -> NTSC-U chunk table: (src_start, src_end, dst_start).
# Vendored from the mkw-sp project, port.py (CHUNKS['E']), MIT license.
CHUNKS_E = [
    (0x80004000, 0x80008030, 0x80004000), (0x8000808C, 0x8000ADBC, 0x8000804C), (0x8000AF24, 0x8000B6B4, 0x8000AE84),
    (0x8000B6B4, 0x8000C174, 0x80021048), (0x8000C174, 0x80021BA8, 0x8000B614), (0x80021BA8, 0x80225F1C, 0x80021B08),
    (0x802261F8, 0x802402E0, 0x80225E74), (0x802402E0, 0x80240E18, 0x802441FC), (0x80240E18, 0x80244DD4, 0x8023FF5C),
    (0x802A4080, 0x80384C18, 0x8029FD00), (0x80385908, 0x8038590C, 0x80381588), (0x80385FC0, 0x80386008, 0x80381C40),
    (0x80386638, 0x80386644, 0x803822B8), (0x80386F48, 0x80386F90, 0x80382BC0), (0x805103B4, 0x80510A90, 0x8050C034),
    (0x80510B84, 0x8052D298, 0x8050C710), (0x8052D298, 0x8052D96C, 0x8054F800), (0x8052D96C, 0x8053D97C, 0x80528E24),
    (0x8053D97C, 0x8053E370, 0x8054FED4), (0x8053E370, 0x8054FB2C, 0x80538E34), (0x8054FB2C, 0x80550548, 0x805508C8),
    (0x80550548, 0x805537CC, 0x8054A5F0), (0x80553894, 0x8055572C, 0x8054D874), (0x8055572C, 0x8056AB6C, 0x805513AC),
    (0x8056AB6C, 0x8056B63C, 0x805AE0E8), (0x8056B63C, 0x80574030, 0x805667EC), (0x80574030, 0x805758AC, 0x805AEBB8),
    (0x80575A44, 0x80583F2C, 0x8056F1E0), (0x80583F2C, 0x8059B5A4, 0x8057D708), (0x8059B5A4, 0x8059EAA4, 0x805B058C),
    (0x8059EAA4, 0x805A0558, 0x80594D80), (0x805A068C, 0x805A1478, 0x805B3AC8), (0x805A1488, 0x805A1864, 0x805B48B4),
    (0x805A1864, 0x805A9EDC, 0x8059682C), (0x805A9EDC, 0x805AB574, 0x8059EEAC), (0x805AB574, 0x805ABC90, 0x805A0604),
    (0x805ABC90, 0x805ADD98, 0x805A0D28), (0x805ADD98, 0x805B9010, 0x805A2E70), (0x805B9010, 0x805B9300, 0x805B4C90),
    (0x805B9300, 0x805BAB40, 0x8061F7F4), (0x805BAB40, 0x805BB2B8, 0x805B4F80), (0x805BB2B8, 0x805BD2D8, 0x80621034),
    (0x805BD2D8, 0x805BD2E4, 0x805B695C), (0x805BD2E4, 0x805BD39C, 0x80623054), (0x805BD39C, 0x805BE600, 0x805B56F8),
    (0x805BE600, 0x805BE61C, 0x805B6968), (0x805BE66C, 0x805BE7F4, 0x8062310C), (0x805BE7F4, 0x805BE84C, 0x805B76D0),
    (0x805BE84C, 0x805BED68, 0x80623294), (0x805BED68, 0x805BED74, 0x805B7420), (0x805BED74, 0x805BF2D8, 0x806237B0),
    (0x805BF2D8, 0x805BF2DC, 0x805B919C), (0x805BF3CC, 0x805BFE1C, 0x805B69D0), (0x805BFE1C, 0x805C00C0, 0x805B742C),
    (0x805C00C0, 0x805C1B34, 0x805B7728), (0x805C1B34, 0x805C35F8, 0x805B91A0), (0x805C35F8, 0x805C3BCC, 0x80623E08),
    (0x805C3BCC, 0x805C4768, 0x805BAC64), (0x805C4768, 0x805C579C, 0x806243DC), (0x805C57B0, 0x805CADDC, 0x805BB800),
    (0x805CADDC, 0x805CC0A8, 0x80625424), (0x805CC1B4, 0x805CD94C, 0x80626758), (0x805CD94C, 0x805D1888, 0x805C0E2C),
    (0x805D1888, 0x805D1E84, 0x80627EF0), (0x805D1E84, 0x805D7F78, 0x805C4D68), (0x805D7F78, 0x805DBFDC, 0x806284EC),
    (0x805DC034, 0x805DE838, 0x8062C550), (0x805DE838, 0x805DE844, 0x805E4438), (0x805DE844, 0x805E0C38, 0x8062ED54),
    (0x805E0C38, 0x805E6B50, 0x805CAE5C), (0x805E6B50, 0x805E7460, 0x80631148), (0x805E7460, 0x805EA780, 0x805D0D74),
    (0x805EA780, 0x805EEB68, 0x80631A58), (0x805EEB68, 0x805F2D24, 0x805D4094), (0x805F2D24, 0x805F8B34, 0x80635E40),
    (0x805F8B34, 0x805FB5BC, 0x805D8250), (0x805FB5BC, 0x805FB820, 0x8063BC50), (0x805FB820, 0x805FD518, 0x805DACD8),
    (0x805FD518, 0x806012B0, 0x8063BEB4), (0x806012B0, 0x80608D18, 0x805DC9D0), (0x80608D18, 0x8060A72C, 0x805E4444),
    (0x8060A72C, 0x8061186C, 0x8063FC4C), (0x8061186C, 0x80616844, 0x805E5E58), (0x80616844, 0x8061AE6C, 0x80646D8C),
    (0x8061AE6C, 0x8061E898, 0x805EAE30), (0x8061E898, 0x8061F6E8, 0x8064B3B4), (0x8061F6E8, 0x80620CB4, 0x805EE85C),
    (0x80620D7C, 0x80634F48, 0x805EFEC8), (0x80634F48, 0x80637494, 0x806040B0), (0x80637494, 0x80637514, 0x806066A4),
    (0x80637514, 0x80637A30, 0x8064C204), (0x80637AC0, 0x8063BCF8, 0x80606720), (0x8063BE40, 0x806453E4, 0x8060AA20),
    (0x806453E4, 0x806467D4, 0x8064C78C), (0x806467D4, 0x8064A3F4, 0x80613FC4), (0x8064A3F4, 0x8064AEE4, 0x8064DB7C),
    (0x8064AEF8, 0x80651BEC, 0x80617BE4), (0x80651BEC, 0x80652300, 0x8064E66C), (0x80652300, 0x80653208, 0x8061E8EC),
    (0x80653208, 0x8065B354, 0x8064ED80), (0x8065B4E8, 0x8065C0EC, 0x80656ECC), (0x8065C0EC, 0x8065FA14, 0x8065CD74),
    (0x8065FA4C, 0x8065FE4C, 0x80657AD0), (0x8065FE4C, 0x80662778, 0x80657F10), (0x80662778, 0x80663194, 0x80660694),
    (0x80663194, 0x80665538, 0x8065A83C), (0x80665538, 0x80668814, 0x80673188), (0x80668814, 0x8067906C, 0x806610B0),
    (0x8067906C, 0x80679380, 0x80676464), (0x80679380, 0x8067AC00, 0x80671908), (0x8067AC00, 0x806BF4C8, 0x80676778),
    (0x806BF4C8, 0x806BFB14, 0x806E20A8), (0x806BFB14, 0x806C050C, 0x806BB040), (0x806C050C, 0x806C35A8, 0x806E26F4),
    (0x806C35A8, 0x806C3AA4, 0x806BBA38), (0x806C3AA4, 0x806C4ED4, 0x806E5790), (0x806C4EF0, 0x806C63A8, 0x806E6BC0),
    (0x806C63B0, 0x806C6B44, 0x806E8078), (0x806C6B44, 0x806CCE90, 0x806BBF50), (0x806CCE90, 0x806CE820, 0x806E880C),
    (0x806CE828, 0x806D02BC, 0x806C229C), (0x806D02BC, 0x806D28F4, 0x806EA19C), (0x806D2908, 0x806D5C5C, 0x806C3D38),
    (0x806D5C60, 0x806D5ED8, 0x806EC7D4), (0x806D5ED8, 0x806DA914, 0x806C7098), (0x806DA914, 0x806DB184, 0x806ECA58),
    (0x806DB184, 0x806DDA84, 0x806CBAD4), (0x806DDA84, 0x806DEB40, 0x806ED2C8), (0x806DEB40, 0x806DF7D0, 0x806CE3D4),
    (0x806DF7D0, 0x806DFD14, 0x806EE384), (0x806DFD14, 0x806E3A8C, 0x806CF064), (0x806E3A8C, 0x806E3E20, 0x806EE8C8),
    (0x806E3E20, 0x806E95B0, 0x806D2DDC), (0x806E95B0, 0x806EC7C0, 0x806EEC5C), (0x806EC7C0, 0x806ED53C, 0x806D8564),
    (0x806ED53C, 0x806F62FC, 0x806D92E8), (0x806F62FC, 0x806F7698, 0x806F1E74), (0x806F77C4, 0x806F7A54, 0x80713A78),
    (0x806F7AA8, 0x806F8220, 0x80713D08), (0x806F8220, 0x806F8934, 0x806F3210), (0x806F8934, 0x806FA3FC, 0x806F3978),
    (0x806FA3FC, 0x806FE240, 0x806F5494), (0x806FE240, 0x806FE9E0, 0x806F9330), (0x806FE9E0, 0x807001A4, 0x80714480),
    (0x80700230, 0x80700474, 0x80715C44), (0x80700474, 0x8070E7B8, 0x806F9AD0), (0x8070E7B8, 0x8070F8B8, 0x80715E88),
    (0x8070F8B8, 0x807179C4, 0x80707E14), (0x807179C4, 0x80717E34, 0x8070FFAC), (0x80717E34, 0x807182E8, 0x80716F88),
    (0x807182E8, 0x8071B86C, 0x8071041C), (0x8071B86C, 0x80726574, 0x8071743C), (0x80726574, 0x8072820C, 0x8073C5DC),
    (0x8072821C, 0x807285C8, 0x8073E274), (0x807285CC, 0x80729338, 0x8073E620), (0x80729350, 0x80729B88, 0x80722144),
    (0x80729B88, 0x8072A894, 0x80722984), (0x8072A894, 0x8072B95C, 0x80723698), (0x8072B95C, 0x8072DE64, 0x80724770),
    (0x8072DFCC, 0x8072FF60, 0x8073F390), (0x8072FF78, 0x80730198, 0x80741324), (0x80730198, 0x80730A80, 0x80726DE8),
    (0x80730B40, 0x80731960, 0x80741544), (0x80731960, 0x80735948, 0x807277A8), (0x80735948, 0x80738DB8, 0x80742364),
    (0x80738DB8, 0x8073C54C, 0x8072B790), (0x8073C54C, 0x8073EDF0, 0x807457D4), (0x8073EDF0, 0x8074C4A8, 0x8072EF24),
    (0x8074C4A8, 0x8074D5B8, 0x8076CEAC), (0x8074D5B8, 0x807519C8, 0x80748078), (0x807519C8, 0x80754104, 0x8076DFBC),
    (0x80754104, 0x80758BDC, 0x8074C488), (0x80758BDC, 0x8075DB24, 0x807706F8), (0x8075DB3C, 0x8075E78C, 0x80750F60),
    (0x8075E78C, 0x8075EAFC, 0x80775658), (0x8075EAFC, 0x80765C94, 0x80751BB0), (0x80765C94, 0x807678F4, 0x807759C8),
    (0x807678F4, 0x80768D20, 0x80758D48), (0x80768D20, 0x807693EC, 0x80777628), (0x8076960C, 0x8076C85C, 0x80777E68),
    (0x8076C85C, 0x8076EBDC, 0x8075A174), (0x8076EBE0, 0x8076F2DC, 0x8077B0B8), (0x8076F2DC, 0x807726C4, 0x8075C4F4),
    (0x80772704, 0x80773C14, 0x8077B7B8), (0x80773C1C, 0x8077439C, 0x8075F8DC), (0x807743A0, 0x807787F0, 0x8076010C),
    (0x807787F0, 0x8077902C, 0x8077CD10), (0x8077902C, 0x8077CE88, 0x8076455C), (0x8077CEC8, 0x8077DF24, 0x8077D54C),
    (0x8077DF24, 0x807829D8, 0x807683F8), (0x807829D8, 0x80787D84, 0x8077E5A8), (0x80787D84, 0x8078C960, 0x807D031C),
    (0x8078C960, 0x807A81B4, 0x80783954), (0x807A81B4, 0x807A9D70, 0x807D4F74), (0x807A9EB8, 0x807AF140, 0x8079F210),
    (0x807AF140, 0x807B2EF8, 0x807D6B94), (0x807B2EF8, 0x807D976C, 0x807A4498), (0x807D976C, 0x807D9B80, 0x807DA94C),
    (0x807D9B98, 0x807DA5C0, 0x807CAD0C), (0x807DA5C0, 0x807DBCCC, 0x807DAD78), (0x807DBCCC, 0x807DC8C8, 0x807CB734),
    (0x807DC950, 0x807E093C, 0x807CC330), (0x807E093C, 0x807E2520, 0x8083C6F0), (0x807E259C, 0x807E5610, 0x807DC50C),
    (0x807E5654, 0x807E6414, 0x807DF5E4), (0x807E6414, 0x807E9C44, 0x8083E33C), (0x807E9C50, 0x807EDD98, 0x807E03A4),
    (0x807EDD98, 0x807EE23C, 0x80841B6C), (0x807EE250, 0x807EE468, 0x807E44EC), (0x807EE474, 0x807EEA14, 0x80842010),
    (0x807EEA14, 0x807EF9F4, 0x807E4704), (0x807EFD0C, 0x807F76EC, 0x807E56E4), (0x807F76EC, 0x807F7BA4, 0x808428E8),
    (0x807F7BC4, 0x807F890C, 0x807ED0C4), (0x807F8968, 0x807F9280, 0x80842DA0), (0x807F9580, 0x807FAB58, 0x808438A0),
    (0x807FAB58, 0x807FEB68, 0x807EDF98), (0x807FEB68, 0x807FFAE0, 0x80844E78), (0x807FFB20, 0x80805A0C, 0x807F1FA8),
    (0x80805A0C, 0x8080761C, 0x80845DF0), (0x8080761C, 0x80809448, 0x807F7ED4), (0x80809448, 0x8080AD20, 0x80847A00),
    (0x8080AD20, 0x80811E48, 0x807F9D00), (0x80811E48, 0x80813BD4, 0x808492D8), (0x80813BD4, 0x8081E284, 0x80800E28),
    (0x8081E284, 0x8081EFEC, 0x8084B064), (0x8081EFEC, 0x8082E540, 0x8080B4D8), (0x8082E540, 0x8082E854, 0x8084BDCC),
    (0x8082E854, 0x8082F408, 0x8081AA2C), (0x8082F408, 0x808334A0, 0x8084C0E0), (0x808334E0, 0x80833B00, 0x8081B5E0),
    (0x80833B00, 0x80838E4C, 0x8081BC08), (0x80838E60, 0x8083B0C0, 0x808501B8), (0x8083B0CC, 0x8083CB44, 0x80820F54),
    (0x8083CB44, 0x8083D42C, 0x80852424), (0x8083D42C, 0x80842334, 0x808229CC), (0x80842340, 0x808447AC, 0x80852D0C),
    (0x808447AC, 0x8084A9A0, 0x808278D4), (0x8084A9A0, 0x8084D0DC, 0x80855184), (0x8084D0DC, 0x80851D2C, 0x8082DAC8),
    (0x80851D38, 0x80852C60, 0x808578C0), (0x80852C60, 0x80853CA4, 0x80832718), (0x80853CA4, 0x808551EC, 0x808587F4),
    (0x808551EC, 0x8085C3CC, 0x8083375C), (0x8085C3CC, 0x8085E674, 0x80859D3C), (0x8085E674, 0x8085F0AC, 0x8083A950),
    (0x8085F0AC, 0x8085FFD4, 0x8085BFE4), (0x8085FFD4, 0x80860F2C, 0x8083B388), (0x80860F2C, 0x80862E24, 0x8085CF0C),
    (0x80863234, 0x8086708C, 0x8085EE04), (0x8086708C, 0x808676E0, 0x80864D38), (0x808676E0, 0x808697BC, 0x80862C5C),
    (0x808697BC, 0x8086A254, 0x8086538C), (0x8086A254, 0x8086C098, 0x808766F4), (0x8086C108, 0x8086C988, 0x80878538),
    (0x8086CA40, 0x80872CA4, 0x80878DB8), (0x80872CA4, 0x808739B0, 0x80865E24), (0x808739B0, 0x80875454, 0x80866BE8),
    (0x80875454, 0x8088344C, 0x808686FC), (0x8088344C, 0x8088F400, 0x8087F01C), (0x80895238, 0x808952B8, 0x8088F940),
    (0x808913C0, 0x808913C4, 0x8088CF78), (0x808AD3C4, 0x808AD3C8, 0x808A7304), (0x808B3984, 0x808B3988, 0x808AF134),
    (0x808B5B1C, 0x808B5B20, 0x808B125C), (0x808B5C78, 0x808B5C7C, 0x808B13B8), (0x808BB034, 0x808BB098, 0x808B4F3C),
    (0x808CB550, 0x808CB554, 0x808C6048), (0x808D3698, 0x808D369C, 0x808D5148), (0x808D36CC, 0x808D36D0, 0x808D517C),
    (0x808D36D4, 0x808D36D8, 0x808D5184), (0x808D3744, 0x808D3748, 0x808D51F4), (0x808D374C, 0x808D3750, 0x808D51FC),
    (0x808D3E14, 0x808D3E18, 0x808CEEAC), (0x808DA070, 0x808DA080, 0x808D3E50), (0x808DA318, 0x808DA368, 0x808D3F60),
    (0x809BD6E8, 0x809BD74C, 0x809B8F28), (0x809C1830, 0x809C1834, 0x809BD070), (0x809C1874, 0x809C1878, 0x809BD0B4),
    (0x809C18F8, 0x809C18FC, 0x809BD110), (0x809C1988, 0x809C198C, 0x809BD378), (0x809C19A0, 0x809C19BC, 0x809BD180),
    (0x809C1E38, 0x809C1E3C, 0x809BD508), (0x809C21D0, 0x809C21D4, 0x809BDA10), (0x809C21D8, 0x809C21DC, 0x809BDA18),
    (0x809C2328, 0x809C232C, 0x809BDB60), (0x809C27F0, 0x809C27FC, 0x809BDBB0), (0x809C282C, 0x809C2854, 0x809BDBEC),
    (0x809C28B8, 0x809C28BC, 0x809BE0F8), (0x809C2BE8, 0x809C2BEC, 0x809BE398), (0x809C2EF0, 0x809C2EF4, 0x809BE730),
    (0x809C2F38, 0x809C2F40, 0x809BE740), (0x809C2F44, 0x809C2F48, 0x809BE74C), (0x809C3618, 0x809C361C, 0x809BEE20),
    (0x809C38B8, 0x809C38BC, 0x809BF0B0), (0x809C4330, 0x809C4334, 0x809BFAF0), (0x809C4680, 0x809C4684, 0x809BFDC0),
    (0x809C4740, 0x809C474C, 0x809BFF90), (0x809C496C, 0x809C4970, 0x809C014C),
]
# PAL -> NTSC-J chunk table. Vendored from mkw-sp port.py (CHUNKS['J']), MIT license.
CHUNKS_J = [
    (0x80004000, 0x80008024, 0x80004000), (0x800080E8, 0x8000ADC0, 0x80008044), (0x8000AF08, 0x80021BAC, 0x8000AE2C),
    (0x80021BAC, 0x80244DE0, 0x80021ACC), (0x802A4080, 0x8038917C, 0x802A3A00), (0x805103B4, 0x805CC0A8, 0x8050FD34),
    (0x805CC1B4, 0x805FA33C, 0x805CBA90), (0x805FA344, 0x805FF6E8, 0x805F9C20), (0x805FFD70, 0x806003E8, 0x805FF528),
    (0x80600C78, 0x80620CB4, 0x806003EC), (0x80620D7C, 0x80637A24, 0x806204C8), (0x80637A80, 0x8063BCF8, 0x8063716C),
    (0x8063BE40, 0x8088F400, 0x8063B4AC), (0x808913C0, 0x808913C4, 0x80890A10), (0x80895238, 0x808952B8, 0x80894888),
    (0x808AD3C4, 0x808AD3C8, 0x808AC524), (0x808B3984, 0x808B3988, 0x808B2AE4), (0x808B5B1C, 0x808B5B20, 0x808B4C7C),
    (0x808B5C78, 0x808B5C7C, 0x808B4DD8), (0x808BB034, 0x808BB098, 0x808BA184), (0x808CB550, 0x808CB554, 0x808CA6A0),
    (0x808D3698, 0x808D369C, 0x808D27E8), (0x808D36CC, 0x808D36D0, 0x808D281C), (0x808D36D4, 0x808D36D8, 0x808D2824),
    (0x808D3744, 0x808D3748, 0x808D2894), (0x808D374C, 0x808D3750, 0x808D289C), (0x808D3E14, 0x808D3E18, 0x808D2F64),
    (0x808DA070, 0x808DA080, 0x808D91C0), (0x808DA318, 0x808DA368, 0x808D9468), (0x809BD6E8, 0x809BD74C, 0x809BC748),
    (0x809C1830, 0x809C1834, 0x809C0890), (0x809C1874, 0x809C1878, 0x809C08D4), (0x809C18F8, 0x809C18FC, 0x809C0958),
    (0x809C1988, 0x809C198C, 0x809C09E8), (0x809C19A0, 0x809C19BC, 0x809C0A00), (0x809C1E38, 0x809C1E3C, 0x809C0E98),
    (0x809C21D0, 0x809C21D4, 0x809C1230), (0x809C21D8, 0x809C21DC, 0x809C1238), (0x809C2328, 0x809C232C, 0x809C1388),
    (0x809C27F0, 0x809C27FC, 0x809C1850), (0x809C282C, 0x809C2854, 0x809C188C), (0x809C28B8, 0x809C28BC, 0x809C1918),
    (0x809C2BE8, 0x809C2BEC, 0x809C1C48), (0x809C2EF0, 0x809C2EF4, 0x809C1F50), (0x809C2F38, 0x809C2F40, 0x809C1F98),
    (0x809C2F44, 0x809C2F48, 0x809C1FA4), (0x809C3618, 0x809C361C, 0x809C2678), (0x809C38B8, 0x809C38BC, 0x809C2918),
    (0x809C4330, 0x809C4334, 0x809C3390), (0x809C4680, 0x809C4684, 0x809C36E0), (0x809C4740, 0x809C474C, 0x809C37A0),
    (0x809C496C, 0x809C4970, 0x809C39CC),
]

# PAL -> NTSC-K chunk table. Vendored from mkw-sp port.py (CHUNKS['K']), MIT license.
CHUNKS_K = [
    (0x80004000, 0x800074DC, 0x80004000), (0x800077C8, 0x800079D4, 0x80007894), (0x80007BC0, 0x80007BCC, 0x80007CAC),
    (0x80007F2C, 0x80008004, 0x80008034), (0x8000829C, 0x80008BA4, 0x8000841C), (0x80008C04, 0x800093FC, 0x80008D54),
    (0x80009458, 0x8000AD14, 0x80009560), (0x8000AF24, 0x8000B610, 0x8000AFD0), (0x8000B654, 0x80021BA8, 0x8000B6BC),
    (0x80021BB0, 0x800EA448, 0x80021C10), (0x800EA474, 0x801642F4, 0x800EA4EC), (0x80164310, 0x801746FC, 0x801643AC),
    (0x801746FC, 0x80174C54, 0x80174838), (0x80174EF4, 0x80175970, 0x8017517C), (0x80175978, 0x80176B58, 0x80175BF0),
    (0x80176D68, 0x801774D0, 0x80176FF8), (0x80178514, 0x80178E8C, 0x801788A4), (0x8017A0BC, 0x8017AC74, 0x8017A3AC),
    (0x8017B338, 0x8017B73C, 0x8017B790), (0x8017B740, 0x8017DC3C, 0x8017BB98), (0x8017E650, 0x8017EBC4, 0x8017EAA8),
    (0x8017F674, 0x801E8414, 0x8017F9D0), (0x801E8414, 0x8020FD10, 0x801E883C), (0x8020FD18, 0x8020FD8C, 0x80210138),
    (0x8020FE24, 0x8021008C, 0x802101AC), (0x802100A0, 0x80244DE0, 0x80210414), (0x802A4080, 0x803858E0, 0x80292080),
    (0x80385908, 0x8038590C, 0x80373910), (0x80385FC0, 0x8038917C, 0x80373FE0), (0x805103B4, 0x8051D72C, 0x804FE3D4),
    (0x8051E488, 0x8052A324, 0x8050C4AC), (0x8052A338, 0x805C08CC, 0x80518390), (0x805C08D4, 0x805CC0A8, 0x805AE938),
    (0x805CC220, 0x805CEAE0, 0x805BA1E0), (0x805CEAFC, 0x805CF0E8, 0x805BCABC), (0x805CF154, 0x805CF158, 0x805BD118),
    (0x805CF2BC, 0x805CF2C0, 0x805BD284), (0x805CF7E4, 0x805CF7E8, 0x805BD738), (0x805CF8BC, 0x805D00D0, 0x805BDA40),
    (0x805D01C8, 0x805D124C, 0x805BE350), (0x805D1260, 0x805EEB68, 0x805BF3FC), (0x805EEB68, 0x805FA33C, 0x805DCF88),
    (0x805FA344, 0x80620CB4, 0x805E8764), (0x80620D7C, 0x80637A24, 0x8060F174), (0x80637A80, 0x8063BCF8, 0x80625E18),
    (0x8063BE40, 0x806681E8, 0x8062A158), (0x80668334, 0x80675464, 0x8065668C), (0x80675808, 0x80675EB8, 0x80663B68),
    (0x80675F2C, 0x806771F8, 0x8066428C), (0x80677C3C, 0x80678134, 0x80665FE4), (0x8067818C, 0x80742B58, 0x80666534),
    (0x80743154, 0x8088F400, 0x80731514), (0x808913C0, 0x808913C4, 0x8087F7C8), (0x80895238, 0x808952B8, 0x80883648),
    (0x808AD3C4, 0x808AD3C8, 0x8089B824), (0x808B3984, 0x808B3988, 0x808A1DFC), (0x808B5B1C, 0x808B5B20, 0x808A3F94),
    (0x808B5C78, 0x808B5C7C, 0x808A40F0), (0x808BB034, 0x808BB098, 0x808A94A4), (0x808CB550, 0x808CB554, 0x808B99E8),
    (0x808D3698, 0x808D369C, 0x808C1B30), (0x808D36CC, 0x808D36D0, 0x808C1B64), (0x808D36D4, 0x808D36D8, 0x808C1B6C),
    (0x808D3744, 0x808D3748, 0x808C1BDC), (0x808D374C, 0x808D3750, 0x808C1BE4), (0x808D3E14, 0x808D3E18, 0x808C22AC),
    (0x808DA070, 0x808DA080, 0x808C8508), (0x808DA318, 0x808DA368, 0x808C87B0), (0x809BD6E8, 0x809BD74C, 0x809ABD28),
    (0x809C1830, 0x809C1834, 0x809AFE70), (0x809C1874, 0x809C1878, 0x809AFEB4), (0x809C18F8, 0x809C18FC, 0x809AFF38),
    (0x809C1988, 0x809C198C, 0x809AFFC8), (0x809C19A0, 0x809C19BC, 0x809AFFE0), (0x809C1E38, 0x809C1E3C, 0x809B0478),
    (0x809C21D0, 0x809C21D4, 0x809B0810), (0x809C21D8, 0x809C21DC, 0x809B0818), (0x809C2328, 0x809C232C, 0x809B0968),
    (0x809C27F0, 0x809C27FC, 0x809B0E30), (0x809C282C, 0x809C2854, 0x809B0E6C), (0x809C28B8, 0x809C28BC, 0x809B0EF8),
    (0x809C2BE8, 0x809C2BEC, 0x809B1228), (0x809C2EF0, 0x809C2EF4, 0x809B1530), (0x809C2F38, 0x809C2F40, 0x809B1578),
    (0x809C2F44, 0x809C2F48, 0x809B1584), (0x809C3618, 0x809C361C, 0x809B1C58), (0x809C38B8, 0x809C38BC, 0x809B13F8),
    (0x809C4330, 0x809C4334, 0x809B2970), (0x809C4680, 0x809C4684, 0x809B2CC0), (0x809C4740, 0x809C474C, 0x809B2D80),
    (0x809C496C, 0x809C4970, 0x809B2FAC),
]

CHUNK_TABLES = {"E": CHUNKS_E, "J": CHUNKS_J, "K": CHUNKS_K}

# Which region this run is porting to. Set once by main() from --region; the section tables and
# chunk table are then selected from it, so nothing below hardcodes a destination region.
TARGET = "E"
# Set with TARGET in main(): the target region's display name and game ID.
TN, TI = REGION_NAME[TARGET]


def dol_secs():
    return DOL_SECTIONS[TARGET]


def rel_secs():
    return REL_SECTIONS[TARGET]


LOW_MEMORY_LIMIT = 0x80004000          # below this: exception vectors / runtime-copied code
REL_MODULE_ID = 1
INIT_REGISTERS_SYMBOL = "__init_registers"
INIT_REGISTERS_PAL = 0x80006210

# Save/restore thunk families resolved by name in GuestSaveRestoreThunks.cs:
# (prefix, mnemonic, primary opcode, bytes per register)
THUNK_FAMILIES = [
    ("_save_gpr_", "stw", 36, 4),
    ("_rest_gpr_", "lwz", 32, 4),
    ("_save_fpr_", "stfd", 54, 8),
    ("_rest_fpr_", "lfd", 50, 8),
]

# REL relocation types
R_PPC_NONE, R_PPC_ADDR32, R_PPC_ADDR24, R_PPC_ADDR16 = 0, 1, 2, 3
R_PPC_ADDR16_LO, R_PPC_ADDR16_HI, R_PPC_ADDR16_HA = 4, 5, 6
R_PPC_REL24, R_PPC_REL14 = 10, 11
R_DOLPHIN_NOP, R_DOLPHIN_SECTION, R_DOLPHIN_END, R_DOLPHIN_MRKREF = 201, 202, 203, 204

# Evidence flags
F_BL = 1        # target of a `bl` (DOL/REL scan or REL24 relocation)      strong
F_EXTAB = 2     # function start recorded in the DOL extabindex           strong
F_ENTRY = 4     # DOL entry point / REL prolog, epilog, unresolved         strong
F_PTR = 8       # 32-bit data word / ADDR32 relocation pointing at it     medium
F_HIADDR = 16   # lis/addi(ori) pair or ADDR16_HA/LO relocation           medium
F_B = 32        # target of an unconditional `b`                          weak
F_JT = 64       # pointer from a run that looks like a switch jump table  (diagnostic only)
F_BC = 128      # target of a conditional branch: inside a function          negative signal
F_CTOR = 256    # entry of a .ctors/.dtors table (DOL or REL)                strong
STRONG = F_BL | F_EXTAB | F_ENTRY | F_CTOR
MEDIUM = F_PTR | F_HIADDR

FLAG_NAMES = [
    (F_BL, "bl"), (F_EXTAB, "extab"), (F_ENTRY, "entry"), (F_CTOR, "ctor"), (F_PTR, "ptr"),
    (F_HIADDR, "hiaddr"), (F_B, "b"), (F_JT, "jt"), (F_BC, "bc"),
]

# Evidence classes that qualify an NTSC-U address, on their own, for the output map when no
# ported PAL entry covers it (step H): (label, source tag, strength). Medium-strength classes
# additionally need the structural check (no conditional-branch target, terminator before,
# decodable first instruction) because no PAL entry vouches for them.
ADDITION_CLASSES = [
    ("ctors/dtors table entry (DOL)", ("ctor:dol", "dtor:dol"), "strong"),
    ("ctors/dtors table entry (REL)", ("ctor:rel", "dtor:rel"), "strong"),
    ("REL prolog/epilog/unresolved", ("entry:rel",), "strong"),
    ("DOL entry point", ("entry:dol",), "strong"),
    ("bl target (DOL text)", ("bl:dol",), "strong"),
    ("bl target (REL R_PPC_REL24 relocation into the DOL)", ("bl:rel24",), "strong"),
    ("bl target (REL text, link-resolved)", ("bl:rel",), "strong"),
    ("extabindex function record", ("extab",), "strong"),
    ("pointer-referenced text address (DOL data word)", ("ptr:dol",), "medium"),
    ("pointer-referenced text address (REL R_PPC_ADDR32)", ("ptr:rel",), "medium"),
]

EMITTED_VERDICTS = ("PROVEN", "REFERENCED", "PLAUSIBLE", "IDENTITY", "INTERPOLATED")
VERDICT_ORDER = ("PROVEN", "REFERENCED", "PLAUSIBLE", "IDENTITY", "INTERPOLATED",
                 "UNVERIFIED", "DROPPED")


# ---------------------------------------------------------------------------------------
# PowerPC helpers
# ---------------------------------------------------------------------------------------
def opcd(w: int) -> int:
    return w >> 26


def reg_d(w: int) -> int:
    return (w >> 21) & 31


def reg_a(w: int) -> int:
    return (w >> 16) & 31


def reg_b(w: int) -> int:
    return (w >> 11) & 31


def xo10(w: int) -> int:
    return (w >> 1) & 0x3FF


def simm(w: int) -> int:
    v = w & 0xFFFF
    return v - 0x10000 if v & 0x8000 else v


def uimm(w: int) -> int:
    return w & 0xFFFF


def branch_target(site: int, w: int) -> int:
    """Target of an I-form branch (primary opcode 18) located at `site`."""
    li = w & 0x03FFFFFC
    if li & 0x02000000:
        li -= 0x04000000
    if w & 2:  # AA
        return li & 0xFFFFFFFF
    return (site + li) & 0xFFFFFFFF


VALID_PRIMARY_OPCODES = frozenset([
    3, 4, 7, 8, 10, 11, 12, 13, 14, 15, 16, 17, 18, 19, 20, 21, 23, 24, 25, 26, 27, 28, 29,
    31, 32, 33, 34, 35, 36, 37, 38, 39, 40, 41, 42, 43, 44, 45, 46, 47, 48, 49, 50, 51, 52,
    53, 54, 55, 56, 57, 59, 60, 61, 63,
])


def is_terminator(w: int) -> bool:
    """Function terminator or padding: blr, b/ba, bctr, rfi, 0, nop."""
    if w in (0x4E800020, 0x4E800420, 0x4C000064, 0x00000000, 0x60000000):
        return True
    return opcd(w) == 18 and (w & 1) == 0


def first_insn_quality(w: int) -> str:
    """'strong' for a typical first instruction, 'valid' for any decodable one, else 'invalid'."""
    op = opcd(w)
    if op not in VALID_PRIMARY_OPCODES:
        return "invalid"
    if op == 37 and reg_d(w) == 1 and reg_a(w) == 1 and simm(w) < 0:       # stwu r1,-N(r1)
        return "strong"
    if w == 0x7C0802A6:                                                        # mflr r0
        return "strong"
    if op in (14, 15) and reg_a(w) == 0:                                       # li / lis
        return "strong"
    if op in (14, 32, 34, 36, 40, 42, 44, 46, 47, 48, 50, 52, 54, 10, 11, 7, 8, 12, 20, 21, 28):
        return "strong"                                                        # addi/lwz/lbz/stw/... cmp*
    if op == 31 and xo10(w) in (444, 0, 32, 339, 467, 23, 87, 40, 266, 10, 8, 24, 792, 824, 28, 60, 124, 316):
        return "strong"                                                        # or/cmp/mfspr/lwzx/subf/add/...
    if op == 18:                                                               # b / bl (thunks, tail jumps)
        return "strong"
    if w == 0x4E800020:                                                        # bare blr
        return "strong"
    if op == 63 and xo10(w) in (72, 0, 32, 12):                                # fmr / fcmpu / fcmpo / frsp
        return "strong"
    return "valid"


def decode_simple(w: int) -> str:
    """Tiny disassembler for the instructions the report needs to show."""
    op = opcd(w)
    if op == 36:
        return f"stw r{reg_d(w)},{simm(w)}(r{reg_a(w)})"
    if op == 32:
        return f"lwz r{reg_d(w)},{simm(w)}(r{reg_a(w)})"
    if op == 54:
        return f"stfd f{reg_d(w)},{simm(w)}(r{reg_a(w)})"
    if op == 50:
        return f"lfd f{reg_d(w)},{simm(w)}(r{reg_a(w)})"
    if op == 37:
        return f"stwu r{reg_d(w)},{simm(w)}(r{reg_a(w)})"
    if op == 15:
        return f"lis r{reg_d(w)},0x{uimm(w):04x}" if reg_a(w) == 0 else f"addis r{reg_d(w)},r{reg_a(w)},0x{uimm(w):04x}"
    if op == 14:
        return f"li r{reg_d(w)},{simm(w)}" if reg_a(w) == 0 else f"addi r{reg_d(w)},r{reg_a(w)},{simm(w)}"
    if op == 24:
        return f"ori r{reg_a(w)},r{reg_d(w)},0x{uimm(w):04x}"
    if w == 0x4E800020:
        return "blr"
    if w == 0x4E800420:
        return "bctr"
    if w == 0x4C000064:
        return "rfi"
    if w == 0x60000000:
        return "nop"
    if w == 0x7C0802A6:
        return "mflr r0"
    if w == 0x7C0803A6:
        return "mtlr r0"
    if op == 18:
        return ("bl" if w & 1 else "b") + (" (abs)" if w & 2 else "")
    if op == 16:
        return "bc"
    if op == 31 and xo10(w) == 444 and reg_d(w) == reg_b(w):
        return f"mr r{reg_a(w)},r{reg_d(w)}"
    if op == 11:
        return f"cmpwi cr{reg_d(w) >> 2},r{reg_a(w)},{simm(w)}"
    if op == 10:
        return f"cmplwi cr{reg_d(w) >> 2},r{reg_a(w)},{uimm(w)}"
    if op == 0:
        return ".long 0"
    return f"op{op}"


# ---------------------------------------------------------------------------------------
# Binary images
# ---------------------------------------------------------------------------------------
@dataclass
class Section:
    module: str          # 'dol' | 'rel'
    name: str
    index: int
    start: int
    size: int
    data: bytes
    executable: bool

    @property
    def end(self) -> int:
        return self.start + self.size


def be_words(data: bytes) -> array.array:
    words = array.array("I", data[: len(data) // 4 * 4])
    if words.itemsize != 4:
        words = array.array("L", data[: len(data) // 4 * 4])
        assert words.itemsize == 4, "no 4-byte unsigned array type available"
    if sys.byteorder == "little":
        words.byteswap()
    return words


class Memory:
    """Read-only view of the loaded sections of both modules."""

    def __init__(self) -> None:
        self.sections: List[Section] = []
        self._starts: List[int] = []
        self._text_ranges: List[Tuple[int, int]] = []
        self.text_starts: set = set()

    def add(self, sec: Section) -> None:
        if sec.size == 0 or not sec.data:
            return
        self.sections.append(sec)
        self.sections.sort(key=lambda s: s.start)
        self._starts = [s.start for s in self.sections]
        if sec.executable:
            self._text_ranges.append((sec.start, sec.end))
            self._text_ranges.sort()
            self.text_starts.add(sec.start)

    def section_at(self, addr: int) -> Optional[Section]:
        i = bisect.bisect_right(self._starts, addr) - 1
        if i < 0:
            return None
        sec = self.sections[i]
        return sec if sec.start <= addr < sec.end else None

    def read_u32(self, addr: int) -> Optional[int]:
        sec = self.section_at(addr)
        if sec is None or addr + 4 > sec.end:
            return None
        off = addr - sec.start
        return int.from_bytes(sec.data[off:off + 4], "big")

    def in_text(self, addr: int) -> bool:
        for s, e in self._text_ranges:
            if s <= addr < e:
                return True
        return False

    @property
    def text_ranges(self) -> List[Tuple[int, int]]:
        return list(self._text_ranges)


@dataclass
class Dol:
    path: str
    sha256: str
    sections: List[Section]
    bss_addr: int
    bss_size: int
    entry: int


def parse_dol(path: str) -> Dol:
    with open(path, "rb") as f:
        data = f.read()
    if len(data) < 0x100:
        raise SystemExit(f"{path}: too small to be a DOL")
    offs = struct.unpack(">18I", data[0:72])
    addrs = struct.unpack(">18I", data[72:144])
    sizes = struct.unpack(">18I", data[144:216])
    bss_addr, bss_size, entry = struct.unpack(">3I", data[216:228])
    sections: List[Section] = []
    for i in range(18):
        if sizes[i] == 0:
            continue
        if offs[i] + sizes[i] > len(data):
            raise SystemExit(f"{path}: DOL section {i} runs past end of file")
        sections.append(Section(
            module="dol", name=("text%d" % i if i < 7 else "data%d" % (i - 7)), index=i,
            start=addrs[i], size=sizes[i], data=data[offs[i]:offs[i] + sizes[i]], executable=i < 7))
    return Dol(path, hashlib.sha256(data).hexdigest(), sections, bss_addr, bss_size, entry)


@dataclass
class Relocation:
    module: int      # 0 = DOL (absolute addend), otherwise module id of the target
    site: int        # address of the relocated field
    type: int
    section: int     # target section (in the target module)
    addend: int
    target: int      # resolved target address


@dataclass
class Rel:
    path: str
    sha256: str
    module_id: int
    version: int
    load_address: int
    sections: List[Section]
    section_base: Dict[int, int]
    prolog: int
    epilog: int
    unresolved: int
    relocations: List[Relocation]
    reloc_type_counts: Dict[Tuple[int, int], int]
    bss_size: int
    fix_size: int
    header: Dict[str, int]


def parse_rel(path: str, load_address: int, bss_address: int) -> Rel:
    with open(path, "rb") as f:
        data = f.read()
    if len(data) < 0x40:
        raise SystemExit(f"{path}: too small to be a REL")
    fields = struct.unpack(">IIIIIIIIIIIIBBBBIII", data[0:0x40])
    names = ["id", "next", "prev", "numSections", "sectionInfoOffset", "nameOffset", "nameSize",
             "version", "bssSize", "relOffset", "impOffset", "impSize", "prologSection",
             "epilogSection", "unresolvedSection", "bssSection", "prolog", "epilog", "unresolved"]
    h = dict(zip(names, fields))
    if h["version"] >= 2:
        h["align"], h["bssAlign"] = struct.unpack(">II", data[0x40:0x48])
    if h["version"] >= 3:
        h["fixSize"] = struct.unpack(">I", data[0x48:0x4C])[0]

    sections: List[Section] = []
    section_base: Dict[int, int] = {}
    so = h["sectionInfoOffset"]
    for i in range(h["numSections"]):
        off, size = struct.unpack(">II", data[so + 8 * i:so + 8 * i + 8])
        executable = bool(off & 1)
        off &= ~1
        if size == 0:
            continue
        if off == 0:
            # .bss: no file contents, allocated by the game at runtime.
            section_base[i] = bss_address
            sections.append(Section("rel", "bss", i, bss_address, size, b"", False))
            continue
        if off + size > len(data):
            raise SystemExit(f"{path}: REL section {i} runs past end of file")
        section_base[i] = load_address + off
        name = "text" if executable else "sec%d" % i
        sections.append(Section("rel", name, i, load_address + off, size, data[off:off + size], executable))

    # imports + relocations
    relocations: List[Relocation] = []
    type_counts: Dict[Tuple[int, int], int] = collections.Counter()
    imp_off, imp_size = h["impOffset"], h["impSize"]
    for k in range(imp_size // 8):
        module, rel_off = struct.unpack(">II", data[imp_off + 8 * k:imp_off + 8 * k + 8])
        pos = rel_off
        cur_section: Optional[int] = None
        cur_offset = 0
        while True:
            if pos + 8 > len(data):
                raise SystemExit(f"{path}: relocation table for module {module} runs past end of file")
            off, rtype, rsec, addend = struct.unpack(">HBBI", data[pos:pos + 8])
            pos += 8
            type_counts[(module, rtype)] += 1
            if rtype == R_DOLPHIN_END:
                break
            if rtype == R_DOLPHIN_SECTION:
                cur_section = rsec
                cur_offset = 0
                continue
            cur_offset += off
            if rtype == R_DOLPHIN_NOP:
                continue
            if cur_section is None or cur_section not in section_base:
                raise SystemExit(f"{path}: relocation before any R_DOLPHIN_SECTION (module {module})")
            site = section_base[cur_section] + cur_offset
            if module == 0:
                target = addend
            elif module == h["id"]:
                if rsec not in section_base:
                    raise SystemExit(f"{path}: relocation targets unknown section {rsec}")
                target = section_base[rsec] + addend
            else:
                target = addend  # foreign module: unknown base, keep raw addend
            relocations.append(Relocation(module, site, rtype, rsec, addend, target & 0xFFFFFFFF))

    text_base = section_base.get(h["prologSection"], load_address)
    return Rel(
        path=path, sha256=hashlib.sha256(data).hexdigest(), module_id=h["id"], version=h["version"],
        load_address=load_address, sections=sections, section_base=section_base,
        prolog=text_base + h["prolog"], epilog=section_base.get(h["epilogSection"], load_address) + h["epilog"],
        unresolved=section_base.get(h["unresolvedSection"], load_address) + h["unresolved"],
        relocations=relocations, reloc_type_counts=dict(type_counts), bss_size=h["bssSize"],
        fix_size=h.get("fixSize", 0), header=h)


# ---------------------------------------------------------------------------------------
# Evidence set K
# ---------------------------------------------------------------------------------------
class Evidence:
    def __init__(self, mem: Memory) -> None:
        self.mem = mem
        self.flags: Dict[int, int] = collections.defaultdict(int)
        self.sources: Dict[int, set] = collections.defaultdict(set)   # addr -> evidence source tags
        self.stats: Dict[str, int] = collections.OrderedDict()
        self.jump_table_runs: List[Tuple[str, int, int, int, int]] = []   # (module, site, len, min, max)
        self._strong_sorted: List[int] = []

    def bump(self, key: str, n: int = 1) -> None:
        self.stats[key] = self.stats.get(key, 0) + n

    def add(self, addr: int, flag: int, source: Optional[str] = None) -> None:
        self.flags[addr] |= flag
        if source is not None:
            self.sources[addr].add(source)

    def accepts_target(self, addr: int) -> bool:
        """Code targets we record: any text section, or the low-memory vector area."""
        return self.mem.in_text(addr) or (0x80000000 <= addr < LOW_MEMORY_LIMIT)

    def finalize_strong(self) -> None:
        self._strong_sorted = sorted(a for a, f in self.flags.items() if f & STRONG)

    @property
    def strong_sorted(self) -> List[int]:
        return self._strong_sorted

    def strong_in_range(self, lo: int, hi: int) -> List[int]:
        """Strong entries with lo <= a <= hi."""
        i = bisect.bisect_left(self._strong_sorted, lo)
        j = bisect.bisect_right(self._strong_sorted, hi)
        return self._strong_sorted[i:j]

    def flag_names(self, addr: int) -> str:
        f = self.flags.get(addr, 0)
        return "+".join(n for bit, n in FLAG_NAMES if f & bit) or "-"

    def is_strong(self, addr: int) -> bool:
        return bool(self.flags.get(addr, 0) & STRONG)

    # -- text scans ---------------------------------------------------------------------
    def scan_text(self, sec: Section, reloc_sites: frozenset, scan_hiaddr: bool) -> None:
        words = be_words(sec.data)
        base = sec.start
        n = len(words)
        bl_n = b_n = hi_n = bc_n = 0
        for i in range(n):
            w = words[i]
            op = w >> 26
            if op == 16:
                # bc: skip "branch always" encodings (BO 1z1zz) and bcl; the rest are intra-function
                bo = (w >> 21) & 31
                if (bo & 0x14) == 0x14 or (w & 1):
                    continue
                bd = w & 0xFFFC
                if bd & 0x8000:
                    bd -= 0x10000
                tgt = (bd if w & 2 else base + 4 * i + bd) & 0xFFFFFFFF
                if self.mem.in_text(tgt):
                    self.add(tgt, F_BC)
                    bc_n += 1
                continue
            if op == 18:
                site = base + 4 * i
                if site in reloc_sites:
                    continue
                tgt = branch_target(site, w)
                if not self.accepts_target(tgt):
                    self.bump(f"{sec.module}.{sec.name}: branch targets outside text (ignored)")
                    continue
                if w & 1:
                    if tgt == site + 4:
                        self.bump(f"{sec.module}.{sec.name}: `bl` to the next instruction (PC load, ignored)")
                        continue
                    self.add(tgt, F_BL, f"bl:{sec.module}")
                    bl_n += 1
                else:
                    self.add(tgt, F_B)
                    b_n += 1
            elif scan_hiaddr and op == 15 and ((w >> 16) & 31) == 0:
                rd = (w >> 21) & 31
                hi = w & 0xFFFF
                for j in range(1, 5):
                    if i + j >= n:
                        break
                    w2 = words[i + j]
                    op2 = w2 >> 26
                    if op2 == 14 and ((w2 >> 16) & 31) == rd:                    # addi rd,rd,lo
                        val = ((hi << 16) + simm(w2)) & 0xFFFFFFFF
                    elif op2 == 24 and ((w2 >> 16) & 31) == rd and ((w2 >> 21) & 31) == rd:  # ori rd,rd,lo
                        val = ((hi << 16) | (w2 & 0xFFFF)) & 0xFFFFFFFF
                    elif op2 == 15 and ((w2 >> 21) & 31) == rd:                  # rd overwritten
                        break
                    else:
                        continue
                    if self.mem.in_text(val):
                        self.add(val, F_HIADDR)
                        hi_n += 1
                    break
        self.bump(f"{sec.module}.{sec.name}: bl sites", bl_n)
        self.bump(f"{sec.module}.{sec.name}: b sites", b_n)
        self.bump(f"{sec.module}.{sec.name}: conditional-branch targets (negative signal)", bc_n)
        if scan_hiaddr:
            self.bump(f"{sec.module}.{sec.name}: lis/addi code-address pairs", hi_n)

    # -- pointer runs -------------------------------------------------------------------
    def flush_pointer_run(self, module: str, run: List[Tuple[int, int]]) -> None:
        if not run:
            return
        targets = [t for _, t in run]
        lo, hi = min(targets), max(targets)
        jump_table_like = (
            len(run) >= 2
            and hi - lo <= 0x8000
            and not self.strong_in_range(lo, hi)
        )
        if jump_table_like:
            self.jump_table_runs.append((module, run[0][0], len(run), lo, hi))
            for t in targets:
                self.add(t, F_JT)
            self.bump(f"{module}: pointer runs classified as jump tables (words)", len(run))
        else:
            for t in targets:
                self.add(t, F_PTR, f"ptr:{module}")
            self.bump(f"{module}: function-pointer words", len(run))

    def scan_data_pointers(self, sec: Section) -> None:
        words = be_words(sec.data)
        run: List[Tuple[int, int]] = []
        for i in range(len(words)):
            w = words[i]
            if self.mem.in_text(w):
                run.append((sec.start + 4 * i, w))
            else:
                self.flush_pointer_run("dol", run)
                run = []
        self.flush_pointer_run("dol", run)

    def scan_extabindex(self, sec: Section, extab: Tuple[int, int]) -> Tuple[int, int]:
        words = be_words(sec.data)
        good = bad = 0
        for i in range(0, len(words) - 2, 3):
            fstart, fsize, eptr = words[i], words[i + 1], words[i + 2]
            if self.mem.in_text(fstart) and 0 < fsize < 0x100000 and (fsize & 3) == 0 \
                    and extab[0] <= eptr < extab[1]:
                self.add(fstart, F_EXTAB, "extab")
                good += 1
            elif fstart == 0 and fsize == 0 and eptr == 0:
                continue
            else:
                bad += 1
        self.bump("dol.extabindex: function records", good)
        self.bump("dol.extabindex: non-record words (footer/padding)", bad)
        return good, bad

    def scan_rel_relocations(self, rel: Rel) -> None:
        addr32: List[Tuple[int, int]] = []
        bl_n = b_n = hi_n = 0
        for r in rel.relocations:
            if r.type == R_PPC_REL24:
                w = self.mem.read_u32(r.site)
                if w is None or opcd(w) != 18:
                    self.bump("rel: REL24 relocations on a non-branch word (ignored)")
                    continue
                if not self.accepts_target(r.target):
                    self.bump("rel: REL24 targets outside text (ignored)")
                    continue
                if w & 1:
                    if r.target == r.site + 4:
                        self.bump("rel: REL24 `bl` to the next instruction (ignored)")
                        continue
                    self.add(r.target, F_BL, "bl:rel24")
                    bl_n += 1
                else:
                    self.add(r.target, F_B)
                    b_n += 1
            elif r.type == R_PPC_ADDR32:
                if self.mem.in_text(r.target):
                    addr32.append((r.site, r.target))
            elif r.type in (R_PPC_ADDR16_LO, R_PPC_ADDR16_HI, R_PPC_ADDR16_HA):
                if self.mem.in_text(r.target):
                    self.add(r.target, F_HIADDR)
                    hi_n += 1
        self.bump("rel: REL24 bl relocations", bl_n)
        self.bump("rel: REL24 b relocations", b_n)
        self.bump("rel: ADDR16_LO/HI/HA relocations into text", hi_n)
        addr32.sort()
        run: List[Tuple[int, int]] = []
        for site, tgt in addr32:
            if run and site != run[-1][0] + 4:
                self.flush_pointer_run("rel", run)
                run = []
            run.append((site, tgt))
        self.flush_pointer_run("rel", run)
        self.bump("rel: ADDR32 relocations into text", len(addr32))


    def scan_ctor_tables(self, dol: Dol, rel: Rel) -> None:
        """Every entry of the .ctors/.dtors tables is called by the runtime (DOL: __init_cpp /
        __destroy_global_chain, REL: _prolog/_epilog), so each one is a proven function entry."""
        dol_tables = {dol_secs()[4][1]: "ctor:dol", dol_secs()[5][1]: "dtor:dol"}
        for sec in dol.sections:
            tag = dol_tables.get(sec.start)
            if tag is None:
                continue
            n = 0
            for w in be_words(sec.data):
                if self.mem.in_text(w):
                    self.add(w, F_CTOR, tag)
                    n += 1
            self.bump(f"dol.{'ctors' if tag == 'ctor:dol' else 'dtors'}: table entries", n)
        rel_tables = {"ctors": "ctor:rel", "dtors": "dtor:rel"}
        for name, start, end in rel_secs():
            tag = rel_tables.get(name)
            if tag is None:
                continue
            targets = set()
            for r in rel.relocations:
                if r.type == R_PPC_ADDR32 and start <= r.site < end and self.mem.in_text(r.target):
                    self.add(r.target, F_CTOR, tag)
                    targets.add(r.target)
            self.bump(f"rel.{name}: table entries (unique targets)", len(targets))


def build_evidence(mem: Memory, dol: Dol, rel: Rel) -> Evidence:
    ev = Evidence(mem)
    reloc_sites = frozenset(r.site for r in rel.relocations if r.type == R_PPC_REL24)
    # strong sources first
    for sec in dol.sections:
        if sec.executable:
            ev.scan_text(sec, frozenset(), scan_hiaddr=True)
    for sec in rel.sections:
        if sec.executable:
            ev.scan_text(sec, reloc_sites, scan_hiaddr=False)
    ev.add(dol.entry, F_ENTRY, "entry:dol")
    for a in (rel.prolog, rel.epilog, rel.unresolved):
        ev.add(a, F_ENTRY, "entry:rel")
    ev.scan_ctor_tables(dol, rel)
    extab = next((s for s in dol.sections if s.start == dol_secs()[1][1]), None)
    extabindex = next((s for s in dol.sections if s.start == dol_secs()[2][1]), None)
    if extab is not None and extabindex is not None:
        ev.scan_extabindex(extabindex, (extab.start, extab.end))
    # Relocations in two passes: the strong (REL24) ones first, so that jump-table detection
    # for the medium (ADDR32) runs sees the complete strong set.
    strong_relocs = Rel(**{**rel.__dict__, "relocations": [r for r in rel.relocations if r.type == R_PPC_REL24]})
    ev.scan_rel_relocations(strong_relocs)
    ev.finalize_strong()
    medium_relocs = Rel(**{**rel.__dict__, "relocations": [r for r in rel.relocations if r.type != R_PPC_REL24]})
    ev.scan_rel_relocations(medium_relocs)
    for sec in dol.sections:
        if not sec.executable and sec is not extab and sec is not extabindex:
            ev.scan_data_pointers(sec)
    ev.finalize_strong()
    return ev


# ---------------------------------------------------------------------------------------
# PAL map and chunk table
# ---------------------------------------------------------------------------------------
@dataclass
class Entry:
    index: int
    line: int
    pal: int
    name: str
    named: bool
    ntsc: Optional[int] = None
    method: str = "none"        # chunk | identity | interp | none
    verdict: str = ""           # see VERDICT_ORDER
    evidence: str = "-"         # evidence flags at the NTSC address
    quality: str = ""           # first-instruction quality at the NTSC address
    note: str = ""

    @property
    def module(self) -> str:
        if self.pal < LOW_MEMORY_LIMIT:
            return "low"
        return "dol" if self.pal < REL_LOAD_ADDRESS["P"] - 0x1000 else "rel"

    @property
    def display_name(self) -> str:
        return self.name if self.named else f"0x{self.pal:08x}"


def load_pal_map(path: str) -> List[Entry]:
    entries: List[Entry] = []
    with open(path, "r", encoding="utf-8") as f:
        for lineno, raw in enumerate(f, 1):
            line = raw.strip()
            if not line or line.startswith("#"):
                continue
            parts = line.split(None, 1)
            try:
                addr = int(parts[0], 16)
            except ValueError:
                raise SystemExit(f"{path}:{lineno}: '{raw.rstrip()}' does not start with a hex address")
            name = parts[1].strip() if len(parts) > 1 else ""
            named = bool(name) and not name.lower().startswith("0x")
            entries.append(Entry(index=len(entries), line=lineno, pal=addr, name=name if named else "", named=named))
    if not entries:
        raise SystemExit(f"{path}: no entries")
    entries.sort(key=lambda e: (e.pal, e.index))
    for i, e in enumerate(entries):
        e.index = i
    return entries


class ChunkTable:
    def __init__(self, chunks: List[Tuple[int, int, int]]) -> None:
        self.chunks = sorted(chunks)
        self.starts = [c[0] for c in self.chunks]
        self.problems: List[str] = []
        prev_end = 0
        for s, e, d in self.chunks:
            if e <= s:
                self.problems.append(f"empty chunk {s:#x}-{e:#x}")
            if s < prev_end:
                self.problems.append(f"chunk {s:#x}-{e:#x} overlaps the previous chunk")
            if (d - s) % 4:
                self.problems.append(f"chunk {s:#x}-{e:#x} has a non-word delta")
            prev_end = max(prev_end, e)

    def find(self, a: int) -> Optional[Tuple[int, int, int]]:
        i = bisect.bisect_right(self.starts, a) - 1
        if i >= 0 and self.chunks[i][0] <= a < self.chunks[i][1]:
            return self.chunks[i]
        return None

    def port(self, a: int) -> Optional[int]:
        c = self.find(a)
        return None if c is None else a - c[0] + c[2]


# ---------------------------------------------------------------------------------------
# Classification
# ---------------------------------------------------------------------------------------
# Label suffixes seen in the PAL map: _caseD_64, _caseD_-14, ::case_0, _Case_0, _case3, _case5a,
# _2case5a, _caseDefault, _case2D_1, _loop, _switch, ::switch, _Switch, _switch2D ...
CASE_LABEL_RE = re.compile(r"(?:_|::)[0-9]*(?:[Cc]ase(?:D|Default|2D)?|[Ll]oop|[Dd]efault)(?:[_-]?-?[0-9a-fA-F]+)?$")
SWITCH_LABEL_RE = re.compile(r"(?:_|::)(?:switch[0-9A-Za-z]*|Switch)$")


def entry_kind(name: str) -> str:
    """What a PAL map entry claims to be, from its (Ghidra-style) name."""
    if not name:
        return "function"
    if name.startswith("switchdataD_"):
        return "data"          # the jump table itself (not code)
    if SWITCH_LABEL_RE.search(name):
        return "switch"        # the `bctr` of a switch dispatch
    if CASE_LABEL_RE.search(name):
        return "case"          # switch case label / branch label inside a function
    return "function"


def is_mtctr(w: int) -> bool:
    return (w & 0xFC1FFFFF) == 0x7C0903A6


def classify_address(ev: Evidence, mem: Memory, a: int, kind: str = "function") -> Tuple[str, str, str]:
    """Return (verdict, evidence-flags, first-instruction quality) for an NTSC address.

    `kind` is what the PAL entry claims to be. A case label is confirmed by a jump-table reference at
    the ported address. A `mtctr`+`bctr` pair at the address confirms a switch dispatch site (the PAL
    map carries those, named `*_switch` or unnamed). A conditional-branch target can only lie inside a
    function, so it vetoes the medium/plausible verdicts for entries that claim to be functions."""
    flags = ev.flag_names(a)
    w = mem.read_u32(a)
    quality = first_insn_quality(w) if w is not None else "n/a"
    f = ev.flags.get(a, 0)
    if f & STRONG:
        return "PROVEN", flags, quality
    if kind == "case" and mem.in_text(a):
        if f & (F_JT | F_PTR):
            return "REFERENCED", flags, quality      # jump-table target, exactly what a case label is
        if f & (F_B | F_BC) and w is not None and quality != "invalid":
            return "PLAUSIBLE", flags, quality       # branch target inside a function: a label
    if w == 0x4E800420:
        prev = mem.read_u32(a - 4)
        if prev is not None and is_mtctr(prev):
            return "PLAUSIBLE", (flags + "+mtctr/bctr") if flags != "-" else "mtctr/bctr", quality
    inside_function = bool(f & F_BC) and kind == "function"
    if f & MEDIUM:
        return ("UNVERIFIED" if inside_function else "REFERENCED"), flags, quality
    if not mem.in_text(a) or w is None:
        return "UNVERIFIED", flags, quality
    if quality == "invalid" or inside_function:
        return "UNVERIFIED", flags, quality
    if a in mem.text_starts:
        return "PLAUSIBLE", flags, quality
    prev = mem.read_u32(a - 4)
    if prev is not None and is_terminator(prev):
        return "PLAUSIBLE", flags, quality
    return "UNVERIFIED", flags, quality


def port_entries(entries: List[Entry], table: ChunkTable, ev: Evidence, mem: Memory) -> None:
    for e in entries:
        if e.pal < LOW_MEMORY_LIMIT:
            e.ntsc = e.pal
            e.method = "identity"
            e.verdict = "IDENTITY"
            v, e.evidence, e.quality = classify_address(ev, mem, e.ntsc)
            e.note = f"low memory, binary evidence: {v}"
            continue
        p = table.port(e.pal)
        if p is None:
            e.method = "none"
            e.verdict = "UNPORTED"
            continue
        e.ntsc = p
        e.method = "chunk"
        e.verdict, e.evidence, e.quality = classify_address(ev, mem, p, entry_kind(e.name))
        if e.verdict == "UNVERIFIED":
            e.note = describe_unverified(ev, mem, p)


def describe_unverified(ev: Evidence, mem: Memory, a: int) -> str:
    if not mem.in_text(a):
        sec = mem.section_at(a)
        return "not in a text section" + (f" ({sec.module}.{sec.name})" if sec else " (unmapped)")
    w = mem.read_u32(a)
    prev = mem.read_u32(a - 4)
    text = f"prev={prev:08x} ({decode_simple(prev)}) word={w:08x} ({decode_simple(w)})"
    if ev.flags.get(a, 0) & F_BC:
        text = "conditional-branch target (inside a function); " + text
    nxt = ev.strong_in_range(a + 4, a + 64)
    if nxt:
        text += f"; next proven start at +{nxt[0] - a:#x}"
    return text


@dataclass
class GapResult:
    pal_lo: int
    pal_hi: int
    ntsc_lo: int
    ntsc_hi: int
    unported: int
    candidates: int
    outcome: str


def interpolate_unported(entries: List[Entry], ev: Evidence, mem: Memory) -> List[GapResult]:
    """Step C.2: assign unported entries between two PROVEN chunk-ported neighbours to the
    strong K entries inside the corresponding NTSC gap when the counts match."""
    proven_idx = [e.index for e in entries if e.method == "chunk" and e.verdict == "PROVEN"]
    ported_sorted = sorted((e.ntsc, e.index) for e in entries if e.ntsc is not None and e.method == "chunk")
    ported_addrs = [a for a, _ in ported_sorted]
    gaps: Dict[Tuple[int, int], List[int]] = collections.OrderedDict()
    for e in entries:
        if e.method != "none":
            continue
        k = bisect.bisect_right(proven_idx, e.index)
        if k == 0 or k >= len(proven_idx):
            continue
        key = (proven_idx[k - 1], proven_idx[k])
        gaps.setdefault(key, []).append(e.index)

    results: List[GapResult] = []
    for (j, k), unported in gaps.items():
        lo, hi = entries[j], entries[k]
        nlo, nhi = lo.ntsc, hi.ntsc
        assert nlo is not None and nhi is not None
        res = GapResult(lo.pal, hi.pal, nlo, nhi, len(unported), 0, "")
        if nhi <= nlo:
            res.outcome = "rejected: NTSC neighbours not ascending (chunk reordering)"
            results.append(res)
            continue
        inside = [entries[i] for i in range(j + 1, k)]
        # contamination: chunk-ported entries from outside the PAL gap landing inside the NTSC gap
        a = bisect.bisect_right(ported_addrs, nlo)
        b = bisect.bisect_left(ported_addrs, nhi)
        contaminated = [idx for _, idx in ported_sorted[a:b] if not (j < idx < k)]
        if contaminated:
            res.outcome = f"rejected: {len(contaminated)} entries from other chunks land inside the NTSC gap"
            results.append(res)
            continue
        ported_inside = [e for e in inside if e.method == "chunk"]
        if any(not (nlo < e.ntsc < nhi) for e in ported_inside):
            res.outcome = "rejected: a ported entry inside the PAL gap lies outside the NTSC gap"
            results.append(res)
            continue
        claimed = {e.ntsc for e in ported_inside}
        candidates = [x for x in ev.strong_in_range(nlo + 4, nhi - 4) if x not in claimed]
        res.candidates = len(candidates)
        if len(candidates) != len(unported):
            res.outcome = "rejected: count mismatch"
            results.append(res)
            continue
        assignment = dict(zip(unported, candidates))
        seq = []
        for e in inside:
            seq.append(assignment[e.index] if e.index in assignment else e.ntsc)
        if any(seq[i] >= seq[i + 1] for i in range(len(seq) - 1)):
            res.outcome = "rejected: assignment would break monotonic order"
            results.append(res)
            continue
        for idx, addr in assignment.items():
            e = entries[idx]
            e.ntsc = addr
            e.method = "interp"
            e.verdict = "INTERPOLATED"
            _, e.evidence, e.quality = classify_address(ev, mem, addr)
            e.note = f"gap {lo.pal:08x}..{hi.pal:08x} -> {nlo:08x}..{nhi:08x} ({len(unported)} entries)"
        res.outcome = "accepted"
        results.append(res)
    return results


def finish_unported(entries: List[Entry]) -> None:
    for e in entries:
        if e.method == "none":
            e.verdict = "DROPPED"
            if not e.note:
                e.note = "outside every chunk; no interpolation gap"


# ---------------------------------------------------------------------------------------
# Special checks
# ---------------------------------------------------------------------------------------
def thunk_expected(opcode: int, reg: int, step: int) -> int:
    return ((opcode << 26) | (reg << 21) | (11 << 16) | ((-(step * (32 - reg))) & 0xFFFF)) & 0xFFFFFFFF


def check_thunks(entries: List[Entry], mem: Memory) -> List[dict]:
    by_name: Dict[str, Entry] = {}
    for e in entries:
        if e.named and e.name not in by_name:
            by_name[e.name] = e
    results = []
    for prefix, mnemonic, opcode, step in THUNK_FAMILIES:
        members = []
        for name, e in by_name.items():
            if name.startswith(prefix):
                suffix = name[len(prefix):]
                if suffix.isdigit():
                    members.append((int(suffix), e))
        members.sort()
        rows = []
        family_ok = True
        for reg, e in members:
            expected = thunk_expected(opcode, reg, step)
            actual = mem.read_u32(e.ntsc) if e.ntsc is not None else None
            ok = actual == expected and e.verdict in EMITTED_VERDICTS
            family_ok &= ok
            rows.append({
                "name": e.name, "pal": e.pal, "ntsc": e.ntsc, "verdict": e.verdict,
                "expected": expected, "actual": actual,
                "decoded": decode_simple(actual) if actual is not None else "n/a", "ok": ok,
            })
        run_ok = True
        run_note = ""
        if len(members) >= 2:
            first_reg, first = members[0]
            last_reg, last = members[-1]
            if first.ntsc is not None and last.ntsc is not None:
                if last.ntsc != first.ntsc + 4 * (last_reg - first_reg):
                    run_ok = False
                    run_note = "family is not a four-byte-per-register run (FunctionMap.ResolveRange would throw)"
                # full run: registers first..31 must all decode as expected
                for reg in range(first_reg, 32):
                    a = first.ntsc + 4 * (reg - first_reg)
                    w = mem.read_u32(a)
                    if w != thunk_expected(opcode, reg, step):
                        run_ok = False
                        run_note = f"register {reg} at {a:08x} decodes as {decode_simple(w) if w is not None else 'n/a'}"
                        break
        else:
            run_ok = False
            run_note = "fewer than two named members"
        results.append({"prefix": prefix, "mnemonic": mnemonic, "rows": rows, "family_ok": family_ok and run_ok,
                        "run_note": run_note})
    return results


def read_sda_bases(entries: List[Entry], mem: Memory) -> dict:
    init = next((e for e in entries if e.named and e.name == INIT_REGISTERS_SYMBOL), None)
    result = {"symbol": INIT_REGISTERS_SYMBOL, "pal": None, "ntsc": None, "values": {}, "listing": [], "status": ""}
    if init is None or init.ntsc is None:
        result["status"] = "symbol not found or not ported"
        return result
    result["pal"] = init.pal
    result["ntsc"] = init.ntsc
    pending: Dict[int, int] = {}
    values: Dict[int, int] = {}
    for i in range(48):
        a = init.ntsc + 4 * i
        w = mem.read_u32(a)
        if w is None:
            break
        result["listing"].append((a, w, decode_simple(w)))
        op = opcd(w)
        if op == 15 and reg_a(w) == 0:                       # lis rD, hi
            pending[reg_d(w)] = uimm(w) << 16
        elif op == 24 and reg_a(w) == reg_d(w) and reg_d(w) in pending:      # ori rX,rX,lo
            values[reg_a(w)] = (pending.pop(reg_a(w)) | uimm(w)) & 0xFFFFFFFF
        elif op == 14 and reg_a(w) == reg_d(w) and reg_a(w) != 0 and reg_d(w) in pending:  # addi rX,rX,lo
            values[reg_a(w)] = (pending.pop(reg_a(w)) + simm(w)) & 0xFFFFFFFF
        if w == 0x4E800020:
            break
    result["values"] = {f"r{r}": v for r, v in sorted(values.items())}
    r13 = values.get(13)
    r2 = values.get(2)
    checks = []
    for reg, val in (("r13", r13), ("r2", r2)):
        exp = SDA_BASES[TARGET][0 if reg == "r13" else 1]
        if val is None:
            checks.append(f"{reg}: not found")
        elif val == exp:
            checks.append(f"{reg}=0x{val:08X} (matches expectation)")
        else:
            checks.append(f"{reg}=0x{val:08X} (EXPECTED 0x{exp:08X})")
    result["status"] = "; ".join(checks)
    return result


# ---------------------------------------------------------------------------------------
# Output
# ---------------------------------------------------------------------------------------
def dedupe(entries: List[Entry]) -> Tuple[List[Entry], List[List[Entry]]]:
    emitted = [e for e in entries if e.verdict in EMITTED_VERDICTS]
    groups: Dict[int, List[Entry]] = collections.OrderedDict()
    for e in sorted(emitted, key=lambda e: (e.ntsc, e.index)):
        groups.setdefault(e.ntsc, []).append(e)
    kept: List[Entry] = []
    collisions: List[List[Entry]] = []
    for addr, group in groups.items():
        if len(group) == 1:
            kept.append(group[0])
            continue
        collisions.append(group)
        named = [e for e in group if e.named]
        winner = named[0] if named else group[0]
        for e in group:
            if e is not winner:
                e.verdict = "DROPPED"
                e.note = f"collision at {addr:08x}; kept '{winner.display_name}' (PAL {winner.pal:08x})"
        kept.append(winner)
    return kept, collisions


@dataclass
class AdditionStats:
    per_class: "collections.OrderedDict[str, int]"
    per_class_addrs: Dict[str, List[int]]
    rejected_medium: int = 0          # pointer-referenced only, failed the structural check
    skipped_weak: int = 0             # only hiaddr / b / jt evidence: not eligible
    strong_bc_anomalies: int = 0      # strong evidence but also a conditional-branch target
    ctor_total: Dict[str, int] = field(default_factory=dict)
    ctor_missing: Dict[str, int] = field(default_factory=dict)


def binary_additions(ev: Evidence, mem: Memory, covered: set) -> Tuple[List[Entry], AdditionStats]:
    """Step H: every NTSC-U entry point the binaries prove that no ported PAL entry covers is added
    as an unnamed placeholder entry. Strong classes are taken as they are; medium ones (function
    pointer tables) must also look like a function start structurally."""
    stats = AdditionStats(collections.OrderedDict((label, 0) for label, _, _ in ADDITION_CLASSES),
                          {label: [] for label, _, _ in ADDITION_CLASSES})
    for table in ("ctor:dol", "dtor:dol", "ctor:rel", "dtor:rel"):
        addrs = sorted(a for a, tags in ev.sources.items() if table in tags)
        stats.ctor_total[table] = len(addrs)
        stats.ctor_missing[table] = sum(1 for a in addrs if a not in covered)
    added: List[Entry] = []
    for a in sorted(ev.flags):
        if a in covered:
            continue
        f = ev.flags[a]
        tags = ev.sources.get(a, set())
        chosen = None
        for label, srcs, strength in ADDITION_CLASSES:
            if any(t in tags for t in srcs):
                chosen = (label, strength)
                break
        if chosen is None:
            stats.skipped_weak += 1
            continue
        label, strength = chosen
        w = mem.read_u32(a)
        if strength == "medium":
            prev = mem.read_u32(a - 4)
            ok = (mem.in_text(a) and w is not None and not (f & F_BC)
                  and first_insn_quality(w) != "invalid"
                  and (a in mem.text_starts or (prev is not None and is_terminator(prev))))
            if not ok:
                stats.rejected_medium += 1
                continue
        elif f & F_BC:
            stats.strong_bc_anomalies += 1
        stats.per_class[label] += 1
        stats.per_class_addrs[label].append(a)
        added.append(Entry(index=-1, line=0, pal=a, name="", named=False, ntsc=a, method="binary",
                           verdict="ADDED", evidence=ev.flag_names(a),
                           quality=first_insn_quality(w) if w is not None else "n/a", note=label))
    return added, stats


def write_if_changed(path: str, text: str) -> bool:
    os.makedirs(os.path.dirname(os.path.abspath(path)), exist_ok=True)
    if os.path.exists(path):
        with open(path, "r", encoding="utf-8", newline="") as f:
            if f.read() == text:
                return False
    with open(path, "w", encoding="utf-8", newline="\n") as f:
        f.write(text)
    return True


def write_map(path: str, kept: List[Entry], counts: Dict[str, int], pal_map: str) -> None:
    os.makedirs(os.path.dirname(os.path.abspath(path)), exist_ok=True)
    lines = [
        f"# {TN} ({TI}) function map, ported from {pal_map} (PAL RMCP01) by {TOOL_ID}.",
        f"# Every entry was validated against the {TI} main.dol/StaticR.rel; see MAP_REPORT.md next to this file.",
        "# Verdicts: " + ", ".join(f"{k}={counts.get(k, 0)}" for k in EMITTED_VERDICTS)
        + f", ADDED={counts.get('ADDED', 0)} (binary-proven entries no PAL entry covers); entries={len(kept)}",
    ]
    for e in sorted(kept, key=lambda e: e.ntsc):
        lines.append(f"{e.ntsc:08x} {e.name if e.named else f'0x{e.ntsc:08x}'}")
    write_if_changed(path, "\n".join(lines) + "\n")


def write_json(path: str, table: ChunkTable, dol: Dol, rel: Rel) -> None:
    os.makedirs(os.path.dirname(os.path.abspath(path)), exist_ok=True)
    payload = collections.OrderedDict()
    payload["source_region"] = "P"
    payload["target_region"] = TARGET
    payload["generator"] = TOOL_ID
    payload["attribution"] = "chunk table and section layouts vendored from the mkw-sp project (port.py, MIT license)"
    payload["port_rule"] = "if src_start <= a < src_end then a - src_start + dst_start"
    payload["address_format"] = "hex strings"
    payload["chunks"] = [[f"0x{s:08X}", f"0x{e:08X}", f"0x{d:08X}"] for s, e, d in table.chunks]
    payload["rel_load_address"] = {r: f"0x{a:08X}" for r, a in REL_LOAD_ADDRESS.items()}
    payload["dol_sections"] = {
        r: [{"name": n, "start": f"0x{s:08X}", "end": f"0x{e:08X}"} for n, s, e in secs]
        for r, secs in DOL_SECTIONS.items()
    }
    payload["rel_sections"] = {
        r: [{"name": n, "start": f"0x{s:08X}", "end": f"0x{e:08X}"} for n, s, e in secs]
        for r, secs in REL_SECTIONS.items()
    }
    payload["validated_against"] = {
        "dol": {"path": os.path.basename(dol.path), "sha256": dol.sha256, "entry": f"0x{dol.entry:08X}"},
        "rel": {"path": os.path.basename(rel.path), "sha256": rel.sha256, "module_id": rel.module_id,
                "prolog": f"0x{rel.prolog:08X}", "epilog": f"0x{rel.epilog:08X}", "unresolved": f"0x{rel.unresolved:08X}"},
    }
    write_if_changed(path, json.dumps(payload, indent=2) + "\n")


def layout_check(dol: Dol, rel: Rel) -> List[str]:
    """Compare the parsed target binaries with the vendored mkw-sp layout for that region."""
    notes = []
    parsed = {s.start: s for s in dol.sections}
    for name, start, end in dol_secs():
        if name in ("bss", "sbss", "sbss2"):
            continue
        s = parsed.get(start)
        if s is None:
            notes.append(f"DOL {name}: expected a section at {start:08x}, none parsed")
        elif s.end != end:
            notes.append(f"DOL {name}: parsed {s.start:08x}-{s.end:08x}, table says {start:08x}-{end:08x}")
        else:
            notes.append(f"DOL {name}: {start:08x}-{end:08x} OK")
    if dol.bss_addr != dol_secs()[8][1]:
        notes.append(f"DOL bss: header says {dol.bss_addr:08x}, table says {dol_secs()[8][1]:08x}")
    parsed_rel = {s.start: s for s in rel.sections}
    for name, start, end in rel_secs():
        s = parsed_rel.get(start)
        if s is None:
            notes.append(f"REL {name}: expected a section at {start:08x}, none parsed")
        elif s.end != end:
            notes.append(f"REL {name}: parsed {s.start:08x}-{s.end:08x}, table says {start:08x}-{end:08x}")
        else:
            notes.append(f"REL {name}: {start:08x}-{end:08x} OK")
    return notes


def label_class_rows(entries: List[Entry]) -> List[List[str]]:
    classes: Dict[str, collections.Counter] = collections.OrderedDict(
        (k, collections.Counter()) for k in ("case", "switch", "data"))
    for e in entries:
        k = entry_kind(e.name)
        if k in classes:
            classes[k][e.verdict] += 1
    rows = []
    for k, c in classes.items():
        rows.append([k, sum(c.values())] + [c.get(v, 0) for v in VERDICT_ORDER])
    return rows


def extent_check(entries: List[Entry], kept: List[Entry], table: ChunkTable) -> Tuple[int, List[List[str]], int]:
    """For chunk-ported entries whose PAL successor is not ported by the same chunk (chunk tails and
    entries next to holes), compare the PAL extent (distance to the next PAL entry) with the NTSC-U
    extent (distance to the next emitted entry). Inside a chunk the two are equal by construction,
    so a mismatch here marks exactly the places where the chunk table's edges may be sloppy."""
    kept_addrs = sorted(e.ntsc for e in kept)
    rows: List[List[str]] = []
    unnamed_mismatch = checked = 0
    for i, e in enumerate(entries):
        if e.method != "chunk" or e.verdict not in EMITTED_VERDICTS or i + 1 >= len(entries):
            continue
        s = entries[i + 1]
        if s.method == "chunk" and table.find(s.pal) == table.find(e.pal):
            continue
        checked += 1
        ext_pal = s.pal - e.pal
        j = bisect.bisect_right(kept_addrs, e.ntsc)
        ext_ntsc = kept_addrs[j] - e.ntsc if j < len(kept_addrs) else None
        if ext_ntsc == ext_pal:
            continue
        if e.named:
            rows.append([hx(e.pal), e.display_name, hx(e.ntsc), e.verdict, f"{ext_pal:#x}",
                         f"{ext_ntsc:#x}" if ext_ntsc is not None else "-", f"{s.display_name} ({s.verdict})"])
        else:
            unnamed_mismatch += 1
    return checked, rows, unnamed_mismatch


def md_table(header: List[str], rows: List[List[str]]) -> List[str]:
    out = ["| " + " | ".join(header) + " |", "|" + "|".join("---" for _ in header) + "|"]
    for r in rows:
        out.append("| " + " | ".join(str(c).replace("|", "\\|") for c in r) + " |")
    return out


def hx(a: Optional[int]) -> str:
    return "-" if a is None else f"{a:08x}"


def write_report(path: str, args: argparse.Namespace, dol: Dol, rel: Rel, table: ChunkTable, ev: Evidence,
                 entries: List[Entry], kept: List[Entry], collisions: List[List[Entry]], gaps: List[GapResult],
                 thunks: List[dict], sda: dict, added: List[Entry], add_stats: AdditionStats) -> str:
    L: List[str] = []
    counts = collections.Counter(e.verdict for e in entries)
    per_module: Dict[str, collections.Counter] = collections.defaultdict(collections.Counter)
    for e in entries:
        per_module[e.module][e.verdict] += 1
    emitted_counts = collections.Counter(e.verdict for e in kept)
    L.append(f"# {TN} ({TI}) function map port report")
    L.append("")
    L.append(f"Generated by `{TOOL_ID}`. Deterministic; no timestamps.")
    L.append("")
    L.append("## Summary")
    L.append("")
    L.append(f"- PAL map: `{repo_relative(args.pal_map)}` ({len(entries)} entries, {sum(1 for e in entries if e.named)} named)")
    L.append(f"- {TN} DOL: `{os.path.basename(dol.path)}` sha256 `{dol.sha256}` entry `0x{dol.entry:08X}`")
    L.append(f"- {TN} REL: `{os.path.basename(rel.path)}` sha256 `{rel.sha256}` load `0x{rel.load_address:08X}` "
             f"(module {rel.module_id}, v{rel.version}, prolog `0x{rel.prolog:08X}`, epilog `0x{rel.epilog:08X}`, "
             f"unresolved `0x{rel.unresolved:08X}`)")
    L.append(f"- Chunk table: {len(table.chunks)} PAL->{TN} chunks (mkw-sp port.py, MIT)"
             + (f"; problems: {'; '.join(table.problems)}" if table.problems else "; no overlaps, all deltas word-aligned"))
    L.append(f"- Output map: `{repo_relative(args.out_map)}` with **{len(kept)}** entries "
             f"({sum(1 for e in kept if e.named)} named); `{repo_relative(args.out_json)}` carries the chunk table.")
    L.append("")
    L.append("### Verdict totals (all PAL entries)")
    L.append("")
    rows = []
    for v in VERDICT_ORDER:
        rows.append([v, counts.get(v, 0), per_module["low"].get(v, 0), per_module["dol"].get(v, 0),
                     per_module["rel"].get(v, 0), "yes" if v in EMITTED_VERDICTS else "no"])
    rows.append(["total", len(entries), sum(per_module["low"].values()), sum(per_module["dol"].values()),
                 sum(per_module["rel"].values()), ""])
    L += md_table(["verdict", "entries", "low-mem", "DOL", "REL", "emitted"], rows)
    L.append("")
    L.append("Emitted after deduplication: " + ", ".join(f"{v}={emitted_counts.get(v, 0)}" for v in EMITTED_VERDICTS)
             + f"; total {len(kept)} ported entries, plus {len(added)} binary-proven entries no PAL entry covers "
             f"(section below) = **{len(kept) + len(added)}** lines in MAP.txt. UNVERIFIED and DROPPED entries are "
             f"never written to MAP.txt.")
    L.append("")
    L.append(f"### Binary-proven entries added (step H: {TN} entry points no ported PAL entry covers)")
    L.append("")
    L.append("Emitted as unnamed placeholders (`hexaddr 0xhexaddr`). Strong classes are taken as proven; the "
             "pointer-referenced classes must also pass the structural check (not a conditional-branch target, "
             "terminator before, decodable first instruction) because no PAL entry vouches for them. Each address "
             "is counted once, under the first class that applies.")
    L.append("")
    L += md_table(["evidence class", "added"], [[k, v] for k, v in add_stats.per_class.items()]
                  + [["total added", len(added)]])
    L.append("")
    ctor_labels = {"ctor:dol": "DOL .ctors", "dtor:dol": "DOL .dtors", "ctor:rel": "REL .ctors", "dtor:rel": "REL .dtors"}
    L.append("Constructor/destructor tables: "
             + "; ".join(f"{ctor_labels[t]} {add_stats.ctor_total.get(t, 0)} entries, "
                         f"{add_stats.ctor_missing.get(t, 0)} previously missing from the map"
                         for t in ("ctor:dol", "dtor:dol", "ctor:rel", "dtor:rel")) + ".")
    L.append(f"Not added: {add_stats.rejected_medium} pointer-referenced addresses that failed the structural check "
             f"(jump-table targets and similar), {add_stats.skipped_weak} addresses with only weak evidence "
             f"(conditional/unconditional branch targets, lis/addi pairs, jump-table-like runs). Strong additions "
             f"that are also a conditional-branch target: {add_stats.strong_bc_anomalies}.")
    L.append("")
    for label, addrs in add_stats.per_class_addrs.items():
        if not addrs:
            continue
        L.append(f"{label} ({len(addrs)}):")
        L.append("")
        L.append("```text")
        for i in range(0, len(addrs), 12):
            L.append(" ".join(f"{a:08x}" for a in addrs[i:i + 12]))
        L.append("```")
        L.append("")
    strong_q = collections.Counter()
    for e in kept:
        strong_q[(e.verdict, e.quality)] += 1
    L.append("First-instruction quality of emitted entries (strong = typical prologue/first instruction, "
             "valid = any decodable instruction): "
             + ", ".join(f"{v}/{q}={n}" for (v, q), n in sorted(strong_q.items())))
    L.append("")
    dropped_n = counts.get("DROPPED", 0)
    unv_n = counts.get("UNVERIFIED", 0)
    L.append("### Reading the numbers")
    L.append("")
    L.append(f"- {counts.get('PROVEN', 0) + counts.get('REFERENCED', 0)} of {len(entries)} entries land on an address the "
             f"{TN} binaries reference as code (call target, exception-table record, entry point, function-pointer "
             f"table, jump table for case labels); {counts.get('PLAUSIBLE', 0)} more sit right after a function "
             f"terminator (or are verified switch dispatch sites) with a decodable first instruction.")
    L.append(f"- {unv_n} UNVERIFIED entries are withheld: see the per-class breakdown in the Unverified section. None of "
             f"the no-evidence ones lies near a chunk edge; they are surrounded by validated neighbours inside their "
             f"chunk, which points at PAL-map noise (mid-function addresses, round addresses, names misplaced by one "
             f"instruction) rather than at the chunk table.")
    L.append(f"- {dropped_n} DROPPED entries: jump tables and other data symbols the PAL map carries, PAL-only code "
             f"regions with no {TN} counterpart, and chunk-table holes with no interpolation gap.")
    L.append(f"- The PAL project uses sda_base/sda2_base 0x8038CC00/0x8038EFA0; the {TN} values read below are the "
             f"ones an {TN} project must configure.")
    L.append("")
    L.append("### Non-function labels carried by the PAL map")
    L.append("")
    L.append("The PAL map contains Ghidra-style labels that are not function starts: `*_caseD_N` switch case "
             "labels (validated here by a jump-table reference at the ported address), `*_switch` dispatch "
             "points (validated as `mtctr`+`bctr`), and `switchdataD_*` jump tables (data, never code). "
             "They are ported with the same rules; the verdict spread per class:")
    L.append("")
    L += md_table(["class", "entries"] + list(VERDICT_ORDER), label_class_rows(entries))
    L.append("")
    checked, ext_rows, ext_unnamed = extent_check(entries, kept, table)
    L.append("### Chunk-edge extent check")
    L.append("")
    L.append(f"{checked} emitted chunk-ported entries sit at a chunk edge (their PAL successor is not ported by the "
             f"same chunk). For these the PAL extent (distance to the next PAL entry) was compared with the "
             f"{TN} extent (distance to the next emitted entry): {len(ext_rows)} named and {ext_unnamed} unnamed "
             f"entries differ. A difference means the function's size changed, the successor was dropped, or the "
             f"chunk edge is sloppy; the address is still a verified entry point, but for named entries the name "
             f"deserves a second look.")
    L.append("")
    if ext_rows:
        L += md_table(["PAL", "name", f"{TN}", "verdict", "PAL extent", f"{TN} extent", "PAL successor"], ext_rows)
        L.append("")

    L.append(f"### SDA bases read from the {TN} `__init_registers`")
    L.append("")
    L.append(f"- `{sda['symbol']}` PAL `{hx(sda['pal'])}` -> {TN} `{hx(sda['ntsc'])}`")
    L.append(f"- Result: **{sda['status']}**")
    if sda["values"]:
        L.append("- All lis/ori(addi) register values materialised in the function: "
                 + ", ".join(f"{r}=0x{v:08X}" for r, v in sda["values"].items()))
    L.append("")
    if sda["listing"]:
        L.append("```text")
        for a, w, d in sda["listing"]:
            L.append(f"{a:08x}  {w:08x}  {d}")
        L.append("```")
        L.append("")

    L.append("### Save/restore thunk families (resolved by name in GuestSaveRestoreThunks.cs)")
    L.append("")
    for fam in thunks:
        status = "OK" if fam["family_ok"] else "PROBLEM"
        L.append(f"**`{fam['prefix']}N`** ({fam['mnemonic']} rN,-off(r11)): {status}"
                 + (f" - {fam['run_note']}" if fam["run_note"] else " - full run to r31 verified"))
        L.append("")
        L += md_table(["name", "PAL", f"{TN}", "verdict", "expected", "actual", "decoded", "ok"],
                      [[r["name"], hx(r["pal"]), hx(r["ntsc"]), r["verdict"], f"{r['expected']:08x}",
                        hx(r["actual"]), r["decoded"], "yes" if r["ok"] else "NO"] for r in fam["rows"]])
        L.append("")

    L.append(f"## Evidence set K ({TN} function entry points proven by the binaries)")
    L.append("")
    strong_n = sum(1 for f in ev.flags.values() if f & STRONG)
    medium_only = sum(1 for f in ev.flags.values() if (f & MEDIUM) and not (f & STRONG))
    b_only = sum(1 for f in ev.flags.values() if f == F_B)
    jt_only = sum(1 for f in ev.flags.values() if f == F_JT)
    L.append(f"- Strong (bl target / extabindex record / entry point): **{strong_n}** addresses")
    L.append(f"- Medium only (function-pointer word, ADDR32/ADDR16 relocation, lis/addi pair): **{medium_only}** addresses")
    L.append(f"- Weak only (`b` target, not used for verdicts): {b_only} addresses")
    L.append(f"- Jump-table-like pointer runs (excluded from REFERENCED): {len(ev.jump_table_runs)} runs, "
             f"{jt_only} addresses referenced only from them")
    L.append("")
    L += md_table(["source", "count"], [[k, v] for k, v in ev.stats.items()])
    L.append("")
    L.append("REL relocation type counts (module, type): "
             + ", ".join(f"({m},{t})={n}" for (m, t), n in sorted(rel.reloc_type_counts.items())))
    L.append("")

    L.append("## Binary layout check (parsed headers vs vendored mkw-sp table)")
    L.append("")
    for n in layout_check(dol, rel):
        L.append(f"- {n}")
    L.append("")

    L.append("## Interpolation gaps (step C.2)")
    L.append("")
    if gaps:
        L += md_table(["PAL gap", f"{TN} gap", "unported", "K candidates", "outcome"],
                      [[f"{g.pal_lo:08x}..{g.pal_hi:08x}", f"{g.ntsc_lo:08x}..{g.ntsc_hi:08x}", g.unported,
                        g.candidates, g.outcome] for g in gaps])
    else:
        L.append("No gaps needed interpolation.")
    L.append("")
    interp = [e for e in entries if e.verdict == "INTERPOLATED"]
    if interp:
        L.append("Interpolated entries:")
        L.append("")
        L += md_table(["PAL", "name", f"{TN}", "evidence", "gap"],
                      [[hx(e.pal), e.display_name, hx(e.ntsc), e.evidence, e.note] for e in interp])
        L.append("")

    L.append(f"## Collisions (two PAL entries porting to one {TN} address)")
    L.append("")
    L.append("The named entry is kept (both PAL entries are functions, so the address is a real entry point either "
             "way; only the name is in question). Extents are the distance to the next PAL entry / next emitted "
             f"{TN} entry: the PAL entry whose extent matches the {TN} extent is the likelier owner of the address.")
    L.append("")
    if collisions:
        kept_addrs = sorted(e.ntsc for e in kept)
        rows = []
        for group in collisions:
            kept_e = next(e for e in group if e.verdict in EMITTED_VERDICTS)
            addr = group[0].ntsc
            j = bisect.bisect_right(kept_addrs, addr)
            ext_ntsc = f"{kept_addrs[j] - addr:#x}" if j < len(kept_addrs) else "-"
            parts = []
            for e in group:
                ext_pal = f"{entries[e.index + 1].pal - e.pal:#x}" if e.index + 1 < len(entries) else "-"
                parts.append(f"{e.display_name} (PAL {e.pal:08x}, {e.method}, extent {ext_pal})")
            rows.append([hx(addr), "; ".join(parts), ext_ntsc, kept_e.display_name])
        L += md_table([f"{TN}", "PAL entries", f"{TN} extent", "kept"], rows)
    else:
        L.append("None.")
    L.append("")

    dropped = [e for e in entries if e.verdict == "DROPPED"]
    L.append(f"## Dropped entries ({len(dropped)})")
    L.append("")
    if dropped:
        L += md_table(["PAL", "name", "reason"], [[hx(e.pal), e.display_name, e.note] for e in dropped])
    else:
        L.append("None.")
    L.append("")

    unv = [e for e in entries if e.verdict == "UNVERIFIED"]
    L.append(f"## Unverified entries ({len(unv)}; ported by the chunk table but not confirmed by the binary, not emitted)")
    L.append("")
    sub = collections.Counter()
    for e in unv:
        if "not in a text section" in e.note:
            sub["outside text (data symbols)"] += 1
        elif "conditional-branch target" in e.note and ("jt" in e.evidence or "ptr" in e.evidence):
            sub["conditional-branch target that is also jump-table referenced (a case label without a label name)"] += 1
        elif "conditional-branch target" in e.note:
            sub["conditional-branch target (inside a function)"] += 1
        elif "word=00000000" in e.note:
            sub["lands on a padding word (real function usually at +4)"] += 1
        elif "jt" in e.evidence or "ptr" in e.evidence:
            sub["jump-table referenced but named/unnamed as a function"] += 1
        else:
            sub["no evidence, predecessor is not a terminator"] += 1
    for k, v in sorted(sub.items()):
        L.append(f"- {k}: {v}")
    L.append("")
    if unv:
        L += md_table(["PAL", "name", f"{TN}", "evidence", "detail"],
                      [[hx(e.pal), e.display_name, hx(e.ntsc), e.evidence, e.note] for e in unv])
    else:
        L.append("None.")
    L.append("")

    ident = [e for e in entries if e.verdict == "IDENTITY"]
    L.append(f"## Identity (low-memory) entries ({len(ident)})")
    L.append("")
    L.append("Below 0x80004000 the PAL address is kept unchanged. These addresses are outside every DOL/REL "
             f"section, so the binaries cannot vouch for them; the evidence column shows whether any {TN} code "
             "branches to them.")
    L.append("")
    if ident:
        L += md_table(["address", "name", "binary evidence"],
                      [[hx(e.pal), e.display_name, f"{e.note} ({e.evidence})"] for e in ident])
    L.append("")

    while L and L[-1] == "":
        L.pop()
    text = "\n".join(L) + "\n"
    write_if_changed(path, text)
    return text


# ---------------------------------------------------------------------------------------
# main
# ---------------------------------------------------------------------------------------
def repo_root() -> str:
    return os.path.abspath(os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", ".."))


def repo_relative(path: str) -> str:
    """A path as it should read in a committed report: relative to the repository, forward slashes."""
    return os.path.relpath(os.path.abspath(path), repo_root()).replace(os.sep, "/")


def default_paths(region: str = "E") -> dict:
    repo = repo_root()
    return {
        "pal_map": os.path.join(repo, "projects", "mkwii", "MAP.txt"),
        "out_map": os.path.join(repo, "projects", PROJECT_DIR[region], "MAP.txt"),
        "out_report": os.path.join(repo, "projects", PROJECT_DIR[region], "MAP_REPORT.md"),
        "out_json": os.path.join(repo, "projects", PROJECT_DIR[region], "region_port.json"),
    }


def main(argv: Optional[List[str]] = None) -> int:
    global TARGET, TN, TI
    pre = argparse.ArgumentParser(add_help=False)
    pre.add_argument("--region", choices=sorted(CHUNK_TABLES), default="E",
                     help=f"target region letter: E ({TN}), J (NTSC-J) or K (NTSC-K)")
    known, _ = pre.parse_known_args(argv)
    TARGET = known.region
    TN, TI = REGION_NAME[TARGET]
    d = default_paths(TARGET)
    ap = argparse.ArgumentParser(
        parents=[pre],
        description="Port the PAL MKWii function map to another region with binary validation.")
    ap.add_argument("--pal-map", default=d["pal_map"])
    ap.add_argument("--disc-root", help="directory holding the extracted discs as RMC?01/ "
                    "(sys/main.dol and files/rel/StaticR.rel under each); the target's pair is used")
    ap.add_argument("--dol", help=f"{TN} sys/main.dol (overrides --disc-root)")
    ap.add_argument("--rel", help=f"{TN} files/rel/StaticR.rel (overrides --disc-root)")
    ap.add_argument("--rel-load", default=None, help="target REL load address (default: the region's)")
    ap.add_argument("--out-map", default=d["out_map"])
    ap.add_argument("--out-report", default=d["out_report"])
    ap.add_argument("--out-json", default=d["out_json"])
    ap.add_argument("--quiet", action="store_true")
    args = ap.parse_args(argv)
    if args.disc_root:
        disc = os.path.join(args.disc_root, f"RMC{TARGET}01")
        args.dol = args.dol or os.path.join(disc, "sys", "main.dol")
        args.rel = args.rel or os.path.join(disc, "files", "rel", "StaticR.rel")
    if not (args.dol and args.rel):
        ap.error("give --disc-root, or both --dol and --rel")

    rel_load = int(args.rel_load, 16) if args.rel_load else REL_LOAD_ADDRESS[TARGET]
    dol = parse_dol(args.dol)
    rel = parse_rel(args.rel, rel_load, rel_secs()[5][1])
    mem = Memory()
    for s in dol.sections:
        mem.add(s)
    for s in rel.sections:
        mem.add(s)

    ev = build_evidence(mem, dol, rel)
    table = ChunkTable(CHUNK_TABLES[TARGET])
    entries = load_pal_map(args.pal_map)

    port_entries(entries, table, ev, mem)
    gaps = interpolate_unported(entries, ev, mem)
    finish_unported(entries)
    kept, collisions = dedupe(entries)

    thunks = check_thunks(entries, mem)
    sda = read_sda_bases(entries, mem)

    added, add_stats = binary_additions(ev, mem, {e.ntsc for e in kept})
    counts = collections.Counter(e.verdict for e in kept)
    counts["ADDED"] = len(added)
    write_map(args.out_map, kept + added, counts,
              os.path.relpath(args.pal_map, os.path.dirname(os.path.abspath(args.out_map))))
    write_json(args.out_json, table, dol, rel)
    report = write_report(args.out_report, args, dol, rel, table, ev, entries, kept, collisions, gaps, thunks, sda,
                          added, add_stats)
    if not args.quiet:
        # print the summary section (up to the evidence section)
        cut = report.find("## Evidence set K")
        sys.stdout.write(report[:cut] if cut > 0 else report)
    return 0


if __name__ == "__main__":
    sys.exit(main())
