---
title: std.xml
description: XML read into a tree as Python's ElementTree reads it, with the line and column of what isn't well-formed, elements found by ElementTree's paths, and trees written back plain or indented - parse, Element, find_all, find_each, format, pretty.
---

# `std.xml`

XML text read into a tree of elements, the way Python's `xml.etree.ElementTree` reads it, and written back. An element has its tag, its attributes in the order they're written, the elements in it, its text and its tail. Text that isn't well-formed XML is an `XmlError` with the line and column where it stops being XML, and a document ElementTree reads gives the same tree here.

```calx
import std.xml.{Element, XmlError, format, parse}

fn main() ! XmlError {
    let doc = try parse(#"
        <inventory owner="Steve">
          <item id="1" count="64">stone</item>
          <item id="2" count="12">dirt</item>
          <chest><item id="3" count="1">diamond</item></chest>
        </inventory>
        "#)
    println("{doc.get("owner")} {doc.children.len()}")    // out: some(Steve) 3
    for item in doc.find_all(".//item") {
        println("{item.text} x{item.get("count") ?? String("0")}")
    }
    // out: stone x64
    // out: dirt x12
    // out: diamond x1
    var chest = Element.new(String("chest"))
    chest.set(String("open"), String("yes"))
    chest.text = String("empty & dusty")
    println(format(chest))    // out: <chest open="yes">empty &amp; dusty</chest>
}
```

## The tree

`<item id="7">a<b/>c</item>d` is an `item` with the attribute `id`, the text `a`, one child `b` whose tail is `c`, and the tail `d`. Text is where ElementTree has it: an element's `text` is what comes before its first child, and the text after a child's end tag is that child's `tail`.

| Field | Holds |
| --- | --- |
| `tag: String` | The name, as written |
| `attributes: LinkedMap<String, String>` | The attributes, in the order they're written |
| `children: List<Element>` | The elements in it |
| `text: String` | The text before the first child, or all of it when there are none. Empty where ElementTree has `None` |
| `tail: String` | The text after the end tag, up to the next element or the parent's end tag |

| Method | Gives |
| --- | --- |
| `Element.new(tag)` | An element with the tag and nothing else |
| `e.get(name) -> String?` | A copy of the attribute's value, if it has one |
| `e.set(name, value)` | Sets the attribute. One it had keeps its place |
| `e.add(child)` | Adds a child after the others |
| `e.find(path) -> Element?` | A copy of the first element the [path](#paths) finds |
| `e.find_all(path) -> List<Element>` | Copies of the elements the path finds, in document order |
| `e.find_each(path, f)` | Calls `f` with each element the path finds, in document order, where it is in the tree |
| `e.find_text(path) -> String?` | The text of the first element the path finds, which may be empty, or `none` if it finds nothing |
| `e.all_text() -> String` | All the text in it, without the tags: its text, then each child's text and tail in order. Its own tail isn't in it |

`==` compares whole trees: tags, attributes in order, text, tails and children. `println("{e}")` writes `e` as `format` does.

`find` and `find_all` give copies, since a function gives nothing back that points into its arguments. Changing a copy doesn't change the tree. `find_each` copies nothing: it gives each element to a closure where it is, and the closure, passed to an `FnMut` bound, can add to the variables around it ([closures](../reference/closures.md#closures-that-change-what-they-capture)). On a large tree it takes about half the time `find_all` does ([speed](#speed)). To change elements in place, go through `children` by index:

```calx
import std.xml.{XmlError, parse}

fn main() ! XmlError {
    var doc = try parse("<list><n>1</n><n>2</n><n>3</n></list>")
    var sum = 0
    doc.find_each("n", |n| {
        sum += i32.parse(n.text) ?? 0
    })
    for i in 0..doc.children.len() {
        doc.children[i].set(String("seen"), String("yes"))
    }
    println("{sum} {doc.find_all("n[@seen='yes']").len()}")    // out: 6 3
}
```

## Reading

`parse(text) -> Element ! XmlError` reads a whole document and gives its root element, as ElementTree's `fromstring` does.

- Before the root element can come an XML declaration, a DOCTYPE, comments and processing instructions, and after it, comments and processing instructions, with white space anywhere between them. Nothing else can be outside the root element.
- `&lt;`, `&gt;`, `&amp;`, `&quot;`, `&apos;`, `&#233;` and `&#xE9;` are read, and so are the entities the DOCTYPE declares as text, `<!ENTITY who "Ada">`, in text and in attribute values. Any other entity is an error. `&nbsp;` is one, since XML doesn't have HTML's names: write `&#160;`.
- CDATA sections are text, and their `&` and `<` are themselves.
- Comments and processing instructions are left out, as ElementTree's parser leaves them out.
- Line ends are `\n`, however the file has them. In an attribute's value, each tab and line end is a space, as XML says, but a character reference to one, `&#10;`, stays what it is.
- A start tag must be closed by its own end tag, an attribute written once, and the text can hold only characters XML allows.
- The text is UTF-8, whatever the XML declaration says. The declaration's form is still checked.

```calx
import std.xml.{format, parse}

fn main() {
    let doc = #"<!DOCTYPE r [ <!ENTITY who "Ada"> ]><r who="&who;"><![CDATA[1 < 2]]> &amp; &#233;</r>"#
    match parse(String(doc)) {
        ok(e) => println("{e.get("who")} {e.text}"),    // out: some(Ada) 1 < 2 & é
        err(e) => println("{e}"),
    }
    match parse("<a>\n  <b>\n</a>") {
        ok(e) => println(format(e)),
        err(e) => println("{e}"),    // out: line 3, column 1: </a> doesn't close <b>, which starts at line 2, column 3
    }
}
```

`XmlError` has the `message`, and the `line` and `column` where the text stops being XML, both from 1, with the column counted in runes. Printed, it's `line 3, column 1: ...`. ElementTree's `ParseError` counts columns from 0, and its messages are expat's.

It differs from ElementTree in these places:

- **Names are kept as written.** ElementTree turns `<svg:rect xmlns:svg="http://www.w3.org/2000/svg">` into `{http://www.w3.org/2000/svg}rect` and drops the `xmlns` attribute. Here the tag is `svg:rect` and `xmlns:svg` is an attribute like any other, so the tree writes back as it was read, and paths name elements as the file does.
- **An entity can't hold markup.** With `<!ENTITY b "<b/>">`, `&b;` is an error, where expat reads the element in it. Write the elements where they're used.
- **Entities can't grow without limit.** References can nest 16 deep, and all the text they give can come to 100 times the document's length, or 16 MB if that's more. Past that it's an error, so a few lines can't stand for gigabytes, as the "billion laughs" do. Python's expat has a limit of its own since 2.4.
- **Other files are never read.** An entity declared as another file, `<!ENTITY e SYSTEM "e.xml">`, is an error where it's used, and an external DTD is checked for its form and skipped, as ElementTree skips it.

## Paths

`find`, `find_all`, `find_each` and `find_text` take ElementTree's paths, read from the element they're called on. A path is steps between `/`, and each step can have conditions in `[...]`:

| Path | Finds |
| --- | --- |
| `item` | The children with the tag `item` |
| `*` | All the children |
| `.` | The element itself |
| `chest/item` | The `item` children of its `chest` children |
| `.//item` | Elements with the tag at any depth below it. `//` works between steps too: `chest//item` |
| `item[@id]` | Those with the attribute `id` |
| `item[@id='7']`, `item[@id!='7']` | Those whose `id` is `7`, or has an `id` that isn't |
| `chest[item]` | Those with an `item` child |
| `book[title='Emma']`, `book[title!='Emma']` | Those with a `title` child whose text, all of it, is `Emma`, or isn't |
| `title[.='Emma']`, `title[.!='Emma']` | Those whose text, all of it, is `Emma`, or isn't |
| `item[2]` | The second among the children with its tag, from 1 |
| `item[last()]`, `item[last()-1]` | The last among the children with its tag, or the one before it |

Conditions can follow each other, as in `item[@id][2]`, and values can be in single or double quotes. A path ElementTree wouldn't read is a mistake in the program rather than in the XML, so it panics, where ElementTree raises `SyntaxError`, and the panic says where the call is. A path that starts with `/` panics. So does `..`, since an element doesn't know its parent.

## Writing

| Function | Gives |
| --- | --- |
| `format(e) -> String` | `e` as XML, as ElementTree's `tostring` writes it, with its tail after it |
| `pretty(e, indent) -> String` | `e` with each element on a line of its own, indented `indent` spaces for each level, as ElementTree's `indent` then `tostring` write it. `e` doesn't change |
| `escape(text) -> String` | `text` for an element's text: `&`, `<` and `>` as `&amp;`, `&lt;` and `&gt;` |
| `escape_attribute(text) -> String` | `text` for an attribute's value in double quotes: as `escape`, with `"` as `&quot;`, and tabs and line ends as `&#09;`, `&#10;` and `&#13;`, so reading it back keeps them |

Attributes are written in their order, and an element with no text and no children is written `<tag />`. `pretty` replaces text and tails that are only white space with the line ends and indents, and keeps others, such as mixed content's, as they are.

```calx
import std.xml.{Element, pretty}

fn main() {
    var list = Element.new(String("list"))
    var item = Element.new(String("item"))
    item.set(String("name"), String("\"a\" & b"))
    list.add(item)
    list.add(Element.new(String("empty")))
    println(pretty(list, 2))
    // out: <list>
    // out:   <item name="&quot;a&quot; &amp; b" />
    // out:   <empty />
    // out: </list>
}
```

## Python's ElementTree

| ElementTree | `std.xml` |
| --- | --- |
| `ET.fromstring(t)` | `parse(t)` |
| `ET.tostring(e, encoding="unicode")` | `format(e)` |
| `ET.indent(e, "  ")`, then `tostring` | `pretty(e, 2)` |
| `e.tag`, `e.attrib`, `e.text`, `e.tail` | `e.tag`, `e.attributes`, `e.text`, `e.tail` |
| `e.get(k)`, `e.set(k, v)`, `e.append(c)` | `e.get(k)`, `e.set(k, v)`, `e.add(c)` |
| `list(e)`, `len(e)` | `e.children`, `e.children.len()` |
| `e.find(p)`, `e.findall(p)`, `e.findtext(p)` | `e.find(p)`, `e.find_all(p)`, `e.find_text(p)` |
| `for x in e.iterfind(p): ...` | `e.find_each(p, \|x\| ...)` |
| `"".join(e.itertext())` | `e.all_text()` |
| `ET.ParseError` | `XmlError` |

## How it's built

[std/xml.calx](../../std/xml.calx) is written in Calx from the XML 1.0 standard and from how ElementTree behaves. It's checked against Python 3.14's ElementTree, which reads with expat, on 24,000 random documents. They had entities, CDATA, comments, processing instructions, DOCTYPEs, mixed content, odd line ends, and mistakes of every kind. Each document was checked for whether it's read or refused, what `format` and `pretty` write, and what 24 paths find with `find`, `find_all` and `find_text`. Every one matched.

The parser goes through the text once, without recursion, keeping where each open element starts. So a deeply nested document can't overflow the stack, and a mistake can say where the element it concerns began. Text without references is copied in one piece. Tags and attribute names are [interned](../reference/strings.md#interned-strings), as ElementTree's parser keeps one copy of each name. So the elements with a tag share its bytes, a copy of an element copies no names, and a search compares bytes already in the cache. Entities are kept as declared, read where they're used, and counted against the limits above. Writing escapes in runs, copying the text between the characters it changes.

## Speed

On our benchmark, an i9-13900K pinned to its performance cores with other work running, so rough: the best of 9 runs, over rounds taken in turn. The file is 30.3 MB of XML: 120,000 `book` elements with 3 attributes and 4 children each, with references and non-ASCII text. The comparisons are Python 3.14's ElementTree, whose parser is expat in C, and Rust 1.91 with roxmltree 0.20 and quick-xml 0.37, built for release with `x86-64-v3`:

| Work | Calx | Python | Rust |
| --- | --- | --- | --- |
| `parse`, the text to a tree | 102 ms | 455 ms | 194 ms with roxmltree, 192 ms with quick-xml |
| `format`, the tree back to text | 35.1 ms | 679 ms | 38.0 ms |
| `find_all(".//book[@lang='no']")`, 40,140 found | 13.4 ms | 20.4 ms | 40.7 ms, copies |
| `find_each(".//book[@lang='no']", ...)` | 7.6 ms | 21.3 ms, `iterfind` | 8.3 ms with roxmltree, 5.6 ms by hand |

- Python's times are `ET.fromstring`, `ET.tostring(root, encoding="unicode")`, `root.findall` and a loop over `root.iterfind`.
- roxmltree builds a read-only tree whose text points into the document where it can. quick-xml reads events, which the benchmark builds into a tree of owned strings like `std.xml`'s. Rust's `format` is a writer like `std.xml`'s over that tree.
- `find_all` here copies each element it finds, with its children, while Python's `findall` gives references to the elements in the tree. Rust's `find_all` row is copies of what a loop over quick-xml's tree finds.
- `find_each` copies nothing, as `iterfind` and Rust's references don't. roxmltree's is `descendants()` filtered by tag and attribute. By hand is a loop over quick-xml's tree with the tag and the attribute written into it, so it reads no path.
