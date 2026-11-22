---
title: std.env
description: The program's command-line arguments and environment variables, as UTF-8 Strings on every platform, the system it runs on, and the user it runs as, elevated or not.
---

# `std.env`

The program's command line, its environment, the system it runs on, and the user it runs as.

```calx
import std.env.{args}

// args: --threads
// args: 24
// args: two words

fn main() {
    let a = args()
    println("{a.len()}")    // out: 3
    for s in a {
        println("[{s}]")
    }
    // out: [--threads]
    // out: [24]
    // out: [two words]
}
```

| Function | Gives |
| --- | --- |
| `args() -> List<String>` | The arguments the program was started with, after its own name, in order |
| `program() -> String` | The program's name, for its help and messages: the last part of the path it was started as, without `.exe` or `.calx`. `calx run tools/count.calx` starts it as `tools/count.calx`, so it's `count`, and so is a program built as `count.exe` or started by a `#!` line as `./count` |
| `script() -> String?` | The `.calx` file `calx run` built the program from, as a full path, as Python's `__file__` is for the script it runs: for finding the files that go with a script, beside it, from wherever it's run. `none` for a program started another way, such as one `calx build` made, whose own path is [`std.process.current_exe()`](process.md) |
| `variable(name: String) -> String?` | The value of the environment variable `name`, or `none` if it isn't set or `name` can't be one: empty, or with `=` or a zero byte in it. Windows matches names without regard to case |
| `os() -> String` | The system the program runs on, `"windows"`, `"linux"`, `"macos"` or `"ios"`, as Rust's `std::env::consts::OS` names them, for the few things a script does differently on each. A program is built for one system, so it's the same on every call. A build for the iOS simulator gives `"ios"` too |
| `arch() -> String` | The processor the program was built for, `"x86_64"` or `"aarch64"`, as Rust's `std::env::consts::ARCH` names them. An x64 program that Windows or macOS runs on an Arm processor, translating it as it goes, says `"x86_64"` |
| `native_arch() -> String` | The processor the system has: `arch()`, but `"aarch64"` for an x64 program that Windows on Arm or macOS's Rosetta translates. For choosing which build of a program to download, as `calx update` does. Python's `platform.machine()` says what the system says to the program, which is `AMD64` under Windows' translation |
| `is_elevated() -> bool` | Whether the program runs with the system's full rights, which every program it starts gets too: as root on Linux and macOS, or on Windows with an elevated token ([the user](#the-user)) |
| `user_name() -> String?` | The name of the user the program runs as: Windows' account name, without its domain, or the effective user id's name in Linux's and macOS's user database. `none` for a user id with no entry there, as a container can be started with |
| `user_id() -> u32?` | The effective user id on Linux and macOS, 0 for root, as Python's `os.geteuid()` gives it. `none` on Windows, which names users by SID |

Run a program with arguments by putting them after `--`: `calx run tool.calx -- --threads 24` ([the calx command](../reference/command.md)). [`std.cli`](cli.md) reads them into a struct, as options, flags and arguments by place, with `--help`.

A script finds what goes with it by `script()`, as a Python script does by `__file__`:

```calx
import std.env
import std.path

fn main() {
    // The folder this file is in, wherever it was started from.
    let here = path.parent_dir(env.script() ?? String()) ?? String(".")
    println("{path.is_absolute(here)}")    // out: true
}
```

`calx run` passes the path in the environment variable `CALX_SCRIPT`, which the program's runtime takes out of its environment as it starts, so a program it starts doesn't see it.

## Text

Every argument is a valid UTF-8 `String`, so the platforms differ only where an argument can't be text:

| Platform | What it passes | What `args` does |
| --- | --- | --- |
| Windows | One UTF-16 string for the whole command line | Splits it as the C runtime documents: whitespace separates arguments outside quotes, `\"` is a quote, and backslashes are literal unless they come before a quote. Converts each to UTF-8, which is exact except for an unpaired surrogate, which becomes U+FFFD |
| Linux | Bytes, already split | Keeps valid UTF-8 as it is, and turns each byte that doesn't start a valid sequence into U+FFFD |

An environment variable's value is converted in the same way, and `variable` reads the environment as it is when it's called.

```calx
import std.env.{variable}

fn main() {
    // PATH is set on every system a program runs on.
    println("{variable("PATH") != none}")    // out: true
    println("{variable("CALX_NOT_A_VARIABLE_EVER") == none} {variable("A=B") == none} {variable("") == none}")    // out: true true true
}
```

The runtime reads the command line once at startup. Each call to `args` gives new copies, which the caller owns.

```calx
import std.env.{os}

fn main() {
    let shell = if os() == "windows" { "cmd" } else { "sh" }
    println("{shell == "cmd" or shell == "sh"}")    // out: true
}
```

## The user

A program that starts others, such as a server that runs agents or a build that runs scripts, gives them its own rights. `is_elevated()` lets it refuse to run with the system's full rights unless it's told to:

```calx
import std.env

// Whether to go on: not with full rights, unless --allow-root says to.
fn may_run(allow_root: bool) -> bool {
    if env.is_elevated() and not allow_root {
        eprintln("refusing to run as {env.user_name() ?? String("root")}, as what this starts would get the same rights; pass --allow-root to run anyway")
        return false
    }
    return true
}

fn main() {
    println("{may_run(true)}")    // out: true
    println("{env.user_name() != none}")    // out: true
}
```

| System | `is_elevated()` is true | As Python says |
| --- | --- | --- |
| Linux, macOS | The effective user id is 0, root's, as it is under `sudo` | `os.geteuid() == 0` |
| Windows | The process's token is elevated (`GetTokenInformation` with `TokenElevation`): an Administrator's once UAC has asked, as in a terminal opened with "Run as administrator", an Administrator's always where UAC is off, and SYSTEM's, as a service's can be | `ctypes.windll.shell32.IsUserAnAdmin()` |

An Administrator's program that UAC hasn't asked for runs with a filtered token, without the Administrators group's rights, so it isn't elevated.

`user_name()` is the name Python's `getpass.getuser()` gives where no environment variable sets another: `getpass` reads `LOGNAME`, `USER`, `LNAME` and `USERNAME` first, which a program's starter can set to anything, and `user_name()` asks the system. On Linux and macOS it's `getpwuid(geteuid())`'s name, so `root` under `sudo`, and on Windows `GetUserNameW`'s. `user_id()` is `geteuid()`, and is `none` on Windows.

`std.env` is written in Calx: [std/env.calx](../../std/env.calx).
