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
| `child.read_line(ms: u64) -> String?` | The next line of its output, without `\n` or `\r\n`, waiting at most `ms` milliseconds. `none` if it doesn't come in time, or the output has ended. The last line needn't end with a line break |
| `child.wait_for_line(text: String, ms: u64) -> String?` | Reads lines until one has `text` in it, and gives it, waiting at most `ms` milliseconds in all: a server's "Done" line, say. The lines before it are dropped |
| `child.read(ms: u64) -> List<u8>` | What has come from its output since the last read, waiting at most `ms` milliseconds if nothing has. Empty at the end, or if nothing comes in time |
| `child.ended() -> bool` | Whether its output has ended and all of it has been read |
| `child.try_wait() -> Status?` | How it ended, if it has, without waiting |
| `child.kill()` | Kills it, if it's running, without letting it clean up |
| `child.id() -> u32` | Its process ID |
| `unsafe child.usage() -> Usage` | What it has used so far, as the system says while it runs: see [What it used](#what-it-used) |
| `child.wait() -> Output` | Ends its input, waits for it, and gives how it ended with what it wrote that wasn't read. Panics if it was waited for already |

Reading its output panics if the output isn't `Stdio.Pipe`, and writing panics if the input isn't.

## How it ended

| Item | Is |
| --- | --- |
| `out.status: Status` | How it ended |
| `out.stdout: List<u8>`, `out.stderr: List<u8>` | What it wrote to each stream that was `Pipe` |
| `out.text() -> String`, `out.error_text() -> String` | The same as text, each byte that doesn't start valid UTF-8 becoming U+FFFD |
| `status.code: i32?` | The status it exited with, or `none` if it was killed: by `kill`, at its timeout, or on Linux by a signal. On Windows it's all 32 bits, so an access violation is `-1073741819`, `0xC0000005` |
| `status.signal: i32?` | On Linux, the signal that ended it, such as 9 for `SIGKILL` |
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

## When this program ends

A program that ends without meaning to takes the programs it started with it, so a bug can't leave them running:

- **On Windows**, every program a Calx program starts goes into a job, with every program those start in turn. If the Calx program panics, crashes or is killed, the job kills all of them, however deep they go. A program they start may still leave the job, as one that starts something in the background does.
- **On Linux**, a panic kills the programs the Calx program started. What those started runs on, since Linux has no job to hold them, but stopping the process group, as a terminal's Ctrl+C or a test runner does, reaches them all.
- **A program that ends as it means to**, by returning from `main` or calling `exit`, leaves what it started running, as in other languages. A `Child` that's never waited for keeps running until it ends.

`calx run` stands for the program the same way:
- **On Windows**, killing `calx run` kills the program and everything it started.
- **On Linux**, killing `calx run` kills the program, which the system sends `SIGKILL` when `calx run` ends, however it ends.

So a test runner that stops a `calx run` that ran too long stops the program too, and it doesn't keep the runner's pipes open.

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
