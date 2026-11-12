---
title: std.csv
description: CSV read as Python's csv.reader and DictReader read it, quirks included, and written as csv.writer writes it - parse, parse_records, Reader, format, format_row.
---

# `std.csv`

CSV text, the way Python's `csv` module reads and writes it with its default dialect, which is Excel's and RFC 4180's. A field in quotes holds delimiters, line breaks and doubled quotes, and rows are written with `\r\n` after them. Reading never fails: text that isn't quite CSV is read the way Python reads it, so a file gives the same rows here as there.

```calx
import std.csv.{parse, parse_records, format}

fn main() {
    let text = "name,hp,note\nzombie,20,\"slow, loud\"\nwitch,26,\"says \"\"hi\"\"\"\n"
    for row in parse(text) {
        println("{row.len()} {row[2]}")
    }
    // out: 3 note
    // out: 3 slow, loud
    // out: 3 says "hi"
    for mob in parse_records(text) {
        println("{mob["name"]} has {mob["hp"]}")
    }
    // out: zombie has 20
    // out: witch has 26
    let rows: List<List<String>> = [["id", "name"], ["1", "Ann, Bo"]]
    print(format(rows, line_end: "\n"))
    // out: id,name
    // out: 1,"Ann, Bo"
}
```

## Reading

| Function | Gives |
| --- | --- |
| `parse(text, delimiter = ',') -> List<List<String>>` | The rows, each a list of its fields. Python's `list(csv.reader(f))`, or `csv.reader(f, delimiter=...)` |
| `parse_records(text, delimiter = ',') -> List<LinkedMap<String, String>>` | The rows after the first, each a map from the first row's names to its fields, in the header's order. Python's `list(csv.DictReader(f))` |
| `Reader.new(text, delimiter = ',')` | A reader that goes a row at a time, below |
| `reader.read(mut row) -> bool` | Reads the next row into `row`, replacing what it held, and gives `true`. At the end it empties `row` and gives `false` |

A row ends at `\n`, at `\r\n` or at a lone `\r`, wherever the file came from. So an empty line is a row with no fields, as in Python, and `a,` is two fields, the second one empty. Text that isn't quite CSV is read as Python reads it:

| Text | Fields | |
| --- | --- | --- |
| `a"b,c` | `a"b`, `c` | A quote in a field that didn't start with one is just text |
| `"ab"c,d` | `abc`, `d` | Text after the closing quote is added to the field |
| `"a" ,b` | `a `, `b` | Including spaces |
| ` "a",b` | ` "a"`, `b` | A space before the quote makes the quote text |
| `"ab` | `ab` | A quote that's never closed takes the rest of the text |

```calx
import std.csv.{parse}

fn main() {
    println("{parse("a,b\r\n\nc,\n")}")       // out: [[a, b], [], [c, ]]
    let odd = #""ab"c,a"b"#
    println("{parse(odd)}")                   // out: [[abc, a"b]]
    println("{parse("x;\"y;z\"", ';')}")      // out: [[x, y;z]]
}
```

- The delimiter is an ASCII character other than a quote or a line break. Any other delimiter panics. Python takes any character other than those.
- A field has no size limit. Python's is 131,072 characters unless `csv.field_size_limit` raises it.
- `parse_records` skips empty rows, as `DictReader` does. In a row shorter than the header, the names past its last field have no entry, where Python gives them `None`. Fields past the header's last name are left out, where Python puts them in a list under `None`. If the header has a name twice, the name keeps its first place and the later field, as in a Python `dict`.

### A row at a time

`Reader` reads one row at a time into the same list, clearing and refilling its strings rather than making new ones. Once the first rows are read, it allocates almost nothing, which makes it the way to go through a large file. It's Python's `for row in csv.reader(f)`, and its rows are `parse`'s.

```calx
import std.csv.{Reader}

fn main() {
    var reader = Reader.new("item,count\nstone,64\ndirt,12\n")
    var row = List<String>()
    _ = reader.read(mut row)    // the header
    var total = 0
    while reader.read(mut row) {
        total += i32.parse(row[1]) ?? 0
    }
    println("{total}")    // out: 76
}
```

`Reader.new` takes the text and owns it, so pass the `String` that `read_text` gave rather than a copy.

## Writing

| Function | Gives |
| --- | --- |
| `format(rows, delimiter = ',', line_end = "\r\n") -> String` | The rows as CSV text, with `line_end` after each row. Python's `csv.writer(f).writerows(rows)` |
| `format_row(fields, delimiter = ',') -> String` | One row, without a line end after it, for writing rows one at a time |

A field is quoted only when it has to be, with any quote inside it doubled. That's when it holds the delimiter, a quote, a line break or a character of `line_end`, or when it's the only field in its row and empty, so it reads back as one empty field rather than as an empty row. This is Python's `QUOTE_MINIMAL`. `\r\n` is Python's line end and RFC 4180's. Give `"\n"` for Unix's.

```calx
import std.csv.{format, format_row}

fn main() {
    println(format_row(["plain", "a,b", #"say "hi""#, ""]))    // out: plain,"a,b","say ""hi""",
    let rows: List<List<String>> = [[""], ["x", "y"]]
    print(format(rows, '|', "\n"))
    // out: ""
    // out: x|y
}
```

A line end that holds the delimiter or a quote panics, as Python refuses it, since what was written couldn't be read back.

## How it's built

[std/csv.calx](../../std/csv.calx) is written in Calx from how Python's reader behaves, and checked against Python 3.14's `csv` module on 11,500 random cases. The cases were awkward text read with six delimiters, random rows written with eight line ends and read back, and header-keyed records with repeated and missing names. Every case matched, and 4,000 more did once fields were scanned 16 bytes at a time, with runs of up to 100 bytes and texts that end inside a field or a quote.

Each row is read in one pass over its bytes. A field without quotes is cut straight from the text, its end, the delimiter or a line break, found 16 bytes at a time with SSE2, which every x86-64 has, or NEON, which every Arm64 has: most fields are shorter, so one step finds it. A field in quotes is built from the runs between its doubled quotes, each quote found the same way. Against a byte at a time, that took 16 to 29% off the reading times below. `parse` gives each row room for as many fields as the row before had. `parse_records` [interns](../reference/strings.md#interned-strings) the header's names, so the records share them and copying them into each costs nothing: 10% less time on a 15 MB file of 150,000 rows. Writing checks each field against a table of the bytes that need quoting.

## Speed

On our benchmark, an i9-13900K otherwise quiet, each program on its performance cores, the best of 30 runs taken in turns. The file is 13.5 MB of CSV like a real export: 150,000 rows of 8 fields, with commas, quotes, line breaks and non-ASCII text in some fields. The comparisons are Python 3.14's `csv` module, which is written in C, and Rust 1.91 with the `csv` crate 1.4, built for release with `x86-64-v3`:

| Work | Calx | Python | Rust `csv` |
| --- | --- | --- | --- |
| `parse`, every row as a list of `String`s | 13.4 ms | 150 ms | 75.8 ms |
| `parse_records`, every row keyed by the header | 29.2 ms | 276 ms | 107 ms |
| `format`, the rows back to the same text | 10.9 ms | 139 ms | 19.4 ms |
| `Reader`, a row at a time into one list | 11.0 ms | | 13.4 ms |

- Python's times are `list(csv.reader(...))`, `list(csv.DictReader(...))` and `writer.writerows`.
- Rust's are `records()` collected into `Vec<Vec<String>>`, and the same zipped with the header into pairs, which is less work than a map. Its last row is `read_byte_record` into one reused `ByteRecord`. Rust used its default allocator, which on Windows is the system's, and that's much of its cost when every field is a new `String`.
- Calx's `Reader` time includes copying the 13.5 MB text for it, about half a millisecond, since the benchmark reads the same text each run. Rust's reads the text where it is.
