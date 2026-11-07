"""Writes std/text/ucd.calx, the Unicode tables std.regex reads, from Python's unicodedata.

Usage, from the repository's root:

    py -I tools/unicode/gen_ucd.py

The tables are the Unicode Character Database's, as the Python running this has it (3.14 has
Unicode 16, which Java 25 has too):

- WORD, `\\w` as UTS #18 defines it: Alphabetic, marks, decimal digits, connector punctuation and
  the two joiners. Alphabetic is the letters, letter numbers and Other_Alphabetic; unicodedata has
  no Other_Alphabetic, but every one of those is a mark, which WORD has anyway, except the
  circled and squared Latin letters listed in OTHER_ALPHABETIC below.
- DIGIT, `\\d`: the decimal digits, Nd.
- SPACE, `\\s`: White_Space, a short list that hasn't changed since Unicode 6.
- FOLD, simple case folding, as orbits: each rune that folds with others, and the next one in
  its orbit, the smallest past it or else the smallest of all, as Go's unicode.SimpleFold gives.
  A rune folds with what str.casefold gives it when that's one rune, CaseFolding.txt's C entries,
  and otherwise with what str.lower gives it when that's one rune, its S entries, so the dotless
  i, whose only folding is Turkic, folds with nothing.
"""

import sys
import unicodedata
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
OUT = ROOT / "std" / "text" / "ucd.calx"
MAX = 0x10FFFF

# Other_Alphabetic that isn't a mark (PropList.txt, Unicode 16).
OTHER_ALPHABETIC = [(0x24B6, 0x24E9), (0x1F130, 0x1F149), (0x1F150, 0x1F169), (0x1F170, 0x1F189)]
JOIN_CONTROL = [(0x200C, 0x200D)]
WHITE_SPACE = [(0x09, 0x0D), (0x20, 0x20), (0x85, 0x85), (0xA0, 0xA0), (0x1680, 0x1680), (0x2000, 0x200A),
               (0x2028, 0x2029), (0x202F, 0x202F), (0x205F, 0x205F), (0x3000, 0x3000)]

NOTICE = """// The tables below are from the Unicode Character Database, under this notice:
//
// UNICODE LICENSE V3
//
// COPYRIGHT AND PERMISSION NOTICE
//
// Copyright (c) 1991-2024 Unicode, Inc.
//
// NOTICE TO USER: Carefully read the following legal agreement. BY DOWNLOADING, INSTALLING,
// COPYING OR OTHERWISE USING DATA FILES, AND/OR SOFTWARE, YOU UNEQUIVOCALLY ACCEPT, AND AGREE TO BE
// BOUND BY, ALL OF THE TERMS AND CONDITIONS OF THIS AGREEMENT. IF YOU DO NOT AGREE, DO NOT
// DOWNLOAD, INSTALL, COPY, DISTRIBUTE OR USE THE DATA FILES OR SOFTWARE.
//
// Permission is hereby granted, free of charge, to any person obtaining a copy of data files and
// any associated documentation (the "Data Files") or software and any associated documentation
// (the "Software") to deal in the Data Files or Software without restriction, including without
// limitation the rights to use, copy, modify, merge, publish, distribute, and/or sell copies of
// the Data Files or Software, and to permit persons to whom the Data Files or Software are
// furnished to do so, provided that either (a) this copyright and permission notice appear with
// all copies of the Data Files or Software, or (b) this copyright and permission notice appear in
// associated Documentation.
//
// THE DATA FILES AND SOFTWARE ARE PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
// IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY, FITNESS FOR A
// PARTICULAR PURPOSE AND NONINFRINGEMENT OF THIRD PARTY RIGHTS.
//
// IN NO EVENT SHALL THE COPYRIGHT HOLDER OR HOLDERS INCLUDED IN THIS NOTICE BE LIABLE FOR ANY
// CLAIM, OR ANY SPECIAL INDIRECT OR CONSEQUENTIAL DAMAGES, OR ANY DAMAGES WHATSOEVER RESULTING FROM
// LOSS OF USE, DATA OR PROFITS, WHETHER IN AN ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS
// ACTION, ARISING OUT OF OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THE DATA FILES OR
// SOFTWARE.
//
// Except as contained in this notice, the name of a copyright holder shall not be used in
// advertising or otherwise to promote the sale, use or other dealings in these Data Files or
// Software without prior written authorization of the copyright holder.
"""


def runes():
    for c in range(MAX + 1):
        if 0xD800 <= c <= 0xDFFF:
            continue
        yield c


def ranges(test):
    out = []
    for c in runes():
        if not test(c):
            continue
        if out and out[-1][1] == c - 1:
            out[-1][1] = c
        elif out and out[-1][1] == 0xD7FF and c == 0xE000:
            out[-1][1] = c  # across the surrogates, which no text has
        else:
            out.append([c, c])
    return out


def merge(*lists):
    spans = sorted(tuple(r) for rs in lists for r in rs)
    out = []
    for lo, hi in spans:
        if out and lo <= out[-1][1] + 1:
            out[-1][1] = max(out[-1][1], hi)
        else:
            out.append([lo, hi])
    return out


def is_word(c):
    cat = unicodedata.category(chr(c))
    return cat[0] in "LM" or cat in ("Nd", "Nl", "Pc")


def folds():
    parent = {}

    def find(x):
        while parent.get(x, x) != x:
            x = parent[x]
        return x

    def union(a, b):
        ra, rb = find(a), find(b)
        if ra != rb:
            parent[max(ra, rb)] = min(ra, rb)

    for c in runes():
        s = chr(c)
        f = s.casefold()
        if len(f) == 1:
            if ord(f) != c:
                union(c, ord(f))
        else:
            low = s.lower()
            if len(low) == 1 and ord(low) != c:
                union(c, ord(low))
    orbits = {}
    for c in list(parent):
        orbits.setdefault(find(c), set()).add(c)
    for root in list(orbits):
        orbits[root].add(root)
    pairs = []
    for members in orbits.values():
        ms = sorted(members)
        for i, c in enumerate(ms):
            pairs.append((c, ms[(i + 1) % len(ms)]))
    pairs.sort()
    return pairs


def table(name, doc, values, per_line=8):
    lines = [f"/// {doc}", f"pub const {name}: [u32; {len(values)}] = ["]
    for i in range(0, len(values), per_line):
        lines.append("    " + ", ".join(f"0x{v:X}" for v in values[i:i + per_line]) + ",")
    lines.append("]")
    lines.append(f"/// How many numbers `{name}` holds.")
    lines.append(f"pub const {name}_LEN: usize = {len(values)}")
    return "\n".join(lines) + "\n"


def flat(rs):
    return [v for r in rs for v in r]


def main():
    word = merge(ranges(is_word), OTHER_ALPHABETIC, JOIN_CONTROL)
    digit = ranges(lambda c: unicodedata.category(chr(c)) == "Nd")
    space = merge(WHITE_SPACE)
    pairs = folds()
    text = [
        "/// Tables from the Unicode Character Database, for `std.regex`: the runes `\\w`, `\\d` and `\\s`",
        "/// match, and simple case folding. Written by tools/unicode/gen_ucd.py from Unicode "
        + unicodedata.unidata_version + "; don't edit by hand.",
        "module std.text.ucd",
        "",
        NOTICE,
        table("WORD", "`\\w`'s runes, as ranges: each first rune, then the last.", flat(word)),
        table("DIGIT", "`\\d`'s runes, the decimal digits, as ranges.", flat(digit)),
        table("SPACE", "`\\s`'s runes, White_Space, as ranges.", flat(space)),
        table("FOLD_FROM", "Each rune that case folding puts with others, in order.", [p[0] for p in pairs]),
        table("FOLD_NEXT", "The next rune in `FOLD_FROM[i]`'s orbit: the smallest past it, or else the smallest.",
              [p[1] for p in pairs]),
    ]
    OUT.write_text("\n".join(text), encoding="utf-8", newline="\n")
    print(f"{OUT}: {len(word)} word ranges, {len(digit)} digit, {len(space)} space, {len(pairs)} folds")


if __name__ == "__main__":
    sys.exit(main())
