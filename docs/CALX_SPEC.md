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
2. **Fast.** It compiles to C with no garbage collector and no hidden costs, within about 10% of Rust on the [tick-loop benchmark](benchmarks.md).
3. **Safe by default.** No undefined behavior outside `unsafe`, so an untrusted plugin can't corrupt the server (D23).
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
- Strings interpolate with `{expr}`: `"moved {d} blocks"`.
- Loops are `while cond { }`, `for x in iterable { }` and `loop { }`, with `break` and `continue`.
- Logic uses words: `and`, `or`, `not`. Comparison and arithmetic use the usual symbols.
- `match v { Pattern => expr, ... }` matches enum variants, `none` and `some(x)`, `ok(v)` and `err(e)`, literals, tuples and `_`.
- A function with no `->` returns nothing. `fn f() ! E` returns nothing but can fail with an `E`.
- Calls can name their arguments, `teleport(to: home)`, and parameters can have defaults, `fn f(n: i32 = 3)`.
- A closure passed as the last argument can follow the call: `p.after(ticks: 60) |p| { ... }`.
- In an `impl T` block, a function whose first parameter is a `T` in any mode (`self: T`, `r: mut T`, `x: own T`) is a method, called as `value.f(...)`. `self` alone means a read-only `self: T`.

## Types

### Primitive types

| Type | Meaning |
| --- | --- |
| `i8`, `i16`, `i32`, `i64` | Signed integers, two's complement. `i8` is Java's `byte`, `i16` is `short`, `i32` is `int`, `i64` is `long` |
| `u8`, `u16`, `u32`, `u64` | Unsigned integers, for wire formats and bit work |
| `char` | An unsigned 16-bit UTF-16 code unit: Java's `char` |
| `rune` | A Unicode scalar value, what iterating a string gives |
| `f32`, `f64` | IEEE 754 binary32 and binary64: Java's `float` and `double` |
| `bool` | `true` or `false` |
| `usize` | An index or length. 64-bit on every platform Calx targets, 32-bit on `wasm32` |

### Compound types

- **Structs** hold named fields, with optional defaults. `struct Name(String)` is a one-field wrapper, read as `.0`.
- **Enums** are sum types. A variant can carry data: `enum Shape { Circle(f64), Rect { w: f64, h: f64 } }`. An enum with no data can name a representation and values: `enum Flag: u8 { OnGround = 1 }`.
- **Tuples:** `(f32, f32)`, destructured with `let (a, b) = t`.
- **Fixed arrays:** `[T; N]`. **Lists:** `List<T>`, growable, owning their elements.
- **Functions and closures:** `fn(A) -> B` for a plain function. Closures are written `|a| expr` and have their own types, accepted through interfaces such as `Fn(A) -> B`.

### Optionals, no null (D23)

There is no null. A value that may be absent is `T?`:

```calx
fn find(names: List<String>, wanted: String) -> usize? {
    for (i, n) in names.enumerate() {
        if n == wanted {
            return i
        }
    }
    return none
}

fn index_or_zero(names: List<String>, wanted: String) -> usize {
    let i = find(names, wanted) else return 0     // unwrap, or leave the function
    return i
}

fn first_letter(s: String) -> rune {
    if let r = s.runes().next() {                 // unwrap for one block
        return r
    }
    return 'z'
}
```

- `none` is the empty value. A plain `T` converts to `T?` where one is expected.
- `if let x = opt { }`, `let x = opt else <diverge>`, `a ?? b`, `a?.b`, `opt.map(f)` and `opt.or(default)`.
- `opt!` unwraps, and panics if it's empty.
- `T? == T` compares: false when the optional is empty.

### Errors are values

A function that can fail declares an error type after `!`:

```calx
enum ParseError { Empty, TooLong(usize) }

fn parse_name(raw: String) -> String ! ParseError {
    if raw.is_empty() {
        fail ParseError.Empty
    }
    if raw.len() > 16 {
        fail ParseError.TooLong(raw.len())
    }
    return raw
}

fn greet(raw: String) -> String ! ParseError {
    let name = try parse_name(raw)    // passes the error up
    return "hi {name}"
}
```

- `fail e` returns the error.
- `try f()` returns early with the error, converted through `From` if the types differ.
- `match` on a call's result sees `ok(v)` and `err(e)`.
- There are no exceptions. Panics are for bugs ([Panics](#panics)).

### Interfaces and generics (D26)

```calx
interface Shape {
    fn area(self) -> f64
    const sides: i32
}

struct Square { side: f64 }

impl Shape for Square {
    fn area(self) -> f64 {
        return self.side * self.side
    }
    const sides = 4
}

fn total<T: Shape>(shapes: List<T>) -> f64 {
    var sum = 0.0
    for s in shapes {
        sum += s.area()
    }
    return sum
}
```

- Interfaces have methods, associated constants and associated types.
- Generic functions and types are **monomorphized**: each instantiation becomes its own C code, so a generic call costs what a direct call costs.
- `dyn Shape` is an owned value of any type implementing `Shape`, called through a table, for the places that need it (the host's handler tables).
- There's no inheritance and there are no classes.
- **Operators** come from standard interfaces (`Add`, `Sub`, `Mul`, `Neg`, `Index`, `Equal`, `Order`). Only value types should implement the arithmetic ones, such as `Vec3`. The sketches subtract positions this way.

### Derived properties

The compiler works these out for each type, without being asked:

| Property | Holds when |
| --- | --- |
| `Copy` | Every field is `Copy` and the type owns no heap memory. Copying is implicit only for these |
| `Equal`, `Hash`, `Order` | Every field has them. A type can implement them by hand instead |
| `Send` | The value can cross between regions: no views, no closures that capture views, nothing that's only valid in one place. Plain data and handles are `Send` |
| `Share` | Deeply immutable while shared, so many parallel iterations can read it at once |

### Handles (D24)

`Handle<T>` is a generational reference into a slot map: a 32-bit index and a 32-bit generation. Using one gives `T?`-like access:

- `map.get(h)` returns `none` if the object died or moved away.
- `map[h]` panics in that case. Calcite core uses this where a stale handle would be a bug.

Handles are `Copy` and `Send`, which makes them the way to refer to an entity, player or chunk from anything that outlives one handler. Calcite's `PlayerRef` and `EntityRef` wrap handles with a UUID, so they still mean the same object after a region handoff.

## Integer and float semantics

Calx matches Java exactly. Each rule below says what Calx does and how the C emitter does it, because each is undefined or implementation-defined in C.

### Integers

| Rule | Calx | Emitted C |
| --- | --- | --- |
| Overflow | `+`, `-`, `*` and unary `-` wrap (two's complement) for every integer type, as Java does. `checked_add`, `saturating_add` and the like exist for code that wants otherwise | Arithmetic on the unsigned type of the same width, then conversion back, which is defined in C23. Also built with `-fwrapv` |
| Shifts | `<<`, `>>` and `>>>` are defined on `i32`, `i64`, `u32` and `u64`. The count is masked to 31 or 63, as in Java. `>>` is arithmetic on signed types, and `>>>` is a logical shift on any type. Shifting an `i8` or `i16` means converting it first, as Java promotes it | `x << (n & 31)` |
| `MIN / -1` | Gives `MIN`. `MIN % -1` gives `0` | A branch on `-1` before the division, which traps on x86 otherwise |
| Division by zero | **Panics**, for `/` and `%` on every integer type (D55). Java throws `ArithmeticException`, which vanilla never catches, so this is the same outcome. `checked_div` returns `none` instead | A branch on zero, then the runtime's panic |
| Rounding | Division truncates toward zero, and the remainder takes the dividend's sign, as in Java and C. `floor_div` and `floor_mod` are Java's `Math.floorDiv` and `Math.floorMod` | Plain `/` and `%` after the checks |
| Conversions | Always explicit: `i32(x)`, `i8(x)`, `f64(n)`. Narrowing keeps the low bits, as Java's casts do. `x.try_into(T)` returns `T?` | Casts through unsigned types, so they're defined |
| Literals | An integer literal takes its type from context, defaulting to `i32`. A suffix fixes it: `5i64`, `255u8`. A literal that doesn't fit its type is a compile error | |

### Floats

| Rule | Calx | Emitted C |
| --- | --- | --- |
| Format | IEEE 754 binary32 and binary64, round to nearest even, no flush-to-zero | SSE2 on x86-64, never x87 |
| Contraction | No fused multiply-add unless written as `fma(a, b, c)`, and no reassociation. `a * b + c` rounds twice, as in Java | `-ffp-contract=off`, never `-ffast-math` |
| Float to integer | `i32(f)` and `i64(f)` saturate at the type's bounds, and NaN gives 0, as Java's casts do. `i8(f)` and `i16(f)` convert to `i32` first and then narrow, which is what `(byte) f` does in Java | An explicit range and NaN check, since out-of-range conversion is undefined in C |
| Integer to float | Rounds to nearest even | A plain cast |
| Literals | Unsuffixed float literals are `f64`. `1.0e-5f32` is an `f32`. A literal keeps its type, so Java's `1.0E-5F` widened to a double is written `f64(1.0e-5f32)` | |
| Comparison | `==` and `<` are IEEE: NaN is unequal to everything, and `-0.0 == 0.0`. `total_cmp` is Java's `Double.compare` order, for sorting | |
| `min`, `max` | Java's: NaN if either argument is NaN, and `-0.0` is less than `0.0` | Not C's `fmin`, which ignores NaN |
| `round` | Java's `Math.round`: the nearest integer, with halves rounded up, saturating like a cast | |
| Text | `f.java_string()` prints exactly what Java's `Double.toString` and `Float.toString` print (the shortest form that reads back the same, JDK 19 and later), for SNBT and JSON output | |
| Mixed arithmetic | There is none. `f32 * i32` doesn't compile, and Java's implicit widening is written out: `f64(meters * f32(packets))` | |

### Transcendental functions

- `std.math` gives `sqrt`, `abs`, `floor`, `ceil` and `rint`. They're exact in IEEE, so every implementation agrees.
- `sin`, `cos`, `tan`, `asin`, `acos`, `atan`, `atan2`, `exp`, `expm1`, `log`, `log10`, `log1p`, `pow`, `cbrt`, `hypot`, `sinh`, `cosh` and `tanh` give Java's `StrictMath` results bit for bit on every platform.
- They're ported from fdlibm, the library `StrictMath` is defined by, with fdlibm's license notice kept (D59, D60). Porting is the surest way to get the same constants and order of operations, which decide the last bit. The rig checks every function against `StrictMath`.
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

## Memory model (D48)

### Ownership

- Every value has one owner: a variable, a field, or an element of a collection.
- Assigning or passing a non-`Copy` value moves it, and using the old name afterwards is a compile error. `.copy()` makes an explicit copy.
- A value is destroyed when its owner goes out of scope, in reverse order of declaration. There's no garbage collector and no reference counting in safe code.
- `Box<T>` owns a value on the heap, for recursive types. `Shared<T>` is an immutable value with an atomic reference count, for data many regions read: registries, config and the tick snapshot. It's `Share`, and nothing can change it.

### Views

Functions take parameters in one of three modes:

| Mode | Written | The function gets | The caller passes |
| --- | --- | --- | --- |
| Read | `x: T` | A read-only view, or a copy for small `Copy` types | `value` |
| Mutate | `x: mut T` | A mutable view | `mut value` |
| Take | `x: own T` | Ownership. The caller can't use the value afterwards | `value` |

- **Views are second-class.** They can be passed and used, but not stored in a field, returned, captured by a closure that outlives the call, or sent. So the compiler never needs lifetimes. It only checks each function body on its own.
- **No overlapping mutable views.** Within a call, a value passed as `mut` can't also be passed again, as either mode. Within a function, a `mut` view and any other view of the same value can't both be in use. The Calcite sketches hit this four times, and each time it was a real aliasing bug.
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

### `unsafe`

- `unsafe { }` allows raw pointers (`*T`), pointer arithmetic, unchecked indexing, reinterpreting bytes, and calling `extern` functions.
- It's allowed only in modules the build marks trusted: Calcite core and trusted plugins.
- The compiler records in every binary whether it contains `unsafe` or `extern`. Calcite refuses to load a native plugin that does unless the server config trusts it (D15).
- That check protects against mistakes, not against a hostile binary: a native plugin is machine code, so trusting it is trusting whoever built it. WASM is the sandbox for code nobody vouches for.

### Arenas and plugin memory

- `std.arena.Arena` is a bump allocator that frees everything at once. Calcite uses one per region per tick for temporary data.
- **Every plugin instance allocates from its own heap**, which the host supplies. A region instance's heap is reset after each handler, since region code keeps no state (D42) and anything lasting is encoded into host-owned bytes. The global instance's heap lives as long as the instance. Stopping an instance frees its whole heap at once, so a panicking plugin can't leak memory.

## Panics

- A panic is an unrecoverable bug: a failed bounds check, a division by zero, `opt!` on `none`, `map[h]` on a stale handle, or `panic("message")`.
- Calx can't catch a panic. Expected failures are errors (`! E`).
- **In a plugin instance:** the panic unwinds to the host's dispatch point, which stops that instance and frees its heap. The region keeps ticking. A plugin that keeps panicking is disabled (D25). Native plugins unwind with a saved jump point at each dispatch, and no destructors are needed because all of the instance's memory is in its heap. WASM traps.
- **In Calcite core:** the runtime writes a crash report with a Calx stack trace, then saves and stops, as vanilla does.
- **Tick budgets** for plugins use the same path: a handler over budget is stopped like a panic (`P7.3`).

## Strings (D28)

- `String` owns UTF-8 bytes and is always valid UTF-8. `str` is a read-only view of a string's bytes. String literals are `str` until something needs to own them.
- **Indexing is by byte offset.** `s[a..b]` panics if `a` or `b` isn't on a character boundary. `s.runes()` iterates Unicode scalar values, and `s.chars()` iterates UTF-16 code units, as Java sees the string.
- **Java compatibility:**
  - `s.java_hash()` is Java's `String.hashCode`, computed over UTF-16 code units.
  - `s.utf16_len()` is Java's `String.length()`, which the protocol's string limits count in.
  - `std.text.mutf8` encodes and decodes Java's Modified UTF-8, which NBT uses.
  - `to_lower_root` and `to_upper_root` match Java's `toLowerCase(Locale.ROOT)`.
- **Interpolation** calls the `Display` interface: `"{x}"`. Text with a prefix, such as `t"..."`, is handled by a library at compile time ([Codegen hooks](#codegen-hooks)).
- `==` compares contents.

## Collections (D27)

Two families: Java-compatible ones where vanilla's order can be seen, and fast native ones everywhere else.

### Java-compatible

These iterate in exactly the order the Java collections do, for the same sequence of operations, including across resizes:

| Calx | Java |
| --- | --- |
| `java.HashMap<K, V>`, `java.HashSet<T>` | `java.util.HashMap`, `HashSet`, including hash spreading, power-of-two tables, the resize split order and tree bins |
| `java.LinkedHashMap<K, V>`, `java.LinkedHashSet<T>` | `LinkedHashMap`, `LinkedHashSet`, insertion order and access order |
| `java.TreeMap<K, V>`, `java.TreeSet<T>` | `TreeMap`, `TreeSet` (`SortedMap` in the samples) |
| `java.ArrayList<T>`, `java.ArrayDeque<T>` | The same, including growth sizes where they can be seen |
| `fastutil.Long2ObjectOpenHashMap<V>`, `Int2ObjectOpenHashMap<V>`, `Object2IntOpenHashMap<K>`, their `Linked` versions, and the matching sets | fastutil's open-addressing maps, with its own hash mixing (`HashCommon.mix`) and load factors |

Keys hash with the `JavaHash` interface: Java's `hashCode` for each type. The standard library implements it for the boxed primitive equivalents, strings, and records of them. Calcite implements it for vanilla's types, such as `BlockPos`.

### Native

| Type | Use |
| --- | --- |
| `List<T>` | A growable array |
| `Map<K, V>`, `Set<T>` | A fast open-addressing hash map, for when order can't be seen |
| `LinkedMap<K, V>` | A map that iterates in insertion order |
| `SortedMap<K, V>` | A B-tree map in key order |
| `SlotMap<T>` | Storage behind `Handle<T>` |
| `BitSet`, `Deque<T>` | |

**Native maps are deterministic (D56).** There's no random hash seed per process, so the same operations always give the same iteration order. Code that iterates a native map by mistake where the order shows still gives the same answer with any thread count and in every replay, even if it doesn't match vanilla.

## Region actors and messages

Actors are the only concurrency model **for game state**. Network I/O, chunk loading and worldgen workers are Calcite core services that talk to the tick loop through queues, written with `unsafe`, and plugins never see threads (D49).

### Actors

```calx
actor struct Counter {
    total: i64 = 0
}

@message
struct Add {
    amount: i64
}

impl Counter {
    fn receive(c: mut Counter, m: Add) {
        c.total += m.amount
    }
}
```

- An `actor struct` gets an inbox and an outbox from the runtime.
- Actors are values that live in a collection, not threads.
- `send dest msg` appends to the current actor's outbox. A message must be `Send`, and sending moves it.
- The destination is any value whose type implements `Destination<M>`, so libraries decide what can be sent to: Calcite's `PlayerRef`, `global` and `region_at(world, pos, load: true)`. Options after a comma go to the destination: `send p Msg { }, offline: keep`.

### Parallel loops

```calx
fn step(counters: mut SlotMap<Counter>, bonus: Shared<Config>) {
    parallel for c in mut counters.values() {
        c.total += bonus.amount       // bonus is Share, so every iteration may read it
    }
}
```

- `parallel for x in mut xs { }` runs the body for every element at once, on the runtime's work-stealing pool. `parallel(pool) for` picks a pool. Each iteration has exclusive access to its own element.
- **The capture check.** The body may read only values that are `Share`, or read-only views of values nothing writes during the loop. It may write only to its own element. The compiler rejects anything else. That's what makes the result the same at any thread count (D21) without locks.
- The loop returns when every iteration has finished. That's the barrier.

### Lockstep ticks

- The runtime provides the barrier, outboxes and ordered delivery. Calcite's tick loop decides the phases: packet phase, global phase, world phase, barrier (D50). The loop itself is ordinary code; see the tick sketch.
- **Delivery order** at a barrier is ascending sender ID, then send order (D20). The global actor is sender 0.
- **No hidden timing.** Nothing in safe Calx can see time, thread IDs or scheduling order. The wall clock comes into Calcite as a recorded input (D44, D54). So region split and merge decisions can only depend on world state (D22).

### Region scoping

Region scoping is enforced at compile time, for Calcite and for native plugins, by three rules together:

- Views can't be stored or sent.
- A `parallel for` body can't reach outside its element.
- A message holds only `Send` values.

So nothing one region can name belongs to another. WASM plugins get the same rules checked at run time by the host, since their compiler isn't Calx's.

## Modules and builds

- **A file is a module.** It starts with `module a.b.c`, and its path must match (`a/b/c.calx`).
- `import a.b.{X, Y}` brings names in. Imports between modules can't form a cycle.
- **A package** is a folder with a `calx.toml` giving its name, version and dependencies. Calcite, each plugin and the standard library are packages.
- **Separate compilation (D26).** Each module compiles to one C translation unit and a generated header. A module is recompiled when its source or the public interface of something it imports changes. The compiler tracks this with a hash of each module's public interface.
- **Generics** are instantiated in the module that uses them. Identical instantiations from different modules are merged at link time, using linkage that lets the linker keep one copy.
- **Build modes:** `debug` (checks, no optimization, full debug info) and `release` (checks kept, optimized). Bounds checks stay on in both. `unsafe` code is the only way to remove one.
- **Targets:** `x86_64-windows`, `x86_64-linux` and `wasm32` from Phase 3 and Phase 7. `aarch64-macos` comes later (D7).

## FFI

- `extern "C" fn name(args) -> ret` declares a C function. Calling one needs `unsafe`.
- C types are in `std.c`: `c.int`, `c.size_t`, `*T`, `*const T`, and `CStr` for C strings.
- `@export_c("name")` exports a Calx function with the C ABI. Plugin entry points use it, and the host calls them through it.
- `extern` is allowed only in trusted modules, the same rule as `unsafe`. A WASM build has no `extern` except the host's imports.
- Struct layout follows C's rules for `@repr(c)` structs. Other structs' layout is the compiler's choice.

## Codegen hooks

Three kinds of generated code:

### Library-defined attributes

Attributes like `@packet`, `@command`, `@save` and `@message` are defined by libraries, not built into the compiler (D57).

```calx
/// Declares a packet. Generates its reader, its writer and its ID lookup.
@attribute
pub fn packet(decl: StructDecl, state: State, direction: Direction, name: str) -> Generated {
    var g = Generated()
    g.add_impl(decl, "PacketRead", read_body(decl))
    g.add_impl(decl, "PacketWrite", write_body(decl))
    return g
}
```

- An attribute is a Calx function marked `@attribute`, which the compiler runs while compiling. It gets a typed description of the declaration it's on, and returns new declarations as data.
- These functions run in the compiler's interpreter. They're deterministic and can't do I/O, except reading files their package lists as build inputs, such as the protocol report that gives packet IDs.
- Their output is ordinary Calx, type-checked like the rest, and shown by `calx expand` for debugging.
- **Attribute blocks.** `name { declarations }` at the top level of a module applies the attribute `@name` to every declaration inside it. Calcite's plugin library defines `region` and `global` this way, so the plugin structure in the samples isn't built into the language.
- **Hidden parameters.** An attribute on a function may add parameters the caller supplies and bind names in the body. That's how a plugin handler sees `region`, `config` and `server` without declaring them: the host passes them in.
- **String prefixes** like `t"..."` work the same way: a function marked `@prefix("t")` gets the literal and its interpolations at compile time and returns an expression. The Calcite text library checks the tags this way.

### Generated data tables

Phase 2's converter writes vanilla's data as neutral JSON tables: block states, registries, items, packet IDs and the command tree ([Calcite's data generator docs](https://github.com/ellipog/calcite/blob/main/oracle/datagen/README.md)). Calcite's build turns them into Calx source, which compiles like any other module. That needs nothing special from the compiler, only that large constant arrays compile quickly. Block states are stored as arithmetic, each block's first state ID and its properties' value counts, so the tables stay small: 1,286 blocks and 121 properties on 26.3, not 35,723 rows.

### Debugging

The emitter writes `#line` directives, so debuggers and profilers on Windows and Linux show Calx source lines, not the generated C (`P3.10`).

## Non-goals

What Calx leaves out, on purpose:

- **No garbage collector, no reference counting in safe code except `Shared<T>`, and no lifetimes.** Ownership and second-class views cover what Calcite needs.
- **No null and no exceptions.** Optionals and error values instead, and panics for bugs.
- **No classes or inheritance.** Structs, enums and interfaces.
- **No implicit conversions**, numeric or otherwise, and no implicit copies of anything that owns memory.
- **No threads, locks or atomics in safe code.** Only `parallel for` and actors.
- **No global mutable state** outside `unsafe`. Constants are fine.
- **No macros** beyond attributes and string prefixes, which are typed and run as Calx.
- **No reflection at run time.** What plugins need from it, such as encoding saved data, is generated at compile time.
- **No `async`/`await` in v0.** It may come back for plugins that wait across ticks (samples README).
- **No undefined behavior outside `unsafe`.** Every case C leaves undefined is either checked or defined above.

## Must checklist

| Must (from the plan and decision log) | Settled in |
| --- | --- |
| Generics and interfaces, monomorphized into C (D26) | [Interfaces and generics](#interfaces-and-generics-d26) |
| No null: absence is an optional type (D23) | [Optionals](#optionals-no-null-d23) |
| Handle types for game objects, checked on use (D24) | [Handles](#handles-d24) |
| Signed overflow wraps | [Integers](#integers) |
| Shift counts masked, `>>>` exists | [Integers](#integers) |
| `MIN / -1` and `MIN % -1` | [Integers](#integers) |
| Integer division by zero: pick a behavior | [Integers](#integers): panics (D55) |
| Float to int saturates, NaN to 0 | [Floats](#floats) |
| `char` unsigned 16-bit, `byte` signed | [Primitive types](#primitive-types) |
| IEEE floats, no contraction, no fast-math | [Floats](#floats) |
| Transcendentals ported from fdlibm, matching `StrictMath` bit for bit | [Transcendental functions](#transcendental-functions) |
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
| Same result with one thread and many (D21) | [Parallel loops](#parallel-loops) |
| Split and merge depend only on world state (D22) | [Lockstep ticks](#lockstep-ticks): no hidden timing |
| Region scoping enforced at compile time | [Region scoping](#region-scoping) |
| Each module is its own C translation unit (D26) | [Modules and builds](#modules-and-builds) |
| FFI: `extern`, C ABI, what trusted plugins unlock | [FFI](#ffi) |
| Codegen hooks: packets, data tables, `#line` | [Codegen hooks](#codegen-hooks) |
| What Calx leaves out | [Non-goals](#non-goals) |
