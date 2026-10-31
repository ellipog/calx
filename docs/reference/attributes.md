---
title: Attributes
description: Attributes a library defines, as Calx functions the compiler runs while it compiles. What they take and return, using them, attributes on fields, the code they generate, errors, the cache, and calx expand.
---

# Attributes

An attribute is `@name` or `@name(...)` before a declaration. The compiler knows three itself: [`@track_caller`](printing.md#panics-that-name-the-caller), [`@export`](libraries.md#building-a-library) and `@attribute`. Any other comes from a library, and is a Calx function that the compiler runs while it compiles (D124). The function reads the declaration it's on and gives back code to add beside it. For example, a network library's `@packet` can write a packet's reader and writer from its fields.

```calx
// file: main.calx
import show.{show}

@show
struct Point {
    x: i32
    y: i32
}

fn main() {
    let p = Point { x: 3, y: 4 }
    p.show()
    // out: x = 3
    // out: y = 4
}

// file: show.calx
import std.meta.{StructDecl, Generated}

/// A `show` method that prints each field.
@attribute
pub fn show(decl: StructDecl) -> Generated {
    var lines = String()
    for f in decl.fields {
        lines.push_str(#"        println("#{f.name} = {self.#{f.name}}")"#)
        lines.push('\n')
    }
    var g = Generated.new()
    g.add(#"
        impl #{decl.name} {
            pub fn show(self) {
        #{lines}    }
        }
        "#)
    return g
}
```

The code is text, so an attribute is ordinary Calx and needs no special API to build it. [Raw strings](syntax.md#raw-strings) keep the code's braces and quotes as they are, and `#{...}` fills in the parts. `calx expand main.calx` shows what `@show` wrote:

```text
// `@show` on `Point`, at main.calx:3, in C:/Users/you/AppData/Local/calx/cache/expand/c8d56f910be4a26b3b105e12a0f42795/main.Point.show.calx
impl Point {
    pub fn show(self) {
        println("x = {self.x}")
        println("y = {self.y}")
    }
}
```

## Writing one

An attribute is a `pub fn` marked `@attribute`:

- **Its first parameter is the declaration**, as [std.meta](../std/meta.md) describes it: a `StructDecl`, an `EnumDecl` or an `FnDecl`. That says what the attribute can go on.
- **Its other parameters are the arguments** a use gives it. Each is a `String`, an integer type, a `bool`, or an enum whose variants have no data.
- **It returns a `Generated`**: the code to add. One that can refuse a declaration returns `Generated ! String`, and fails with the reason, which is the error at the use.
- **Every parameter is looked at.** None is `mut` or `own`.
- **It's a function on its own.** It isn't generic, `unsafe`, in an `impl` or an interface, `@track_caller`, or `@export`.

```calx
// file: main.calx
import gen.{at_most}

@at_most(2)
struct Pair {
    a: i32
    b: i32
}

@at_most(2)    // error: `Triple` has 3 fields, and `@at_most` takes at most 2
struct Triple {
    a: i32
    b: i32
    c: i32
}

fn main() {}

// file: gen.calx
import std.meta.{StructDecl, Generated}

@attribute
pub fn at_most(decl: StructDecl, n: usize) -> Generated ! String {
    if decl.fields.len() > n {
        fail "`{decl.name}` has {decl.fields.len()} fields, and `@at_most` takes at most {n}"
    }
    return Generated.new()
}
```

The checker holds an attribute's code to the same rules as the rest, before anything runs it. One with errors stops the build, with its errors and an error at the first use: "`@x` can't run until the errors in the attributes' code are fixed".

## Using one

An attribute is imported like any function, and `@name` names it. It goes on a struct, enum or function at the top of a module, of the kind its first parameter reads.

```calx
// file: main.calx
import gen.{describe, Style}

@describe("p.", Style.Long, times: 2)
struct Point {
    x: i32
}

fn main() {
    let p = Point { x: 7 }
    p.describe()
    // out: p.x: i32 = 7
    // out: p.x: i32 = 7
}

// file: gen.calx
import std.meta.{StructDecl, Generated}

pub enum Style {
    Short
    Long
}

@attribute
pub fn describe(decl: StructDecl, prefix: String, style: Style, times: u8) -> Generated {
    var body = String()
    for f in decl.fields {
        var shown = "{prefix}{f.name}"
        if style == Style.Long {
            shown.push_str(": {f.ty.text}")
        }
        for _ in 0..times {
            body.push_str(#"        println("#{shown} = {self.#{f.name}}")"#)
            body.push('\n')
        }
    }
    var g = Generated.new()
    g.add(#"
        impl #{decl.name} {
            pub fn describe(self) {
        #{body}    }
        }
        "#)
    return g
}
```

- Arguments are given like a call's: by position, then by name. Each parameter needs one, and none is given twice.
- Each argument is a literal, since the attribute runs before the program does:

| Parameter | Argument |
| --- | --- |
| `String` | A string literal, without `{...}` parts |
| An integer type | An integer literal, which may be negative. It must fit the type, and a suffix must name the same type |
| `bool` | `true` or `false` |
| An enum | One of its variants, as `Enum.Variant`, with the enum named as the using module sees it |

- An attribute with no arguments can be written `@show` or `@show()`.
- An unknown name is an error: "there's no attribute `@x` here", which says attributes are imported like functions. So is a name that isn't an attribute, and one on the wrong kind of declaration: "`@show` is for a struct, since its first parameter is a `StructDecl`".

## Attributes on fields

An attribute can read attributes on the fields of the declaration it's on, including the fields of an enum's variants. It lists their names in its `@attribute(...)`, and they need no definition of their own:

```calx
// file: main.calx
import gen.{columns}

@columns
struct Player {
    @column("player_name")
    name: String
    @column("hp")
    health: i32
    level: i32
}

fn main() {
    println(player_columns())    // out: player_name, hp, level
}

// file: gen.calx
import std.meta.{StructDecl, Generated}

@attribute("column")
pub fn columns(decl: StructDecl) -> Generated {
    var names = String()
    for f in decl.fields {
        var name = f.name.copy()
        for a in f.attributes {
            if a.name == "column" {
                name = a.args[0][1..a.args[0].len() - 1]    // the literal, without its quotes
            }
        }
        if not names.is_empty() {
            names.push_str(", ")
        }
        names.push_str(name)
    }
    var g = Generated.new()
    g.add(#"
        fn #{decl.name.to_lower_root()}_columns() -> String {
            return "#{names}"
        }
        "#)
    return g
}
```

- Each argument of a field's attribute reaches the attribute as written, such as `255`, `"hp"` or `MAX_LEN`.
- An attribute on a field that the declaration's attribute doesn't read is an error: "`@packet` doesn't read `@max`; the attributes it reads on fields are `@varint`". So is one on a field of a declaration with no attribute.

## The code it generates

- **It's items**: functions, `impl`s, structs, enums, constants and imports, as Calx text. `Generated.add` takes one or more, and an attribute can call it any number of times.
- **They go in the declaration's module**, as if they were written at its end, so they see what the module sees and can use its private items. An import the module already has is left out. Generated code can't have a `module` line.
- **It's checked like the rest.** An error in it is at its line in the generated file, with a note at the attribute: "in the code `@x` generated for `Y`". Editors get the error at the attribute, with that said in front of the message.
- **An item it generates may go unused**, since an attribute can't know which of them a program wants. Code written by hand still may not ([strictness](strictness.md)).
- **It costs what the same code would cost written by hand**, since it's compiled like the rest.

## When it runs

Before the program is checked, the compiler finds the attributes' uses and builds the modules that define them, with the modules they import, into a program of their own. That program runs once for all the uses, as native code in a debug build, reads each declaration and gives back each answer. The compiler then parses the code into the using modules and checks the whole program.

- **An attribute can't be used in its own module**, nor in a module the attributes' code imports, since that module is needed before its attributes have run.
- **What an attribute prints goes to standard error**, after a line that names the use, to help whoever's writing it. Its standard output isn't the answer, so printing can't spoil it.
- **A panic stops the build**, with an error at the use: "`@x` stopped while it generated code for `Y`; it panicked: MESSAGE". A note follows for each frame of its trace, at its line in the attribute's code.
- **An attribute should depend only on what it's given**, since its answers are cached. It can read the declaration and its arguments, and nothing else is promised to stay the same.

### The cache

The answers are kept in the [cache](command.md#environment), under a hash of the compiler, every use with its declaration and arguments, and the text of the attributes' modules. A build of a program that hasn't changed them doesn't run them again. Each answer's code is in a file of its own there, named after the module, declaration and attribute, which is the file errors and traces in it point to.

| What changed (Windows, the example at the top) | `calx check` takes |
| --- | --- |
| Nothing | 48 ms |
| A declaration with an attribute on it | 101 ms: the attributes' program runs again, from the cache |
| The attributes' code, building it from a warm object cache | 0.24 s |

The cache keeps the expansions of the 256 programs used most recently, and deletes the rest.

## calx expand

`calx expand FILE` prints what every attribute in the program generated: a line naming the use and the file the code is in, then the code. It doesn't check the program, so it works while the generated code has errors, which is when it's most useful. A program with no attributes that generate code prints "// FILE uses no attributes that generate code".
