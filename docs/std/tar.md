---
title: std.tar
description: Tar archives, plain, gzipped, bzip2'd, xz'd or zstd'd, read and written in memory, or an entry at a time to and from files, sockets and zstd streams - ustar, GNU long names and pax headers, links, modes, and extracting that no path can climb out of.
---

# `std.tar`

Tar archives, plain, gzipped, bzip2'd, xz'd or zstd'd, read from memory and written to it, or [a piece at a time](#a-piece-at-a-time), an entry at a time to and from any [`std.io`](io.md) `Writer` or `Reader`: a file, a socket, or a [`std.zstd`](zstd.md#a-piece-at-a-time) stream, for folders bigger than memory. It reads what GNU tar and bsdtar write: POSIX ustar, GNU's long names, and pax headers. Calx's updater reads its Linux releases with it.

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
| `Tar.open(bytes: own List<u8>) -> Tar ! String` | The archive, or why it can't be read. Gzipped bytes, which start `1f 8b`, are inflated first, bzip2'd bytes, which start `BZh` and a digit, are decompressed with [`std.bz2`](bz2.md), xz'd ones with [`std.xz`](xz.md), and zstd'd ones with [`std.zstd`](zstd.md), so a `.tar.gz`, a `.tar.bz2` or `.tbz2`, a `.tar.xz` or a `.tar.zst` opens the same way |
| `t.entries() -> List<Entry>` | Its entries, in order |
| `t.len() -> usize`, `t.name(i: usize) -> String` | How many there are, and the path of one |
| `t.find(name: String) -> usize?` | The index of the entry at `name`. A later entry of the same name wins, as it does when tar extracts |
| `t.read(i: usize) -> List<u8>` | What entry `i` holds. A hard link holds what the entry it names holds |
| `t.read_named(name: String) -> List<u8> ! String` | What the entry at `name` holds |
| `t.extract(to: String) ! String` | Writes every entry into the folder `to`, making folders on the way, with each file's mode |

`Entry` has `name: String`, without the `/` a directory's may end with; `kind: Kind`, one of `File`, `Dir`, `Symlink`, `Hardlink` and `Other`; `size: usize`; `mode: u32`, the permission bits; `link: String`, what a link points to; and `modified: Timestamp` ([std.time](time.md)), to the second.

- **Formats:** ustar, with its 155-byte prefix; GNU's `L` and `K` entries for long names and links; and pax `x` headers, whose `path`, `linkpath`, `size` and `mtime` win over the header's. Sizes in GNU's base-256 are read too. Each header's checksum is checked.
- **Extracting is safe from hostile archives.** Before writing anything, `extract` checks every path, and fails if one is absolute, names a drive, or climbs out of `to` with `..`. A symbolic link that's absolute, names a drive, or points out of `to` fails it too. `a/../b` is fine, and lands at `b`.
- **Nothing goes through a link the archive made.** A path through one, such as `d/x` after a link `d`, fails, and so does a link whose target climbs back out of one with `..`, such as `out -> up/../x` after `up -> .`: the system would follow the link, and land outside `to`, though the path itself stays in. bsdtar refuses the same.
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
| `w.finish_bz2(level: u32 = 9) -> List<u8>` | The archive compressed by [`std.bz2`](bz2.md) at `level`, from 1 to 9, into the bytes `bzip2 -1` to `bzip2 -9` make of it: a `.tar.bz2` |
| `w.finish_xz(preset: u32 = 6) -> List<u8>` | The archive compressed by [`std.xz`](xz.md) at `preset`, from 0 to 9, as `xz -0` to `xz -9` do: a `.tar.xz` |
| `w.finish_zst(level: i32 = 3) -> List<u8>` | The archive compressed by [`std.zstd`](zstd.md) at `level`, from 1 to 22 as `zstd -1` to `zstd --ultra -22` do, or negative for faster: a `.tar.zst` |

- **The same files make the same bytes.** Entries are dated 1970-01-01 and owned by user and group 0 until `set_time` says otherwise. `finish_gz`, `finish_bz2`, `finish_xz` and `finish_zst` give the same bytes on any machine, however many processors it has.
- **Long paths** of up to 255 bytes are split between ustar's prefix and name at a `/`. Longer ones, and ones with no `/` to split at, get a pax `path` header, as GNU tar and bsdtar write them, and so do files of 8 GiB or more.
- **Non-ASCII names get a pax `path` too.** A header's name field has no encoding, and bsdtar on Windows reads it in the console's code page. A pax path is always UTF-8.

## A piece at a time

`StreamWriter` writes each entry to a [`std.io`](io.md) `Writer` as it's added, and `StreamReader` reads entries one at a time from a `Reader`, as Python's `tarfile.open(fileobj=f, mode="w|")` and `"r|"` do. With a [`std.zstd`](zstd.md#a-piece-at-a-time) `Writer` or `Reader` between, that's a `.tar.zst` of folders bigger than memory, to or from a file, or over a connection with [`std.net`](net.md)'s `Stream`, as a server moves to another: an archive of 5 GiB went through in 24 MiB of memory.

```calx
import std.fs.{create, make_dir_all, open_read, read_text, remove_dir_all, temp_dir, write_text}
import std.tar.{StreamReader, StreamWriter}
import std.zstd

fn main() {
    let dir = temp_dir()!
    make_dir_all("{dir}/site/pages")!
    write_text("{dir}/site/index.html", "<h1>hi</h1>")!
    write_text("{dir}/site/pages/about.html", "about")!

    // A folder and a file made on the way, into a .tar.zst, each written as it's added.
    var w = StreamWriter.new(zstd.Writer.new(create("{dir}/backup.tar.zst")!))
    w.add_tree("site", "{dir}/site")!
    w.add("snapshot.db", "rows".to_bytes())!
    w.finish()!.finish()!.close()!

    // Back an entry at a time; what isn't read is skipped.
    var r = StreamReader.new(zstd.Reader.new(open_read("{dir}/backup.tar.zst")!))
    while let e = r.next()! {
        if e.name == "snapshot.db" {
            println("{e.name}: {String.from_utf8(r.read_all()!)!}")
        } else {
            println("{e.name} {e.kind}")
        }
    }
    // out: site Dir
    // out: site/index.html File
    // out: site/pages Dir
    // out: site/pages/about.html File
    // out: snapshot.db: rows
    r.finish()!.into_inner().close()!

    // Or extracted as it comes.
    var x = StreamReader.new(zstd.Reader.new(open_read("{dir}/backup.tar.zst")!))
    x.extract("{dir}/restored")!
    println(read_text("{dir}/restored/site/pages/about.html")!!)    // out: about
    remove_dir_all(dir)!
}
```

| Item | Does |
| --- | --- |
| `StreamWriter.new(out: own W) -> StreamWriter<W>` | An archive written onto `out`, any `Writer`, dated 1970-01-01 as `Writer`'s is |
| `w.add(name: String, bytes: List<u8>, mode: u32 = 0o644) ! String` | Adds a file, as `Writer.add` does, and writes it to `out` |
| `w.add_dir(name: String) ! String`, `w.add_symlink(name: String, target: String) ! String` | Adds a directory or a symbolic link |
| `w.add_from(name: String, size: u64, input: mut R, mode: u32 = 0o644) ! String` | Adds a file of `size` bytes read from `input`, any `Reader`, a piece at a time, as `addfile(info, fileobj)` does. Fails if `input` ends first |
| `w.add_file(name: String, path: String) ! String` | Adds the file at `path` as `name`, with its permission bits and modified time, read a piece at a time, as `add(path, arcname=name)` does |
| `w.add_tree(name: String, path: String) ! String` | Adds the folder at `path` as `name` and everything in it, each folder before what's in it and the names in each in order of their bytes, so the same folder makes the same archive. Links go in as links, not followed; devices, pipes and sockets are passed over. With an empty `name`, what's in the folder goes at the archive's top |
| `w.set_time(at: Timestamp)` | Dates the entries `add`, `add_dir`, `add_symlink` and `add_from` add after it |
| `w.finish() -> W ! String` | Ends the archive with its two empty blocks, flushes `out` and gives it back |
| `StreamReader.new(input: own R) -> StreamReader<R>` | A reader of the archive in `input`, any `Reader`, plain: a compressed one goes through its decompressor's `Reader` first |
| `r.next() -> Entry? ! String` | The next entry, after what's left of the one before, or `none` at the archive's end: its empty blocks, or the end of the input |
| `r.read(buf: mut List<u8>, n: usize) -> usize ! String` | Up to `n` of the entry's bytes onto the end of `buf`, 0 once they've all been read: a `StreamReader` is a `Reader` of the entry it's at |
| `r.read_all() -> List<u8> ! String` | The rest of the entry's bytes |
| `r.extract(to: String) ! String` | Writes each entry from here on into `to` as it comes, as `Tar.extract` does |
| `r.finish() -> R ! String` | Reads the rest of the input after the archive's end, which must be zeros, so a compressed stream it's in is read to its end and its checksum checked, and gives `input` back |
| `r.into_inner() -> R` | Gives `input` back, with what it hasn't read of it, which may be past where it stopped, as it reads ahead |

- **What `Writer` writes.** The same entries through `StreamWriter` and `Writer` make the same bytes, and `Tar.open` and Python's `tarfile` read them.
- **Memory.** A file from the disk or a `Reader` goes 1 MiB at a time, and a `StreamReader` reads 1 MiB of its input ahead at most, and a long name or a pax header whole, up to 8 MiB.
- **Extracting as it comes** checks each entry as `Tar.extract` does, but can't look ahead: an entry that fails stops it after those before it are written. A hard link is a copy of a file before it in the archive, and fails if there's none; a link Windows won't make becomes a copy of what it points to at the end, once everything it could point to is there.
- Python's `tarfile` pads a stream to a record of 10 KiB, and GNU tar does too; `StreamWriter` doesn't, which nothing needs, and `finish` reads that padding.

`std.tar` is written in Calx over [std.zlib](zlib.md), [std.bz2](bz2.md), [std.xz](xz.md) and [std.zstd](zstd.md): [std/tar.calx](../../std/tar.calx). GNU tar and bsdtar read what it writes, and it reads what they write in the gnu, pax and ustar formats. Python's `tarfile`, in its stream modes `"w|"` and `"r|"` over zstandard's `stream_writer` and `stream_reader`, reads the `.tar.zst` streams it writes, and it reads theirs: 80 MB of folders, long names, a non-ASCII name and a link, both ways, with every file's SHA-256 the same.
