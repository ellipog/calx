---
title: Calx documentation
description: Index of the Calx docs, the page order, and the rules for writing them.
---

# Calx documentation

These pages are the source for the docs on the Calx website, [calxlang.com](https://calxlang.com/docs/), which is rebuilt from them on every push to main. They read the same here.

## Pages

In reading order:

### Guide

Installing Calx, then learning it by building one small program, `stash`, which keeps count of items from commands it reads:

1. [Install Calx](guide/install.md): one command on Windows or Linux, clang, checking that it runs a program, staying up to date, completions, uninstalling, and building from source
2. [Getting started](guide/getting-started.md): the program, the `calx` command, values and variables, functions, loops, and the checker's errors
3. [Reading the commands](guide/reading-input.md): standard input a line at a time, optionals and `while let`, and splitting a line into words
4. [What the commands mean](guide/commands.md): commands as an enum, parsing numbers, functions that fail, matching on text, and copying out of a list
5. [The stash](guide/stash.md): a sorted map, parameters that own or change what they're given, `try`, the whole program, and modules
6. [Coming from Python](guide/from-python.md): the Python a script reaches for next to the Calx that does the same, from text, lists and files to JSON, CSV, the web and dates, and where Calx differs on purpose

### Reference

What's built, one page per feature:

1. [The calx command](reference/command.md): new, check, build, run, test, eval, fmt, emit-c, how builds stay fast, cleaning, uninstalling, completions, and errors for editors
2. [Editor support](reference/editors.md): the VS Code extension, and the grammar, error format and formatter other editors can use
3. [Syntax](reference/syntax.md): files, comments, names, keywords, literals, raw strings and operators
4. [Types](reference/types.md): the primitive types, arrays, `Copy`, and where the other types are
5. [Variables](reference/variables.md): let, var, constants, assignment, unused names, no shadowing
6. [Integers](reference/integers.md): wrapping, division, shifts and constant expressions, as in Java
7. [Floats](reference/floats.md): IEEE 754 arithmetic and printing, as in Java
8. [Conversions](reference/conversions.md): exact `T(x)`, the named lossy conversions, and `try_from`
9. [Control flow](reference/control-flow.md): if, while, while let, for, `for x in mut xs`, `enumerate`, `zip`, `rev`, `step_by`, iterators, loop, break and continue
10. [Functions](reference/functions.md): parameter modes, named arguments, returns, evaluation order
11. [Closures](reference/closures.md): `|x| expr`, `Fn` bounds, what a closure captures, trailing closures, `map`, `filter`, `sort_by` and the other methods that take one, and their C
12. [Structs](reference/structs.md): fields, defaults, literals and methods
13. [Enums](reference/enums.md): variants with and without data, representations, conversions
14. [Tuples](reference/tuples.md): values grouped by position, and destructuring
15. [Optionals](reference/optionals.md): `T?` instead of null, `??`, `!`, `?.`
16. [Errors](reference/errors.md): functions that can fail, `fail`, `try`, matching `ok` and `err`, `!`, a failing `main`, and their C
17. [Patterns and match](reference/patterns.md): match, exhaustiveness, `if let`, `let ... else`
18. [Strings](reference/strings.md): `str` and `String`, free literals, building, methods, slicing, Java's case mapping and matching
19. [Lists](reference/lists.md): `List<T>`, its methods, indexing, loops and printing
20. [Deques](reference/deques.md): `Deque<T>`, added to and taken from at both ends, its methods, indexing, Java's and Rust's names for them, and its C
21. [Priority queues](reference/priority-queues.md): `PriorityQueue<T>`, least first, its elements in Java's places, its methods, taking out marked elements, Java's names for them, and its C
22. [Boxes](reference/boxes.md): `Box<T>`, one value on the heap so a type can hold itself, its value as the place `b.value`, and its C
23. [Shared values](reference/shared.md): `Shared<T>`, one value that many owners and threads read without copying it, its read-only place `s.value`, and its C
24. [Maps and sets](reference/maps.md): `Map<K, V>` and `Set<T>`, their methods, indexing, loops, deterministic order, `Hash`, and their C
25. [Linked maps and sets](reference/linked-maps.md): `LinkedMap<K, V>` and `LinkedSet<T>`, in the order keys went in, their ends, Java's and fastutil's methods written in Calx, and their C
26. [Sorted maps and sets](reference/sorted-maps.md): `SortedMap<K, V>` and `SortedSet<T>`, in key order, their ends, the nearest keys, ranges, Java's and fastutil's methods written in Calx, and their C
27. [Ownership](reference/ownership.md): `Copy`, moves, views, partial moves, when values are freed, and their C
28. [Arenas](reference/arenas.md): `Arena` and `ArenaList<T>`, lists freed all at once, the rules that keep them safe without lifetimes, parallel loops, and their C
29. [Generics](reference/generics.md): generic functions and types, inference, bounds, `Copy`, `Equal`, `Display`, `Hash` and `JavaHash`, and their C
30. [Interfaces](reference/interfaces.md): methods, constants and defaults, impls, naming the interface, generic interfaces, and operators on a type of one's own
31. [Comparing and printing](reference/comparing.md): the derived `Equal` and `Display`, `==` and `{x}` on every type, and their bounds
32. [Modules](reference/modules.md): files as modules, imports, `pub`, and one C file per module
33. [Printing and panics](reference/printing.md): print, println, eprint, eprintln, formats such as `{x:.2}` and `{n:>8}`, assert and panic, and `@track_caller`
34. [Unsafe code and C functions](reference/unsafe.md): `unsafe` blocks and functions, raw pointers, and calling C
35. [Libraries](reference/libraries.md): `calx build --lib`, `@export`, opening a library while a program runs, types checked down to the fields, and the one runtime they share
36. [Attributes](reference/attributes.md): attributes a library defines as Calx functions the compiler runs, their arguments, attributes on fields, the code they generate, the cache, and `calx expand`
37. [Parallel loops](reference/parallel.md): `parallel for` over a range or a list's elements, the capture check, and the same result with any number of threads
38. [Strictness](reference/strictness.md): the rules that make the checker stricter than Rust's

### Standard library

The `std` modules, one page each ([modules](reference/modules.md#the-standard-library)):

1. [std.io](std/io.md): standard input and output as bytes, and standard input a line at a time
2. [std.math](std/math.md): fast math functions, accurate to about half an ULP and the same on every platform
3. [std.java.math](std/java-math.md): math functions with `StrictMath`'s results bit for bit
4. [std.time](std/time.md): a monotonic clock and the wall clock, which only `unsafe` code can read, the local time zone, and dates: their arithmetic, strftime and strptime codes and ISO 8601
5. [std.env](std/env.md): the command-line arguments, the environment variables, and the system the program runs on
6. [std.pool](std/pool.md): the threads `parallel for` runs on
7. [std.net](std/net.md): TCP connections driven by events, on IOCP and epoll
8. [std.hint](std/hint.md): `black_box`, which hides a value from the optimizer, for benchmarks
9. [std.text.mutf8](std/text-mutf8.md): Java's Modified UTF-8, as `DataOutputStream.writeUTF` writes it
10. [std.mail](std/mail.md): messages between actors, delivered at the barrier in order of sender, then of sending
11. [std.hash](std/hash.md): MD5, SHA-1, SHA-256, SHA-384, SHA-512 and HMAC with any of them, as Java's `MessageDigest` and `Mac` and Python's `hashlib` give them, with SHA-1 and SHA-256 on the processor's SHA instructions
12. [std.crypto](std/crypto.md): random bytes from the system, AES-CFB8 and RSA
13. [std.zlib](std/zlib.md): zlib, gzip and raw deflate, as Java's `Deflater`, `Inflater` and GZIP streams write and read them
14. [std.http](std/http.md): HTTP and HTTPS requests on the system's own client, each on a thread of its own, and `fetch`, which waits for one, for scripts
15. [std.lz4](std/lz4.md): LZ4 block streams, as lz4-java's `LZ4BlockOutputStream` and `LZ4BlockInputStream` write and read them
16. [std.thread](std/thread.md): threads for services, which only `unsafe` code starts, the typed queues they talk through, and sleeping
17. [std.meta](std/meta.md): declarations as data, and the code an attribute gives back
18. [std.fs](std/fs.md): files and directories, read and written whole or in parts in place, copied, walked and found by globs, with atomic renames, locks, the current folder, absolute and real paths, and temporary directories
19. [std.build](std/build.md): the program's build ID, which changes exactly when its code does, for keying caches
20. [std.term](std/term.md): the terminal's size, raw input a key at a time, and the text that puts it back however the program ends
21. [std.process](std/process.md): starting other programs, with their arguments, folder, environment, input, output and timeouts, talking to one while it runs, several at once, finding them, and the CPU time and memory they used; and this process's memory, CPU time and processors, and `exit`
22. [std.text.utf8](std/text-utf8.md): bytes that may not be UTF-8 as text, with U+FFFD in their place, as Java reads them or as Python, Rust and browsers do
23. [std.palette](std/palette.md): Minecraft's paletted containers, a chunk section's block states and biomes, grown, sent, saved and loaded as Java Edition 26.3 does them
24. [std.zip](std/zip.md): zip archives read and written in memory, with Zip64, file modes, and extracting that no path can climb out of
25. [std.tar](std/tar.md): tar archives, plain, gzipped, xz'd or zstd'd, read and written in memory: ustar, GNU long names and pax headers, links and modes
26. [std.memory](std/memory.md): giving the system back the memory freed values held, for a program that's gone quiet after a busy while
27. [std.path](std/path.md): paths as text, joined, taken apart, normalized and matched against globs such as `src/**/*.calx`, with Windows drives and shares
28. [std.json](std/json.md): JSON parsed into a tree with the line and column of any mistake, read by JSON Pointers, written compact or indented with keys in order or sorted, exact 64-bit integers, and `@json`, which reads and writes a struct from its fields' types
29. [std.regex](std/regex.md): regular expressions in Rust's syntax, found, counted, captured, replaced and split in time linear in the text, with Unicode classes and case folding; and Python's `re` as it is, with `Regex.python`
30. [std.cli](std/cli.md): `@cli`, which reads a program's command-line options, flags and arguments into a struct, with `--help` written from its docs
31. [std.random](std/random.md): random numbers from a seed, the same on every system, or seeded from the system: integers, floats, normal values, shuffles, picks and samples
32. [std.bytes](std/bytes.md): bytes as hex and base64 and back, finding bytes in bytes, and numbers read from and written to bytes in either byte order
33. [std.bigint](std/bigint.md): integers of any size, with the arithmetic operators, powers, roots, gcd, and text in any base
34. [std.stats](std/stats.md): means, medians, quantiles, modes, variances, standard deviations, correlation and linear regression, as Python's `statistics` gives them to the last bit
35. [std.diff](std/diff.md): the fewest changes between two lists with Myers' algorithm, and unified diffs of text as `diff -u` writes them
36. [std.decimal](std/decimal.md): decimals of any precision, as Python's `decimal` module has them, with exact money arithmetic, every rounding, and correctly rounded roots, exponentials, logarithms and powers
37. [std.image](std/image.md): images read from PNG, JPEG and ICO files, resized, blurred, cropped, turned, drawn on with lines, shapes and text, blended, quantized and written back, with Pillow's pixels and Pillow's JPEG bytes
38. [std.image.png](std/image-png.md): PNG files read and written as libpng does it, every color type and bit depth, interlaced or not
39. [std.image.jpeg](std/image-jpeg.md): JPEG files read as libjpeg-turbo reads them, baseline and progressive, and written byte for byte as Pillow writes them
40. [std.url](std/url.md): percent-encoding and form bodies as Python's `urllib.parse` gives them, and URLs taken apart, put back together and joined as RFC 3986 says, or exactly as Python's `urlsplit` and `urljoin` do
41. [std.uuid](std/uuid.md): UUIDs at random, from a name, and as Java's `nameUUIDFromBytes` makes them, read and written as text and bytes
42. [std.csv](std/csv.md): CSV read as Python's `csv.reader` and `DictReader` read it, quirks included, a row at a time or all at once, and written as `csv.writer` writes it
43. [std.xml](std/xml.md): XML read into a tree as Python's ElementTree reads it, with the line and column of what isn't well-formed, elements found by ElementTree's paths, and trees written back plain or indented
44. [std.html](std/html.md): HTML text escaped, and its character references read back with every name HTML5 has, exactly as Python's `html.escape` and `html.unescape` do
45. [std.sqlite](std/sqlite.md): SQLite databases as Python's `sqlite3` reads and writes them, with SQLite built in: SQL with parameters, rows of typed values, transactions, and prepared statements kept for the next run
46. [std.unicode](std/unicode.md): text normalized to NFC, NFD, NFKC or NFKD, as Python's `unicodedata.normalize` and `is_normalized` give it, with Unicode 16's tables, and combining classes
47. [std.toml](std/toml.md): TOML 1.0 config files read as Python's `tomllib` reads them, with the line and column of any mistake, written back byte for byte as tomli-w writes them, and `@toml`, which reads a config straight into a struct
48. [std.textwrap](std/textwrap.md): text wrapped and filled to a width, shortened, dedented and indented, exactly as Python's `textwrap` does it, with its settings
49. [std.shell](std/shell.md): command lines split into words as a POSIX shell reads their quotes and escapes, and words quoted and joined back, exactly as Python's `shlex.split`, `quote` and `join` do it
50. [std.ini](std/ini.md): INI files read, given and written exactly as Python's `configparser` does it, with sections, comments, values on several lines, `[DEFAULT]`, `%(name)s`, and the line of any mistake
51. [std.http.server](std/http-server.md): a small HTTP/1.1 server for scripts, as Python's `http.server` is, with each request handed to the program in turn, and a folder served as `python -m http.server` serves it
52. [std.yaml](std/yaml.md): YAML read into a tree as PyYAML's `safe_load` reads it, YAML 1.1 and PyYAML's quirks included, with the line and column of any mistake, and written back byte for byte as `safe_dump` writes it, and `@yaml`, which reads a config straight into a struct
53. [std.log](std/log.md): logging as Python's `logging` module does it, messages at a level written in Python's formats with the time, the level and the logger's name, to standard error or a file, set up once with `config`
54. [std.xz](std/xz.md): .xz, .lzma and .lz files decompressed as Python's `lzma.decompress` reads them, every filter and check, and .xz and .lzma files compressed at `xz`'s presets, both on all the processors, whole or a piece at a time
55. [std.zstd](std/zstd.md): Zstandard files decompressed as Python's `compression.zstd` reads them, with checksums, skippable frames and dictionaries, compressed at `zstd`'s levels, with or without a dictionary, big ones on all the processors, and dictionaries trained from samples
56. [std.font](std/font.md): TrueType and OpenType fonts and collections read as FreeType reads them, glyphs drawn antialiased by the exact area they cover, and text measured and drawn on images, as Pillow's `ImageFont` and `ImageDraw.text` do
57. [std.wave](std/wave.md): WAV files read as Python's `wave` module reads them and written byte for byte as it writes them, floats too, with their samples as numbers instead of `struct.unpack`
58. [std.text.python](std/text-python.md): Python's `repr` of strings and floats, `str.isprintable` and `str.isspace`, as CPython 3.14 gives them, for messages that quote values as Python does
59. [std.json.schema](std/json-schema.md): JSON Schema checked as Python's jsonschema checks it, drafts 3 to 2020-12, with the same errors, messages, paths and order, and `$ref`s found as the referencing library finds them

### Java compatibility

Hash tables that iterate in exactly the order Java's or fastutil's do, for code ported from Java whose results depend on it. They're in the reference too, but most programs want [`Map` and `Set`](reference/maps.md):

1. [Java's hash maps](reference/java-maps.md): `JavaHashMap<K, V>` and `JavaHashSet<T>`, in Java's order, `JavaHash`, Java's methods written in Calx, and their C
2. [fastutil's hash maps](reference/fastutil-maps.md): `OpenHashMap<K, V>` and `OpenHashSet<T>`, in fastutil's order, which fastutil class each is, fastutil's methods written in Calx, and their C

### Design

1. [Roadmap](roadmap.md): what's built, what's being built now, and what comes after
2. [Calx spec](CALX_SPEC.md): the design of every feature that isn't built yet
3. [Tick-loop benchmark](benchmarks.md): the workload Calx is timed against Rust on, how it's measured, and how the two versions are kept comparable


## How the docs are organized

| Section | Folder | For | Normative? |
| --- | --- | --- | --- |
| Guide | `guide/` | Learning Calx, in reading order, as tutorials | No |
| Reference | `reference/` | One page per shipped feature: syntax, meaning, examples, errors | Yes |
| Standard library | `std/` | One page per `std` module | Yes |
| Spec | `CALX_SPEC.md` | Features that are designed but not built yet | Yes, until the feature ships |

A feature lives in exactly one normative place. When it ships, its spec section moves to a reference page, and a link is left behind in the spec.

## Rules

- **Docs ship with the feature.** A change to the language, the compiler's behavior, the standard library or the runtime updates its pages in the same change. A feature without a reference page isn't done.
- **Examples are tests.** From Phase 3, every Calx code block in these docs is compiled by the compiler test suite, and run if it shows output. A block that should fail to compile is marked with the error it must produce.
- **One topic per page.** Each page starts with front matter giving its `title` and `description`, so a site generator can build the website from these files unchanged. The front matter is YAML, so a description with `: ` in it goes in double quotes.
- **Descriptions of 70 to 160 characters.** The description is the page's summary in search results, which cut it off after about 160 characters, so it says what the page is first and stays under that.
- **Relative links only.** Link to other pages by relative path, such as `reference/integers.md`, so the links work both here and on the site.
- **Decisions in parentheses.** Cite a decision from the [decision log](https://github.com/ellipog/calcite/blob/main/docs/decisions.md) as `(D21)` at the end of the sentence it supports, never in a heading or as a word in the sentence. The site leaves these out, since its readers don't have the log beside them, so the sentence has to make sense without one (D180).
- **Written for any program.** The guide, reference and std pages describe each feature for programs in general. Minecraft comes up where a module exists for it, such as `std.palette`, or as one example among others, and "vanilla" and Calcite's own needs stay in the spec (D180).
- **Page order lives here.** When you add a page, add it to the list above.
