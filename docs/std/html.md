---
title: std.html
description: HTML text escaped so it shows as it is, and its character references read back, exactly as Python's html.escape and html.unescape do, with every name HTML5 has - escape, unescape.
---

# `std.html`

Text for HTML: escaped, so it shows as it is in a page, and with its character references read back. Both do exactly what Python's `html.escape` and `html.unescape` do, and `unescape` knows every one of the 2,231 names HTML5 has.

```calx
import std.html.{escape, unescape}

fn main() {
    println(escape(String("<b>Tom & \"Jerry\"</b>")))    // out: &lt;b&gt;Tom &amp; &quot;Jerry&quot;&lt;/b&gt;
    println(unescape(String("caf&eacute; &copy;2024 &#8364;5 &rarr; &bogus;")))    // out: café ©2024 €5 → &bogus;
}
```

| Function | Gives |
| --- | --- |
| `escape(text, quote = true) -> String` | `text` with `&`, `<` and `>` as `&amp;`, `&lt;` and `&gt;`, and, with `quote`, `"` and `'` as `&quot;` and `&#x27;`, so it can go in an attribute's value too. Python's `html.escape` |
| `unescape(text) -> String` | `text` with each character reference read back, as Python's `html.unescape` reads them. Text with no `&` comes back as it is |

## Reading references

`unescape` reads references as a browser reads them in a page's text, which is what Python's does:

- A name with its `;`, such as `&amp;` or `&NotNestedGreaterGreater;`, is the text it stands for. HTML5's list is in [std/text/entities.calx](../../std/text/entities.calx).
- The 106 old names, such as `&amp`, `&lt` and `&copy`, are read without their `;` too, even when more letters follow, so `&copy2024` is `©2024` and `&notit;` is `¬it;`.
- `&#233;` and `&#xE9;` are the character, and the `;` can be left out.
- A number that isn't a character becomes what HTML5 puts for it. `&#128;` to `&#159;` are Windows-1252's characters, so `&#150;` is an en dash. `&#0;`, a surrogate and anything past U+10FFFF are U+FFFD. A control character or a noncharacter, such as `&#1;` or `&#xFFFF;`, is dropped. `&#13;` stays a carriage return.
- Anything else after `&`, such as `&bogus;`, `&#x;` or a lone `&`, is left as it is.

```calx
import std.html.{unescape}

fn main() {
    println(unescape(String("&lt&gt &AMP &notin; &notit; &#65&#x42; &#150; [&#1;] & a&b")))    // out: <> & ∉ ¬it; AB – [] & a&b
}
```

## How it's built

[std/html.calx](../../std/html.calx) is written in Calx from how Python's `html` module behaves. It's checked against Python 3.14 on 160,000 random strings full of names, cut-off names, numbers of every kind and stray `&`s, run through `unescape` and both kinds of `escape`. Every result matched.

`escape` looks for the first character it changes and copies the text up to it in one piece, then the runs between the others. `unescape` copies the runs between `&`s whole. Names are looked up in a hash table, which [tools/html/gen_entities.calx](../../tools/html/gen_entities.calx) builds from the HTML Standard's `entities.json` along with the table of names. An old name without its `;` is at most 6 ASCII letters, so after a name that isn't one, only its first 2 to 6 letters are tried, where Python tries every length.

The names and the text they stand for are from the [HTML Standard](https://html.spec.whatwg.org/), © WHATWG (Apple, Google, Mozilla, Microsoft), under [CC BY 4.0](https://creativecommons.org/licenses/by/4.0/).

## Speed

On our benchmark, an i9-13900K with other work running, so rough, the best of five runs. The text is 5.0 MB of words with names, old names without `;`, numbers and `&nbsp;` among them. The comparisons are Python 3.14's `html` module, and Rust 1.91 with htmlize 1.1, built for release with `x86-64-v3`:

| Work | Calx | Python | Rust `htmlize` |
| --- | --- | --- | --- |
| `unescape`, the 5.0 MB | 12.6 ms | 155 ms | 17.2 ms |
| `escape`, what that gives back | 5.1 ms | 8.9 ms | 7.0 ms |

- Rust's times are `htmlize::unescape` and `htmlize::escape_all_quotes`, each made into an owned `String`.
