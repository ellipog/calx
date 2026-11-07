---
title: std.path
description: Paths as text - joined, taken apart into names, stems and extensions, normalized, and matched against globs such as src/**/*.calx, with Windows drives and shares.
---

# `std.path`

Paths as text: joined, taken apart, normalized and matched against glob patterns, without looking at the file system. [`std.fs`](fs.md) reads and changes what they name, and finds files by the same globs.

```calx
import std.path.{join_path, file_name, parent_dir, file_stem, file_extension, with_extension, normalize_path, glob_matches}

fn main() {
    let p = join_path("world/region", "r.0.0.mca")
    println(p)    // out: world/region/r.0.0.mca
    println("{file_name(p) ?? ""} {parent_dir(p) ?? ""} {file_stem(p) ?? ""} {file_extension(p) ?? ""}")    // out: r.0.0.mca world/region r.0.0 mca
    println(with_extension(p, "bak"))    // out: world/region/r.0.0.bak
    println(normalize_path("world/./region/../data/"))    // out: world/data
    println("{glob_matches(p, "world/**/*.mca")} {glob_matches(p, "*.mca")}")    // out: true false
}
```

## Items

| Item | Gives |
| --- | --- |
| `join_path(path: String, more: String) -> String` | `path` and then `more`, with a separator between, as Python's `os.path.join` and Rust's `Path::join` put them together. An absolute `more` is `more` alone |
| `parent_dir(path: String) -> String?` | The path without its last name: `logs` for `logs/report.txt`, `/` for `/srv`, and `.` for `report.txt`, the folder it's in. `none` with no name to take off, as for `/`, `.` and `""` |
| `file_name(path: String) -> String?` | The last name, as Rust's `Path::file_name`: `report.txt` for `logs/report.txt`, and `logs` for `logs/`. `none` with no name after the root, or if the last is `..` |
| `file_stem(path: String) -> String?` | The last name without its extension: `archive.tar` for `archive.tar.gz`, and `.bashrc` for `.bashrc` |
| `file_extension(path: String) -> String?` | The last name's extension, after its last dot, without the dot: `gz` for `archive.tar.gz`, and `""` for `notes.`. `none` with no dot, or only a first one, as `.bashrc` has |
| `with_extension(path: String, ext: String) -> String` | The path with the last name's extension made `ext`, or taken off for `""` |
| `with_file_name(path: String, name: String) -> String` | The path with its last name replaced by `name`, or `join_path(path, name)` with no name to replace |
| `path_parts(path: String) -> List<String>` | The root, if there is one, and then each name: `["/", "srv", "world"]` for `/srv//world/`. Without the `.` names, and the empty ones doubled separators make |
| `path_root(path: String) -> String` | The root as it's written: `/`, or on Windows `C:\`, `C:`, `\\server\share\` or a `\` alone, or `""` for a relative path |
| `is_absolute(path: String) -> bool` | Whether the path names the same thing from any folder: one that starts with `/`, or on Windows with `C:\` or `\\server\share` |
| `normalize_path(path: String) -> String` | The path without its `.` names and doubled separators, and with each `..` after a name taken out, as Python's `os.path.normpath`: `a/c` for `a/./b/../c/` |
| `glob_matches(path: String, pattern: String) -> bool` | Whether the path matches the glob, name by name. See [Globs](#globs) |
| `glob_matches_name(name: String, pattern: String) -> bool` | Whether one name matches one name of a glob, the test [`std.fs.glob`](fs.md#trees) puts each name in a folder to |

- The functions only read the text, so they never fail, and a path needn't exist. `..` is a name like any other to them: the parent of `logs/..` is `logs`, and `normalize_path` takes a `..` off with the name before it, which a link can make wrong, where [`std.fs.real_path`](fs.md) follows the link.
- `.` names and a separator at the end don't count as names, so `logs`, `logs/` and `logs/.` have the same name and the same parent.
- The parts they give are the text as it was written. Where they put a separator, it's `/`, or on Windows `\` if the path uses only that one, so a Windows path that came from the system stays as Windows writes it.
- Their names say what they work on, since Calx imports names as they are and doesn't allow shadowing, and a function named `name` or `parent` would take those names from every program that imports it.

```calx
import std.path.{path_parts, with_file_name, with_extension, normalize_path, is_absolute}

fn main() {
    println(path_parts("./logs/../x").join(" | "))    // out: logs | .. | x
    println(with_file_name("logs/old.txt", "new.txt"))    // out: logs/new.txt
    println("{with_extension("notes", "md")} {with_extension("notes.txt", "")}")    // out: notes.md notes
    println("{normalize_path("/../srv")} {normalize_path("../a/..")} {normalize_path("a/..")}")    // out: /srv .. .
    println("{is_absolute("logs")} {is_absolute("../logs")}")    // out: false false
}
```

## Globs

A glob matches a path name by name:

| Written | Matches |
| --- | --- |
| `*` | Any run of characters in a name, none too |
| `?` | Any one character |
| `[abc]`, `[a-z]` | Any one of those characters, or any in the range. A `]` first is one of them, and a `-` first or last is itself |
| `[!abc]`, `[^a-z]` | Any one character but those |
| `[*]` | A `*` itself, and so on for `?` and `[` |
| `**` | As a whole name, any number of names: none or more in the middle, as in `src/**/*.calx`, and one or more at the end, as in `logs/**` |

- A name that starts with `.` is matched only by a pattern name that starts with `.`, so `*` and `**` pass over hidden files and folders, as the shell's globs and Python's `glob` do: `src/*` doesn't find `src/.git`, and `src/.*` does.
- Letters match only in the same case, on Windows as well, so a pattern finds the same paths on every system. A Windows drive or share is the exception, matched without regard to case.
- Separators match separators, so on Windows `\` and `/` match each other. `.` names and a separator at the end count in neither the path nor the pattern.
- The roots must be the same: `/a/*` matches `/a/b` but not `a/b`.
- Matching never allocates. Each name is matched by moving through the pattern and backing up only to the last `*`, so a name costs time in proportion to its length times the pattern's, and only `**` tries more than one way through the names.

```calx
import std.path.{glob_matches, glob_matches_name}

fn main() {
    println("{glob_matches("src/a/b/main.calx", "src/**/*.calx")} {glob_matches("src/main.calx", "src/**/*.calx")}")    // out: true true
    println("{glob_matches("src/.git/x.calx", "src/**/*.calx")} {glob_matches("src/.h.calx", "src/.*")}")    // out: false true
    println("{glob_matches_name("r.0.0.mca", "r.*.mca")} {glob_matches_name("a5", "a[!0-9]")} {glob_matches_name("é", "?")}")    // out: true false true
}
```

## Windows paths

`/` separates names everywhere, and on Windows `\` does too, so a path written either way works there. A Windows path can also start with a root Linux doesn't have:

| Root | Is |
| --- | --- |
| `C:\` | A drive. `is_absolute` |
| `C:` | A drive alone, relative to that drive's own current folder: `C:x`. Not `is_absolute` |
| `\\server\share\` | A share on the network. `is_absolute` |
| `\` | The root of the current drive. Not `is_absolute` |

`join_path` keeps the drive or share of `path` for a `more` that starts with `\` alone, and gives `more` alone for one with a drive or a share, as Python's `ntpath.join` does. On Linux each of these is a name like any other, with `\` in it.

## How it works

`std.path` is written in Calx: [std/path.calx](../../std/path.calx). It works on the bytes of the text: separators, dots and wildcards are ASCII, so they're never part of another rune, and `?` and `[...]` take a whole rune.
