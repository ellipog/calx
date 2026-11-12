---
title: std.fs
description: Files and directories - read and written whole or in place, copies, atomic renames, locks, links, times and modes, directories listed, walked, globbed, copied and removed, the current folder, and temporary directories.
---

# `std.fs`

Files and directories. A file is read or written whole, or in parts in place, and a program replaces one safely by writing another and renaming it over the first. A lock on a file marks something as in use, so two programs don't both change it. A script walks a tree, finds files by a glob such as `src/**/*.calx`, and copies files and whole trees. [`std.path`](path.md) takes paths apart as text.

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
| `write_text(path: String, text: String) ! FsError` | Makes the file hold `text`, as UTF-8, as `write` does, without copying it first |
| `write_parts(path: String, parts: List<(u64, List<u8>)>, sync: bool) ! FsError` | Writes each part's bytes at its offset, in order, creating the file if nothing's there and keeping the rest of what it holds. Past the end, the file grows, with zeros in a gap. With `sync`, it returns once the system says the bytes are on the disk, as Java's `FileChannel.force(true)` |
| `append(path: String, bytes: List<u8>) ! FsError` | Adds `bytes` to the end of the file, creating it if nothing's there, after everything it held, even what another program added |
| `append_text(path: String, text: String) ! FsError` | Adds `text` to the end of the file, as UTF-8, as `append` adds bytes |
| `copy(from: String, to: String) ! FsError` | Copies a file, replacing one at `to`, keeping its permission bits and modified time, as Python's `shutil.copy2`. A file copied onto itself stays as it is, and a directory is `IsDirectory`. See [Trees](#trees) |
| `metadata(path: String) -> Metadata ! FsError` | The size, modified time, permission bits and kind of what's at the path, following links, as Java's `Files.size` and `Files.getLastModifiedTime` read them |
| `set_modified(path: String, at: Timestamp) ! FsError` | Sets when what's at the path was last modified, following links, as Java's `File.setLastModified` does, without changing what it holds |
| `set_mode(path: String, mode: u32) ! FsError` | Sets the permission bits of what's at the path, following links, as `chmod` does: `0o755` for a program anyone may run, `0o644` for a file only its owner may change. On Windows only the owner's write bit, `0o200`, counts: without it the file is read-only |
| `rename(from: String, to: String) ! FsError` | Moves what's at `from` to `to`, replacing a file there. Atomic on one volume |
| `rename_synced(from: String, to: String) ! FsError` | Moves what's at `from` to `to` as `rename` does, and returns once the system says the move is on the disk. If it fails after the move, the move may not be on the disk yet |
| `remove_file(path: String) ! FsError` | Removes a file, or a link but not what it leads to |
| `make_dir(path: String) ! FsError` | Makes a directory. `Exists` if something's there |
| `make_dir_all(path: String) ! FsError` | Makes a directory and each one missing on the way to it. Fine if it's there already |
| `remove_dir(path: String) ! FsError` | Removes an empty directory |
| `remove_dir_all(path: String) ! FsError` | Removes a directory and everything in it, never following a link out of it |
| `copy_dir_all(from: String, to: String) ! FsError` | Copies a directory and everything in it, making `to` and the directories inside as needed, each file with `copy` and each link as a link, never followed. See [Trees](#trees) |
| `walk(dir: String) -> List<Entry> ! FsError` | Everything in a directory and in the directories inside it, each directory just before what's in it, the names in each sorted, links listed and not followed. See [Trees](#trees) |
| `glob(pattern: String) -> List<String> ! FsError` | The paths that match a glob, such as `src/**/*.calx`, sorted, as Python's `glob.glob` finds them with `recursive=True`. See [Trees](#trees) |
| `list(path: String) -> List<String> ! FsError` | The names in a directory, without `.` and `..`, sorted by their bytes |
| `kind(path: String) -> Kind? ! FsError` | What's at the path, without following a link, or `none` if nothing is |
| `is_file(path: String) -> bool`, `is_dir(path: String) -> bool` | Whether the path is a file or a directory, following links. An error is `false`, as with Java's `Files.isRegularFile` and `Files.isDirectory` |
| `exists(path: String) -> bool` | Whether anything is at the path, following links, as Python's `os.path.exists`: `false` for a link to nothing, and for an error |
| `real_path(path: String) -> String ! FsError` | The absolute path of what's at `path`, with every link followed and each `.` and `..` resolved, as Java's `Path.toRealPath` gives it. `NotFound` if nothing is there |
| `absolute(path: String) -> String ! FsError` | The path made absolute without looking at what's there, as Python's `os.path.abspath` makes it: on the current folder, with `.` and each `..` after a name taken out of the text. See [The current folder](#the-current-folder) |
| `current_dir() -> String ! FsError` | The folder the program runs in, as the system gives it, which relative paths are on |
| `set_current_dir(path: String) ! FsError` | Makes `path` the folder the program runs in, for every thread |
| `read_link(path: String) -> Link? ! FsError` | What the link at `path` points to, without following it, or `none` if what's there isn't a link, as Java's `Files.readSymbolicLink` reads it. See [Links](#links) |
| `make_link(target: String, path: String) ! FsError` | Makes a symbolic link at `path` to `target`, kept as it's given, as Java's `Files.createSymbolicLink` makes it. `Exists` if something's at `path`. See [Links](#links) |
| `try_lock(path: String, bytes: List<u8>) -> FileLock? ! FsError` | Opens the file to write, creating it if nothing's there and keeping what it holds, writes `bytes` at its start and syncs them, then takes an exclusive lock on the whole file without waiting, as Java's `FileChannel.tryLock`. `none` if another lock is held on it, by this program or another |
| `separator() -> String` | What the system puts between a path's names, as Java's `File.separator`: `\` on Windows and `/` elsewhere, as in the paths `real_path` and `temp_dir` give. Either works in the paths `std.fs` is given |
| `temp_dir() -> String ! FsError` | A new, empty directory of the program's own in the system's temporary folder, for it to remove. On Linux and macOS its links are followed, as `current_dir` names it: macOS's is under `/private/var` |
| `unsafe disk_usage(path: String) -> DiskUsage ! FsError` | How big the file system that holds the path is, and how much of it is used and free, as Python's `shutil.disk_usage` and `df` count them. See [Room on the disk](#room-on-the-disk) |
| `Metadata` | `size: u64`, the length in bytes, which for a directory is whatever the system says, and `modified: Timestamp`, when what it holds last changed, to the millisecond. Writing or appending to a file sets it. And `mode: u32`, its permission bits, such as `0o644`. On Windows, which has only a read-only flag, they're `0o666`, or `0o444` when it's read-only, with `0o111` added for a directory. And `kind: Kind`, `File`, `Dir` or `Other`, what `is_file` and `is_dir` would say, from the same look at it |
| `DiskUsage` | `total: u64`, how big a file system is, `used: u64`, how much of it is in use, and `available: u64`, how much more this program may write to it, `df`'s `Avail`, all in bytes |
| `open(path: String) -> File ! FsError` | The file kept open to read and write, made if nothing's there, keeping what it holds. Other programs, and this one, can read, write and remove it while it's open, as Java lets them while it has a `FileChannel` open. See [Files kept open](#files-kept-open) |
| `open_read(path: String) -> File ! FsError` | The file kept open only to read, as Python's `open(path, "rb")`: `NotFound` if nothing's there, rather than a new empty file, and writing through it fails |
| `File` | An open file: its `path()`, `size() -> u64`, `metadata() -> Metadata`, what `metadata` says of its path but from the file already open, which on Windows is quicker than opening it again, `read_part(from, n)` and `read_parts(ranges)`, which give what the functions of those names do, `write_parts(parts, sync)`, as the function does, `sync()`, `close(own self)`, and `raw() -> i64`, the system's handle to it, as Python's `fileno()`: a `HANDLE` on Windows, a file descriptor on Linux. All but `path` and `raw` fail with `FsError`. It's open until `close`, or until the program ends. It's a [handle](../reference/ownership.md#handles), so it can't be copied |
| `FileLock` | A lock `try_lock` took, held until its `release(own self) ! FsError`, which lets it go and closes the file, or until the program ends. It's a handle too |
| `Link` | `target: String`, where a link leads, as it was made, and `junction: bool`, whether it's a Windows junction |
| `Kind` | `File`, `Dir`, `Link` (a symbolic link, or a junction on Windows) or `Other`, such as a device |
| `Entry` | What `walk` found: `path: String`, the directory walked and the names down to it, joined as [`std.path.join`](path.md) joins them, and `kind: Kind`, what's there, without following a link |
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

Each of these functions opens the file, does all it was asked, and closes it, so nothing a program forgets stays open. A batch of parts costs one opening, so a program writes a region's chunks together, not one call each.

## Files kept open

A program that goes back to the same file again and again keeps it open with `open`, as Java keeps a `FileChannel`, and reads and writes its parts through the `File`. Opening costs a little each time, and on Windows closing a file that was written costs more: the antivirus scans it again. On the reference machine (an i9-13900K, with Windows Defender on), writing 20 KB into a 4 MB file and then 8 KB at its start took 7.8 ms as two calls of `write_parts`, and 8 µs through a `File` kept open; with `sync`, 11.5 ms and 4.5 ms, which is the wait for the disk. A server that saved every chunk it made through `write_parts` spent nearly all of its saving thread's time there, and its players' chunks waited behind it.

```calx
import std.fs.{open, read, remove_dir_all, temp_dir}

fn main() {
    let dir = temp_dir()!
    let file = open("{dir}/r.0.0.mca")!
    file.write_parts([(8u64, [0, 0, 0, 2, 2])], true)!
    file.write_parts([(0u64, [0, 0, 2, 1])], true)!
    println("{file.size()!} {file.read_part(8, 5)!}")    // out: 13 [0, 0, 0, 2, 2]
    file.close()!
    println("{read("{dir}/r.0.0.mca")!.len()}")    // out: 13
    remove_dir_all(dir)!
}
```

A `File` is open until `close`, or until the program ends, as a `FileLock` is held: nothing closes it when it's let go, so a program keeps the files it uses most and closes the others, as Minecraft keeps 256 region files. It's a [handle](../reference/ownership.md#handles), so there's never a copy of one to close it twice.

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

A path with a NUL byte fails with `BadPath`, since the system would read only the part before it.

Windows' file functions refuse paths of 260 UTF-16 units or more unless they're in the `\\?\` form, and `CreateDirectoryW` at 248. So `std.fs` gives a path that long in that form: made absolute first, as `absolute` makes it, since the form turns off the rest of Windows' handling of paths. Programs read, write, list and remove files at any length, as Linux's do. Still limited to 260: `set_current_dir`, and a link `make_link` makes.

Names from `list` are UTF-8. On Linux, a name is bytes, and each part that isn't UTF-8 becomes U+FFFD. On Windows, an unpaired surrogate in a name becomes U+FFFD. `list` sorts the names, since each system lists them in its own order, and a program's output shouldn't depend on which it runs on.

## Trees

`walk`, `glob`, `copy` and `copy_dir_all` are what a script does with a tree of files: find them, look at each, and back them up.

```calx
import std.fs.{Kind, write_text, make_dir_all, walk, glob, copy_dir_all, remove_dir_all, temp_dir}

fn main() {
    let dir = temp_dir()!
    make_dir_all("{dir}/src/net")!
    write_text("{dir}/src/main.calx", "fn main\n")!
    write_text("{dir}/src/net/http.calx", "fn get\n")!
    write_text("{dir}/src/notes.txt", "to do\n")!
    for path in glob("{dir}/src/**/*.calx")! {
        println(path[dir.len() + 1..])
    }
    // out: src/main.calx
    // out: src/net/http.calx
    copy_dir_all("{dir}/src", "{dir}/backup")!
    var files = 0
    for e in walk("{dir}/backup")! {
        match e.kind {
            Kind.File => {
                files += 1
            },
            _ => {},
        }
    }
    println("{files} files backed up")    // out: 3 files backed up
    remove_dir_all(dir)!
}
```

- A glob's wildcards are [`std.path`'s](path.md#globs): `*` and `?` in a name, `[a-z]` and `[!a-z]`, and `**` for any number of directories. They pass over hidden names, those that start with `.`, as the shell's do, unless the pattern's name starts with `.` too. Letters match only in the same case, on Windows too, so a glob finds the same files everywhere.
- `glob` gives the pattern up to the name with its first wildcard as it's written, and then the names it found, joined with `/`, or with `\` on Windows when that's all the pattern used. The paths are sorted by their bytes.
- A pattern that ends with a separator, `*/`, finds only directories. A pattern with no wildcards gives itself if something's there, as Python's does.
- `**` goes into directories but not through links to them, so it can't go round a loop, and neither can `walk`, which lists a link as a `Link` and doesn't follow it.
- A directory `glob` may not list, or that went while it looked, has nothing in it to match, as in Python, so a glob over a home folder isn't stopped by one folder the system keeps to itself. `walk` fails instead, since it's to find everything.
- `copy` keeps the permission bits and the modified time, as Python's `shutil.copy2` does. On Linux the kernel copies the bytes with `copy_file_range`, without bringing them into the program, and a file system that can, such as Btrfs or XFS, shares them instead. Windows copies with its own `CopyFileW`.
- `copy_dir_all` is Python's `shutil.copytree` with `symlinks=True` and `dirs_exist_ok=True`: it makes a link again as a link to the same target, so nothing outside `from` is copied, and passes over devices, sockets and pipes. Each directory is listed before anything is made in it, so a copy into a directory inside `from` doesn't copy itself. On Windows a tree with a link in it needs the right to make links ([Links](#links)).

## Room on the disk

`disk_usage` says how big the file system that holds a path is, and how much of it is used and free, in bytes, as Python's `shutil.disk_usage` and `df -B1` count them. The path can be any file or directory on it. It's `unsafe`, since the numbers change as any program writes, as the clock's do.

```calx
import std.fs.{disk_usage}

fn main() {
    let d = unsafe { disk_usage("/") }!
    let percent = d.used * 100 / d.total
    println("{percent < 100} {d.used + d.available <= d.total}")    // out: true true
}
```

`available` is what this program may still write, which `df` calls `Avail` and Python `free`. On Linux and macOS a file system keeps some room back for root, 5% on a new ext4, which counts in neither `used` nor `available`, so the two add up to less than `total`, as they do in `df`. On Windows, `used` is the size less what's free, and a user's quota can make `available` less than that.

## The current folder

A relative path is on the folder the program runs in, `current_dir`, which `set_current_dir` changes for the whole program: every thread, and each program [`std.process`](process.md) starts without a folder of its own. A script that works in one folder can pass that folder to each call instead, which leaves the rest of the program alone.

`absolute` makes a path absolute by its text alone, as Python's `os.path.abspath` does: on the current folder if it's relative, then without its `.` names and with each `..` taking off the name before it, as [`std.path.normalize`](path.md) does. It doesn't look at what's there, so the path needn't exist, and a `..` after a link goes back past the link's name. `real_path` follows links instead, and needs the path to be there. On Windows `absolute` is what the system's `GetFullPathNameW` makes of the path, with `\` between the names, and `C:x` is on drive C's own current folder.

```calx
import std.fs.{absolute, current_dir, separator}

fn main() {
    let here = current_dir()!
    let sep = separator()
    let wanted = "{here}{sep}logs{sep}latest.log"
    println("{absolute("logs/old/../latest.log")! == wanted} {absolute("")! == here}")    // out: true true
}
```

## Links

A symbolic link is a name that leads to another path. `read_link` reads where it leads without going there, so a program can check a folder's links before it trusts them, as a Minecraft server checks its world's against `allowed_symlinks.txt`. `kind`, `rename`, `remove_file` and `remove_dir_all` see the link itself too, and every other call follows it.

```calx
import std.fs.{read_link, write, remove_dir_all, temp_dir}

fn main() {
    let dir = temp_dir()!
    write("{dir}/level.dat", [])!
    match read_link("{dir}/level.dat")! {
        some(link) => println("a link to {link.target}"),
        none => println("not a link"),    // out: not a link
    }
    remove_dir_all(dir)!
}
```

A link's `target` is what it was made with, not resolved: relative to the link's directory or absolute, and maybe to nothing at all. A program resolves it if it needs to, as `real_path` does.

On Windows, a link holds two names, the one it follows and the one the system shows, and `read_link` gives the one shown, without the `\??\` the system may put on the front of the other, as Java does. So `C:\Users\All Users`, a link every Windows has, reads as `C:\ProgramData`. A junction, an older kind of link to a directory, reads too, with `junction` set: `C:\Documents and Settings` reads as `C:\Users`. Java takes a junction for the directory it leads to, so a program matching Java treats one as a directory and looks inside it.

`make_link` makes a symbolic link, never a junction. On Windows each `/` in the target becomes `\`, which the system needs in a link, and the link is to a directory if the target is one when it's made. Windows lets a program make links only in developer mode or with the privilege to, and `make_link` fails with `Denied` otherwise. Linux lets any program make one.

## Errors

The same failure is the same `FsError` on both systems, and `{e}` says it in words, as Python does: `not found`, `permission denied`, `already exists`, `not a valid path`, `is a directory`, `directory not empty`, or `system error 32` with the system's code. In an [`Error`](../reference/errors.md), after what was being done, that's `reading config.toml: not found`.

```calx
import std.fs.{FsError, read, make_dir, write, remove_dir_all, temp_dir}

fn main() {
    let dir = temp_dir()!
    let errors: [String; 3] = ["{dir}/missing.dat", dir.copy(), "{dir}/a\0b"]
    for path in errors {
        match read(path) {
            ok(bytes) => println("{bytes.len()} bytes"),
            err(e) => println("{e}"),
        }
    }
    // out: not found
    // out: is a directory
    // out: not a valid path
    write("{dir}/x", [1, 2, 3])!
    match make_dir("{dir}/x") {
        ok => println("made"),
        err(e) => println("{e}"),    // out: already exists
    }
    match read("{dir}/gone") {
        ok(_) => println("read it"),
        err(e) => println("{e == FsError.NotFound}"),    // out: true
    }
    remove_dir_all(dir)!
}
```

`Denied` is a permission the system refuses, such as writing a read-only file, and `Os` is anything else, such as a file another program holds open on Windows.

## Saving safely

`write` replaces a file's bytes in place, so a crash part way through leaves it half written. To replace a file that must never be seen broken, such as a world's `level.dat`, write the new bytes to a file beside it and `rename` that over it. Readers then see the old file or the new one, whole.

`write` doesn't wait for the bytes to reach the disk, as Java's `Files.write` doesn't, and `rename` doesn't wait for the move to. A crash of the program loses neither, since the system has them, but a power cut soon after can leave the old file, or on some file systems an empty or broken new one.

To keep a file whole through a power cut, write the new bytes with `write_parts` and its `sync`, then `rename_synced` them over it. Each returns once the system says what it did is on the disk, so after a power cut there's the old file or the new one, whole, and once `rename_synced` has returned, the new one. That costs a wait for the disk, which `write` and `rename` don't, so it's for files that must survive one, such as a world's saves, not for every file.

## How it works

| Platform | Source |
| --- | --- |
| Windows | `CreateFileW`, `ReadFile` and `WriteFile`; `LockFileEx`; `MoveFileExW` with `MOVEFILE_REPLACE_EXISTING`, and `MOVEFILE_WRITE_THROUGH` to sync; `CopyFileW`; `FindFirstFileExW`; `GetFinalPathNameByHandleW`, without the `\\?\` it puts first; `GetFullPathNameW`; `GetCurrentDirectoryW` and `SetCurrentDirectoryW`; `GetTempPathW`; `GetDiskFreeSpaceExW`, on the folder a file is in |
| Linux | `open`, `read` and `write`; `fcntl` with `F_OFD_SETLK`; `rename`, then `fsync` on the folder it moved into and the one it moved from, to sync, where a file system that can't sync a folder offers nothing more; `copy_file_range`, or reads and writes where a file system can't, then `fchmod` and `futimens`; `opendir`; `realpath`; `getcwd` and `chdir`; `mkdtemp` in `TMPDIR` or `/tmp`; `statvfs`, or on macOS `statfs` |

`read` asks for the file's size and reads it whole in one call, so the list never grows, and a second call sees the end. A file can still grow as it's read, and `read` keeps going until the end.

`walk` and `glob` list each directory once and know what each entry is from the listing, as Python's `os.scandir` does: the find data on Windows has its attributes and its reparse tag, and on Linux `readdir` gives its type. Only an entry whose file system doesn't say is looked at, and in a glob a link where `*` follows it. So `src/**/*.calx` costs one listing a directory and nothing a file, which matters most where each call is slow, as Windows' files are from WSL: there it globs this repository's tests and docs in 0.23 s, where looking at each file took 2.9 s, and Python's `rglob` takes 0.41 s.

A program that doesn't import `std.fs` doesn't link it.

`std.fs` is written in Calx: [std/fs.calx](../../std/fs.calx). The calls to the system are in the runtime, [runtime/calx_fs.c](../../runtime/calx_fs.c).
