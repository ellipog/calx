---
title: std.shell
description: Command lines split into words as a POSIX shell reads their quotes and escapes, and words quoted and joined into a command line, exactly as Python's shlex.split, quote and join do it.
---

# `std.shell`

Command lines split into their words, as a POSIX shell reads quotes and escapes and as Python's `shlex.split` gives them, and words quoted and joined into a command line a shell reads back, as `shlex.quote` and `shlex.join` write them. Nothing is expanded: `$HOME`, `*` and `~` stay as they are.

```calx
import std.shell.{join, quote, split}

fn main() ! Error {
    let words = try split(String("cp -r 'my photos' \"backup/2026 \\\"old\\\"\" ~/x\\ y"))
    println("{words}")    // out: [cp, -r, my photos, backup/2026 "old", ~/x y]
    println(quote(String("it's here")))    // out: 'it'"'"'s here'
    println(join(words))    // out: cp -r 'my photos' 'backup/2026 "old"' '~/x y'
    match split(String("echo 'not closed")) {
        ok(w) => println("{w}"),
        err(e) => println("{e}"),    // out: line 1, column 6: a `'` that isn't closed
    }
}
```

## Items

| Item | Gives |
| --- | --- |
| `split(text: String, comments: bool = false) -> List<String> ! ShellError` | The words in `text`. Python's `shlex.split(text, comments=...)` |
| `quote(word: String) -> String` | `word` as a shell reads back as itself: as it is if it's only ASCII letters, digits and `%+,-./:=@_`, and otherwise in `'...'`. Python's `shlex.quote` |
| `join(words: List<String>) -> String` | The words quoted and joined with spaces, a command line `split` splits back into the same words. Python's `shlex.join` |
| `ShellError` | Why text can't be split, with its `message`, and the `line` and `column` it's at, from 1 |

## How text splits

- **Between words** is white space: spaces, tabs, newlines and carriage returns. Other white space, such as the no-break space, is part of a word.
- **Outside quotes**, a `\` escapes whatever comes after it, white space and quotes too, so `x\ y` is one word, `x y`.
- **In `'...'`**, everything is itself, `\` too.
- **In `"..."`**, a `\` escapes only `"` and `\`. Before anything else it stays, so `"a\b"` is `a\b`.
- **Quoted parts** join the rest of their word, as in `--name="my file"`, and `''` and `""` are empty words.
- **With `comments`**, a `#` starts a comment that runs to the end of its line and ends the word it's in, so `a#b` is `a`. Without it, as `split(text)` has it, `#` is like any other rune.

Text can't be split, and `split` gives a `ShellError`, if a quote isn't closed or it ends with a `\` that has nothing after it to escape. Python raises `ValueError` there.

## With std.process

[std.process](process.md)'s `Command` takes a program and its arguments, each a word of its own, and runs it without a shell, so nothing in an argument needs quoting. `split` turns a command line, from a config file or a user, into those words:

```calx
import std.shell.{split}

fn main() ! Error {
    var words = try split(String("git log --format='%h %s' -n 3"))
    let program = words.remove(0)
    println("{program}: {words}")    // out: git: [log, --format=%h %s, -n, 3]
    // Command.new(program).args(words) runs it.
}
```

`quote` and `join` are for command lines a shell reads, such as `sh -c` and scripts you write, and for showing a command to someone.

## Checked against Python

`split`, with comments and without, and `quote` and `join` give the same as Python 3.14's `shlex` on 400,000 random cases, of quotes, escapes, `#`, every kind of white space, empty words, non-ASCII runes and NUL, with an error from `split` wherever Python raises one. What `join` writes always splits back into its words.

## Speed

On our benchmark, an i9-13900K with other work running, so rough, the best of five runs. The text is 100,000 command lines, 5.7 MB in 750,314 words, split one line at a time, and each line's words joined back. The comparisons are Python 3.14's `shlex`, and Rust 1.91 with the `shlex` crate 2.0.1, built for release with `x86-64-v3`.

| Work | Calx | Python | Rust |
| --- | --- | --- | --- |
| Each line split | 18.7 ms | 1,776 ms | 65.3 ms |
| Each line's words joined | 16.2 ms | 275 ms | 49.8 ms |

Rust's `shlex` reads some text the way a shell does where Python's doesn't: it always starts a comment at a `#` at the start of a word, it escapes `$` and `` ` `` in double quotes too, and a `\` before a newline joins two lines. The command lines have none of these, and all three split them into the same words. It quotes differently, so its joined lines are 51,294 bytes longer.
