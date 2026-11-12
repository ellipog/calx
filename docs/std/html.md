---
title: std.html
description: HTML escaped, its references read back, and its tags, attributes and text read, as Python's html module does - escape, unescape, tokens, start_tags.
---

# `std.html`

Text for HTML: escaped, so it shows as it is in a page, and with its character references read back. Both do exactly what Python's `html.escape` and `html.unescape` do, and `unescape` knows every one of the 2,231 names HTML5 has. And HTML read into its tags, their attributes, its text and its comments, exactly as Python's `html.parser` reads it: see [Reading HTML](#reading-html).

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

## Reading HTML

`tokens` reads HTML into what's in it, in order: each start tag with its attributes, each end tag, the text between, comments, the doctype, processing instructions and CDATA sections. It reads exactly as Python 3.14's `html.parser.HTMLParser` does, given the whole text and then closed, so a script ported from a Python `HTMLParser` subclass finds the same things. `start_tags` gives only the start tags, for the common case of reading a page's links, images or icons:

```calx
import std.html.{start_tags}

fn main() {
    let page = "<head><LINK rel=icon href='/favicon.svg' type=image/svg+xml>\n<link rel=\"apple-touch-icon\" sizes=180x180 href=/apple.png></head>"
    for tag in start_tags(page) {
        if tag.name == "link" and (tag.get("rel") ?? String()).contains("icon") {
            println("{tag.get("href") ?? String()} {tag.get("sizes") ?? String("any")}")
        }
    }
    // out: /favicon.svg any
    // out: /apple.png 180x180
}
```

```calx
import std.html.{tokens}

fn main() {
    for t in tokens("<!DOCTYPE html><p class=note hidden>Caf&eacute; <b>&amp;</b><!-- x --><script>a<b</script>") {
        match t {
            Start(tag) => println("start {tag.name} {tag.attrs.len()} {tag.get("class")} {tag.get("hidden")}"),
            End(name) => println("end {name}"),
            Text(s) => println("text [{s}]"),
            Comment(s) => println("comment [{s}]"),
            Declaration(s) => println("declaration [{s}]"),
            Instruction(s) => println("instruction [{s}]"),
            Cdata(s) => println("cdata [{s}]"),
        }
    }
    // out: declaration [DOCTYPE html]
    // out: start p 2 some(note) some()
    // out: text [Café ]
    // out: start b 0 none none
    // out: text [&]
    // out: end b
    // out: comment [ x ]
    // out: start script 0 none none
    // out: text [a<b]
    // out: end script
}
```

| Item | Gives |
| --- | --- |
| `tokens(html: String) -> List<Token>` | What's in the HTML, in order, as Python's `HTMLParser` reads it. It never fails: HTML has no mistakes, only text read as best it can be |
| `start_tags(html: String) -> List<Tag>` | The start tags `tokens` finds, without the rest |
| `Token` | `Start(Tag)`; `End(String)`, an end tag's name; `Text(String)`, with its references read back; `Comment(String)`; `Declaration(String)`, a doctype, such as `DOCTYPE html`; `Instruction(String)`, what's between `<?` and `>`, such as `xml version="1.0"?`; and `Cdata(String)`, what's between `<![CDATA[` and `]]>` |
| `Tag` | `name: String`, lowercased; `attrs: List<Attr>`, in the order they're written, two of the same name too; and `self_closing: bool`, whether it ends in `/>`, which Python hands to `handle_startendtag` |
| `tag.get(name: String) -> String?` | The value of the first attribute called `name`, which is lowercase, as a browser's `getAttribute` gives it: `""` for one without a value, `none` if there's none. Python's `dict(attrs)` keeps the last of two instead |
| `Attr` | `name: String`, lowercased, and `value: String?`, without its quotes and with its references read back, or `none` for one written without `=`, as `hidden` is above |

The rules are Python's, which follow HTML5's tokenizer in most things:

- **Tags.** `<` starts a tag only before an ASCII letter, so `a < b` and `<3` are text. The name runs to white space, `/` or `>`, and is lowercased as Python's `str.lower` lowercases it, which is [`std.text.python.lower`](text-python.md). A start tag the text ends inside, such as `<a href="x`, is left out, as a browser leaves it.
- **Attributes.** A value is in double quotes, in single quotes, which can hold `>` and line breaks, or without quotes, up to white space or `>`, so `<a href=/x/>` has the value `/x/`. Its references are read as HTML5 reads them in a value: `&#38;` and `&#x26` always, and a name only whole, as HTML5 lists it, with or without `;`, and not before `=`. So `&amp;`, `&amp` and `&copy` are read, but `&ampx`, `&amp=1` and `&notin` stay as they are, where `unescape` reads text's `&notin` as `¬in`.
- **Text.** Its references are read back as [`unescape`](#reading-references) reads them, and text next to text is one `Text`. Text that's nothing once read, such as `&#1;`, is left out.
- **Raw text.** After `<script>`, `<style>`, `<xmp>`, `<iframe>`, `<noembed>` and `<noframes>`, everything up to the element's end tag, in any case, is its text as it's written: `<` and `&` mean nothing there. `<textarea>` and `<title>` are read the same way, but with their references read back. After `<plaintext>` the rest of the text is its text. A tag that closes itself, such as `<script/>`, starts none of these, as in Python.
- **Comments.** `<!-- ... -->` ends at the first `-->` or `--!>`. `<!-->` and `<!--->` are empty comments. Anything else after `<!` but a doctype or CDATA, and `</` before something other than a letter, as in `</1>`, are bogus comments, to the next `>`. `</>` is nothing.
- **The end.** A comment, doctype, CDATA section or processing instruction the text ends inside runs to the end, a comment without the `--` or `-` it ends in.

## How it's built

[std/html.calx](../../std/html.calx) is written in Calx from how Python's `html` module behaves. It's checked against Python 3.14 on 160,000 random strings full of names, cut-off names, numbers of every kind and stray `&`s, run through `unescape` and both kinds of `escape`. Every result matched.

`escape` looks for the first character it changes and copies the text up to it in one piece, then the runs between the others, each found 16 bytes at a time with SSE2, which every x86-64 has, or NEON, which every Arm64 has. That made it 5 times as fast as testing a byte at a time, and it matched Python on 5,000 more random strings once it did. `unescape` copies the runs between `&`s whole. Names are looked up in a hash table, which [tools/html/gen_entities.calx](../../tools/html/gen_entities.calx) builds from the HTML Standard's `entities.json` along with the table of names. An old name without its `;` is at most 6 ASCII letters, so after a name that isn't one, only its first 2 to 6 letters are tried, where Python tries every length.

`tokens` is Python 3.14's `html.parser`, from CPython's 3.14 branch as it was in July 2026 (commit 07efb08), with its regular expressions written out as loops. Where Python's engine backtracks, as it does after a quote that's never closed and takes back a space so that `b= "c` is an empty value and an attribute `"c`, the loops do the same. [tools/html/check.calx](../../tools/html/check.calx) checks it against Python on random markup: tags with attributes in every kind of quote and none, references in values, the elements whose text is raw, comments, doctypes, CDATA, white space Python's `strip` takes and its regular expressions don't, names that aren't ASCII, and all of it cut off anywhere. On 160,000 texts every token matched. Python's parser before 3.14 reads 1 text in 5 of those differently.

The names and the text they stand for are from the [HTML Standard](https://html.spec.whatwg.org/), © WHATWG (Apple, Google, Mozilla, Microsoft), under [CC BY 4.0](https://creativecommons.org/licenses/by/4.0/).

## Speed

On our benchmark, an i9-13900K with a little other work running, each program on its performance cores, the best of 30 runs taken in turns. The text is 5.0 MB of words, one piece in four a character reference: names, old names without `;`, decimal and hex numbers, and `&nbsp;`. The comparisons are Python 3.14's `html` module, and Rust 1.91 with htmlize 1.1, built for release with `x86-64-v3`:

| Work | Calx | Python | Rust `htmlize` |
| --- | --- | --- | --- |
| `unescape`, the 5.0 MB | 8.3 ms | 85 ms | 12.5 ms |
| `escape`, what that gives back | 1.4 ms | 24.6 ms | 9.1 ms |

- Rust's times are `htmlize::unescape` and `htmlize::escape_all_quotes`, each made into an owned `String`.
