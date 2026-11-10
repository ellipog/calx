---
title: std.text.python
description: Python's repr of strings and floats, str.isprintable and str.isspace, as CPython 3.14 gives them, for messages that quote values as Python does - repr_str, repr_float, is_printable and is_space.
---

# `std.text.python`

Python's `repr` of a string and of a float, `str.isprintable` and `str.isspace`, as CPython 3.14 gives them, for text that has to quote values the way Python does: error messages ported from Python, such as `Regex.python`'s, and output another program compares with Python's.

```calx
import std.text.python.{repr_str, repr_float, is_printable, is_space}

fn main() {
    println(repr_str("it's"))    // out: "it's"
    println(repr_str("tab\tand \"quote\" and 'apostrophe'"))    // out: 'tab\tand "quote" and \'apostrophe\''
    println(repr_str("\u{E9}\u{A0}\u{1F600}\u{200B}"))    // out: 'é\xa0😀\u200b'
    println("{repr_float(0.1)} {repr_float(1e16)} {repr_float(1.5e-5)} {repr_float(100.0)} {repr_float(-f64.INFINITY)}")    // out: 0.1 1e+16 1.5e-05 100.0 -inf
    println("{is_printable(0x41)} {is_printable(0xA0)} {is_printable(0x1F600)}")    // out: true false true
    println("{is_space(0x20)} {is_space(0x1F)} {is_space(0xA0)} {is_space(0x200B)}")    // out: true true true false
}
```

| Function | Gives |
| --- | --- |
| `repr_str(s: String) -> String` | `repr(s)`: in single quotes, or double ones when `s` has a `'` and no `"`; with `\\`, `\n`, `\r`, `\t` and the quote escaped, and each rune Python doesn't print as `\xhh`, `\uhhhh` or `\Uhhhhhhhh` |
| `repr_str_into(out: mut String, s: String)` | The same, added to `out` |
| `repr_float(f: f64) -> String` | `repr(f)`: the shortest digits that read back as `f`, with `.0` on a whole number, in fixed notation from 1e-4 up to 1e16 and with an exponent of two digits or more outside that, `1e+16` and `1.5e-05`; `inf`, `-inf` and `nan` |
| `repr_float_into(out: mut String, f: f64)` | The same, added to `out` |
| `is_printable(r: u32) -> bool` | `str.isprintable` of the one rune `r`: false for Unicode's control, format, surrogate, private-use, unassigned and separator runes, but true for the space |
| `is_space(r: u32) -> bool` | `str.isspace` of the one rune `r`, what `strip()`, `split()` and `int()` take as white space: the 29 runes from the tab and the space to U+3000, checked against CPython on every code point |

Which runes are printable comes from a table in [std/text/ucd.calx](../../std/text/ucd.calx), which [tools/unicode/gen_ucd.calx](../../tools/unicode/gen_ucd.calx) writes from the Unicode Character Database, version 16.0.0, as CPython 3.14's is. Both functions were checked against CPython's `repr` on twenty thousand floats, most of them of random bits, and eight thousand strings of the runes `repr` treats specially.

The module is [std/text/python.calx](../../std/text/python.calx).
