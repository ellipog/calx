---
title: std.signal
description: Ctrl+C, SIGTERM, SIGHUP, Windows' console close and shutdown, and a service's stop, handled by the program, as Python's signal.signal handles them, with a server's event loop woken to stop cleanly, and a program run as a Windows service.
---

# `std.signal`

A program that's asked to stop, by Ctrl+C at the terminal, by `kill`, by systemd or launchd stopping its unit, by Windows shutting down or by the Service Control Manager stopping its service, ends at once. That's right for a script, but a server or a daemon has state to save and connections to close first. `handle` says the program handles those requests, as Python's `signal.signal(signal.SIGTERM, on_stop)` does. A signal that arrives then only records itself, and the program learns of it when it's ready: `take` says whether one came, `wait` waits for one, and a `Net` or a [`Server`](http-server.md) whose `wake_on_signal` was called wakes from its poll, as asyncio's `loop.add_signal_handler` wakes its loop.

```calx
import std.signal.{Signal, handle, raise, take, wait}

fn main() {
    handle()    // Interrupt, Terminate and Hangup, all three
    println("{take() == none}")    // out: true

    // As if the system had sent it: on POSIX it's the signal itself.
    _ = raise(Signal.Terminate)
    if let s = take() {
        println("asked to stop: {s}")    // out: asked to stop: terminate
    }
    // Waiting for one, a tenth of a second at most.
    println("{wait(100) == none}")    // out: true
}
```

A daemon's loop takes a signal each time round, and stops when one has come:

```
import std.signal.{handle, wait}

fn main() {
    handle()
    let state = load_state()
    loop {
        do_some_work(state)
        // Waits up to a second, so the work runs once a second, and stops as soon as asked.
        if let s = wait(1000) {
            println("stopping on {s}")
            break
        }
    }
    save_state(state)
}
```

A server stops taking requests at a signal, answers the ones it has, and closes its connections, with [`Server.wake_on_signal`](http-server.md#stopping-cleanly) and `shutdown`. A program on [`std.net`](net.md#stopping-at-a-signal) does the same with `Net.wake_on_signal` and `drain`.

## Items

| Item | Gives |
| --- | --- |
| `Signal` | `Interrupt`, `Terminate` or `Hangup` ([below](#which-signal-is-which)). It shows as `interrupt`, `terminate` or `hangup` |
| `handle(signals: List<Signal> = [])` | Handles each of `signals`, or all three if it's empty, from now until the program ends: one that arrives no longer ends the program, but waits for `take` or `wait`, and wakes each poll `wake_on_signal` was called on. It can be called again to handle more |
| `take() -> Signal?` | The signal that arrived and hasn't been taken, taken, or `none` |
| `pending() -> bool` | Whether a signal has arrived that `take` hasn't taken, leaving it there |
| `wait(timeout_ms: i64 = -1) -> Signal?` | As `take`, waiting up to `timeout_ms` milliseconds for one, or forever if it's negative. `none` once the time is up. Waiting forever when nothing is handled panics, since nothing would come |
| `raise(s: Signal) -> bool` | Sends `s` to this program, as Python's `signal.raise_signal` does: handled, it's recorded as one from outside is, and not, it ends the program as it would ([below](#raise)) |
| `service() -> bool` | Whether Windows' Service Control Manager started this program as a service, which then runs as one ([below](#windows-services)). `false` started any other way, and on other systems |
| `Net.wake_on_signal() ! NetError` | In [`std.net`](net.md#stopping-at-a-signal): each signal wakes the `Net`'s poll, which reports `Woken` |
| `Server.wake_on_signal() ! NetError`, `server.shutdown(timeout_ms: i64 = 5000)` | In [`std.http.server`](http-server.md#stopping-cleanly): `next` and `next_event` give `none` while a signal is pending, and `serve` returns, after `shutdown` has closed every connection cleanly |
| `Command.own_group()`, `child.stop()` | In [`std.process`](process.md#stopping-it-and-what-it-started): a child in a process group of its own, and asking it and what it started to stop, SIGTERM on POSIX and Ctrl+Break on Windows, which a Calx program hears as `Terminate` |

- **Signals that come before they're taken** wait in order, `Interrupt`, then `Terminate`, then `Hangup`, and one kind that comes twice before it's taken comes as one, as the system's own pending signals do.
- **The handler only records the signal.** On POSIX a handler can run on any thread at any moment, in the middle of an allocation even, so it sets a flag and writes a byte to a pipe, which is all that's safe there, and a thread of the runtime's own wakes what waits. On Windows the system runs the handler on a thread of its own, which wakes what waits itself. Either way the program's own code runs only where it asks: at `take`, `wait`, or a poll.
- **It ends as it means to.** A program that handled a signal ends by returning from `main` or with `exit`, with its own status: 0 after a clean stop, where a program the signal ended would have 130 or 143 from a shell. [`std.term`](term.md)'s `at_exit` text is written then, and standard input put back, as at any end.
- **`take`, `pending` and `wait` aren't `unsafe`**, though when a signal comes depends on what's outside the program, as reading standard input and the network aren't. Like those, `wait` can't be called inside a `parallel for`.

## Which signal is which

The three are the same on every system, so a program handles them the same way everywhere:

| `Signal` | On Linux and macOS | On Windows |
| --- | --- | --- |
| `Interrupt` | SIGINT: Ctrl+C at the terminal | Ctrl+C at the console |
| `Terminate` | SIGTERM: `kill`, systemd's and launchd's stop, Docker's, `child.stop()` | Ctrl+Break, which is how `child.stop()` asks; the user logging off and the system shutting down; a service's stop |
| `Hangup` | SIGHUP: the terminal went away | The console window closed |

- **SIGHUP and SIGINT that the program was started ignoring stay ignored**, as `nohup` ignores SIGHUP and a shell ignores SIGINT for a program it runs in the background with `&`. Python's `signal.signal` would undo that.
- **After a console closes, a logoff or a shutdown on Windows**, the system ends the program as soon as the handler returns, so the handler waits for the program to end instead, until the system's time is up: 5 seconds after a close, and what Windows is set to after a shutdown, 5 seconds for a program and 20 for a service to begin with. A service isn't stopped by a user logging off, only by the Service Control Manager.
- **Ctrl+Break is `Terminate`, not `Interrupt`**, since it's the one console event a program can send to another, as `child.stop()` does: so `stop` is `Terminate` on every system. Go makes it its `Interrupt`, and Python its own `SIGBREAK`.
- **SIGQUIT, SIGUSR1 and the rest** aren't handled: SIGQUIT still ends a program with a core dump, as it should. SIGKILL can't be caught on any system, and neither can Windows' `TerminateProcess`, which Task Manager's End task and `child.kill()` use.

## `raise`

`raise` sends a signal to this program, as Python's `signal.raise_signal` does, for tests, and for a program that stops itself the way it would be stopped from outside. On POSIX it's the system's `raise`, so a handled signal comes through the handler, and one that isn't ends the program with that signal, after what it printed is written. On Windows, as C's `raise` there, it doesn't go through the console: a handled signal is recorded as the console's would be, and one that isn't ends the program as the system's own handler would, with status `0xC000013A`, `STATUS_CONTROL_C_EXIT`, after the terminal is put back and what it printed is written.

## Windows services

`service()` lets a program run as a Windows service, as pywin32's `win32serviceutil.ServiceFramework` does, without a framework: the program asks once, as it starts, and goes on as it would.

```
import std.signal.{handle, service, wait}

fn main() {
    // Ctrl+C when it's run at a console; a service is handed Terminate as well.
    handle()
    if service() {
        log_to_file()    // a service has no console to print to
    }
    let server = start()
    _ = wait()
    server.stop()
}
```

- **It's `StartServiceCtrlDispatcher` on a thread of the runtime's own.** Started by the Service Control Manager, the program is told it's running, and `service` returns `true`. The manager's stop, and the system's preshutdown, come as `Terminate`, which the program handles from then on. The service says it's stopping, with 30 seconds before the manager thinks it stuck, and says it has stopped as the program ends, with no error after a status of 0, and with its status as the service's own error code otherwise, 101 after a panic.
- **Started any other way**, from a console or by another program, the dispatcher fails at once, and `service` returns `false`, so the same program runs as a service and at a console. The manager waits 30 seconds after starting a service for it to ask, so a program asks first, before anything slow.
- **Installing it is the system's job**: `sc.exe create name binPath= "C:\path\to\program.exe"` as an administrator, then `sc.exe start name`. Pause and continue, and a service that shares its process with others, aren't supported: a Calx service is a process of its own that can be stopped.
- **On Linux and macOS**, `service` returns `false`. systemd and launchd stop a program with SIGTERM, which `handle` handles, and want nothing else from it: a unit's `Type=simple` or a launchd agent runs the program as it is.

`std.signal` is written in Calx over the runtime: [std/signal.calx](../../std/signal.calx), and [runtime/calx_signal.c](../../runtime/calx_signal.c).
