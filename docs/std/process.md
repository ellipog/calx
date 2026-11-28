---
title: std.process
description: Running other programs with their arguments, folder, environment, input, output and timeouts, finding them, and the CPU time and memory they used; and this process's memory, CPU time and exit.
---

# `std.process`

Other programs, and this one. `Command` starts another program, as a script runs `git` or a compiler: with its arguments, a folder to run in and changes to its environment, its input given and its output kept as asked, and a timeout. And this process: the memory it holds, the CPU time it has used, the processors it may run on, and `exit`.

```calx
import std.env.{args}
import std.process.{Command, current_exe}

fn main() {
    // Started by the code below: say hello, as another program would answer.
    let a = args()
    if a.len() > 0 {
        println("hello, {a[0]}")
        return
    }
    // Start this program again with an argument, and read what it says.
    match Command.new(current_exe()).arg("world").read() {
        ok(text) => println("it said: {text}"),    // out: it said: hello, world
        err(e) => println("{e}"),
    }
}
```

A script names a program and goes on in the same chain, `Command.new("git").args(["log", "-1", "--format=%H"]).dir("calx").read()`, and handles the error, which says what went wrong: `can't find the program git on PATH`, or ``git log -1 --format=%H` exited with status 128``.

## Saying what to run

Each method gives the command back with one more thing said, so they chain. A command can be run again.

| Method | Says |
| --- | --- |
| `Command.new(program: String)` | The program: a name, looked for in each folder on PATH, or a path with `/` or `\` in it, relative to this program's current folder |
| `c.arg(a: String)`, `c.args(list: List<String>)` | Adds arguments. Each reaches the program as it is, spaces and quotes and all: no shell reads it |
| `c.dir(path: String)` | Runs it in that folder, relative to this program's current folder |
| `c.env(name: String, value: String)` | Sets a variable in its environment, which is otherwise this program's |
| `c.env_remove(name: String)` | Takes a variable out of its environment |
| `c.env_clear()` | Starts its environment empty, with only what `env` sets after this |
| `c.stdin(mode: Stdio)`, `c.stdout(mode: Stdio)`, `c.stderr(mode: Stdio)` | Where a stream goes, in place of the default of the method that runs it |
| `c.input(bytes: List<u8>)`, `c.input_text(text: String)` | Gives it this as its standard input, which then ends |
| `c.timeout(ms: u64)` | Kills it if it's still running that many milliseconds after it started. 0, how a command starts, is no limit |
| `c.affinity(cpus: List<u32>)` | Runs it only on these processors, numbered from 0 as the system numbers them, pinned before it runs anything. Panics if the list is empty: see [Pinning](#pinning-and-the-machines-load) |
| `c.full_speed()` | Takes it out of Windows' power throttling, so Windows doesn't move it to the efficiency cores or slow it down to save power. Other systems have none, so it does nothing there |
| `c.measured()` | Starts it so its peak memory is its own on Linux, not at least this program's: see [What it used](#what-it-used). Elsewhere it's its own already, so it does nothing there |
| `c.own_group()` | Starts it in a process group of its own, with the programs it starts, so they can be stopped together: see [Stopping it and what it started](#stopping-it-and-what-it-started) |
| `c.ends_with_this()` | Ends it when this program ends, however this program ends, killed too, and with `own_group` its whole group: see [Ending with this program](#ending-with-this-program) |

`Stdio` is where a stream goes:

| `Stdio` | Means |
| --- | --- |
| `Inherit` | This program's own: the terminal, or wherever this program's stream goes |
| `Pipe` | A pipe to this program: input comes from `input` or `Child.write`, and output is kept |
| `Null` | Nothing: input that has ended already, output thrown away |
| `Stdout` | Standard error only: wherever standard output goes, as `2>&1`, so the two arrive together in the order written |

## Running it

| Method | Does |
| --- | --- |
| `c.run() ! ProcessError` | Runs it with this program's streams and waits. Fails if it doesn't start or doesn't succeed: Python's `subprocess.run(..., check=True)` |
| `c.read() -> String ! ProcessError` | Runs it and gives its standard output as text, without the line breaks at its end, as a shell's `$(...)` does. Its errors go to this program's. Fails if it doesn't start or doesn't succeed |
| `c.output() -> Output ! ProcessError` | Runs it, keeping both its output streams, and waits. Fails only if it doesn't start: how it ended is in the `Output` |
| `c.status() -> Status ! ProcessError` | Runs it with this program's streams and says how it ended. Fails only if it doesn't start |
| `c.spawn() -> Child ! ProcessError` | Starts it and goes on: the `Child` writes to it, reads from it, waits for it or kills it |
| `output_all(commands: List<Command>, at_once: usize) -> List<Output> ! ProcessError` | Runs each as `output` does, at most `at_once` at a time, the next starting as one ends, and gives their outputs in order. Fails if one doesn't start, after killing the rest. Panics if `at_once` is 0 |

A program starts with SIGPIPE at its default, and no signals blocked, whatever this program has: a Calx program ignores SIGPIPE ([`std.io`](io.md#when-the-reader-has-gone)), but `yes | head` run through `sh` ends when `head` does, as it would from a shell, only if `yes` doesn't. A Calx program it starts ignores it again itself. Writing to a child's input once it has stopped reading fails with `InputClosed`, never ending this program.

Where each stream goes unless the command says:

| | Input | Output | Errors |
| --- | --- | --- | --- |
| `run`, `status`, `spawn` | `Inherit` | `Inherit` | `Inherit` |
| `read` | `Inherit` | `Pipe` | `Inherit` |
| `output`, `output_all` | `Null` | `Pipe` | `Pipe` |

`input` and `input_text` make the input a pipe. `stdin` and `stdout` panic when given `Stdio.Stdout`, which is only for standard error.

What a program writes is read as it comes, whatever this program is waiting for, so a program that fills one stream while this one waits for the other never stops for it, and a large input and a large output can't wait on each other. `output_all` reads every program it's running the same way, in one loop.

## Talking to one that's running

```calx
import std.env.{args}
import std.io.{Lines, flush}
import std.process.{Command, Stdio, current_exe}

fn main() {
    // Started by the code below: answer each line in capitals, until "bye".
    if args().len() > 0 {
        var lines = Lines {}
        while let line = lines.next() {
            if line == "bye" {
                return
            }
            println(line.to_upper_root())
            flush()
        }
        return
    }
    var child = Command.new(current_exe()).arg("child").stdin(Stdio.Pipe).stdout(Stdio.Pipe).spawn()!
    for word in ["hello", "there"] {
        child.write_text("{word}\n")!
        println(child.read_line(5000) ?? String("no answer"))
    }
    // out: HELLO
    // out: THERE
    child.write_text("bye\n")!
    println("{child.wait().status}")    // out: exited with status 0
}
```

| Method | Does |
| --- | --- |
| `child.write(bytes: List<u8>) ! ProcessError`, `child.write_text(text: String) ! ProcessError` | Writes to its input, waiting until it has taken it, and reading what it writes meanwhile. Fails with `InputClosed` if it stops reading first |
| `child.close_input()` | Ends its input once what was written has gone, so a program that reads to the end goes on |
| `child.read_line(ms: u64) -> String?` | The next line of its output, without `\n` or `\r\n`, waiting at most `ms` milliseconds. `none` if it doesn't come in time, a `Waker` woke it first, or the output has ended. The last line needn't end with a line break. With 0 it doesn't wait, but takes what's in the pipe already, as a read of a nonblocking pipe in Python does |
| `child.wait_for_line(text: String, ms: u64) -> String?` | Reads lines until one has `text` in it, and gives it, waiting at most `ms` milliseconds in all: a server's "Done" line, say. The lines before it are dropped |
| `child.read(ms: u64) -> List<u8>` | What has come from its output since the last read, waiting at most `ms` milliseconds if nothing has. Empty at the end, if nothing comes in time, or if a `Waker` woke it first. With 0, what's in the pipe already, without waiting |
| `child.ended() -> bool` | Whether its output has ended and all of it has been read |
| `child.waker() -> Waker` | A `Waker` another thread can call `wake()` on, so the `read_line` or `read` waiting gives `none` or nothing at once ([below](#waking-a-read)). `wait_for_line` doesn't stop for it. Panics if the system can't make one, as when the program is out of file descriptors |
| `Waker` | Wakes one child's reads, from any thread. It can be copied and sent to other threads, and once the child has been waited for a wake does nothing |
| `child.wake_on(waker: std.net.Waker)` | Wakes the `Net` of a [`std.net.Waker`](net.md), such as a server's, each time output or errors come, as its output ends and as it exits, and at once if output is waiting, the output has ended or it has exited ([below](#waking-a-servers-loop)). Panics if the system won't watch its pipes |
| `child.try_wait() -> Status?` | How it ended, if it has, without waiting |
| `child.wait_timeout(ms: u64) -> Status?` | How it ended, waiting at most `ms` milliseconds for it to, reading its output meanwhile: Python's `p.wait(timeout=...)`. `none` if it's still running |
| `child.kill()` | Kills it, if it's running, without letting it clean up |
| `child.stop()`, `child.kill_tree()`, `child.signal(sig: i32) ! ProcessError` | Ask it and what it started to end, end them all, or send a signal: see [Stopping it and what it started](#stopping-it-and-what-it-started) |
| `child.id() -> u32` | Its process ID |
| `unsafe child.usage() -> Usage` | What it has used so far, as the system says while it runs: see [What it used](#what-it-used) |
| `child.wait() -> Output` | Ends its input, waits for it, and gives how it ended with what it wrote that wasn't read. Panics if it was waited for already |

Reading its output panics if the output isn't `Stdio.Pipe`, and writing panics if the input isn't.

A loop that watches many programs at once reads each with `read_line(0)` or `read(0)`, which take what each has written without waiting for more, so none of them stops on a full pipe while the loop is busy with the others.

### Waking a read

A thread that talks to a child, writing what other threads queue for it and reading its answers, would otherwise write each only once `read_line(10)` gives up, up to 10 ms later, or about 15.6 ms on Windows, whose timer is that coarse. With the child's `Waker`, the others wake its read once they've queued something, so it can wait for seconds:

```calx
import std.env.{args}
import std.io.{Lines, flush}
import std.process.{Command, Stdio, Waker, current_exe}
import std.thread.{Task}

// Another thread with a line for the child, as a program's main loop hands one to the thread
// that talks to it.
struct Asker {
    out: Sender<String>
    waker: Waker
}

impl Task for Asker {
    fn run(own self) {
        var out = self.out
        out.send("hello")
        self.waker.wake()
    }
}

fn main() {
    if args().len() > 0 {
        var lines = Lines {}
        while let line = lines.next() {
            println(line.to_upper_root())
            flush()
        }
        return
    }
    var child = Command.new(current_exe()).arg("child").stdin(Stdio.Pipe).stdout(Stdio.Pipe).spawn()!
    var outbox = Receiver<String>()
    _ = unsafe { Thread.spawn(Asker { out: outbox.sender(), waker: child.waker() }) }
    loop {
        while let text = outbox.next() {
            child.write_text("{text}\n")!
        }
        // Waits up to a minute for a line, or until a line is queued.
        if let answer = child.read_line(60000) {
            println(answer)    // out: HELLO
            break
        }
    }
    _ = child.wait()
}
```

A wake makes the `read_line` or `read` that's waiting give way once what has come is taken, or makes the next one give way if none is waiting. Wakes that come before a read are one, `ended` leaves them for the read, and every copy of a `Waker` wakes the same child.

### Waking a server's loop

A server's loop that also reads a program it started would otherwise call `next_event(10)` and then `read_line(0)`, and hear of each line up to 10 ms late, or about 15.6 ms on Windows. Given the server's [`std.net.Waker`](net.md), the child wakes `next_event` when something comes from it, so the loop waits as long as it likes, as Python's `selectors` hears of a pipe:

```
var child = Command.new("tool").stdout(Stdio.Pipe).spawn()!
child.wake_on(server.waker()!)
loop {
    if let e = server.next_event() {
        serve(e)
        continue
    }
    // Woken: take every line that has come, then see if it has exited.
    while let line = child.read_line(0) {
        handle(line)
    }
    if child.try_wait() != none {
        break
    }
}
```

It wakes it for output, and for errors, which `wait` gives, so neither pipe fills while the loop waits; as the output ends, and as the child exits; and, while the input given with `input` is still going in, for room in it. It wakes it at once if output is waiting, the output has ended or the child has exited. Wakes that come before a call are one, so after one the loop reads until there's nothing. A later `wake_on` wakes the `Net` it's given instead.

On Windows the system's thread pool waits on each pipe's reads and on the exit. On Linux and macOS the pipes, and the pidfd or kqueue that says when the child exits, go into the server's own epoll or kqueue, so its poll sees them with no thread at all. A Linux older than 5.3, which has no pidfds, hears of the exit as the output ends.

## Stopping it and what it started

A server is often started by a launcher, a script that starts the real server and helpers. `kill()` ends only the launcher, and leaves the server running. Started with `own_group()`, the launcher leads a process group of its own, which every program it starts is in too, so it can be asked to stop, and then ended, with all of them:

```calx
import std.env.{args}
import std.io.{flush}
import std.process.{Command, Stdio, current_exe}
import std.thread.{sleep}

fn main() {
    // Started by the code below: a launcher, which starts the server, this program again, and waits.
    let a = args()
    if a.len() > 0 {
        if a[0] == "launcher" {
            _ = Command.new(current_exe()).arg("server").stdout(Stdio.Null).spawn()!
            println("ready")
            flush()
        }
        unsafe { sleep(60000000000) }
        return
    }
    var server = Command.new(current_exe()).arg("launcher").stdout(Stdio.Pipe).own_group().spawn()!
    _ = server.wait_for_line("ready", 30000)
    // Ask them to stop, give them 10 seconds to, and end what's left.
    server.stop()
    if server.wait_timeout(10000) == none {
        server.kill_tree()
    }
    println("{server.wait().status.success()}")    // out: false
}
```

| Method | Does |
| --- | --- |
| `c.own_group()` | Starts it in a process group of its own, which the programs it starts are in too. On Linux and macOS it's a session of its own, as Python's `start_new_session=True` starts. On Windows it's a console process group, as `CREATE_NEW_PROCESS_GROUP` makes, and a job of its own |
| `child.stop()` | Asks it and every program in its group to end, letting them clean up: `SIGTERM` to the group on Linux and macOS, as `os.killpg(p.pid, signal.SIGTERM)` sends it, and Ctrl+Break on Windows, as `p.send_signal(signal.CTRL_BREAK_EVENT)` does. It doesn't wait |
| `child.kill_tree()` | Ends it and every program in its group at once: `SIGKILL` to the group on Linux and macOS, and on Windows the end of its job, which `psutil`'s loop over `children(recursive=True)` does in Python |
| `child.signal(sig: i32) ! ProcessError` | Sends it a signal, such as 1 for `SIGHUP`, as `p.send_signal(sig)` does: to its whole group if it was started with `own_group`, as `os.killpg` does. Fails with `BadArgument` for a number that isn't a signal, and on Windows, which has none |

- **`stop` and `kill_tree` panic** unless it was started with `own_group`, on every system, so a program that works on Linux works on Windows. Ctrl+Break reaches only a group, and ending what it started needs one.
- **What it started is ended even once it has exited itself**, as a launcher does once it has started the server. On Linux and macOS the group's ID stays its own while anything is in the group, and on Windows the job holds what's in it.
- **The terminal's Ctrl+C doesn't reach it**, on any system: it isn't in the terminal's group, so this program decides when it ends. A panic in this program still ends it and its group, as [When this program ends](#when-this-program-ends) says.
- **On Windows, Ctrl+Break** ends a program that doesn't handle it with the status `-1073741510`, `0xC000013A`. It's sent through the console the program runs in, so one that has no console of its own, such as a service, joins the child's console for as long as it takes. A program that can't be sent it keeps running, which is why `kill_tree` comes after.
- **A Calx program hears `stop` as [`std.signal`](signal.md)'s `Terminate`**, on every system, so one that handles it can save its state and end as it means to.
- **Signal numbers** are the system's. 1 `SIGHUP`, 2 `SIGINT`, 9 `SIGKILL` and 15 `SIGTERM` are the same everywhere, but others differ: `SIGUSR1` is 10 on Linux and 30 on macOS.
- **On Windows, a program it starts may leave the job**, as one that starts something in the background with `CREATE_BREAKAWAY_FROM_JOB` does, and `kill_tree` doesn't reach that one. A program that starts one in a group of its own, a `setsid` on Linux, leaves the group the same way.

## How it ended

| Item | Is |
| --- | --- |
| `out.status: Status` | How it ended |
| `out.stdout: List<u8>`, `out.stderr: List<u8>` | What it wrote to each stream that was `Pipe` |
| `out.text() -> String`, `out.error_text() -> String` | The same as text, each byte that doesn't start valid UTF-8 becoming U+FFFD |
| `status.code: i32?` | The status it exited with, or `none` if it was killed: by `kill`, at its timeout, or on Linux and macOS by a signal. On Windows it's all 32 bits, so an access violation is `-1073741819`, `0xC0000005` |
| `status.signal: i32?` | On Linux and macOS, the signal that ended it, such as 9 for `SIGKILL` or 11 for `SIGSEGV` |
| `status.timed_out: bool` | Whether its timeout ran out, so it was killed |
| `status.success() -> bool` | Whether it exited with 0 |
| `unsafe status.usage() -> Usage` | What it used in all, as the system recorded it when the program ended: see [What it used](#what-it-used) |

A `Status` shows as `exited with status 3`, `was killed by signal 9`, `was killed`, or `ran out of time, and was killed`.

A program has finished once it has exited and what it wrote has been read. Once it exits, its pipes are read until they're empty, not until every end is closed, since a program it started in the background may hold them open long after it.

## What it used

A benchmark that starts a server measures what the server used: its CPU time and its memory, sampled while it runs, and its totals once it ends. `child.usage()` gives what it has used so far, and `status.usage()` what it used in all, what GNU `time` says of a program. Both are `unsafe`, since the numbers differ from run to run, as the clock's do. The same `Usage` comes from every system:

| `Usage` | Is |
| --- | --- |
| `cpu_ns: u64` | The nanoseconds of CPU time all its threads used, in the kernel and out. Windows adds to it at each tick of its clock, about every 15.6 ms |
| `memory: u64` | The bytes of its memory in RAM now: its working set on Windows, its resident set on Linux, `top`'s `RES`, and its footprint on macOS, Activity Monitor's "Memory". 0 once it has ended |
| `peak_memory: u64` | The most bytes it held in RAM at once: its peak working set on Windows, `VmHWM` on Linux and its lifetime peak footprint on macOS while it runs, and `ru_maxrss` once it has ended. On Linux that's at least this program's own peak, unless it was started [`measured`](#a-peak-of-its-own-on-linux) |
| `wall_ns: u64` | The nanoseconds from just before it started until it was seen to exit, or until now while it runs, on the monotonic clock |

```calx
import std.env.{args}
import std.process.{Command, Stdio, current_exe}

fn main() {
    if args().len() > 0 {
        let held: List<u64> = [7; 1000000]    // 8 MB, for the parent to see
        println("ready {held.len()}")
        return
    }
    var c = Command.new(current_exe()).arg("child").stdout(Stdio.Pipe).spawn()!
    _ = c.read_line(30000)
    let now = unsafe { c.usage() }
    let out = c.wait()
    let total = unsafe { out.status.usage() }
    println("{now.peak_memory >= now.memory} {total.peak_memory > 8000000} {total.memory}")    // out: true true 0
}
```

- On Windows the numbers are `GetProcessTimes` and `GetProcessMemoryInfo`'s, from the handle to the process, which still says what it used after it ends. Python needs `ctypes` for them.
- On Linux, while it runs, they're `/proc/<pid>/stat` and `/proc/<pid>/status`. Once it ends, they're `wait4`'s, as Python's `os.wait4` gives them, so they also count the programs it started and waited for.
- On macOS, while it runs, they're `proc_pid_rusage`'s, and once it ends `wait4`'s, as on Linux.
- Once it has ended, `child.usage()` gives what `status.usage()` will, so a sampler that's late still gets the totals.
- **The wall time** starts just before the system is asked to start it, so it counts the system starting it, as a stopwatch around `status()` would, but not looking for the program or making its pipes. It ends when this program sees it exit, which is at once while it waits for it. One waited for late ends late, so time it with `status()`, `output()` or `wait()`.
- [This process's](#this-process) own are `memory()` and `cpu_time()`.

### A peak of its own on Linux

On Linux a program's peak memory starts at the peak of the program that started it. Every way of starting a program there runs the new one in the old one's memory until it loads its own, in a copy of it after a `fork` and in the same memory after `posix_spawn`, and the kernel keeps that memory's peak as the new program's. So `true`, started by a program that once held a gigabyte, says it peaked at a gigabyte. Python's `os.wait4` and `resource.getrusage(RUSAGE_CHILDREN)` say the same, for the same reason.

`c.measured()` starts it from a helper instead: this program's own file run again, which starts the child before it has done anything else, when it holds a megabyte or two. That's what GNU `time` does, from its own small process. The helper starts it as this program's child, so its ID, its streams, killing it and waiting for it are as any child's, and the helper exits once it has started it.

```calx
import std.env.{args}
import std.process.{Command, current_exe}

fn main() {
    if args().len() > 0 {
        return
    }
    let held: List<u8> = [1; 100000000]    // 100 MB
    let alone = Command.new(current_exe()).arg("child").measured().status()!
    let used = unsafe { alone.usage() }
    println("{used.peak_memory < 50000000} {held.len()}")    // out: true 100000000
}
```

Starting it takes a few hundred microseconds more, 0.6 ms in all on the reference machine rather than 0.3 ms, but its wall time starts when the helper starts it, so it's the same as if it had been started directly. On Windows and macOS a program's peak is its own however it starts, so `measured` does nothing there. A Calx library in another program, or a system without `/proc`, starts it as any other.

## Errors

| `ProcessError` | When |
| --- | --- |
| `NotFound(program)` | Nothing by that name is on PATH, or nothing is at that path |
| `Denied(program)` | The system won't run it: it can't be run, or isn't a program |
| `BadDirectory(folder)` | The folder to run it in isn't there |
| `BadArgument(why)` | It can't be given what the command says: a name that's empty, a zero byte in an argument, a variable's name with `=` in it, a line break in an argument to a batch file, or processors to pin it to that the machine doesn't have, or that macOS can't pin it to |
| `Failed { command, status }` | It ran, and didn't succeed. Only `run` and `read` fail with it |
| `InputClosed` | It stopped reading before all of its input was written. Only `Child.write` fails with it |
| `Unsupported` | The system starts no programs: iOS, where an app runs alone in its sandbox, so every way to start one fails with it there, on the simulator too ([iOS](#ios)) |
| `Unconfined(why)` | It was to be [confined](#confining-it) and couldn't be: the system can't confine a program, a folder given isn't there, or the program isn't in a folder it may read. `Confine.remove` fails with it too |
| `Os(code)` | Something else, with the system's error code |

Each shows as a sentence, such as `can't find the program git on PATH`.

## Finding the program

The program is found the same way on each system, by Calx, not by the system's own rules:

- **A name** is looked for in each folder on PATH, in order, and nowhere else: not in the current folder, as Windows' own search would. If the command sets PATH with `env`, that's the PATH searched.
- **A path**, one with `/` or `\` in it, is relative to this program's current folder, even when `dir` says where it runs.
- **On Windows** a name or path is tried as it is if it has an extension, then with `.exe`, `.com`, `.cmd` and `.bat`, in that order.
- **A batch file**, `.cmd` or `.bat`, runs through `cmd.exe`, which reads its arguments by rules of its own before the batch file sees them. Each argument is quoted so `cmd.exe` passes it on as it is, `%PATH%`, `&`, `|` and quotes included, as Rust does since CVE-2024-24576. A line break would end the command, so an argument with one fails with `BadArgument`.
- **`cmd.exe`'s own commands**, such as `dir` and `echo`, aren't programs, so they're run as `Command.new("cmd").args(["/c", "dir"])`.

| Function | Gives |
| --- | --- |
| `which(name: String) -> String?` | The path of the program `name` would start, made whole, or `none` if there's nothing to run |
| `current_exe() -> String` | The path of this program's own executable, to start it again |

## iOS

An app on iOS runs alone in its sandbox, which lets it start no other program, so every way to start one fails with `Unsupported`, which shows as `iOS doesn't let an app start other programs`. The simulator would let it, since it runs on a Mac, but a build for it fails the same way, so what works there works on an iPhone. `which`, `current_exe` and the rest of [this process](#this-process) work as elsewhere (CLX-408).

## When this program ends

A program that ends without meaning to takes the programs it started with it, so a bug can't leave them running:

- **On Windows**, every program a Calx program starts goes into a job, with every program those start in turn. If the Calx program panics, crashes or is killed, the job kills all of them, however deep they go. A program they start may still leave the job, as one that starts something in the background does.
- **On Linux**, a panic kills the programs the Calx program started, and the whole group of each started with [`own_group`](#stopping-it-and-what-it-started). What the others started runs on, since Linux has no job to hold them, but stopping the process group, as a terminal's Ctrl+C or a test runner does, reaches them all.
- **A program that ends as it means to**, by returning from `main` or calling `exit`, leaves what it started running, as in other languages, unless it started it with [`ends_with_this`](#ending-with-this-program). A `Child` that's never waited for keeps running until it ends.
- **On Linux and macOS, a program that's killed**, with `kill -9` or by the OOM killer, can do nothing as it ends, so what it started runs on, unless it was started with `ends_with_this`.

### Ending with this program

`c.ends_with_this()` ends the program it starts when this one ends, however this one ends: by returning from `main` or with `exit`, by a panic or a crash, or killed, by `kill -9`, the OOM killer or Task Manager. With `own_group`, its whole group goes with it, so a browser goes with its renderers, and a launcher's server goes too after the launcher has ended. A program that starts helpers of its own, such as a headless browser or a language server, uses it so that none of them is left running, whatever happens to the program. In Python that's `preexec_fn` calling `prctl(PR_SET_PDEATHSIG, SIGKILL)` through `ctypes`, which works on Linux only and reaches only the child.

```
var browser = Command.new("chrome").args(["--headless", "--user-data-dir={profile}"]).own_group().ends_with_this().spawn()!
```

- **On Windows** it goes into a job that kills what's in it as the job closes, which happens as this program ends, however it ends. A program it starts goes too, unless that program leaves the job.
- **On Linux and macOS** a watcher ends it. The watcher is a small process, this program's own file run again. It holds the read end of a pipe whose write end only this program holds. However this program ends, the system closes the write end, and the watcher kills the child with `SIGKILL`, and with `own_group` its whole group.
  - Linux's death signal, `PR_SET_PDEATHSIG`, isn't used: it comes when the *thread* that started the child ends, so a child started from a thread that ends first would die with that thread, and it reaches only the child, not its group. macOS has no death signal at all.
  - On Linux, the helper that starts the child, as for [`measured`](#a-peak-of-its-own-on-linux), forks the watcher before it says the child has started. On macOS the child starts suspended, as under a debugger, and goes on once its watcher is watching. Either way, this program ending in between can't leave it running.
- **The watcher** is in no group of the child's, and holds nothing else open. So it doesn't keep a pipe from ending, and the terminal's Ctrl+C doesn't reach it. It ends once the child has, or, with `own_group`, once nothing is left in the group, which it checks each second. It's not a child of this program's, so nothing has to wait for it.
- **Starting it costs** about as much again as starting a program does, as with `measured`: 0.66 ms rather than 0.33 ms to run `true` on the reference machine under WSL. On Windows it's a job assignment while the child is suspended.
- **Its ID, its streams, `kill`, `stop` and waiting for it** work as for any child: on Linux the helper starts it as this program's child.
- **`SIGKILL` gives it no chance to clean up.** A program that can stop it gently does so itself before it ends, with `stop()` and `wait_timeout`, as with any child. `ends_with_this` is for when it can't.
- **In a Calx library** in another program, starting a child with it fails with `BadArgument` on Linux and macOS, since the library can't run its host's file again.

`calx run` stands for the program the same way:
- **On Windows**, killing `calx run` kills the program and everything it started.
- **On Linux**, killing `calx run` kills the program, which the system sends `SIGKILL` when `calx run` ends, however it ends.

So a test runner that stops a `calx run` that ran too long stops the program too, and it doesn't keep the runner's pipes open. Asking it to stop is different: Ctrl+C, SIGTERM, SIGHUP and Ctrl+Break don't end `calx run` while its program runs, so a program that [handles](signal.md) them ends as it means to, and `calx run` ends with its status ([`calx run`](../reference/command.md#running-a-file)).

## Confining it

`c.confine(jail)` runs the program, and every program it starts, so that it sees only the folders a `Confine` gives it, as an agent that's to start fresh needs (CLX-479). It reads and runs the system's own programs and libraries, so `git`, `node` and a shell work, reads and writes the folders given to write, reads those given to read, and reaches the internet only if it may. The user's home, other folders and other drives are closed to it.

```
let c = Confine.new("agent-a_1")
    .write("C:/agents/a_1")
    .read("C:/Users/x/AppData/Local/tool/bin")
    .internet(true)
var agent = Command.new("node").args(["agent.js"]).dir("C:/agents/a_1").confine(c).spawn()!
```

| `Confine` | Says |
| --- | --- |
| `Confine.new(name: String) -> Confine` | A confinement named `name`, 1 to 64 letters, digits, `.`, `-` and `_`, that may go nowhere yet. The same name is the same confinement |
| `c.write(path: String) -> Confine` | It may read and write the folder, and what's in it, or the file |
| `c.read(path: String) -> Confine` | It may read the folder, and what's in it, or the file, and run the programs there |
| `c.internet(on: bool) -> Confine` | Whether it may connect to the internet. Without it, it connects to nothing |
| `c.name() -> String` | Its name |
| `Confine.remove(name: String) ! ProcessError` | Takes back what the confinement was given, once nothing in it runs |
| `confine_supported() -> bool` | Whether this system can confine a program |

A folder given must be there when the program starts, and the program itself must be in the system's folders or in one given: a Calx program started confined needs its own folder given to read. The system's folders are Windows' System32 and Program Files, macOS' `/usr`, `/bin`, `/sbin`, `/System`, `/Library`, `/opt` and `/Applications/Xcode*.app`, and Linux's `/usr`, `/bin`, `/sbin`, `/lib*`, `/etc`, `/opt`, `/proc`, `/sys`, `/run`, `/nix` and `/snap`, with `/dev` to read and write.

**On Windows** it's an AppContainer of the confinement's name, as Chromium's sandboxed processes are:

- Each folder given gets an ACE for the container, which what's in it inherits. Each folder above it gets an ACE of its own, which nothing inherits, that lets the container read the folder's attributes and pass through it, but not list it, as `git` needs to look at every folder above its own. What was given is kept in the registry, under `HKCU\Software\Calx\Confine`, where the container can't write, and `Confine.remove` takes each ACE back off and deletes the container's profile, with what Windows kept for it there.
- Windows doesn't let a user change `C:\` or `C:\Users`, and they let a container pass through but not look at them, so a program that looks at every folder above its own fails for a folder under `C:\Users`: `git clone` does. A folder outside it, such as one in `C:\` or on another drive, works.
- Windows Server 2025 doesn't let a container open `NUL`, and Git for Windows opens it each time it starts, so git doesn't run confined there. Windows 11 lets it.
- With the internet it has the `internetClient` capability. It never reaches this machine's own addresses, `127.0.0.1` among them.
- Its temporary folder is its own, in its profile, as Windows gives every AppContainer, and its `USERPROFILE` is the user's, which it can't read: `git` takes that as no config.

**On macOS** it's the system's sandbox, Seatbelt, as `sandbox-exec -p` runs it, with a profile that denies everything but what's given and what a program needs of the system to run, such as looking users up and checking certificates. It may read any path's metadata, but not a folder's listing nor a file's bytes. With the internet it may connect anywhere but this machine's own addresses.

**On Linux** it's Landlock, which a thread of this program takes on before it starts the program, as a pinned one is started, so only the program and what it starts have it. Without the internet it can't make a TCP connection, which needs Linux 6.7; on an older Linux, starting a program confined without the internet fails. UDP isn't Landlock's to stop, and with the internet it may reach this machine's own addresses too. On Linux 6.12 and later it can't signal programs outside it nor connect to their abstract Unix sockets. A program started `measured` or `ends_with_this` is started by this program's own file run again, so that file is readable to it.

**On macOS and Linux** it gets a temporary folder of its own as `TMPDIR`, `calx-confine-NAME` in the user's temporary folder on macOS and `/tmp/calx-confine-UID-NAME` on Linux, which `Confine.remove` deletes, and `HOME` is the first folder given to write, unless the command sets them itself.

## Speed

Starting a program and taking its output costs what the system's own process creation does. Measured on the reference machine (an i9-13900K), running a program that prints a line and keeping its output, the best of ten batches of 100:

| | Windows 11 | Linux (WSL 2) |
| --- | --- | --- |
| Calx `output()` | 4.0 to 4.6 ms | 401 to 409 µs |
| Python 3.14 `subprocess.run(capture_output=True)` | 3.9 to 4.4 ms | 426 to 434 µs |
| Rust 1.91 `Command::output()` | 3.8 to 5.0 ms | |
| C, `posix_spawn` with two pipes and nothing else | | 376 to 389 µs |

On Windows almost all of it is Windows creating the process, so the three are level within the noise. On Linux, Calx is 5% from C doing nothing but spawning, and ahead of Python. Each program's pipes are read straight into one buffer, 64 KB at a time or more, and copied once into the `Output`.

## This process

```calx
import std.process.{memory, memory_limit, processors}

fn main() {
    unsafe {
        let held = memory()
        println("{held > 0} {memory_limit() > held}")    // out: true true
        println("{processors() >= 1}")    // out: true
    }
}
```

| Function | Gives |
| --- | --- |
| `unsafe memory() -> u64` | The bytes of this process's memory in RAM now: its working set on Windows, its resident set on Linux and macOS, `top`'s `RES` |
| `unsafe memory_limit() -> u64` | The bytes of RAM it may use: the machine's, or less where a job object's limit on Windows, or a cgroup's on Linux, says |
| `unsafe cpu_time() -> u64` | The nanoseconds of CPU time all its threads have used since it started, in the kernel and out |
| `unsafe processors() -> u32` | The processors it may run on, as `parallel for` counts them: on Windows and Linux, those its affinity allows |
| `exit(code: u8) -> never` | Doesn't return: it ends the program now, with the status `code` |

- **A share of the CPU** over a second is the change in `cpu_time()` over the change in a monotonic clock, divided by `processors()`. Windows adds to the CPU time at each tick of its clock, about every 15.6 ms, so it's for rates over a second or so, not for timing code.
- **The limit in a container** is its cgroup's: on cgroup v2 the lowest `memory.max` of the process's group and those above it, and on v1, `memory.limit_in_bytes`. Where none is set it's the machine's RAM. Each function gives 0 where the system can't say.
- **They're `unsafe`**, as the clock is (D88): each changes as the program runs, or with the machine it runs on. `exit` isn't: it reads nothing. Starting other programs isn't either, as reading files and the network aren't, though what they say may change from run to run.
- **`exit` ends the program from any thread**, as returning from `main` does: the terminal is put back, with what [`std.term.at_exit`](term.md) says, and what was printed is written. Nothing else runs: other threads stop where they are, no value is dropped, and a debug build doesn't check for leaks. The status is the same on every system, since it's a `u8`. Inside a `parallel for` it [panics](../reference/printing.md#panics), at `<exit>`, since which iteration ended the program would depend on timing.
- **`exit` returns [`never`](../reference/functions.md#functions-that-never-return)**, so a function that returns a value needs no `return` after it, it can stand in for a value, as in `variable("HOME") ?? exit(1)`, and code right after it is an error, since it can't run.

## Pinning and the machine's load

A program can be kept to some of the machine's processors, its own or one it starts, and can read how busy the whole machine is. A benchmark does both: it pins what it times to the same cores each run, and notes how much else was running meanwhile.

```calx
import std.process.{machine_cpu, processors, set_affinity, set_full_speed}

fn main() {
    // This program on processor 0 alone, out of Windows' power throttling.
    set_full_speed()
    let pinned = match set_affinity([0]) {
        ok => unsafe { processors() } == 1,
        err(e) => "{e}" == "macOS can't pin a program to processors",
    }
    println("{pinned}")    // out: true

    // How busy the whole machine was while this ran, every program's work counted.
    let before = unsafe { machine_cpu() }
    var spin: u64 = 0
    for i in 0u64..20000000 {
        spin = spin ^ (i * 2654435761)
    }
    let after = unsafe { machine_cpu() }
    let busy = f64.nearest(after.busy_ns - before.busy_ns)
    let all = busy + f64.nearest(after.idle_ns - before.idle_ns)
    let share = if all > 0.0 { busy / all } else { 0.0 }
    println("{share >= 0.0 and share <= 1.0} {spin != 1}")    // out: true true
}
```

| Function | Does |
| --- | --- |
| `set_affinity(cpus: List<u32>) ! ProcessError` | Keeps this program to these processors, numbered from 0 as the system numbers them, and so the programs it starts after. Panics if the list is empty |
| `set_full_speed()` | Takes this program out of Windows' power throttling, as `c.full_speed()` does a program it starts, and does nothing elsewhere |
| `unsafe machine_cpu() -> MachineCpu` | The CPU time of the whole machine since it started, summed over its processors: `busy_ns`, spent running anything, and `idle_ns`, spent idle |

- **Which processors are which** is the system's numbering. Windows numbers a core's two threads next to each other, so on the i9-13900K 0, 2, 4 and on to 14 is one thread on each performance core. Linux often does too, but not always: `lscpu --extended` shows which are which.
- **Processors the machine doesn't have** give `BadArgument`, and nothing changes. On Windows they're those of the program's processor group, up to 64. On Linux, a container that allows only some of them keeps the program to those it allows, and it's `BadArgument` only if it allows none.
- **macOS can't pin a program to processors**: there `set_affinity` and a command with `affinity` give `BadArgument`, saying so. It runs where the system puts it, and `full_speed` does nothing, as on Linux.
- **The share busy** over a stretch is the change in `busy_ns` over the change in both. A program's own work counts in it, so take its `cpu_time()` out to see what else was running. Under WSL, `machine_cpu` sees only the Linux system's processes, not Windows'. Windows adds to the times at each tick of its clock, as with `cpu_time()`.
- **`machine_cpu` is `unsafe`** for the reason `cpu_time` is, and gives zeros where the system can't say. Setting affinity isn't: it changes where the program runs, not what it computes.

`std.process` is written in Calx over the runtime: [std/process.calx](../../std/process.calx), and [runtime/calx_proc.c](../../runtime/calx_proc.c), which starts programs and reads and writes their pipes.
