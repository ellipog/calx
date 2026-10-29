---
title: Calx spec
description: The design of every Calx feature that isn't built yet.
---

# Calx spec v0

Status: v0 (`P0.3`). It was written from what the five sample addons and the three Calcite sketches needed:

- [Sample addons](https://github.com/ellipog/calcite/blob/main/plugins/samples/README.md)
- [Calcite sketches](https://github.com/ellipog/calcite/blob/main/docs/sketches/README.md)

Every section settles the "Must" items the build plan and the [decision log](https://github.com/ellipog/calcite/blob/main/docs/decisions.md) set for it. A [checklist](#must-checklist) at the end maps each one to where it's settled.

This file holds features that are designed but not built. When a feature ships, its section moves to a page in [`reference/`](reference/), which becomes the normative text, and a link is left here (D40). Each feature lives in exactly one place.

The examples use the syntax as it stands now. From Phase 3 every example here is compiled by the test suite (D41), so the syntax will firm up as the compiler meets it.

## Goals

Calx exists to write Calcite and its plugins. In order:

1. **Java-exact where it matters.** Integer and float results, hash codes and collection orders match Java bit for bit, so Calcite can match vanilla.
2. **Fast to run, and fast to build.** It compiles to C with no garbage collector and no hidden costs, and runs the [tick-loop benchmark](benchmarks.md) faster than Rust, which is the target, not just a tie (D89). Unlike Rust, building is quick too: a change rebuilds only what it touched, and `calx run` starts in moments ([Build speed](#build-speed-d64)). When the two conflict, run time wins in release builds and build time wins in debug builds.
3. **Safe and strict by default.** No undefined behavior outside `unsafe`, so an untrusted plugin can't corrupt the server (D23). The type checker is stricter than Rust's ([Strictness](#strictness-d63)).
4. **Simple to write plugins in.** No lifetimes, no locks, no null. Errors are values. The compiler checks region scoping.

## Syntax overview

The basics the rest of this spec uses:

```calx
module example.basics

import std.collections.List

/// Doc comments start with three slashes, and the reference docs are built from them.
pub struct Point {
    x: i32
    y: i32 = 0                    // a field default
}

pub fn manhattan(a: Point, b: Point) -> i32 {
    let dx = (a.x - b.x).abs()   // let: immutable
    var dy = a.y - b.y           // var: mutable
    if dy < 0 {
        dy = -dy
    }
    return dx + dy
}
```

- Statements end at the end of the line. A line that ends in an operator or an open bracket continues on the next.
- Blocks use braces. `if`, `match` and blocks are expressions: `let x = if c { 1 } else { 2 }`.
- Comments are `//`. Doc comments are `///` and are required on every `pub` item of the Calcite host API (D41).
- `pub` makes an item visible outside its module. Everything else is private to the module.
- Attributes are `@name` or `@name(args)` before a declaration ([codegen hooks](#codegen-hooks)).
- Strings interpolate with `{expr}`: `"moved {d} blocks"`. A raw string, `#"..."#`, has no escapes and interpolates with `#{expr}`, and one that ends its line is a block. Built: [raw strings](reference/syntax.md#raw-strings) (D125)
- Loops are `while cond { }`, `while let pattern = value { }`, `for x in iterable { }` and `loop { }`, with `break` and `continue`. Built: [control flow](reference/control-flow.md#while-let) (D111)
- Logic uses words: `and`, `or`, `not`. Comparison and arithmetic use the usual symbols.
- `match v { Pattern => expr, ... }` matches enum variants, `none` and `some(x)`, `ok(v)` and `err(e)`, literals, tuples and `_`.
- A function with no `->` returns nothing. `fn f() ! E` returns nothing but can fail with an `E`.
- Calls can name their arguments, `teleport(to: home)`, and parameters can have defaults, `fn f(n: i32 = 3)`.
- A closure passed as the last argument can follow the call: `p.after(ticks: 60) |p| { ... }`.
- In an `impl T` block, a function whose first parameter is a `T` in any mode (`self: T`, `r: mut T`, `x: own T`) is a method, called as `value.f(...)`. `self` alone means a read-only `self: T`.

## Types

### Primitive types

Built: see [Types](reference/types.md).

### Compound types

Built: [Structs](reference/structs.md), [Enums](reference/enums.md), [Tuples](reference/tuples.md), fixed arrays ([Types](reference/types.md#arrays)), [Lists](reference/lists.md), and plain functions as values, `fn(A, mut B) -> R ! E` ([functions as values](reference/functions.md#functions-as-values), D122). Still to build:

- **Wrapper structs:** `struct Name(String)` holds one field, read as `.0`.
- **Closures:** written `|a| expr`, with types of their own, accepted through interfaces such as `Fn(A) -> B`. A plain `fn(A) -> B` holds no captures.

### Optionals, no null (D23)

Built: see [Optionals](reference/optionals.md) and [Patterns](reference/patterns.md). Still to build:

- `opt.map(f)` and `opt.or(default)`, with closures and generics.
- `?.` calling a method, `opt?.len()`, as well as reading a field.
- Binding names in or-patterns, `Circle(r) | Ring(r)`, when every alternative binds the same names with the same types.

### Errors are values

Built: see [Errors](reference/errors.md). `-> T ! E` and `fn f() ! E`, `fail e`, `try f()`, matching `ok(v)` and `err(e)` with `match`, `if let` and `let ... else`, `f()!`, and `fn main() ! E`, which shows the error and exits with status 1. A result is handled at the call, so it's never stored or dropped (D91). `try` converts the call's error to the function's through the built-in `From<T>` when their types differ ([converting errors](reference/errors.md#converting-errors), D103). Still to build:

- `! E` in closures' types.

### Interfaces and generics (D26)

Built: see [Generics](reference/generics.md) and [Interfaces](reference/interfaces.md), including [generic interfaces](reference/interfaces.md#generic-interfaces) such as `From<T>`, implemented once for each list of arguments, with calls that choose between the impls by their types (D103). Still to build:

- **`dyn Shape`:** an owned value of any type implementing `Shape`, called through a table, for the places that need it (the host's handler tables).
- **Associated types** in interfaces.
- **Generic functions in interfaces**, and **defaults for an interface's constants**.
- **Operators** from standard interfaces (`Add`, `Sub`, `Mul`, `Neg`, `Index`, `Equal`, `Order`). Only value types should implement the arithmetic ones, such as `Vec3`. The sketches subtract positions this way.

### Derived properties

The compiler works these out for each type, without being asked:

| Property | Holds when |
| --- | --- |
| `Copy` | Every field is `Copy` and the type owns no heap memory. Copying is implicit only for these |
| `Equal`, `Display`, `Hash`, `Order` | Every field has them. A struct or an enum can write its own instead |
| `JavaHash` | Java's `hashCode`: built into numbers, `bool`, `char`, `rune`, text and lists. A struct or an enum has the one it writes, as a Java class does |
| `Send` | The value can cross between regions: no views, no closures that capture views, nothing that's only valid in one place. Plain data and handles are `Send` |
| `Share` | Deeply immutable while shared, so many parallel iterations can read it at once |

Built: `Copy` ([Types](reference/types.md)), `Equal`, `Order` and `Display` ([Comparing and printing](reference/comparing.md)), and `Hash` ([Maps and sets](reference/maps.md#hash)), with their bounds, which generic code compares, orders, prints and hashes through. `Order` gives `<`, `cmp` and [sorting](reference/lists.md#sorting). A struct or an enum can [write its own](reference/comparing.md#writing-your-own) `Equal`, `Display`, `Hash` or `Order` by implementing the built-in interface of that name (D102): to compare only some fields, to show a block position as `1, 64, -3`, or to keep a field out of logs. What holds it uses it too, a type with its own `Equal` writes its own `Hash` to be a key, and one with its own `Order` is sorted stably. [`JavaHash`](reference/java-maps.md#javahash) is the key of a `JavaHashMap` (D105). Still to build:

- **`Share`**, with shared values. `Send` is built: every type is `Send` except one that is or holds a pointer or an arena list, and a generic parameter is `Send` when it has the bound ([std.mail](std/mail.md#items)).

### Handles (D24)

`Handle<T>` is a generational reference into a slot map: a 32-bit index and a 32-bit generation. Using one gives `T?`-like access:

- `map.get(h)` returns `none` if the object died or moved away.
- `map[h]` panics in that case. Calcite core uses this where a stale handle would be a bug.

Handles are `Copy` and `Send`, which makes them the way to refer to an entity, player or chunk from anything that outlives one handler. Calcite's `PlayerRef` and `EntityRef` wrap handles with a UUID, so they still mean the same object after a region handoff.

## Integer and float semantics

Calx matches Java exactly. It's all built, and in the reference: [Integers](reference/integers.md), [Floats](reference/floats.md) and [Conversions](reference/conversions.md). That includes `x.mul_add(a, b)`, Java's `Math.fma`; `i64.round(d)` and `i32.round(f)`, Java's `Math.round`; and `x.rint()`, Java's `Math.rint`. `"{x}"` gives exactly what Java's `Double.toString` and `Float.toString` give, for SNBT and JSON, so there's no separate `java_string` method.

### Transcendental functions

- `sqrt`, `abs`, `floor`, `ceil`, `rint` and `mul_add` are methods on the float types. They're exact in IEEE, so every implementation agrees.
- Built: `sin`, `cos`, `tan`, `asin`, `acos`, `atan`, `atan2`, `exp`, `expm1`, `log`, `log10`, `log1p`, `pow`, `cbrt`, `hypot`, `sinh`, `cosh` and `tanh`, in two modules with the same names (D97):
  - [`std.java.math`](std/java-math.md) gives Java's `StrictMath` results bit for bit on every platform, for code that must compute what vanilla computes. It's ported from fdlibm, the library `StrictMath` is defined by, with fdlibm's license notice kept (D59, D60). Porting is the surest way to get the same constants and order of operations, which decide the last bit. It matched JDK 25 on 1,043,282 arguments, and the rig checks every function against `StrictMath`.
  - [`std.math`](std/math.md) is fast, within about half an ULP, and also the same on every platform, since it's all Calx. Its `exp`, `log`, `pow`, `sin`, `cos` and `tan` are its own, faster than glibc and the Windows CRT and more accurate than fdlibm: `exp`, `log` and `pow` are table-driven, and `sin`, `cos` and `tan` share one kernel with no branch on the quadrant. The rest call `std.java.math` until faster algorithms replace them.
- Vanilla calls `Math`, not `StrictMath`: `sqrt` 102 times, `pow` 70, `sin` 30, `cos` 23, `log` 9, `atan2` 9, `acos` 5, `asin` 2, and `log1p` and `hypot` once each. `Mth`'s sine table is filled with `Math.sin` at startup, and every `nextGaussian()` goes through `Math.log`.
- On x86-64, HotSpot replaces some of these with its own intrinsics. The rig measured how far they drift from `StrictMath` on JDK 25.0.5 (`P1.5`, 500,000 arguments per input range):

  | Function | Results that differ from `StrictMath` | Worst |
  | --- | --- | --- |
  | `sin`, `cos`, `tan` | 1.7-3.7% | 1 ULP |
  | `pow` | 8.8-9.8% on ordinary inputs | 1 ULP |
  | `log` | 7.2% on (0, 1), 0.9% on [1, 10^6) | 1 ULP |
  | `exp` | 9.4% | 1 ULP |
  | `log10` | 1.7% | 1 ULP |
  | `cbrt` | 8.3% | 1 ULP |
  | `tanh` | 0.7-2.8% | 2 ULP |
  | `asin`, `acos`, `atan`, `atan2`, `hypot`, `log1p`, `expm1`, `sinh`, `cosh`, `sqrt` | none | |

  `Mth`'s sine table is the same either way, since rounding to `f32` hides the difference.
- Matching the intrinsics would mean copying HotSpot's stubs, which are GPL and can't go in an MIT project (D59). Instead, the rig's reference is vanilla with HotSpot's flags `-XX:+UnlockDiagnosticVMOptions -XX:-UseLibmIntrinsic`. With them, `Math` matched `StrictMath` bit for bit on all 22 million arguments, so Calx can match that configuration exactly. The rig also records vanilla with default flags, and reports every case where the two differ (D60).

## Strictness (D63)

Calx's checker is stricter than Rust's: every rule is an error, no flag turns one off, and the only warnings are for deprecated APIs. The rules that are built are in [Strictness](reference/strictness.md), which links each to its feature's page. These apply to features that aren't built yet:

- **Unused imports** are errors, like unused names.
- **Unreachable code** includes code after `fail`.
- **Closures** end with their value, like blocks used as values. Only a function's body has to use `return`.

## Memory model (D48)

### Ownership

Built: owners, `Copy`, moves, `.copy()`, partial moves, patterns that take values apart, and freeing in reverse order of declaration ([Ownership](reference/ownership.md)), and `Box<T>`, which owns a value on the heap, for recursive types ([Boxes](reference/boxes.md)). Still to build:

- `Shared<T>` is an immutable value with an atomic reference count, for data many regions read: registries, config and the tick snapshot. It's `Share`, and nothing can change it.

### Views

Built: the three parameter modes, views that can't be moved, stored or returned, pattern names that are views, the rule against passing a `mut` value twice in one call, and `for` loops that keep only what they go over from changing, so a loop over one field lets the body change the others (D106) ([Functions](reference/functions.md#parameter-modes), [Ownership](reference/ownership.md#views), [Control flow](reference/control-flow.md#looping-over-a-field)). Still to build:

- **Views are second-class.** Besides not being stored or returned, they can't be captured by a closure that outlives the call, or sent. So the compiler never needs lifetimes. It only checks each function body on its own.
- **No overlapping mutable views.** Within a function, a `mut` view and any other view of the same value can't both be in use. The Calcite sketches hit this four times, and each time it was a real aliasing bug.
- **Projections.** `p.data(Homes)` or `r.chunks[c]` can return a view into the receiver, valid for the expression or for a `let` binding in the same block.
- **`checkout`.** This is for the element-and-owner case:

```calx
fn tick_all(r: mut Region) {
    for id in r.entities.ids_at_start() {
        checkout e = r.entities[id] else continue
        tick_entity(mut e, mut r)     // e and r are separate now
    }                                 // e goes back here, in its place in the order
}
```

`checkout x = c[key]` takes one element out of a collection until the end of the enclosing block, and puts it back at the same position in the collection's order. While it's out, `c[key]` gives `none`. `checkout x = s.field` does the same for an optional field, and the compiler stops `s.field` from being used inside the block.

### Memory safety (D23)

- **Bounds checks** on every index. The compiler removes the ones it can prove, such as `for i in 0..xs.len()`.
- **No pointers in safe code.** No pointer arithmetic, no casts between unrelated types, no uninitialized values: every variable is assigned before use, and struct fields take their defaults.
- **No data races.** The only parallelism is `parallel for` ([Region actors](#region-actors-and-messages)), and its capture check makes races impossible.
- **`restrict`.** Since `mut` views never overlap, the emitter marks every view and owned-value pointer parameter `restrict` in C, which closes Rust's main codegen advantage (D26).
- **Views are always valid.** Every function starts by telling clang that each view parameter points at a whole, live, aligned value (`CX_VIEW`, from `__builtin_assume_dereferenceable` and `__builtin_assume_aligned`). Views can't be stored or outlive what they view, so that holds for the whole call. Clang can then read a view's fields once before a loop rather than on every pass, even after inlining, which Rust's `dereferenceable` parameter attributes don't survive (D89).

### `unsafe`

Built: `unsafe` blocks, `unsafe fn`, raw pointers and their methods, and calling C functions ([Unsafe code and C functions](reference/unsafe.md)). Still to build:

- Unchecked indexing and reinterpreting bytes inside `unsafe`, and pointers inside other types.
- It's allowed only in modules the build marks trusted: Calcite core and trusted plugins.
- The compiler records in every binary whether it contains `unsafe` or `extern`. Calcite refuses to load a native plugin that does unless the server config trusts it (D15).
- That check protects against mistakes, not against a hostile binary: a native plugin is machine code, so trusting it is trusting whoever built it. WASM is the sandbox for code nobody vouches for.

### Arenas and plugin memory

Built: `Arena`, whose lists take memory from its chunks and are freed all at once by `reset()`, and `ArenaList<T>`, which can only be a variable or a view parameter, so the checker keeps it from outliving its memory without lifetimes ([Arenas](reference/arenas.md)). Calcite uses one per region per tick for temporary data. Still to build:

- Other values from an arena, such as maps and strings, once there's a use that a list doesn't cover.
- **Every plugin instance allocates from its own heap**, which the host supplies. A region instance's heap is reset after each handler, since region code keeps no state (D42) and anything lasting is encoded into host-owned bytes. The global instance's heap lives as long as the instance. Stopping an instance frees its whole heap at once, so a panicking plugin can't leak memory.

## Panics

What a panic does in a plain program is built, its stack trace too: see [Printing and panics](reference/printing.md#panics). The rest of this section isn't.

- A panic is an unrecoverable bug: a failed bounds check, a division by zero, `opt!` on `none`, `map[h]` on a stale handle, or `panic("message")`.
- Calx can't catch a panic. Expected failures are errors (`! E`).
- **In a plugin instance:** the panic unwinds to the host's dispatch point, which stops that instance and frees its heap. The region keeps ticking. A plugin that keeps panicking is disabled (D25). Native plugins unwind with a saved jump point at each dispatch, and no destructors are needed because all of the instance's memory is in its heap. WASM traps.
- **In Calcite core:** the runtime writes a crash report with the Calx stack trace ([stack traces](reference/printing.md#stack-traces)), then saves and stops, as vanilla does.
- **Tick budgets** for plugins use the same path: a handler over budget is stopped like a panic (`P7.3`).

## Strings (D28)

Built: `str` and `String`, literals as either, interpolation into a `String`, the basic methods, `==` and string patterns ([Strings](reference/strings.md)). `for r in s.runes()` goes over Unicode scalar values and `for c in s.chars()` over UTF-16 code units, as Java sees the string. `s.java_hash()` is Java's `String.hashCode` and `s.utf16_len()` is its `length()`, both over UTF-16 code units ([Java's view](reference/strings.md#javas-view-of-a-string)), [`std.text.mutf8`](std/text-mutf8.md) encodes and decodes Java's Modified UTF-8, which NBT uses, and [`std.text.utf8`](std/text-utf8.md) reads bytes that may not be UTF-8 as Java does, with U+FFFD in place of what isn't. **Slicing is by byte offset**: `s[a..b]` panics unless the offsets are in order, in the text, and where runes start, and a `String`'s part is copied only where it's kept ([Slicing](reference/strings.md#slicing)). `to_lower_root` and `to_upper_root` match Java's `toLowerCase(Locale.ROOT)` and `toUpperCase(Locale.ROOT)`, final sigma included ([Changing the case](reference/strings.md#changing-the-case), D117). Still to build:

- `String` is always valid UTF-8, and `str` will be able to view part of a `String` past the statement, once a view parameter can hold one.
- **Interpolation** of a type with a `Display` of its own calls it ([Derived properties](#derived-properties)). Text with a prefix, such as `t"..."`, is handled by a library at compile time ([Codegen hooks](#codegen-hooks)).

## Collections (D27)

Two families: Java-compatible ones where vanilla's order can be seen, and fast native ones everywhere else.

### Java-compatible

These iterate in exactly the order the Java collections do, for the same sequence of operations, including across resizes:

| Calx | Java |
| --- | --- |
| `JavaHashMap<K, V>`, `JavaHashSet<T>` | `java.util.HashMap`, `HashSet`, including hash spreading, power-of-two tables, the resize split order and tree bins. Built: [Java's hash maps](reference/java-maps.md) (D105) |
| `LinkedMap<K, V>`, `LinkedSet<T>` | `LinkedHashMap`, `LinkedHashSet`, and fastutil's linked tables such as `Long2ObjectLinkedOpenHashMap`: the order keys went in, which no hashing changes, so it's Calx's own table with a list through it, with Java's `putFirst` and fastutil's `getAndMoveToFirst` and `removeLast`, and access order written as `move_to_last`. Built: [Linked maps and sets](reference/linked-maps.md) (D109) |
| `SortedMap<K, V>`, `SortedSet<T>` | `TreeMap`, `TreeSet`, and fastutil's tree tables such as `LongAVLTreeSet`: key order, which no tree's shape changes, so it's Calx's own B+ tree, with the nearest key on either side as `floor`, `ceiling`, `lower` and `higher`, and `subMap`, `headMap` and `tailMap` written as a loop over `range(a..b)`. A comparator is a key type with that `Order`. Built: [Sorted maps and sets](reference/sorted-maps.md) (D112) |
| `PriorityQueue<T>` | `PriorityQueue`, which vanilla drains its scheduled ticks from: a binary heap whose array is the order its iterator goes in, so it's Calx's own heap that moves the elements as Java's does, with the iterator's `remove` written as `remove_marked`. A comparator is an element type with that `Order`. Built: [Priority queues](reference/priority-queues.md) (D113) |
| `java.ArrayList<T>` | The same, including growth sizes where they can be seen |
| `Deque<T>` | `ArrayDeque`, and the `Queue` and `Deque` vanilla keeps in a `LinkedList` or an `ArrayDeque`: their order is the order elements went in at each end, which growth never changes, so the native `Deque` is them. Built: [Deques](reference/deques.md) (D110) |
| `OpenHashMap<K, V>`, `OpenHashSet<T>` | fastutil's open hash maps and sets, `Long2ObjectOpenHashMap`, `Object2IntOpenHashMap`, `LongOpenHashSet` and the rest, the key type picking the class, with fastutil's hash mixing (`HashCommon.mix`), load factors, removal by shifting and shrinking. Built: [fastutil's hash maps](reference/fastutil-maps.md) (D107) |

Keys hash with `JavaHash`: Java's `hashCode` for each type. It's built into the numbers, `bool`, `char`, `rune`, text and lists, as Java's boxes, `String` and `List` have it, and a struct or an enum writes its own, as a Java class does: Calcite writes it for vanilla's types, such as `BlockPos`. Built: [`JavaHash`](reference/java-maps.md#javahash).

### Native

| Type | Use |
| --- | --- |
| `List<T>` | A growable array. Built: [Lists](reference/lists.md) |
| `Map<K, V>`, `Set<T>` | A fast open-addressing hash map, for when order can't be seen. Built: [Maps and sets](reference/maps.md) |
| `LinkedMap<K, V>`, `LinkedSet<T>` | A map or a set that iterates in the order keys went in, which is Java's and fastutil's too. Built: [Linked maps and sets](reference/linked-maps.md) |
| `SortedMap<K, V>`, `SortedSet<T>` | A B+ tree map or set in key order, with ranges, which is Java's and fastutil's order too. Built: [Sorted maps and sets](reference/sorted-maps.md) |
| `SlotMap<T>` | Storage behind `Handle<T>` |
| `Deque<T>` | A growable ring, added to and taken from at both ends. Built: [Deques](reference/deques.md) |
| `PriorityQueue<T>` | A binary heap, least first, in Java's order. Built: [Priority queues](reference/priority-queues.md) |
| `Box<T>` | One value on the heap, for recursive types. Built: [Boxes](reference/boxes.md) |
| `BitSet` | |

**Native maps are deterministic (D56).** There's no random hash seed per process, so the same operations always give the same iteration order. Code that iterates a native map by mistake where the order shows still gives the same answer with any thread count and in every replay, even if it doesn't match vanilla.

## Region actors and messages

Actors are the only concurrency model **for game state**. Network I/O, chunk loading and worldgen workers are Calcite core services that talk to the tick loop through queues, written with `unsafe`, and plugins never see threads (D49). Built: [std.thread](std/thread.md) (D121). `Thread.spawn(task)` needs `unsafe` and starts a thread that runs a `Task`, and `Receiver<T>()` and its `Sender<T>` ends are typed queues whose messages move between threads. A queue can't be used inside a `parallel for`.

### Actors

Built: [std.mail](std/mail.md) (D115). An actor is a plain struct that a `parallel for` runs, holding an `Outbox<D, M>` for what it sends and an `Inbox<M>` for what it's sent. At the barrier, `Mail` collects the outboxes in ascending sender id and gives the letters back in delivery order. It panics if a sender comes out of order, so the order is checked, not just promised. A message is `Send`.

- Actors are values that live in a collection, not threads, and spawning one is adding it to the collection.
- There's no `actor` keyword and no `send` statement. An earlier design had both, with `send dest msg` appending to the current actor's outbox. An outbox the actor holds as a field does the same work without hidden state, and the capture check already keeps each outbox to its own iteration (D49).
- The destination type `D` is the library's: Calcite's is an enum of the global actor, a region, a chunk with `load`, and a player with `keep`. The library decides how a destination is routed at the barrier, as in the tick sketch.

### Parallel loops

Built: see [Parallel loops](reference/parallel.md) and [std.pool](std/pool.md). `parallel for k in a..b { }` runs on the runtime's pool, each iteration changing only places it indexes with `k`, and the capture check makes the result the same at any thread count (D21). `parallel for x in mut xs { }` gives each iteration a mutable view of its own element, and nothing else in the body reaches `xs` ([over the elements](reference/parallel.md#over-the-elements), D116). A loop inside another gets the threads the outer one has no work for ([loops inside loops](reference/parallel.md#loops-inside-loops)). Still to build:

```calx
fn step(counters: mut SlotMap<Counter>, bonus: Shared<Config>) {
    parallel for c in mut counters.values() {
        c.total += bonus.amount       // bonus is Share, so every iteration may read it
    }
}
```

- Going over the values of a slot map or a map with `in mut`, as above.
- `parallel(pool) for` picks a pool.
- Reading values that are `Share` while something else changes the collection they came from, once shared values exist.

### Lockstep ticks

- The runtime provides the barrier, outboxes and ordered delivery. Calcite's tick loop decides the phases: packet phase, global phase, world phase, barrier (D50). The loop itself is ordinary code; see the tick sketch.
- **Delivery order** at a barrier is ascending sender ID, then send order (D20). The global actor is sender 0.
- **No hidden timing.** Nothing in safe Calx can see time, thread IDs or scheduling order. The clock is an `unsafe fn` ([std.time](std/time.md)), and the wall clock comes into Calcite as a recorded input (D44, D54). So region split and merge decisions can only depend on world state (D22).

### Region scoping

Region scoping is enforced at compile time, for Calcite and for native plugins, by three rules together:

- Views can't be stored or sent.
- A `parallel for` body can't reach outside its element.
- A message holds only `Send` values.

So nothing one region can name belongs to another. WASM plugins get the same rules checked at run time by the host, since their compiler isn't Calx's.

## Modules and builds

Built: see [Modules](reference/modules.md) and [the calx command](reference/command.md). A [library](reference/libraries.md), built with `calx build --lib`, exports functions marked `@export`, which a program opens while it runs and finds by name and type: the type is checked down to every struct's fields, and the library uses the program's runtime, so its memory, output and panics are the program's (D123). That's what a native plugin is. Still to build:

- **A package** is a folder with a `calx.toml` giving its name, version and dependencies. Calcite, each plugin and the standard library are packages, and a package's modules are found under its own root.
- **Reaching items through a module's name:** `import std.math`, then `math.floor(x)`.
- **Merging instances.** Each module's file has its own copy of the generic instances it uses. Release builds can have the linker fold identical copies (`-ffunction-sections` with identical code folding).
- **Targets:** `x86_64-windows`, `x86_64-linux` and `wasm32` from Phase 3 and Phase 7. `aarch64-macos` comes later (D7).

### Build speed (D64)

Building and running Calx code has to be fast. The design choices that make it so:

- **A fast front end.** Lexing, parsing, checking and emitting C use arena allocation and interned names, with no heap allocation per node and no whole-program analysis beyond what monomorphization needs. The front end always rechecks everything, because that's cheaper than tracking what to skip.
- **Clang only where the C changed.** Each module becomes its own C file. An object is cached under a hash of the C text, the flags and the clang version, so a module whose C is unchanged is never compiled again, and changed modules compile in parallel.
- **The runtime is built once** per target and build mode, and reused.
- **Debug builds use `-O0`** with line tables only. Release builds use `-O3` (D89), and ThinLTO, which optimizes the program as a whole when it links, so calls between modules inline as calls within one do (D99). lld caches each file's machine code, so after an edit only the files it could change are compiled again.
- **lld links** on both platforms, since it's several times faster than the default linkers.
- **Lean C.** Emitted code includes only Calx's small runtime header, never `windows.h` or other large system headers.
- **`calx run` reuses the last executable** when nothing changed.

Build time is tracked like run time (D34): [`bench/build.py`](../bench/build.py) times checking and building a large generated program, `calx run` on a small one, and rebuilding the tick benchmark after a change. CI runs it on every push, and the results on the reference machine are in `bench/results/`.

## FFI

Built: `extern "C"` declarations and calls, with integers, floats, `bool` and pointers ([C functions](reference/unsafe.md#c-functions)). Still to build:

- C's own type names are in `std.c`: `c.int`, `c.size_t`, and `CStr` for C strings.
- `@export_c("name")` exports a Calx function with the C ABI, for C code to call. A Calx plugin doesn't need it: [`@export`](reference/libraries.md) exports a function with Calx's own ABI, whose type the host checks when it finds it (D123).
- `extern` is allowed only in trusted modules, the same rule as `unsafe`. A WASM build has no `extern` except the host's imports.
- Struct layout follows C's rules for `@repr(c)` structs. Other structs' layout is the compiler's choice.

## Codegen hooks

Three kinds of generated code:

### Library-defined attributes

Attributes like `@packet`, `@command`, `@save` and `@message` are defined by libraries, not built into the compiler (D57). Built ([attributes](reference/attributes.md), D124): an attribute is a Calx function marked `@attribute`, which the compiler runs while it compiles. It reads the declaration it's on, as a `StructDecl`, `EnumDecl` or `FnDecl` from [std.meta](std/meta.md), with literal arguments and the attributes on its fields, and returns the code to add as text, which is checked like the rest and shown by `calx expand`. The compiler builds the attributes into a native program of their own and caches their answers. The compiler itself knows `@track_caller` (D115), `@export` (D123) and `@attribute`.

```calx
/// Declares a packet. Generates its reader, its writer and its ID lookup.
@attribute("varint", "max")
pub fn packet(decl: StructDecl, id: i32, state: State, bound: Bound) -> Generated ! String {
    var g = Generated.new()
    g.add(try read_impl(decl, id, state, bound))
    g.add(try write_impl(decl, id, state, bound))
    return g
}
```

Still to build:

- **Build inputs.** An attribute may read files its package lists, such as the protocol report that gives packet IDs, and the cache key includes their text. Until then an attribute is expected to depend only on what it's given, which the compiler doesn't enforce.
- **Attribute blocks.** `name { declarations }` at the top level of a module applies the attribute `@name` to every declaration inside it. Calcite's plugin library defines `region` and `global` this way, so the plugin structure in the samples isn't built into the language.
- **Hidden parameters.** An attribute on a function may add parameters the caller supplies and bind names in the body. That's how a plugin handler sees `region`, `config` and `server` without declaring them: the host passes them in.
- **String prefixes** like `t"..."` work the same way: a function marked `@prefix("t")` gets the literal and its interpolations at compile time and returns an expression. The Calcite text library checks the tags this way.

### Generated data tables

Phase 2's converter writes vanilla's data as neutral JSON tables: block states, registries, items, packet IDs and the command tree ([Calcite's data generator docs](https://github.com/ellipog/calcite/blob/main/oracle/datagen/README.md)). Calcite's build turns them into Calx source, which compiles like any other module. That needs nothing special from the compiler, only that large constant arrays compile quickly. Block states are stored as arithmetic, each block's first state ID and its properties' value counts, so the tables stay small: 1,286 blocks and 121 properties on 26.3, not 35,723 rows.

### Debugging

Built: the emitter writes `#line` directives and both build modes keep line tables, so debuggers and profilers on Windows and Linux show Calx source lines, not the generated C ([build modes](reference/command.md#build-modes)). Editors get errors as you type ([editor support](reference/editors.md)). A panic writes its Calx stack trace, inlined calls, `parallel for` bodies and the libraries the program opened included ([stack traces](reference/printing.md#stack-traces)).

## Non-goals

What Calx leaves out, on purpose:

- **No garbage collector, no reference counting in safe code except `Shared<T>`, and no lifetimes.** Ownership and second-class views cover what Calcite needs.
- **No null and no exceptions.** Optionals and error values instead, and panics for bugs.
- **No classes or inheritance.** Structs, enums and interfaces.
- **No implicit conversions**, numeric or otherwise, and no implicit copies of anything that owns memory.
- **No threads, locks or atomics in safe code.** Only `parallel for`, and actors that it runs. Starting a thread takes `unsafe` ([std.thread](std/thread.md)), and threads share no memory, only queues.
- **No global mutable state** outside `unsafe`. Constants are fine.
- **No macros** beyond attributes and string prefixes, which are typed and run as Calx.
- **No reflection at run time.** What plugins need from it, such as encoding saved data, is generated at compile time.
- **No `async`/`await` in v0.** It may come back for plugins that wait across ticks (samples README).
- **No undefined behavior outside `unsafe`.** Every case C leaves undefined is either checked or defined above.

## Must checklist

| Must (from the plan and decision log) | Settled in |
| --- | --- |
| Generics and interfaces, monomorphized into C (D26) | [Generics](reference/generics.md), [Interfaces](reference/interfaces.md) |
| No null: absence is an optional type (D23) | [Optionals](reference/optionals.md) |
| Handle types for game objects, checked on use (D24) | [Handles](#handles-d24) |
| Signed overflow wraps | [Integers](reference/integers.md) |
| Shift counts masked, `>>>` exists | [Integers](reference/integers.md) |
| `MIN / -1` and `MIN % -1` | [Integers](reference/integers.md) |
| Integer division by zero: pick a behavior | [Integers](reference/integers.md): panics (D55) |
| Float to int saturates, NaN to 0 | [Floats](reference/floats.md) |
| `char` unsigned 16-bit, `byte` signed | [Types](reference/types.md) |
| IEEE floats, no contraction, no fast-math | [Floats](reference/floats.md) |
| Transcendentals: `std.java.math` ported from fdlibm, matching `StrictMath` bit for bit, and a faster `std.math` | [Transcendental functions](#transcendental-functions) |
| Memory-safe outside `unsafe` (D23) | [Memory safety](#memory-safety-d23) |
| `unsafe` and `extern` only for Calcite core and trusted plugins | [`unsafe`](#unsafe), [FFI](#ffi) |
| The emitter proves non-aliasing and emits `restrict` (D26) | [Memory safety](#memory-safety-d23) |
| A plugin panic stops only that instance (D25) | [Panics](#panics) |
| No leaked memory when an instance stops | [Arenas and plugin memory](#arenas-and-plugin-memory) |
| UTF-8 strings, Java `hashCode`, UTF-16 length, Modified UTF-8 (D28) | [Strings](#strings-d28) |
| Java-compatible `HashMap`, `HashSet`, `LinkedHashMap` and fastutil maps (D27) | [Java-compatible](#java-compatible) |
| Fast native collections where order isn't observable | [Native](#native) |
| Actors are the only concurrency model | [Region actors](#region-actors-and-messages), refined by D49 |
| Lockstep ticks (D19) | [Lockstep ticks](#lockstep-ticks) |
| Phases and delivery order (D20, now D50) | [Lockstep ticks](#lockstep-ticks) |
| Same result with one thread and many (D21) | [Parallel loops](reference/parallel.md) |
| Split and merge depend only on world state (D22) | [Lockstep ticks](#lockstep-ticks): no hidden timing |
| Region scoping enforced at compile time | [Region scoping](#region-scoping) |
| Each module is its own C translation unit (D26) | [Modules](reference/modules.md), [Modules and builds](#modules-and-builds) |
| FFI: `extern`, C ABI, what trusted plugins unlock | [FFI](#ffi) |
| Codegen hooks: packets, data tables, `#line` | [Codegen hooks](#codegen-hooks) |
| What Calx leaves out | [Non-goals](#non-goals) |
| Stricter types than Rust (D63) | [Strictness](#strictness-d63) |
| Fast builds and fast `calx run` (D64) | [Build speed](#build-speed-d64) |
