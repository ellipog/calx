---
title: std.fs
description: Files and directories - whole files read and written, parts written in place, atomic renames, locks, sizes and modified times, directories made, listed and removed, real paths, and temporary directories.
---

# `std.fs`

Files and directories. A file is read or written whole, or in parts in place, and a program replaces one safely by writing another and renaming it over the first. A lock on a file marks something as in use, so two programs don't both change it.

```calx
import std.fs.{read_text, write, rename, make_dir_all, list, remove_dir_all, temp_dir}

fn main() {
    let dir = temp_dir()!
    make_dir_all("{dir}/world/region")!
    let text = "motd=A Calx server\n"
    write("{dir}/server.properties.tmp", text.to_bytes())!
    rename("{dir}/server.properties.tmp", "{dir}/server.properties")!
    print(read_text("{dir}/server.properties")! ?? String("not UTF-8"))    // out: motd=A Calx server
    for name in list(dir)! {
        println(name)
    }
    // out: server.properties
    // out: world
    remove_dir_all(dir)!
}
```

## Items

| Item | Gives |
| --- | --- |
| `read(path: String) -> List<u8> ! FsError` | The whole file |
| `read_part(path: String, from: u64, n: usize) -> List<u8> ! FsError` | The `n` bytes from byte `from`, or fewer where the file ends first, without reading the rest of the file |
| `read_parts(path: String, ranges: List<(u64, usize)>) -> List<List<u8>> ! FsError` | What `read_part` gives for each range, an offset and a length, in order, from one opening of the file |
| `read_text(path: String) -> String? ! FsError` | The whole file as text, or `none` if it isn't UTF-8 |
| `write(path: String, bytes: List<u8>) ! FsError` | Makes the file hold `bytes`, creating it or replacing what it held |
| `write_parts(path: String, parts: List<(u64, List<u8>)>, sync: bool) ! FsError` | Writes each part's bytes at its offset, in order, creating the file if nothing's there and keeping the rest of what it holds. Past the end, the file grows, with zeros in a gap. With `sync`, it returns once the system says the bytes are on the disk, as Java's `FileChannel.force(true)` |
| `append(path: String, bytes: List<u8>) ! FsError` | Adds `bytes` to the end of the file, creating it if nothing's there, after everything it held, even what another program added |
| `metadata(path: String) -> Metadata ! FsError` | The size and modified time of what's at the path, following links, as Java's `Files.size` and `Files.getLastModifiedTime` read them |
| `set_modified(path: String, at: Timestamp) ! FsError` | Sets when what's at the path was last modified, following links, as Java's `File.setLastModified` does, without changing what it holds |
| `rename(from: String, to: String) ! FsError` | Moves what's at `from` to `to`, replacing a file there. Atomic on one volume |
| `remove_file(path: String) ! FsError` | Removes a file, or a link but not what it leads to |
| `make_dir(path: String) ! FsError` | Makes a directory. `Exists` if something's there |
| `make_dir_all(path: String) ! FsError` | Makes a directory and each one missing on the way to it. Fine if it's there already |
| `remove_dir(path: String) ! FsError` | Removes an empty directory |
| `remove_dir_all(path: String) ! FsError` | Removes a directory and everything in it, never following a link out of it |
| `list(path: String) -> List<String> ! FsError` | The names in a directory, without `.` and `..`, sorted by their bytes |
| `kind(path: String) -> Kind? ! FsError` | What's at the path, without following a link, or `none` if nothing is |
| `is_file(path: String) -> bool`, `is_dir(path: String) -> bool` | Whether the path is a file or a directory, following links. An error is `false`, as with Java's `Files.isRegularFile` and `Files.isDirectory` |
| `real_path(path: String) -> String ! FsError` | The absolute path of what's at `path`, with every link followed and each `.` and `..` resolved, as Java's `Path.toRealPath` gives it. `NotFound` if nothing is there |
| `try_lock(path: String, bytes: List<u8>) -> FileLock? ! FsError` | Opens the file to write, creating it if nothing's there and keeping what it holds, writes `bytes` at its start and syncs them, then takes an exclusive lock on the whole file without waiting, as Java's `FileChannel.tryLock`. `none` if another lock is held on it, by this program or another |
| `separator() -> String` | What the system puts between a path's names, as Java's `File.separator`: `\` on Windows and `/` elsewhere, as in the paths `real_path` and `temp_dir` give. Either works in the paths `std.fs` is given |
| `temp_dir() -> String ! FsError` | A new, empty directory of the program's own in the system's temporary folder, for it to remove |
| `Metadata` | `size: u64`, the length in bytes, which for a directory is whatever the system says, and `modified: Timestamp`, when what it holds last changed, to the millisecond. Writing or appending to a file sets it |
| `FileLock` | A lock `try_lock` took, held until its `release(own self) ! FsError`, which lets it go and closes the file, or until the program ends |
| `Kind` | `File`, `Dir`, `Link` (a symbolic link, or a junction on Windows) or `Other`, such as a device |
| `FsError` | `NotFound`, `Denied`, `Exists`, `BadPath`, `IsDirectory`, `NotEmpty`, or `Os(i32)` with the system's error code |

## Writing in place

`write_parts` changes parts of a file without writing the rest, as a region file's chunks are saved: each chunk into sectors nothing else uses, then the header that points at them. Syncing between the two means a crash leaves either the old header, pointing at the old chunks, or the new one, pointing at chunks that are all on the disk.

```calx
import std.fs.{read, read_parts, write_parts, remove_dir_all, temp_dir}

fn main() {
    let dir = temp_dir()!
    let path = "{dir}/r.0.0.mca"
    let chunk: List<(u64, List<u8>)> = [(8, [0, 0, 0, 2, 2])]
    write_parts(path, chunk, true)!
    let header: List<(u64, List<u8>)> = [(0, [0, 0, 2, 1])]
    write_parts(path, header, true)!
    println("{read(path)!.len()}")    // out: 13
    let ranges: List<(u64, usize)> = [(0, 4), (8, 5)]
    for part in read_parts(path, ranges)! {
        println("{part}")
    }
    // out: [0, 0, 2, 1]
    // out: [0, 0, 0, 2, 2]
    remove_dir_all(dir)!
}
```

There's no handle to a file left open, apart from a lock's: each call opens the file, does all it was asked, and closes it, so nothing a program forgets can stay open. A batch of parts costs one opening, so a program writes a region's chunks together, not one call each.

## Locking

A program marks a folder as in use by locking a file in it, as a Minecraft server locks its world's `session.lock`, so a second server on the same world stops instead of both writing it. `try_lock` does just what Minecraft's `DirectoryLock` does with Java's `FileChannel`: it writes `☃` at the start, syncs it, and takes the lock. So Minecraft and a Calx program refuse each other's lock, on Windows and Linux.

```calx
import std.fs.{try_lock, remove_dir_all, temp_dir}

fn main() {
    let dir = temp_dir()!
    let path = "{dir}/session.lock"
    let snowman: List<u8> = [226, 152, 131]
    let lock = try_lock(path, snowman)! else panic("in use")
    match try_lock(path, snowman)! {
        some(_) => println("locked twice"),
        none => println("in use"),    // out: in use
    }
    lock.release()!
    remove_dir_all(dir)!
}
```

On Windows the lock is `LockFileEx`'s, which every program's reads and writes of the file respect, so another program can't even write the file while it's held, and `try_lock` gives `none` for that too. On Linux it's an open file description lock, which conflicts with the `fcntl` locks Java and most programs take, and which only programs that lock the file respect. Unlike an `fcntl` lock, it isn't let go when the program closes another handle to the file.

A lock is held until `release`, and the system lets it go when the program ends, however it ends.

## Paths

A path is a `String`, relative to the directory the program runs in unless it starts at a root, such as `/srv` or `C:\srv`. `/` separates directories on both systems, and `\` does too on Windows. Windows converts the path to UTF-16, so a name in any script works on both.

A path with a NUL byte fails with `BadPath`, since the system would read only the part before it. Windows paths are limited to 260 UTF-16 units for now.

Names from `list` are UTF-8. On Linux, a name is bytes, and each part that isn't UTF-8 becomes U+FFFD. On Windows, an unpaired surrogate in a name becomes U+FFFD. `list` sorts the names, since each system lists them in its own order, and a program's output shouldn't depend on which it runs on.

## Errors

The same failure is the same `FsError` on both systems:

```calx
import std.fs.{FsError, read, make_dir, write, remove_dir_all, temp_dir}

fn describe(e: FsError) -> String {
    return match e {
        NotFound => String("not found"),
        Denied => String("denied"),
        Exists => String("exists"),
        BadPath => String("bad path"),
        IsDirectory => String("is a directory"),
        NotEmpty => String("not empty"),
        Os(code) => "system error {code}",
    }
}

fn main() {
    let dir = temp_dir()!
    let errors: [String; 3] = ["{dir}/missing.dat", dir.copy(), "{dir}/a\0b"]
    for path in errors {
        match read(path) {
            ok(bytes) => println("{bytes.len()} bytes"),
            err(e) => println(describe(e)),
        }
    }
    // out: not found
    // out: is a directory
    // out: bad path
    write("{dir}/x", [1, 2, 3])!
    match make_dir("{dir}/x") {
        ok => println("made"),
        err(e) => println(describe(e)),    // out: exists
    }
    remove_dir_all(dir)!
}
```

`Denied` is a permission the system refuses, such as writing a read-only file, and `Os` is anything else, such as a file another program holds open on Windows.

## Saving safely

`write` replaces a file's bytes in place, so a crash part way through leaves it half written. To replace a file that must never be seen broken, such as a world's `level.dat`, write the new bytes to a file beside it and `rename` that over it. Readers then see the old file or the new one, whole.

`write` doesn't wait for the bytes to reach the disk, as Java's `Files.write` doesn't. A power cut soon after can lose them, though not leave them half renamed.

## How it works

| Platform | Source |
| --- | --- |
| Windows | `CreateFileW`, `ReadFile` and `WriteFile`; `LockFileEx`; `MoveFileExW` with `MOVEFILE_REPLACE_EXISTING`; `FindFirstFileExW`; `GetFinalPathNameByHandleW`, without the `\\?\` it puts first; `GetTempPathW` |
| Linux | `open`, `read` and `write`; `fcntl` with `F_OFD_SETLK`; `rename`; `opendir`; `realpath`; `mkdtemp` in `TMPDIR` or `/tmp` |

`read` asks for the file's size and reads it whole in one call, so the list never grows, and a second call sees the end. A file can still grow as it's read, and `read` keeps going until the end.

A program that doesn't import `std.fs` doesn't link it.

`std.fs` is written in Calx: [std/fs.calx](../../std/fs.calx). The calls to the system are in the runtime, [runtime/calx_fs.c](../../runtime/calx_fs.c).
