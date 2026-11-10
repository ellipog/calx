---
title: std.font
description: TrueType and OpenType fonts and collections read as FreeType reads them, glyphs drawn antialiased by the exact area they cover, and text measured and drawn on images, as Pillow's ImageFont and ImageDraw.text do.
---

# `std.font`

TrueType and OpenType fonts, read from `.ttf`, `.otf` and `.ttc` files. Their glyphs are drawn antialiased, and text is measured and drawn on [images](image.md), as Python scripts do with Pillow's `ImageFont.truetype` and `ImageDraw.text`.

- The outlines, character maps, advances and kerning are the ones FreeType and fontTools read from the same files.
- Each pixel's coverage is the exact area of the outline inside it.
- Nothing needs installing: it's all Calx.

```calx
import std.font.{Font}
import std.image.{Color, Image, Kind}

// A title card: the text centered, in blue.
fn card(font: Font, title: String) -> Image {
    var im = Image.new(480, 96, Kind.Rgb, Color.rgb(248, 248, 245))
    let size = 40.0
    let x = (480.0 - font.measure(title, size)) / 2.0
    im.draw_text(i64.round(x), 24, title, font, size, Color.rgb(24, 56, 208))
    return im
}

fn main() {
    match Font.open("fonts/Inter-Regular.ttf") {
        ok(font) => card(font, "Hello from Calx").save("card.png")!,
        err(e) => println("{e}"),    // out: reading fonts/Inter-Regular.ttf: not found
    }
}
```

Sizes are in pixels per em, as Pillow's font size is: at 40, the font's em is 40 pixels, and a line of its text is usually a little taller.

## Fonts

| Item | Gives |
| --- | --- |
| `Font.open(path: String, index: usize = 0) -> Font ! Error` | The font in a file. For a collection, `index` picks the font, from 0 |
| `Font.decode(bytes: List<u8>, index: usize = 0) -> Font ! String` | The font in a file's bytes, or why it can't be read |
| `font.family()`, `font.style()` | Its names: "Arial" and "Bold Italic", say |
| `font.glyph_count()`, `font.units_per_em()` | How many glyphs it has, and the units its outlines are measured in to an em |
| `font.glyph_index(c: rune) -> u32` | The glyph for `c`, or 0, the font's glyph for missing characters |
| `font.has_glyph(c: rune) -> bool` | Whether it has a glyph for `c` |
| `font.line_metrics(size: f64) -> LineMetrics` | The ascent, the descent (negative) and the line gap at `size`, in pixels; `.height()` is from one baseline to the next |
| `font.advance(c: rune, size: f64) -> f64` | How far the pen moves past `c`, in pixels |
| `font.kerning(left: rune, right: rune, size: f64) -> f64` | How much closer `right` sits after `left`: negative for a pair such as "AV" |
| `font.measure(text: String, size: f64) -> f64` | How wide `text` is, its advances and kerning, the widest of its lines: Pillow's `getlength` |
| `font.render(c: rune, size: f64) -> Glyph` | `c` drawn at `size` |
| `font.mask(text: String, size: f64) -> Mask` | `text` drawn as one mask, as `draw_text` draws it |
| `font.outline(c: rune) -> Outline`, `font.glyph_outline(index: u32) -> Outline` | A glyph's outline in font units, y up |

A `Glyph` is the coverage of each pixel, 0 to 255, row by row from the top, in `coverage`, `width` by `height`. `x` and `y` place its top left pixel from the pen on the baseline, with y down, so a letter that stands on the baseline has a negative `y`. `advance` is how far the pen moves on. A space is 0 by 0.

A `Mask` is the same for a whole text, with `x` and `y` from where the text is placed. Its box is the box of the text's ink, as Pillow's `textbbox` gives it.

An `Outline` is a glyph's contours as `verbs` and `points`, the points x then y:
- 0 starts a contour at a point.
- 1 is a line to a point.
- 2 is a quadratic curve through a control point to a point.
- 3 is a cubic curve through two.

Each contour ends where it starts. TrueType fonts have quadratic curves and CFF fonts cubic ones.

## Drawing text

`im.draw_text(x, y, text, font, size, color)` on an [`Image`](image.md) draws as `ImageDraw.text` does with its default anchor, `la`:

- (x, y) is the left of the first line, at its ascender, rounded up to a whole pixel as FreeType rounds it.
- Lines split at `\n`, the font's line height apart.
- Each glyph is drawn at its pen's fraction of a pixel, and where glyphs overlap the larger coverage counts.
- The coverage, times the color's alpha, blends the color over the image. On an image with alpha, the text adds alpha, and its edges keep the color's own hue, so text on a clear image has no dark fringe.
- Text past the image's edges is cut off.

## What it reads

- **Files.** TrueType outlines (`glyf`), simple and composite with their scales and offsets. CFF outlines (`.otf`), as Type 2 charstrings with their subroutines, including CID-keyed fonts. Collections (`.ttc`, `.otc`).
- **Characters.** Unicode character maps of formats 4 and 12, and 0 and 6. Symbol fonts, whose characters sit at U+F000, take ASCII as their own.
- **Kerning.** From the `kern` table, as Pillow's basic layout takes it. Fonts that only kern in `GPOS` aren't kerned, in Pillow's basic layout either.
- **Metrics.** From `hhea`, or `OS/2` where `hhea` has none, as FreeType takes them.

Some things aren't done:
- Shaping, the step that joins Arabic letters, places Indic vowel signs and makes ligatures. Text goes a character at a time, left to right, as Pillow's basic layout does without libraqm.
- Color glyphs, such as emoji's `COLR` and bitmap layers: their plain outline is drawn, if they have one.
- Variable fonts' axes: the default instance is drawn.

## How it differs from Pillow

- **No hinting.** FreeType runs a TrueType font's hinting programs, which pull stems onto whole pixels at small sizes. Here each glyph is the outline as drawn, as browsers on macOS draw text, and as the Rust crates fontdue and ab_glyph do. Text is smoother and a little softer at 10 to 14 pixels.
- **Advances aren't rounded.** Pillow takes FreeType's hinted advances, rounded to whole pixels, so its lines of small text run a little wider. Here the pen moves by the font's own advance, a fraction of a pixel at a time.
- **Line spacing** is the font's line height. Pillow's `multiline_text` spaces lines by the height of "A" and 4 pixels.

## How it's built

[std/font.calx](../../std/font.calx) reads the tables where they are in the file's bytes. Opening a font reads only the table directory and a few headers, so it takes microseconds whatever the font's size. A glyph's outline is read when it's drawn.

- A TrueType glyph's points are decoded with their flags, and two points off the curve in a row have one on it halfway between them, as FreeType has it.
- A simple glyph whose box disagrees with its left side bearing is moved to agree, as FreeType and fontTools move it. A composite stays where its components put it.
- A CFF glyph's program runs on a small stack machine with its subroutines, and its hints are skipped.

Drawing scales the outline to pixels and cuts its curves into lines no more than 1/128 of a pixel from them. Then each line adds the signed area it leaves to its right, in each pixel of each row it crosses, as a difference from the pixel before. A running sum along each row is then the area covered. Its size, at most 1, is the coverage: contours wound the same way don't count twice where they overlap, and a hole, wound the other way, takes away.

It was checked against fontTools on 22 fonts, Windows' and some CFF ones, from Arial to Segoe UI Emoji's 64,883 glyphs and the CJK collections. Every glyph's outline, every character in the Unicode character map, every advance and every kerning pair of ASCII were the same, but for one rotated emoji component, a hundredth of a unit apart. The coverage was checked against a reference that finds, in 64 rows a pixel, exactly where the winding number isn't 0: the largest difference was 2.2 in 255, and the mean 0.23.

## Speed

Arial, against Pillow 12.3 with FreeType 2.14.3, on the performance cores of an i9-13900K under Windows, best of 30 runs, 5 times in turns:

| Work | Calx | Pillow |
| --- | --- | --- |
| Opening the font | 0.02 ms | 0.06 ms |
| Drawing the 94 printable ASCII glyphs at 12 pixels | 0.18 ms | 4.33 ms |
| The same at 24 pixels | 0.28 ms | 1.14 ms |
| The same at 72 pixels | 0.65 ms | 1.62 ms |
| A line of 180 characters at 16 pixels, drawn on an image | 0.43 ms | 10.96 ms |

Pillow's times include FreeType's hinting, which Calx doesn't do.
