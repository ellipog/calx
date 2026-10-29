---
title: What the commands mean
description: The guide's third page - commands as an enum, parsing numbers, functions that fail with an error, matching on text, and why a list's element is copied out.
---

# What the commands mean

The [last page](reading-input.md) split each line into words. This one works out which command the words are, or what's wrong with them.

## Commands as values

The stash has three commands, which an enum lists. Two of them carry data:

```calx
enum Command {
    Add { item: String, count: i32 }
    Take { item: String, count: i32 }
    Show
}

fn main() {
    let add = Command.Add { item: String("stone"), count: 64 }
    println("{add}")    // out: Add { item: stone, count: 64 }
    println("{Command.Show}")    // out: Show
}
```

A `Command` is exactly one of its variants, and only the variant it is has fields, so a `Show` can't be asked for its item by mistake. `match` reads the fields back out, which the [last page](stash.md) does. `{add}` shows the variant and its fields, which helps while a program is being built ([comparing and printing](../reference/comparing.md)).

## A function that can fail

Working out a command can fail: the line might name no command, or give `lots` for a count. A function that can fail says what it fails with, after `!`:

```calx
fn half(n: i32) -> i32 ! String {
    if n % 2 != 0 {
        fail "{n} is odd"
    }
    return n / 2
}

fn main() {
    match half(8) {
        ok(h) => println("half is {h}"),    // out: half is 4
        err(message) => println(message),
    }
    match half(3) {
        ok(h) => println("half is {h}"),
        err(message) => println(message),    // out: 3 is odd
    }
}
```

`fail` returns the error, here a `String`, instead of a value. Calx has no exceptions: the caller gets either `ok` with the value or `err` with the error, and must say what to do with each, right where it calls. Forgetting is an error:

```calx
fn half(n: i32) -> i32 ! String {
    if n % 2 != 0 {
        fail "{n} is odd"
    }
    return n / 2
}

fn main() {
    let h = half(3)    // error: `half` can fail, so handle its error here
    println("{h}")
}
```

So an error never slips past unseen. [Errors](../reference/errors.md) has the other ways to handle one, such as passing it up with `try`, which the stash uses on the next page.

## Working out a command

```calx
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

fn main() {
    let lines: List<String> = ["add stone 64", "take dirt 3", "show", "dig diamond 1", "add torch lots", "take stone 0", "add stone"]
    for line in lines {
        match parse(line) {
            ok(command) => println("{command}"),
            err(message) => println("{line}: {message}"),
        }
    }
    // out: Add { item: stone, count: 64 }
    // out: Take { item: dirt, count: 3 }
    // out: Show
    // out: dig diamond 1: there's no command dig
    // out: add torch lots: lots isn't a number
    // out: take stone 0: take needs a count of 1 or more
    // out: add stone: write `add ITEM COUNT`, `take ITEM COUNT` or `show`
}
```

- `and`, `or` and `not` are the logical operators, written as words. `and` checks its right side only when the left is true, so `w[0]` is only read when there's a word.
- `i32.parse(w[2])` gives an `i32?`: the number, or `none` if the text isn't one. `let count = ... else { }` takes the number out, or runs the `else`, which must leave the function, here with `fail`.
- `match` on text picks the arm whose literal is the same text. Any text could come in, so a `_` arm, which matches anything, is needed for the rest. Each arm gives a value, the `Command`, unless it leaves with `fail`.
- `Command.Add { item, count }` is short for `Command.Add { item: item, count: count }`.
- `lines` is a `List<String>`, so its literals are `String`s, which `parse` takes. Looping over an array of literals, `for line in ["show"]`, would give each as a `str`.

## Why `.copy()`

`w[1].copy()` makes a copy of the item's name. Taking the name itself is an error:

```calx
fn main() {
    let w: List<String> = ["add", "stone", "64"]
    let item = w[1]    // error: can't move an element out of `w`
    println(item)
}
```

Every value in Calx has one owner, which frees it when it's done, so a program never frees memory by hand and never frees it twice. The list owns its strings. `let item = w[1]` would move the string out to `item`, leaving a hole in the list, so it isn't allowed; a copy gives `item` a string of its own. Numbers are copied whenever they're used, so `count` needed nothing. [Ownership](../reference/ownership.md) has the rules.

Next: [the stash itself](stash.md).
