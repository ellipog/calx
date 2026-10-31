---
title: Getting started
description: The program the guide builds, running Calx with the calx command, values and variables, functions, loops, and the checker's errors.
---

# Getting started

This guide builds one small, real program: `stash`, which keeps count of the items a player puts away and takes out. It reads commands, one a line, and answers the way a game server's console might:

```text
add stone 64
add stone 66
take stone 5
take dirt 20
show
```

```text
line 4: there's no dirt
stone: 1 stack and 61
```

Each page adds to it, and the [last](stash.md) has the whole program. This page covers what it's made of: values, functions and loops. You'll need to know some programming already, in any language; the guide says when Calx works differently from what you might expect.

## The `calx` command

Calx compiles to C, which clang compiles, so you need clang. Until Calx has releases, you build the compiler from source, as [Install Calx](install.md) says. That gives you `calx`, the one command for checking, building and running programs.

Here's a whole program:

```calx
fn main() {
    println("Hello, Calx!")    // out: Hello, Calx!
}
```

Save it as `hello.calx`, and run it:

```sh
calx run hello.calx
```

- `calx run` checks the program, builds it in debug mode, and runs it. Building takes moments, and the next build only redoes what changed.
- `calx check hello.calx` checks it without building, which is what an editor does as you type ([editor support](../reference/editors.md)).
- `calx build --release hello.calx` builds an optimized executable, `hello` or `hello.exe`.

The `// out:` comments show what a line prints. They're also how the guide is tested: every example on these pages is compiled and run by Calx's test suite, which compares what it prints with the comments.

## Values and variables

```calx
fn main() {
    let item = "stone"
    var count = 64
    count += 32
    println("{count} {item}")    // out: 96 stone
    let stacks = count / 64
    let left = count % 64
    println("{stacks} stack and {left}")    // out: 1 stack and 32
}
```

- `let` names a value that doesn't change, and `var` one that does.
- A name in braces in a string is replaced by its value. Any expression can go in the braces, as in `"{count / 64}"`.
- `count` is an `i32`, a 32-bit integer, which is what a whole-number literal is unless something else is wanted. `/` on integers drops the remainder, and `%` gives it ([integers](../reference/integers.md)).

The compiler checks the program before it builds anything, and it's strict. Changing a `let` is an error:

```calx
fn main() {
    let count = 64
    count += 1    // error: `count` can't be changed; declare it with `var` to change it
    println("{count}")
}
```

So is a `var` that never changes, since a `let` would say so more plainly:

```calx
fn main() {
    var count = 64    // error: `count` is declared with `var` but never changed; declare it with `let`
    println("{count}")
}
```

The checker has more rules like these, such as no unused imports and no names hiding others. They're all on the [strictness](../reference/strictness.md) page. Each says what's wrong and how to fix it.

## Functions

The stash describes counts the way players do, in stacks of 64:

```calx
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

fn main() {
    for count in [5, 64, 128, 130] {
        println("{count}: {describe(count)}")
    }
    // out: 5: 5
    // out: 64: 1 stack
    // out: 128: 2 stacks
    // out: 130: 2 stacks and 2
}
```

- A function names its parameters' types, and its result's after `->`. It gives the result with `return`. Functions can come in any order in a file.
- `const` names a value fixed when the program compiles. A constant gives its type.
- `if` with an `else` gives a value, so it can go where a value does, as in `let stacks = ...`.
- `for` goes over a list, an array or a range: `for i in 0..10` counts from 0 to 9 ([control flow](../reference/control-flow.md)).

Calx has two kinds of text: `str`, fixed text such as a literal in the program, and `String`, text the program builds, which owns its memory. A string with braces in it builds a `String`, so `"{full} stacks"` is one. A plain literal such as `"1 stack"` is a `str`, unless a `String` is wanted where it's written. The two branches of an `if` must have the same type, so `let stacks: String` says a `String` is wanted, and `"1 stack"` is one too ([strings](../reference/strings.md)).

Next: [reading the commands](reading-input.md).
