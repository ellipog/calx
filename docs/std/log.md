---
title: std.log
description: Logging as Python's logging module does it, for scripts - messages at a level from debug to critical, written in Python's formats with the time, the level and the logger's name, to standard error or a file, set up once with config.
---

# `std.log`

Logging as Python's `logging` module does it: messages at a level, `debug` to `critical`, written with the time, the level and the logger's name, to standard error or to a file. `config` sets it up once, as `logging.basicConfig` does, and `info` and the rest can then be called anywhere, with no logger passed around. With nothing set up, a warning is written as Python writes one, `WARNING:root:the message`.

```calx
import std.log.{Level, config, debug, info, logger, warning}

fn main() {
    warning("disk almost full")    // stderr: WARNING:root:disk almost full
    config(level: Level.Info, format: "%(levelname)-7s %(name)s: %(message)s")!
    info("starting")    // stderr: INFO    root: starting
    debug("left out, below Info")
    let net = logger("net")
    net.warning("timed out after 5 s")    // stderr: WARNING net: timed out after 5 s
}
```

## Items

| Item | Gives |
| --- | --- |
| `Level` | `Debug`, `Info`, `Warning`, `Error` or `Critical`. `l.value()` is Python's number, 10 to 50, and `l.name()` and `{l}` its name, `DEBUG` to `CRITICAL` |
| `config(level:, format:, datefmt:, file:, append:) ! LogError` | Sets logging up, replacing what was set before. See [Setting up](#setting-up) |
| `debug(m)`, `info(m)`, `warning(m)`, `error(m)`, `critical(m)` | Logs the message on the root logger, named `root`, at that level |
| `log(level, m)` | Logs the message at `level` |
| `set_level(level)` | Sets the root logger's level |
| `enabled(level) -> bool` | Whether the root logger writes messages at `level`, for one that costs something to put together |
| `logger(name) -> Logger` | A logger for a part of the program, whose records have its name, as `logging.getLogger(name)` gives |
| `l.debug(m)` ... `l.critical(m)`, `l.log(level, m)` | Logs on the logger |
| `l.set_level(level)`, `l.enabled(level)` | Gives the logger a level of its own, and says whether it writes `level`: at its own level and above, or the root logger's when it has none |
| `l.child(suffix) -> Logger` | The logger `name.suffix`, with this one's level, as `getChild` gives |
| `LogError` | Why `config` couldn't: a format it can't read, or a file it can't open. `{e}` says which |
| `BASIC_FORMAT` | Python's default format, `%(levelname)s:%(name)s:%(message)s` |

Each record is a line. A message with line breaks in it is written as it is, as Python writes it.

## Setting up

`config` takes what `logging.basicConfig` does, each with Python's default:

| Option | Default | Is |
| --- | --- | --- |
| `level: Level` | `Level.Warning` | The root logger's level: messages below it are left out |
| `format: String` | `BASIC_FORMAT` | The format of a record's line. See [Formats](#formats) |
| `datefmt: String` | empty | The format of `%(asctime)s`, with strftime's codes, as [`Timestamp.format`](time.md#formatting-and-parsing)'s. Empty, it's `2026-11-08 14:05:09,250`, as Python writes it |
| `file: String` | empty | The file the records go to, or standard error when it's empty |
| `append: bool` | `true` | Whether the file is added to, as `filemode='a'` does, or started again |

Each call replaces what the last one set up, as `basicConfig(force=True)` does: Python's basicConfig does nothing the second time unless it's forced, which is easily missed. A config that fails leaves logging as it was:

```calx
import std.log.{config, warning}

fn main() {
    match config(format: "%(lineno)d %(message)s") {
        ok => {},
        err(e) => println("{e}"),    // out: `%(lineno)` in the format, a field of Python's records that Calx's don't have
    }
    match config(file: "no/such/folder/app.log") {
        ok => {},
        err(e) => println("{e}"),    // out: can't open `no/such/folder/app.log` to log to
    }
    warning("still on standard error")    // stderr: WARNING:root:still on standard error
}
```

Before anything sets logging up, the root logger's functions set it up as `config()` would, as Python's module functions call basicConfig, and a named logger writes a message at `Warning` and above alone, with no level or name, as Python's handler of last resort does.

A record goes to the file at once: it's flushed, as Python's `FileHandler` flushes it, so a line is in the file once the call returns, and a program that crashes keeps what it logged. Threads can log at once: each record is written whole.

The settings are the program's, shared by every thread, as Python's root logger is, and kept in the runtime, in [calx_log.c](../../runtime/calx_log.c), since Calx has no global state of its own. The functions are safe, though they read the clock and the settings, since what they read goes only to the log, not back to the program. A logger's own level is in its `Logger`, not shared by name as Python's are.

## Formats

A format is Python's `%` style: `%(field)s` for a field, with flags, a width and a precision as `%` takes them, and `%%` for a `%`:

| Field | Is |
| --- | --- |
| `%(message)s` | The message |
| `%(levelname)s` | The level's name: `DEBUG`, `INFO`, `WARNING`, `ERROR` or `CRITICAL` |
| `%(levelno)d` | The level's number, 10 to 50 |
| `%(name)s` | The logger's name, `root` for the root logger |
| `%(asctime)s` | The local time, by `datefmt` |
| `%(created)f` | The time in seconds since 1970, to the millisecond |
| `%(msecs)d` | The milliseconds of the second, 0 to 999 |
| `%(relativeCreated)d` | The milliseconds since the program first logged |
| `%(process)d` | The process's id |
| `%(processName)s` | `MainProcess`, as Python's is outside `multiprocessing` |

- `-` pads on the right, `0` pads a number with zeros, and `+` and a space put a sign before one: `%(levelname)-8s`, `%(msecs)03d`.
- `.N` cuts text to N runes, and gives a float N digits after the point: `%(levelname).1s`, `%(created).3f`.
- `s` writes any field, `d`, `i` and `u` a number's whole part, `f` and `F` a float, and `x`, `X` and `o` a whole number in hex or octal.

`config` checks a format: a field Calx's records don't have, as `%(lineno)d` and `%(threadName)s`, is an error there, as is a `%` that starts no field. Python finds those as it writes each record, and writes a traceback instead of the record.

```calx
import std.log.{Level, config, error, logger}

fn main() {
    config(level: Level.Info, format: "[%(levelname).1s] %(name)-6s|%(levelno)03d %(message)s")!
    error("disk full")    // stderr: [E] root  |040 disk full
    logger("net").child("tcp").info("connected")    // stderr: [I] net.tcp|020 connected
}
```

## Python's `logging`

| Python | Calx |
| --- | --- |
| `logging.basicConfig(level=logging.INFO, format=f, filename=p)` | `config(level: Level.Info, format: f, file: p)` |
| `logging.info("x %s", y)` | `info("x {y}")` |
| `log = logging.getLogger(__name__)`, `log.warning(...)` | `let log = logger("app")`, `log.warning(...)` |
| `log.setLevel(logging.DEBUG)`, `log.isEnabledFor(logging.DEBUG)` | `log.set_level(Level.Debug)`, `log.enabled(Level.Debug)` |
| `log.getChild("db")` | `log.child("db")` |
| `logging.exception("failed")` | `error("failed: {e}")`, with the error in the message |

Python puts a message together only when the level is on, from `%s` and its arguments. Calx puts `"x {y}"` together before the call, so a message that costs something to build, as one that walks a big tree, goes under `if enabled(Level.Debug)`.

## Speed

On our benchmark, an i9-13900K with other work running, so rough: Python 3.14's `logging` against `std.log`, with `%(asctime)s %(levelname)s %(name)s: %(message)s` to a file. A message below the level, left out, took 298 ns in Python and under 1 ns in Calx. A record written took 21.5 µs in Python and 3.2 µs in Calx, most of it the flush that puts each line in the file at once.
