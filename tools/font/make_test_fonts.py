"""Makes the two small fonts tests/std/font.calx reads, with fontTools' FontBuilder, and prints
each as hex for the test, 96 digits a line.

    py -3 -P -E tools/font/make_test_fonts.py [OUT_DIR]

With OUT_DIR it also writes test.ttf, test.otf and test.ttc there, for checking against fontTools.

The TrueType font, em 1000, ascender 800, descender -200, line gap 100:
- A: a 500 by 500 square from (100, 0) with a 300 by 300 hole, advance 700.
- O: a circle of radius 300 around (350, 350) in quadratic curves, with points off the curve
  in a row, advance 700.
- B: A at half size, moved by (100, 100), as a composite, advance 400.
- V: a triangle, advance 600.
- space, advance 250, and U+1F600 mapped to O, so the character map has a format 12 table.
- kern: A then V -80, V then A -60.

The CFF font has the same glyphs, except B, as Type 2 charstrings written out by hand to use
the operators a renderer meets: widths before the first operator, stems and hint masks, the
line operators that alternate direction, hvcurveto and vhcurveto, flex, and local and global
subroutines. It kerns in GPOS only: A then V by -80, the class pair of A or V then A, V or O
by -40, which A then V overrides, and in an extension lookup that adds to those, A then V by
-5 more and O then A by -25.
"""
import os
import sys

from fontTools.fontBuilder import FontBuilder
from fontTools.misc.psCharStrings import T2CharString
from fontTools.pens.ttGlyphPen import TTGlyphPen
from fontTools.ttLib import newTable
from fontTools.ttLib.tables._g_l_y_f import Glyph, GlyphComponent
from fontTools.ttLib.tables._k_e_r_n import KernTable_format_0

ORDER = [".notdef", "space", "A", "O", "B", "V"]
CMAP = {0x20: "space", 0x41: "A", 0x4F: "O", 0x42: "B", 0x56: "V", 0x1F600: "O"}
ADV = {".notdef": 500, "space": 250, "A": 700, "O": 700, "B": 400, "V": 600}
NAME = {"familyName": "Calx Test", "styleName": "Regular"}
GPOS_KERN = """
languagesystem DFLT dflt;
languagesystem latn dflt;
@LEFT = [A V];
@RIGHT = [A V O];
lookup EXTRA useExtension {
    pos A V -5;
    pos O A -25;
} EXTRA;
feature kern {
    pos A V -80;
    pos @LEFT @RIGHT -40;
    lookup EXTRA;
} kern;
"""


def ttf():
    fb = FontBuilder(1000, isTTF=True)
    fb.setupGlyphOrder(ORDER)
    fb.setupCharacterMap(CMAP)
    glyphs = {}
    pen = TTGlyphPen(None)
    glyphs[".notdef"] = pen.glyph()
    glyphs["space"] = TTGlyphPen(None).glyph()
    pen = TTGlyphPen(None)
    pen.moveTo((100, 0))
    pen.lineTo((100, 500))
    pen.lineTo((600, 500))
    pen.lineTo((600, 0))
    pen.closePath()
    pen.moveTo((200, 100))
    pen.lineTo((500, 100))
    pen.lineTo((500, 400))
    pen.lineTo((200, 400))
    pen.closePath()
    glyphs["A"] = pen.glyph()
    # A circle as four points off the curve in a row: their midpoints are on it.
    pen = TTGlyphPen(None)
    pen.qCurveTo((50, 350), (50, 650), (350, 650), (650, 650), (650, 350), (650, 50), (350, 50), (50, 50), None)
    pen.closePath()
    glyphs["O"] = pen.glyph()
    pen = TTGlyphPen(None)
    pen.moveTo((0, 700))
    pen.lineTo((600, 700))
    pen.lineTo((300, 0))
    pen.closePath()
    glyphs["V"] = pen.glyph()
    b = Glyph()
    b.numberOfContours = -1
    c = GlyphComponent()
    c.glyphName = "A"
    c.x, c.y = 100, 100
    c.flags = 0x1 | 0x2 | 0x8  # words, offsets, one scale
    c.transform = [[0.5, 0], [0, 0.5]]
    b.components = [c]
    glyphs["B"] = b
    fb.setupGlyf(glyphs)
    # Each side bearing is where its glyph's box starts, as fonts have them.
    glyf = fb.font["glyf"]
    for g in ORDER:
        glyf[g].recalcBounds(glyf)
    fb.setupHorizontalMetrics({g: (ADV[g], getattr(glyf[g], "xMin", 0)) for g in ORDER})
    fb.setupHorizontalHeader(ascent=800, descent=-200, lineGap=100)
    fb.setupNameTable(NAME)
    fb.setupOS2(sTypoAscender=800, sTypoDescender=-200, usWinAscent=800, usWinDescent=200)
    fb.setupPost()
    kern = newTable("kern")
    kern.version = 0
    t = KernTable_format_0()
    t.version = 0
    t.coverage = 1
    t.kernTable = {("A", "V"): -80, ("V", "A"): -60}
    kern.kernTables = [t]
    fb.font["kern"] = kern
    return fb.font


def otf():
    fb = FontBuilder(1000, isTTF=False)
    order = [g for g in ORDER if g != "B"]
    fb.setupGlyphOrder(order)
    fb.setupCharacterMap({k: v for k, v in CMAP.items() if v != "B"})
    progs = {
        # A width (700) first, then a moveto; stems and a hint mask; lines that alternate.
        ".notdef": [500, "endchar"],
        "space": ["endchar"],
        # The square through a local subroutine (500 hlineto 500 vlineto return), its hole
        # through vlineto and hlineto.
        "A": [700, 0, 100, "hstemhm", 100, 100, 400, 100, "vstemhm", "hintmask", bytes([0b11110000]),
              100, 0, "rmoveto", -107, "callsubr", -500, "hlineto",
              100, -400, "rmoveto", 300, 300, -300, "vlineto", -300, "hlineto",
              "endchar"],
        # The circle from its bottom: two curves of one hvcurveto, across then up, and the other
        # two from a global subroutine.
        "O": [350, 50, "rmoveto", -166, -134, 134, 166, 166, 134, 134, 166, "hvcurveto",
              -107, "callgsubr", "endchar"],
        # A triangle drawn as flex, which a renderer without hints draws as its two curves.
        "V": [0, 700, "rmoveto", 50, 0, 100, 0, 150, 0, 50, 0, 100, 0, 150, 0, 50, "flex",
              -300, -700, "rlineto", "endchar"],
    }
    charstrings = {g: T2CharString(program=progs[g]) for g in order}
    fb.setupCFF("CalxTest-Regular", {"FullName": "Calx Test"}, charstrings, {})
    cff = fb.font["CFF "].cff
    top = cff.topDictIndex[0]
    from fontTools.cffLib import GlobalSubrsIndex, SubrsIndex

    subrs = SubrsIndex()
    subrs.append(T2CharString(program=[500, "hlineto", 500, "vlineto", "return"]))
    top.Private.Subrs = subrs
    gsubrs = GlobalSubrsIndex()
    gsubrs.append(T2CharString(program=[166, 134, -134, -166, -166, -134, -134, -166, "hvcurveto", "return"]))
    cff.GlobalSubrs = gsubrs
    for cs in top.CharStrings.values():
        cs.private = top.Private
        cs.globalSubrs = gsubrs
    bounds = {g: top.CharStrings[g].calcBounds(top.CharStrings) for g in order}
    fb.setupHorizontalMetrics({g: (ADV[g], round(bounds[g][0]) if bounds[g] else 0) for g in order})
    fb.setupHorizontalHeader(ascent=800, descent=-200, lineGap=100)
    fb.setupNameTable(NAME)
    fb.setupOS2(sTypoAscender=800, sTypoDescender=-200, usWinAscent=800, usWinDescent=200)
    fb.setupPost()
    # Kerning in GPOS only, as newer fonts have it: a pair, a class pair the pair overrides, and
    # an extension lookup that adds to both.
    fb.addOpenTypeFeatures(GPOS_KERN)
    return fb.font


def bytes_of(font, path):
    font.save(path)
    with open(path, "rb") as f:
        return f.read()


def ttc():
    """A collection of the TrueType font and a copy of it named "Calx Test Two"."""
    from fontTools.ttLib import TTCollection

    a = ttf()
    b = ttf()
    b["name"].setName("Calx Test Two", 1, 3, 1, 0x409)
    c = TTCollection()
    c.fonts = [a, b]
    return c


def main():
    out = sys.argv[1] if len(sys.argv) > 1 else None
    tmp = out or "."
    for name, font in (("test.ttf", ttf()), ("test.otf", otf()), ("test.ttc", ttc())):
        path = os.path.join(tmp, name)
        data = bytes_of(font, path)
        if not out:
            os.remove(path)
        h = data.hex()
        print(f"// {name}, {len(data)} bytes")
        print("\n".join(f'"{h[i:i + 96]}",' for i in range(0, len(h), 96)))


main()
