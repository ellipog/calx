"""Writes std/json/schema/idna.calx and its tables, what std.json.schema.formats' `idn-hostname`
check needs to know of each rune, as Python's `idna.encode` sees it.

Run it with a Python whose `unicodedata` is the one to match, with idna installed, from the
repository's root:

    python -I tools/unicode/gen_idna.py

The tables are Python 3.14's `unicodedata` (Unicode 16.0.0) and idna 3.20's `idnadata` (its
IDNA 2008 derived properties, joining types and scripts, for Unicode 18.0.0). A rune idnadata
takes that Unicode 16 hasn't assigned has no bidirectional class, which `idna.check_bidi`
refuses, so it's written as one idna refuses.

Each range of runes has one record, a u16:

    bits 0-1    0 refused (DISALLOWED, UNASSIGNED or not in Unicode 16), 1 PVALID, 2 CONTEXTJ,
                3 CONTEXTO
    bits 2-5    bidirectional class: L R AL AN EN ES CS ET ON BN NSM, 0 to 10
    bits 6-8    joining type: none, C, D, L, R, T, 0 to 5
    bits 9-11   script: none, Greek, Han, Hebrew, Hiragana, Katakana, 0 to 5
    bit 12      general category M
    bit 13      canonical combining class 9, a virama
    bit 14      no name and class 0, for which `idna.core._combining_class` raises

Only a rune idna takes has the other bits, since a label holding any other is refused anyway.
"""
import pathlib
import struct
import sys
import unicodedata

import idna
import idna.idnadata as data
from idna.intranges import intranges_contain

UNICODE = "16.0.0"
IDNA = "3.20"
BIDI = ["L", "R", "AL", "AN", "EN", "ES", "CS", "ET", "ON", "BN", "NSM"]
JOINING = ["C", "D", "L", "R", "T"]
SCRIPTS = ["Greek", "Han", "Hebrew", "Hiragana", "Katakana"]

NOTICE = """\
// The tables below are from Python's `unicodedata`, which is the Unicode Character Database,
// and from idna's `idnadata`, under these notices:
//
//     UNICODE LICENSE V3
//
//     COPYRIGHT AND PERMISSION NOTICE
//
//     Copyright (c) 1991-2024 Unicode, Inc.
//
//     NOTICE TO USER: Carefully read the following legal agreement. BY DOWNLOADING, INSTALLING,
//     COPYING OR OTHERWISE USING DATA FILES, AND/OR SOFTWARE, YOU UNEQUIVOCALLY ACCEPT, AND AGREE
//     TO BE BOUND BY, ALL OF THE TERMS AND CONDITIONS OF THIS AGREEMENT. IF YOU DO NOT AGREE, DO
//     NOT DOWNLOAD, INSTALL, COPY, DISTRIBUTE OR USE THE DATA FILES OR SOFTWARE.
//
//     Permission is hereby granted, free of charge, to any person obtaining a copy of data files
//     and any associated documentation (the "Data Files") or software and any associated
//     documentation (the "Software") to deal in the Data Files or Software without restriction,
//     including without limitation the rights to use, copy, modify, merge, publish, distribute,
//     and/or sell copies of the Data Files or Software, and to permit persons to whom the Data
//     Files or Software are furnished to do so, provided that either (a) this copyright and
//     permission notice appear with all copies of the Data Files or Software, or (b) this
//     copyright and permission notice appear in associated Documentation.
//
//     THE DATA FILES AND SOFTWARE ARE PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
//     IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY, FITNESS FOR A
//     PARTICULAR PURPOSE AND NONINFRINGEMENT OF THIRD PARTY RIGHTS.
//
//     IN NO EVENT SHALL THE COPYRIGHT HOLDER OR HOLDERS INCLUDED IN THIS NOTICE BE LIABLE FOR ANY
//     CLAIM, OR ANY SPECIAL INDIRECT OR CONSEQUENTIAL DAMAGES, OR ANY DAMAGES WHATSOEVER
//     RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN ACTION OF CONTRACT, NEGLIGENCE
//     OR OTHER TORTIOUS ACTION, ARISING OUT OF OR IN CONNECTION WITH THE USE OR PERFORMANCE OF
//     THE DATA FILES OR SOFTWARE.
//
//     Except as contained in this notice, the name of a copyright holder shall not be used in
//     advertising or otherwise to promote the sale, use or other dealings in these Data Files or
//     Software without prior written authorization of the copyright holder.
//
//     BSD 3-Clause License
//
//     Copyright (c) 2013-2026, Kim Davies and contributors.
//     All rights reserved.
//
//     Redistribution and use in source and binary forms, with or without modification, are
//     permitted provided that the following conditions are met:
//
//     1. Redistributions of source code must retain the above copyright notice, this list of
//        conditions and the following disclaimer.
//
//     2. Redistributions in binary form must reproduce the above copyright notice, this list of
//        conditions and the following disclaimer in the documentation and/or other materials
//        provided with the distribution.
//
//     3. Neither the name of the copyright holder nor the names of its contributors may be used
//        to endorse or promote products derived from this software without specific prior
//        written permission.
//
//     THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND ANY
//     EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES OF
//     MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE
//     COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL,
//     EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
//     SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
//     HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR
//     TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
//     SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
"""


def in_class(cp: int, name: str) -> bool:
    return intranges_contain(cp, data.codepoint_classes[name])


def record(cp: int) -> int:
    kind = 1 if in_class(cp, "PVALID") else 2 if in_class(cp, "CONTEXTJ") else 3 if in_class(cp, "CONTEXTO") else 0
    if kind == 0:
        return 0
    ch = chr(cp)
    bidi = unicodedata.bidirectional(ch)
    if bidi == "":
        return 0
    joining = next((i + 1 for i, k in enumerate(JOINING) if intranges_contain(cp, data.joining_types[k])), 0)
    script = next((i + 1 for i, s in enumerate(SCRIPTS) if intranges_contain(cp, data.scripts[s])), 0)
    ccc = unicodedata.combining(ch)
    mark = unicodedata.category(ch)[0] == "M"
    nameless = ccc == 0 and unicodedata.name(ch, None) is None
    return kind | BIDI.index(bidi) << 2 | joining << 6 | script << 9 | mark << 12 | (ccc == 9) << 13 | nameless << 14


def main() -> None:
    if unicodedata.unidata_version != UNICODE or idna.__version__ != IDNA:
        sys.exit(f"want Unicode {UNICODE} and idna {IDNA}, have {unicodedata.unidata_version} and {idna.__version__}")
    if set(data.joining_types) != set(JOINING):
        sys.exit(f"joining types changed: {sorted(data.joining_types)}")
    starts: list[int] = []
    values: list[int] = []
    for cp in range(0x110000):
        v = record(cp)
        if not values or v != values[-1]:
            starts.append(cp)
            values.append(v)
    root = pathlib.Path("std/json/schema")
    (root / "idna").mkdir(exist_ok=True)
    (root / "idna/starts.bin").write_bytes(struct.pack(f"<{len(starts)}I", *starts))
    (root / "idna/records.bin").write_bytes(struct.pack(f"<{len(values)}H", *values))
    n = len(starts)
    module = f"""\
/// What std.json.schema.formats' `idn-hostname` check knows of each rune, as Python's
/// `idna.encode` sees it. Written by tools/unicode/gen_idna.py from Python 3.14's `unicodedata`
/// (Unicode {UNICODE}) and idna {IDNA}'s `idnadata`; don't edit by hand.
module std.json.schema.idna

{NOTICE}
/// The first rune of each range whose runes share a record, in order, from 0.
pub const STARTS: [u32; {n}] = embed("idna/starts.bin")

/// Each range's record. Bits 0-1: 0 refused, 1 PVALID, 2 CONTEXTJ, 3 CONTEXTO. Bits 2-5: the
/// bidirectional class, L R AL AN EN ES CS ET ON BN NSM from 0. Bits 6-8: the joining type, none
/// C D L R T from 0. Bits 9-11: the script, none Greek Han Hebrew Hiragana Katakana from 0. Bit
/// 12: a mark. Bit 13: combining class 9. Bit 14: no name, with class 0. A refused rune has 0.
pub const RECORDS: [u16; {n}] = embed("idna/records.bin")
"""
    (root / "idna.calx").write_text(module, encoding="utf-8", newline="\n")
    print(f"{n} ranges")


main()
