---
title: std.tar
description: Tar archives, plain or gzipped, read and written in memory - ustar, GNU long names and pax headers, links, modes, and extracting that no path can climb out of.
---

# `std.tar`

Tar archives, plain or gzipped, read from memory and written to it. It reads what GNU tar and bsdtar write: POSIX ustar, GNU's long names, and pax headers. Calx's updater reads its Linux releases with it.

```calx
import std.tar.{Kind, Tar, Writer}

fn main() {
    var w = Writer.new()
    w.add_dir("app")
    w.add("app/run", "#!/bin/sh".to_bytes(), mode: 0o755)
    w.add_symlink("app/latest", "run")
    let gz = w.finish_gz(6)

    let t = Tar.open(gz)!
    for e in t.entries() {
        println("{e.name} {e.kind} {e.size} link={e.link}")
    }
    // out: app Dir 0 link=
    // out: app/run File 9 link=
    // out: app/latest Symlink 0 link=run
    println(String.from_utf8(t.read_named("app/run")!) ?? "not UTF-8")    // out: #!/bin/sh
    println("{t.entries()[1].mode == 0o755} {t.entries()[2].kind == Kind.Symlink}")    // out: true true
}
```

## Reading

| Item | Gives |
| --- | --- |
| `Tar.open(bytes: own List<u8>) -> Tar ! String` | The archive, or why it can't be read. Gzipped bytes, which start `1f 8b`, are inflated first, so a `.tar.gz` opens the same way |
| `t.entries() -> List<Entry>` | Its entries, in order |
| `t.len() -> usize`, `t.name(i: usize) -> String` | How many there are, and the path of one |
| `t.find(name: String) -> usize?` | The index of the entry at `name`. A later entry of the same name wins, as it does when tar extracts |
| `t.read(i: usize) -> List<u8>` | What entry `i` holds. A hard link holds what the entry it names holds |
| `t.read_named(name: String) -> List<u8> ! String` | What the entry at `name` holds |
| `t.extract(to: String) ! String` | Writes every entry into the folder `to`, making folders on the way, with each file's mode |

`Entry` has `name: String`, without the `/` a directory's may end with; `kind: Kind`, one of `File`, `Dir`, `Symlink`, `Hardlink` and `Other`; `size: usize`; `mode: u32`, the permission bits; `link: String`, what a link points to; and `modified: Timestamp` ([std.time](time.md)), to the second.

- **Formats:** ustar, with its 155-byte prefix; GNU's `L` and `K` entries for long names and links; and pax `x` headers, whose `path`, `linkpath`, `size` and `mtime` win over the header's. Sizes in GNU's base-256 are read too. Each header's checksum is checked.
- **Extracting is safe from hostile archives.** Before writing anything, `extract` checks every path, and fails if one is absolute, names a drive, or climbs out of `to` with `..`. A symbolic link that's absolute or points out of `to` fails it too. `a/../b` is fine, and lands at `b`.
- **Links:** a symbolic link is made as one. Windows makes them only in developer mode or for an administrator, so there, as Python's `tarfile` does, `extract` writes a copy of the file the link points to. A hard link is written as a copy. `Other` entries, such as devices, are skipped.

## Writing

| Item | Does |
| --- | --- |
| `Writer.new() -> Writer` | An empty archive |
| `w.add(name: String, bytes: List<u8>, mode: u32 = 0o644)` | Adds a file with the permission bits `mode` |
| `w.add_dir(name: String)` | Adds a directory, with mode `0o755` |
| `w.add_symlink(name: String, target: String)` | Adds a symbolic link at `name` that points to `target` |
| `w.set_time(at: Timestamp)` | Dates the entries added after it |
| `w.finish() -> List<u8>` | The archive, ended with its two empty blocks |
| `w.finish_gz(level: u32) -> List<u8>` | The archive gzipped at `level`, from 1 to 9: a `.tar.gz` |

- **The same files make the same bytes.** Entries are dated 1970-01-01 and owned by user and group 0 until `set_time` says otherwise.
- **Long paths** of up to 255 bytes are split between ustar's prefix and name at a `/`. Longer ones, and ones with no `/` to split at, get a pax `path` header, as GNU tar and bsdtar write them, and so do files of 8 GiB or more.
- **Non-ASCII names get a pax `path` too.** A header's name field has no encoding, and bsdtar on Windows reads it in the console's code page. A pax path is always UTF-8.

`std.tar` is written in Calx over [std.zlib](zlib.md): [std/tar.calx](../../std/tar.calx). GNU tar and bsdtar read what it writes, and it reads what they write in the gnu, pax and ustar formats.
