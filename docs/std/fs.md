---
title: std.fs
description: Files and directories - whole files read and written, atomic renames, directories made, listed and removed, real paths, and temporary directories.
---

# `std.fs`

Files and directories. A file is read or written whole, and a program replaces one safely by writing another and renaming it over the first.

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
| `read_text(path: String) -> String? ! FsError` | The whole file as text, or `none` if it isn't UTF-8 |
| `write(path: String, bytes: List<u8>) ! FsError` | Makes the file hold `bytes`, creating it or replacing what it held |
| `append(path: String, bytes: List<u8>) ! FsError` | Adds `bytes` to the end of the file, creating it if nothing's there, after everything it held, even what another program added |
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
| `temp_dir() -> String ! FsError` | A new, empty directory of the program's own in the system's temporary folder, for it to remove |
| `Kind` | `File`, `Dir`, `Link` (a symbolic link, or a junction on Windows) or `Other`, such as a device |
| `FsError` | `NotFound`, `Denied`, `Exists`, `BadPath`, `IsDirectory`, `NotEmpty`, or `Os(i32)` with the system's error code |

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
| Windows | `CreateFileW`, `ReadFile` and `WriteFile`; `MoveFileExW` with `MOVEFILE_REPLACE_EXISTING`; `FindFirstFileExW`; `GetFinalPathNameByHandleW`, without the `\\?\` it puts first; `GetTempPathW` |
| Linux | `open`, `read` and `write`; `rename`; `opendir`; `realpath`; `mkdtemp` in `TMPDIR` or `/tmp` |

`read` asks for the file's size and reads it whole in one call, so the list never grows, and a second call sees the end. A file can still grow as it's read, and `read` keeps going until the end.

A program that doesn't import `std.fs` doesn't link it.

`std.fs` is written in Calx: [std/fs.calx](../../std/fs.calx). The calls to the system are in the runtime, [runtime/calx_fs.c](../../runtime/calx_fs.c).

Opening a file to read or write parts of it in place, as region files need, comes with Calcite's Phase 5.
