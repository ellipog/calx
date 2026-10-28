---
title: Calx documentation
description: Index of the Calx docs, the page order, and the rules for writing them.
---

# Calx documentation

These pages are the source for the Calx docs website. Until the site exists, read them here.

## Pages

In reading order:

### Reference

What's built, one page per feature:

1. [The calx command](reference/command.md): check, build, run, emit-c, how builds stay fast, and errors for editors
2. [Editor support](reference/editors.md): the VS Code extension, and the grammar and error format other editors can use
3. [Syntax](reference/syntax.md): files, comments, names, keywords, literals, raw strings and operators
4. [Types](reference/types.md): the primitive types, arrays, `Copy`, and where the other types are
5. [Variables](reference/variables.md): let, var, constants, assignment, unused names, no shadowing
6. [Integers](reference/integers.md): wrapping, division, shifts and constant expressions, as in Java
7. [Floats](reference/floats.md): IEEE 754 arithmetic and printing, as in Java
8. [Conversions](reference/conversions.md): exact `T(x)`, the named lossy conversions, and `try_from`
9. [Control flow](reference/control-flow.md): if, while, while let, for, `for x in mut xs`, loop, break and continue
10. [Functions](reference/functions.md): parameter modes, named arguments, returns, evaluation order
11. [Structs](reference/structs.md): fields, defaults, literals and methods
12. [Enums](reference/enums.md): variants with and without data, representations, conversions
13. [Tuples](reference/tuples.md): values grouped by position, and destructuring
14. [Optionals](reference/optionals.md): `T?` instead of null, `??`, `!`, `?.`
15. [Errors](reference/errors.md): functions that can fail, `fail`, `try`, matching `ok` and `err`, `!`, a failing `main`, and their C
16. [Patterns and match](reference/patterns.md): match, exhaustiveness, `if let`, `let ... else`
17. [Strings](reference/strings.md): `str` and `String`, free literals, building, methods, slicing, Java's case mapping and matching
18. [Lists](reference/lists.md): `List<T>`, its methods, indexing, loops and printing
19. [Deques](reference/deques.md): `Deque<T>`, added to and taken from at both ends, its methods, indexing, Java's and Rust's names for them, and its C
20. [Priority queues](reference/priority-queues.md): `PriorityQueue<T>`, least first, its elements in Java's places, its methods, taking out marked elements, Java's names for them, and its C
21. [Boxes](reference/boxes.md): `Box<T>`, one value on the heap so a type can hold itself, its value as the place `b.value`, and its C
22. [Maps and sets](reference/maps.md): `Map<K, V>` and `Set<T>`, their methods, indexing, loops, deterministic order, `Hash`, and their C
23. [Java's hash maps](reference/java-maps.md): `JavaHashMap<K, V>` and `JavaHashSet<T>`, in Java's order, `JavaHash`, Java's methods written in Calx, and their C
24. [fastutil's hash maps](reference/fastutil-maps.md): `OpenHashMap<K, V>` and `OpenHashSet<T>`, in fastutil's order, which fastutil class each is, fastutil's methods written in Calx, and their C
25. [Linked maps and sets](reference/linked-maps.md): `LinkedMap<K, V>` and `LinkedSet<T>`, in the order keys went in, their ends, Java's and fastutil's methods written in Calx, and their C
26. [Sorted maps and sets](reference/sorted-maps.md): `SortedMap<K, V>` and `SortedSet<T>`, in key order, their ends, the nearest keys, ranges, Java's and fastutil's methods written in Calx, and their C
27. [Ownership](reference/ownership.md): `Copy`, moves, views, partial moves, when values are freed, and their C
28. [Arenas](reference/arenas.md): `Arena` and `ArenaList<T>`, lists freed all at once, the rules that keep them safe without lifetimes, parallel loops, and their C
29. [Generics](reference/generics.md): generic functions and types, inference, bounds, `Copy`, `Equal`, `Display`, `Hash` and `JavaHash`, and their C
30. [Interfaces](reference/interfaces.md): methods, constants and defaults, impls, and naming the interface
31. [Comparing and printing](reference/comparing.md): the derived `Equal` and `Display`, `==` and `{x}` on every type, and their bounds
32. [Modules](reference/modules.md): files as modules, imports, `pub`, and one C file per module
33. [Printing and panics](reference/printing.md): print, println, eprint, eprintln, assert and panic, and `@track_caller`
34. [Unsafe code and C functions](reference/unsafe.md): `unsafe` blocks and functions, raw pointers, and calling C
35. [Libraries](reference/libraries.md): `calx build --lib`, `@export`, opening a library while a program runs, types checked down to the fields, and the one runtime they share
36. [Attributes](reference/attributes.md): attributes a library defines as Calx functions the compiler runs, their arguments, attributes on fields, the code they generate, the cache, and `calx expand`
37. [Parallel loops](reference/parallel.md): `parallel for` over a range or a list's elements, the capture check, and the same result with any number of threads
38. [Strictness](reference/strictness.md): the rules that make the checker stricter than Rust's

### Standard library

The `std` modules, one page each ([modules](reference/modules.md#the-standard-library)):

1. [std.io](std/io.md): standard input and output as bytes
2. [std.math](std/math.md): fast math functions, accurate to about half an ULP and the same on every platform
3. [std.java.math](std/java-math.md): math functions with `StrictMath`'s results bit for bit
4. [std.time](std/time.md): a monotonic clock, and the wall clock with its dates and local time zone, which only `unsafe` code can read
5. [std.env](std/env.md): the command-line arguments
6. [std.pool](std/pool.md): the threads `parallel for` runs on
7. [std.net](std/net.md): TCP connections driven by events, on IOCP and epoll
8. [std.hint](std/hint.md): `black_box`, which hides a value from the optimizer, for benchmarks
9. [std.text.mutf8](std/text-mutf8.md): Java's Modified UTF-8, which NBT uses
10. [std.mail](std/mail.md): messages between actors, delivered at the barrier in order of sender, then of sending
11. [std.hash](std/hash.md): MD5, SHA-1, SHA-256 and HMAC-SHA256, as Java's `MessageDigest` and `Mac` give them
12. [std.crypto](std/crypto.md): random bytes, AES-CFB8 and RSA, for vanilla's online-mode login
13. [std.zlib](std/zlib.md): zlib, gzip and raw deflate, as Java's `Deflater`, `Inflater` and GZIP streams write and read them
14. [std.http](std/http.md): HTTP and HTTPS requests on the system's own client, each on a thread of its own
15. [std.lz4](std/lz4.md): LZ4 block streams, as lz4-java's `LZ4BlockOutputStream` and `LZ4BlockInputStream` write and read them
16. [std.thread](std/thread.md): threads for services, which only `unsafe` code starts, and the typed queues they talk through
17. [std.meta](std/meta.md): declarations as data, and the code an attribute gives back
18. [std.fs](std/fs.md): files and directories, with atomic renames, real paths and temporary directories

### Design

1. [Calx spec](CALX_SPEC.md): every feature that is designed but not built yet
2. [Tick-loop benchmark](benchmarks.md): the workload behind the Phase 3 performance gate

The guide starts once enough of the language is built to write a real program.

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
- **One topic per page.** Each page starts with front matter giving its `title` and `description`, so a site generator can build the website from these files unchanged.
- **Relative links only.** Link to other pages by relative path, such as `reference/integers.md`, so the links work both here and on the site.
- **Page order lives here.** When you add a page, add it to the list above.
