---
title: Comparing and printing
description: The derived Equal, Order and Display, which give ==, < and {x} to structs, enums, tuples, arrays, optionals, lists, maps and sets; Ordering and cmp; the bounds T: Equal, T: Order and T: Display; writing your own; and their C.
---

# Comparing and printing

A value can be compared with `==`, ordered with `<` and shown in a string whenever everything it holds can. These are three of the derived properties ([spec](../CALX_SPEC.md#derived-properties)): the compiler works them out for each type, so there's nothing to write. A fourth, [`Hash`](maps.md#hash), says which types can be the keys of a map.

```calx
struct Point {
    x: i32,
    y: i32,
}

enum Shape {
    Dot(Point)
    Line(Point, Point)
    Rect { w: f64, h: f64 }
}

fn main() {
    let a = Shape.Line(Point { x: 0, y: 0 }, Point { x: 1, y: 2 })
    println("{a}")    // out: Line(Point { x: 0, y: 0 }, Point { x: 1, y: 2 })
    println("{a == Shape.Line(Point { x: 0, y: 0 }, Point { x: 1, y: 2 })}")    // out: true
    println("{a != Shape.Rect { w: 1.0, h: 1.0 }}")    // out: true
    let p = Point { x: 3, y: 4 }
    let where = "at {p}"
    println(where)    // out: at Point { x: 3, y: 4 }
}
```

## `Equal`

`a == b` is true when `a` and `b` hold equal values, and `a != b` when they don't. Both sides have the same type.

| Type | Equal when |
| --- | --- |
| A struct | Every field is equal |
| An enum | Both are the same variant, and its values are equal |
| A tuple or an array | Every part is equal |
| An optional | Both are `none`, or both hold equal values ([optionals](optionals.md#comparing)) |
| `List<T>` | Both have the same length, and equal elements in the same order |
| `Deque<T>` | The same, from each one's first element ([deques](deques.md#whats-different)) |
| `PriorityQueue<T>` | The same, in the order of their arrays ([priority queues](priority-queues.md#whats-different)) |
| `Map<K, V>`, `Set<T>` | Both have the same keys, with equal values, in any order ([maps and sets](maps.md#comparing-and-printing)), and the other maps and sets the same way |
| `String` and `str` | Both hold the same bytes |
| A float | As floats compare: `NaN` never equals anything, and `0.0 == -0.0` ([floats](floats.md)) |

The parts are compared in order, and the comparison stops at the first difference. Every field counts, private ones too.

Every type has `Equal` except a generic parameter without the bound, and what holds one.

```calx
enum Item {
    Stone
    Sword(i32)
}

fn main() {
    let a: List<Item> = [Item.Stone, Item.Sword(5)]
    var b: List<Item> = [Item.Stone]
    println("{a == b}")    // out: false
    b.push(Item.Sword(5))
    println("{a == b} {b.contains(Item.Sword(5))}")    // out: true true
    let nan = 0.0 / 0.0
    println("{(1, nan) == (1, nan)}")    // out: false
}
```

## `Order`

`a < b`, `a <= b`, `a > b` and `a >= b` compare two values of the same type in order, and `a.cmp(b)` says which comes first: an `Ordering`, which is `Less`, `Equal` or `Greater`. [`xs.sort()`](lists.md#sorting) puts a list in this order.

| Type | Ordered by |
| --- | --- |
| Integers, `char`, `rune` | Value |
| `bool` | `false` before `true` |
| `String` and `str` | Byte by byte, and a string that runs out first comes first. For UTF-8 that's the order of the code points |
| A struct | Its fields in the order they're declared: the first that differs decides |
| An enum | Its variants in the order they're declared, whatever their values; then the variant's values in order |
| A tuple or an array | Its parts in order |
| `List<T>` | Its elements in order, and a list that runs out first comes first |
| An optional | `none` before any value, as Java's `Comparator.nullsFirst` |
| `Ordering` | `Less`, `Equal`, `Greater` |

Floats have no `Order`, nor do pointers, maps and sets, or what holds them. `<` on two floats compares them as IEEE does, but `NaN` is neither less nor greater than anything, which would leave a sort's result undefined, so a struct holding a float can't be compared with `<`. To order by a float, use [`total_cmp`](floats.md#methods), Java's `Double.compare`, which puts `-0.0` before `0.0` and every `NaN` last.

Java's `String.compareTo` compares UTF-16 code units instead. The two orders differ only where one string has a character above U+FFFF, such as an emoji, and the other has one from U+E000 to U+FFFF in the same place.

```calx
struct Version {
    major: i32,
    minor: i32,
}

enum Rank {
    Guest
    Member
    Admin
}

fn name(o: Ordering) -> str {
    return match o {
        Less => "less",
        Equal => "equal",
        Greater => "greater",
    }
}

fn main() {
    let a = Version { major: 1, minor: 20 }
    let b = Version { major: 2, minor: 0 }
    println("{a < b} {name(a.cmp(b))} {name(b.cmp(b))}")    // out: true less equal
    println("{Rank.Admin > Rank.Member} {"apple" < "apples"}")    // out: true true
    var ranks: List<Rank> = [Rank.Admin, Rank.Guest, Rank.Member]
    ranks.sort()
    println("{ranks}")    // out: [Guest, Member, Admin]
}
```

What lacks `Order` says why:

```calx
struct Mob {
    name: String,
    health: f32,
}

fn main() {
    let a = Mob { name: String("zombie"), health: 20.0 }
    println("{a < a}")    // error: values of type `Mob` can't be compared with `<`, since it holds `f32`, and floats have no `Order`, since `NaN` is neither less than nor greater than anything; compare them with `total_cmp`
}
```

## `Display`

`{x}` in a string shows `x` the way code builds it:

| Type | Shows as |
| --- | --- |
| A struct | Its literal: `Point { x: 1, y: 2 }`, or `Empty {}` with no fields. A generic struct's name has no arguments: `Pair { first: 1, second: one }` |
| An enum | The variant, as it's built but without the enum's name: `Stone`, `Sword(5)`, `Rect { w: 2.0, h: 3.0 }` |
| A tuple | `(1, a, true)` |
| An array or a `List` | `[1, 2, 3]`, as Java's `ArrayList.toString` ([lists](lists.md#printing)) |
| A `Deque` | `[1, 2, 3]`, from the first element, as Java's `ArrayDeque.toString` |
| A `PriorityQueue` | `[1, 3, 2]`, in the order of its array, as Java's `PriorityQueue.toString` |
| A `Map` or a `Set` | `{1: one, 2: two}` or `{1, 2}`, in the order a loop goes ([maps and sets](maps.md#order)) |
| An optional | `some(5)` or `none` |
| `String`, `str`, `char`, `rune` | The text itself, without quotes, even inside a struct or a list |
| A number or a `bool` | As Java prints it ([integers](integers.md), [floats](floats.md#printing)) |

Every field is shown, private ones too, so `Display` is for logs and debugging. Every type has it except a pointer, and a generic parameter without the bound, and what holds either.

```calx
struct Player {
    name: String,
    health: f32,
    friends: List<String>,
    title: String?,
}

fn main() {
    let p = Player { name: String("Steve"), health: 20.0, friends: [String("Alex")], title: none }
    println("{p}")    // out: Player { name: Steve, health: 20.0, friends: [Alex], title: none }
}
```

## In generic code

Generic code compares, orders and prints a `T` only with the bounds `T: Equal`, `T: Order` and `T: Display` ([generics](generics.md#bounds)), and any type that has the property can be the argument. `T: Order` gives `==` too.

```calx
fn index_of<T: Equal>(xs: List<T>, x: T) -> usize? {
    for i in 0usize..xs.len() {
        if xs[i] == x {
            return some(i)
        }
    }
    return none
}

fn labelled<T: Display>(label: str, x: T) -> String {
    return "{label}: {x}"
}

fn largest<T: Order>(xs: List<T>) -> usize {
    var best = 0usize
    for i in 1usize..xs.len() {
        if xs[i] > xs[best] {
            best = i
        }
    }
    return best
}

fn main() {
    let words: List<String> = [String("a"), String("b")]
    println("{index_of(words, String("b"))} {index_of(words, String("c"))}")    // out: some(1) none
    println(labelled("at", (1, 2)))    // out: at: (1, 2)
    println("{largest(words)}")    // out: 1
}
```

Without the bound, the error says which to add:

```calx
fn same<T>(a: T, b: T) -> bool {
    return a == b    // error: values of type `T` can't be compared with `==`; give `T` the bound `Equal`
}

fn main() {
    println("{same(1, 1)}")
}
```

Like `Copy`, `Equal`, `Order`, `Display` and `Hash` are properties: every type that can have one does, with nothing to write. As bounds, they mean the property, however the type has it. [`JavaHash`](java-maps.md#javahash), Java's `hashCode`, is one too, built into numbers, text and lists, which a struct or an enum writes for itself, as a Java class does. The names are reserved, as are `Ordering` and `Hasher`.

## Writing your own

A struct or an enum can write its own `Equal`, `Display`, `Hash` or `Order`, in place of the derived one, by implementing the interface of that name (D102), and its `JavaHash`, which has no derived one (D105). They're built in, so there's nothing to import:

| Interface | Function | What calls it |
| --- | --- | --- |
| `Equal` | `fn eq(self, other: Self) -> bool` | `==` and `!=`, `contains`, a map's or a set's keys |
| `Display` | `fn show(self)` | `{x}` in a string |
| `Hash` | `fn hash(self, h: mut Hasher)` | A map's or a set's keys |
| `Order` | `fn cmp(self, other: Self) -> Ordering` | `<`, `<=`, `>`, `>=`, `a.cmp(b)`, `sort`, the keys of a `SortedMap` or a `SortedSet` ([sorted maps](sorted-maps.md#keys)), and the elements of a `PriorityQueue` ([priority queues](priority-queues.md#the-order)) |
| `JavaHash` | `fn java_hash(self) -> i32` | `x.java_hash()`, the keys of a `JavaHashMap`, a `JavaHashSet`, an `OpenHashMap` or an `OpenHashSet` ([Java's hash maps](java-maps.md#javahash)) |

What holds the type uses its function too: a list of them shows each one its way, and a struct with one as a field compares that field with it.

```calx
struct BlockPos {
    x: i32,
    y: i32,
    z: i32,
}

impl Display for BlockPos {
    fn show(self) {
        print("{self.x}, {self.y}, {self.z}")
    }
}

// A player is the same player whatever its score.
struct Player {
    name: String,
    score: i32,
}

impl Equal for Player {
    fn eq(self, other: Self) -> bool {
        return self.name == other.name
    }
}

impl Hash for Player {
    fn hash(self, h: mut Hasher) {
        h.add(self.name)
    }
}

// Highest score first.
impl Order for Player {
    fn cmp(self, other: Self) -> Ordering {
        return other.score.cmp(self.score)
    }
}

struct Spawn {
    at: BlockPos,
    dimension: str,
}

fn main() {
    let p = BlockPos { x: 1, y: 64, z: -3 }
    println("{p}")    // out: 1, 64, -3
    println("{Spawn { at: p, dimension: "overworld" }}")    // out: Spawn { at: 1, 64, -3, dimension: overworld }

    var online = Set<Player>()
    online.insert(Player { name: String("Alex"), score: 3 })
    online.insert(Player { name: String("Alex"), score: 9 })
    println("{online.len()}")    // out: 1

    var board: List<Player> = [
        Player { name: String("a"), score: 1 },
        Player { name: String("b"), score: 5 },
        Player { name: String("c"), score: 1 },
        Player { name: String("d"), score: 5 },
    ]
    board.sort()
    var names = String()
    for q in board {
        names.push_str(q.name)
    }
    println(names)    // out: bdac
}
```

- **`show`** writes the value with `print`, which goes wherever the value is going: standard output, standard error for `eprint`, or the `String` an interpolation is building. `eprint` inside `show` still writes to standard error, and what comes after it goes back where it was going.
- **`h.add(x)`** adds a value with `Hash` to the hash, so a `hash` adds what its type's `eq` compares, field by field. The `Hasher` mixes them as a derived `Hash` does; nothing else can make one.
- **A type that writes its own `Equal` has no derived `Hash`.** That `Hash` would hash fields `eq` ignores, so two keys `eq` calls equal could land in different places, and a lookup would miss. To be a key, the type writes its own `Hash` too, adding only what `eq` compares. Writing `Hash` needs `Equal`, derived or written.
- **A type that writes its own `Order` is sorted stably**, as Java's `List.sort` is: elements `cmp` calls equal keep the order they were in, as `a` before `c` above. So is a type that holds one ([sorting](lists.md#sorting)). `cmp` should call two values `Equal` only when `==` does, and say `Less` one way exactly when it says `Greater` the other. A `sort` that finds a `cmp` breaking that, in a way that would lose an element, panics.
- **The functions are called only by what they're behind**, so there's one way to compare, show or hash a value, whatever its type: `a.eq(b)`, `p.show()` and `Equal.eq(a, b)` are errors that say what to write.
- **Only structs and enums write their own.** Numbers, strings, lists and the rest keep theirs. A generic type can: with `impl<T: Display> Display for Boxed<T>`, `Boxed<T>` shows its way for each `T` that has `Display`.

```calx
struct Pos {
    x: i32,
    z: i32,
}

impl Equal for Pos {
    fn eq(self, other: Self) -> bool {
        return self.x == other.x and self.z == other.z
    }
}

fn main() {
    let p = Pos { x: 1, z: 2 }
    println("{p.eq(p)}")    // error: `eq` is what `==` calls; compare with `a == b`
    let seen = Set<Pos>()    // error: `Pos` can't be a key of a map or a set, since it has no `Hash`, and `Pos` has an `Equal` of its own, so it needs a `Hash` of its own that agrees with it
    println("{seen.len()}")
}
```

## In C

A file that compares or prints a struct, an enum with data, a tuple, an array, an optional, a list or a table gets a `static inline` function for it, and for each such type it holds: `cx_eq_<type>(a, b)` and `cx_show_<type>(v)`. A key type gets `cx_hash_<type>(v)` the same way. Only the ones the file uses are made.

`cx_eq_` is the field comparisons, in order, joined with `&&`, so comparing two `Point`s is two integer comparisons once clang inlines it. `cx_show_` writes each piece straight to the output, or to the `String` being built, so showing a value builds no strings for its parts, and the text between two values is written in one go.

Ordering one of those types, or an enum whose values don't go up in the order its variants are declared, takes `cx_cmp_<type>(a, b)`, which gives -1, 0 or 1: the parts compared in turn until one differs. `a.cmp(b)` adds 1, so `Less`, `Equal` and `Greater` are 0, 1 and 2. Strings compare with `cx_str_cmp`, which compares the first 8 bytes as one big-endian integer, since most strings differ there, before calling `memcmp` on the rest.

A type that [writes its own](#writing-your-own) gets the same functions, which call what it wrote: `cx_eq_BlockPos(a, b)` returns `app__BlockPos__Equal__eq(&a, &b)`, and clang inlines one into the other. `cx_hash_` makes a `Hasher`, seeded as every hash is, hands it to `hash`, and gives back what it holds; `cx_cmp_` takes 1 from what `cmp` gives. They come after the file's function declarations, since they call one. A list of a type whose `Order` is written, or that holds one, is sorted by `calx_sort.h` with `CX_SORT_STABLE` ([sorting](lists.md#sorting)), which also takes the place of the `sort` call, for the panic.
