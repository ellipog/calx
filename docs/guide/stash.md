---
title: The stash
description: The guide's last page - keeping counts in a sorted map, parameters that own or change what they're given, matching an enum's fields, passing errors up with try, the whole program, and splitting it into modules.
---

# The stash

The [last page](commands.md) turned a line into a `Command`. This one runs the commands against the stash, which finishes the program, then splits it into modules.

## Keeping count

The stash maps each item's name to how many there are. A `SortedMap` keeps its keys in order, so `show` lists the items alphabetically:

```calx
fn main() {
    var stash = SortedMap<String, i32>()
    stash.insert(String("stone"), 64)
    stash.insert(String("dirt"), 10)
    let have = stash.get("stone") ?? 0
    stash["stone"] = have + 66
    println("{stash.get("gold") ?? 0} gold")    // out: 0 gold
    for (item, count) in stash {
        println("{item}: {count}")
    }
    // out: dirt: 10
    // out: stone: 130
}
```

- `stash.get(key)` gives the value, an `i32?`, or `none` when the key isn't there. `a ?? b` is `a`'s value, or `b` if `a` is `none`, so a missing item counts as 0.
- `stash[key]` is the value itself, which can be assigned to. It panics if the key isn't there, so it's for keys the code knows are.
- Calx has other maps for other orders. `Map` is the fastest, for code that doesn't depend on the order of its keys, and there are maps in Java's orders for code that must match Java ([maps](../reference/maps.md), [sorted maps](../reference/sorted-maps.md)).

## Running a command

```calx
enum Command {
    Add { item: String, count: i32 }
    Take { item: String, count: i32 }
    Show
}

// Does what `command` says to `stash`, or says why it can't.
fn run(command: own Command, stash: mut SortedMap<String, i32>) ! String {
    match command {
        Add { item, count } => {
            let have = stash.get(item) ?? 0
            stash.insert(item, have + count)
        },
        Take { item, count } => {
            let have = stash.get(item) ?? 0
            if have == 0 {
                fail "there's no {item}"
            }
            if count > have {
                fail "there's only {have} {item}"
            }
            if count == have {
                _ = stash.remove(item)
            } else {
                stash[item] = have - count
            }
        },
        Show => {
            for (item, count) in stash {
                println("{item}: {count}")
            }
        },
    }
}

// Runs `command`, and says why if it can't.
fn attempt(command: own Command, stash: mut SortedMap<String, i32>) {
    if let err(message) = run(command, mut stash) {
        println(message)
    }
}

fn main() {
    var stash = SortedMap<String, i32>()
    attempt(Command.Add { item: String("stone"), count: 64 }, mut stash)
    attempt(Command.Take { item: String("stone"), count: 100 }, mut stash)    // out: there's only 64 stone
    attempt(Command.Take { item: String("stone"), count: 4 }, mut stash)
    attempt(Command.Show, mut stash)    // out: stone: 60
}
```

A parameter says what the function does with what it's given:

| Written | The function | The caller |
| --- | --- | --- |
| `line: String` | Only looks at it | Keeps it |
| `stash: mut SortedMap<String, i32>` | Can change it | Writes `mut stash` at the call, and sees the changes |
| `command: own Command` | Owns it, and frees it or moves it on | Gives it up: it can't use it after the call |

`run` owns the command so that `Add` can move the item's name into the map without copying it. Writing `mut` at the call means a change is never a surprise to the reader: a call without it never changes what it's given ([functions](../reference/functions.md)).

- `match command` picks the arm for the variant `command` is, and `Add { item, count }` binds its fields to names. A `match` must cover every variant, so a fourth command added to the enum makes the checker point at every `match` that doesn't handle it yet.
- `stash.remove(item)` gives back the value it removed, an `i32?`. Calx doesn't let a value be dropped without saying so, so `_ =` says it isn't needed.
- `if let err(message) = run(...)` runs its block only if `run` fails. `run` returns nothing when it succeeds, so there's nothing else to handle.
- `attempt` owns its command too, and hands it on to `run`. A `for` loop over a list only looks at each element, so it couldn't hand one to `run` without a copy.

## Passing errors up

Running a line takes two steps that can fail, `parse` and `run`. `try` passes either's error up to the caller, so the two make one function:

```calx
fn parse(line: String) -> i32 ! String {
    let n = i32.parse(line) else {
        fail "{line} isn't a number"
    }
    return n
}

fn run(n: i32, total: mut i32) ! String {
    if n < 0 {
        fail "can't take {-n}"
    }
    total += n
}

fn obey(line: String, total: mut i32) ! String {
    let n = try parse(line)
    try run(n, mut total)
}

fn main() {
    var total = 0
    let lines: List<String> = ["3", "lots", "-2", "5"]
    for line in lines {
        if let err(message) = obey(line, mut total) {
            println(message)
        }
    }
    println("{total}")
    // out: lots isn't a number
    // out: can't take 2
    // out: 8
}
```

`try parse(line)` is `parse`'s value when it succeeds. When it fails, `obey` fails at once with the same error, and the rest of it doesn't run.

## The whole program

Here it is, with the pieces from every page. It also says when the stash is empty, and describes counts in stacks:

```calx
import std.io.{Lines}

// Most items stack to 64.
const STACK: i32 = 64

enum Command {
    Add { item: String, count: i32 }
    Take { item: String, count: i32 }
    Show
}

// The words in `line`, which spaces separate.
fn words(line: String) -> List<String> {
    var out = List<String>()
    var start: usize = 0
    for i in 0..line.len() {
        if line.byte_at(i) == 32 {    // a space
            if i > start {
                out.push(line[start..i])
            }
            start = i + 1
        }
    }
    if start < line.len() {
        out.push(line[start..])
    }
    return out
}

// The command `line` gives, or what's wrong with it.
fn parse(line: String) -> Command ! String {
    let w = words(line)
    if w.len() == 1 and w[0] == "show" {
        return Command.Show
    }
    if w.len() != 3 {
        fail "write `add ITEM COUNT`, `take ITEM COUNT` or `show`"
    }
    let count = i32.parse(w[2]) else {
        fail "{w[2]} isn't a number"
    }
    if count < 1 {
        fail "{w[0]} needs a count of 1 or more"
    }
    let item = w[1].copy()
    return match w[0] {
        "add" => Command.Add { item, count },
        "take" => Command.Take { item, count },
        _ => fail "there's no command {w[0]}",
    }
}

// `count` in stacks: "2 stacks and 5".
fn describe(count: i32) -> String {
    let full = count / STACK
    let left = count % STACK
    if full == 0 {
        return "{left}"
    }
    let stacks: String = if full == 1 { "1 stack" } else { "{full} stacks" }
    if left == 0 {
        return stacks
    }
    return "{stacks} and {left}"
}

// Does what `command` says to `stash`, or says why it can't.
fn run(command: own Command, stash: mut SortedMap<String, i32>) ! String {
    match command {
        Add { item, count } => {
            let have = stash.get(item) ?? 0
            stash.insert(item, have + count)
        },
        Take { item, count } => {
            let have = stash.get(item) ?? 0
            if have == 0 {
                fail "there's no {item}"
            }
            if count > have {
                fail "there's only {have} {item}"
            }
            if count == have {
                _ = stash.remove(item)
            } else {
                stash[item] = have - count
            }
        },
        Show => {
            if stash.is_empty() {
                println("the stash is empty")
            }
            for (item, count) in stash {
                println("{item}: {describe(count)}")
            }
        },
    }
}

// Reads `line` as a command, and runs it.
fn obey(line: String, stash: mut SortedMap<String, i32>) ! String {
    let command = try parse(line)
    try run(command, mut stash)
}

fn main() {
    var stash = SortedMap<String, i32>()
    var input = Lines()
    var number = 0
    while let line = input.next() {
        number += 1
        if let err(message) = obey(line, mut stash) {
            println("line {number}: {message}")
        }
    }
}
// stdin: show
// stdin: add stone 64
// stdin: add stone 66
// stdin: add dirt 10
// stdin: take stone 5
// stdin: take dirt 20
// stdin: take gold 1
// stdin: dig diamond 1
// stdin: show
// stdin: take dirt 10
// stdin: show
// out: the stash is empty
// out: line 6: there's only 10 dirt
// out: line 7: there's no gold
// out: line 8: there's no command dig
// out: dirt: 10
// out: stone: 1 stack and 61
// out: stone: 1 stack and 61
```

`calx build --release stash.calx` makes it an executable of its own, with Calx's runtime built in, so there's nothing else to install to run it.

## Modules

A program this size is fine in one file, but a bigger one is split up. Each file is a module, and a module shares only what it marks `pub`. Here the commands move to `command.calx`:

```calx
// file: stash.calx
import command.{Command, parse}
import std.io.{Lines}

// Most items stack to 64.
const STACK: i32 = 64

// `count` in stacks: "2 stacks and 5".
fn describe(count: i32) -> String {
    let full = count / STACK
    let left = count % STACK
    if full == 0 {
        return "{left}"
    }
    let stacks: String = if full == 1 { "1 stack" } else { "{full} stacks" }
    if left == 0 {
        return stacks
    }
    return "{stacks} and {left}"
}

// Does what `command` says to `stash`, or says why it can't.
fn run(command: own Command, stash: mut SortedMap<String, i32>) ! String {
    match command {
        Add { item, count } => {
            let have = stash.get(item) ?? 0
            stash.insert(item, have + count)
        },
        Take { item, count } => {
            let have = stash.get(item) ?? 0
            if have == 0 {
                fail "there's no {item}"
            }
            if count > have {
                fail "there's only {have} {item}"
            }
            if count == have {
                _ = stash.remove(item)
            } else {
                stash[item] = have - count
            }
        },
        Show => {
            if stash.is_empty() {
                println("the stash is empty")
            }
            for (item, count) in stash {
                println("{item}: {describe(count)}")
            }
        },
    }
}

// Reads `line` as a command, and runs it.
fn obey(line: String, stash: mut SortedMap<String, i32>) ! String {
    let command = try parse(line)
    try run(command, mut stash)
}

fn main() {
    var stash = SortedMap<String, i32>()
    var input = Lines()
    var number = 0
    while let line = input.next() {
        number += 1
        if let err(message) = obey(line, mut stash) {
            println("line {number}: {message}")
        }
    }
}
// stdin: add stone 130
// stdin: take stone ten
// stdin: show
// out: line 2: ten isn't a number
// out: stone: 2 stacks and 2

// file: command.calx
pub enum Command {
    Add { item: String, count: i32 }
    Take { item: String, count: i32 }
    Show
}

// The words in `line`, which spaces separate.
fn words(line: String) -> List<String> {
    var out = List<String>()
    var start: usize = 0
    for i in 0..line.len() {
        if line.byte_at(i) == 32 {    // a space
            if i > start {
                out.push(line[start..i])
            }
            start = i + 1
        }
    }
    if start < line.len() {
        out.push(line[start..])
    }
    return out
}

// The command `line` gives, or what's wrong with it.
pub fn parse(line: String) -> Command ! String {
    let w = words(line)
    if w.len() == 1 and w[0] == "show" {
        return Command.Show
    }
    if w.len() != 3 {
        fail "write `add ITEM COUNT`, `take ITEM COUNT` or `show`"
    }
    let count = i32.parse(w[2]) else {
        fail "{w[2]} isn't a number"
    }
    if count < 1 {
        fail "{w[0]} needs a count of 1 or more"
    }
    let item = w[1].copy()
    return match w[0] {
        "add" => Command.Add { item, count },
        "take" => Command.Take { item, count },
        _ => fail "there's no command {w[0]}",
    }
}
```

- `import command.{Command, parse}` names what `stash.calx` uses from `command.calx`, which is found next to it. `words` isn't `pub`, so it stays `command`'s own.
- `calx run stash.calx` builds both. Each module compiles to a C file of its own, and only the ones that changed are compiled again, so builds stay fast as a program grows. A release build still inlines across modules, so splitting a program costs nothing when it runs ([modules](../reference/modules.md)).

## Where next

The stash used most of what everyday Calx code needs. The [reference](../README.md#reference) has a page for each feature, in reading order, and the [standard library](../README.md#standard-library) a page for each module. Some places to go from here:

- [Structs](../reference/structs.md) and [interfaces](../reference/interfaces.md), for types with methods of their own.
- [Generics](../reference/generics.md), for functions and types that work on any type.
- [Ownership](../reference/ownership.md), for exactly when values are moved, copied and freed.
- [Parallel loops](../reference/parallel.md), which run a loop on every core and give the same result with any number of threads.
- [Strictness](../reference/strictness.md), for every rule the checker enforces, and why.
