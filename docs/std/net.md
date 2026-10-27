---
title: std.net
description: TCP connections driven by events, on IOCP on Windows and epoll on Linux, with the buffers kept by the runtime.
---

# `std.net`

TCP servers and clients. A `Net` holds listening sockets and connections, and `poll` reports what happened to them as events. Nothing else waits for the network: `write` queues bytes and returns, and `read` takes bytes that have already arrived.

```calx
import std.net.{Event, Net}

fn main() {
    let net = Net.new()!
    let port = net.listen("127.0.0.1", 0)!    // 0: the system picks a free port
    let client = net.connect("127.0.0.1", port)!
    var events = List<Event>()
    var got = List<u8>()
    var done = false
    while not done {
        net.poll(1000, mut events)
        for e in events {
            match e {
                Connected(c) => {
                    let hi: List<u8> = [104, 105]
                    net.write(c, hi)
                }
                Accepted(_) => {}
                Received(c) => {
                    if c.slot() == client.slot() {
                        _ = net.read(c, mut got)
                        if got.len() == 2 {
                            net.close(c)
                        }
                    } else {
                        var bytes = List<u8>()
                        _ = net.read(c, mut bytes)
                        net.write(c, bytes)    // echo it back
                    }
                }
                Closed(c) => {
                    if c.slot() == client.slot() {
                        done = true
                    }
                }
            }
        }
    }
    println("client got {String.from_utf8(got) ?? String("?")}")    // out: client got hi
}
```

## Items

| Item | Gives |
| --- | --- |
| `Net` | Listening sockets, connections and their events. It's `Copy`: copies are the same `Net` |
| `Net.new() -> Net ! NetError` | A new, empty `Net` |
| `net.listen(host: String, port: u16) -> u16 ! NetError` | Listens on `host` and `port`, and returns the port, which the system picks when `port` is 0. An empty host listens on every address, IPv4 and IPv6 |
| `net.connect(host: String, port: u16) -> Conn ! NetError` | Starts opening a connection. A poll reports `Connected` once it's open, or `Closed` if it couldn't be |
| `net.poll(timeout_ms: i64, events: mut List<Event>)` | Waits up to `timeout_ms` milliseconds for something to happen, or forever if it's negative, and replaces what's in `events` with what happened |
| `net.read(conn: Conn, buf: mut List<u8>) -> usize` | Moves every byte that has arrived on the connection onto the end of `buf`, and returns how many there were |
| `net.write(conn: Conn, bytes: List<u8>)` | Queues the bytes to send. They go as soon as the socket takes them |
| `net.close(conn: Conn)` | Closes the connection once what was queued is sent |
| `Conn` | A connection. It's `Copy` |
| `conn.slot() -> usize` | A small number for the connection, counting from 0, for indexing what the program keeps per connection |
| `Event` | `Accepted(Conn)`, `Connected(Conn)`, `Received(Conn)` or `Closed(Conn)` |
| `NetError` | `AddressInUse`, `BadAddress`, `Refused`, or `Os(i32)` with the system's error code |

## Events

| Event | Means |
| --- | --- |
| `Accepted(c)` | A listening socket accepted the new connection `c` |
| `Connected(c)` | The connection `c` from `connect` is open |
| `Received(c)` | Bytes arrived on `c`, and `read` takes them. A connection gets at most one `Received` per poll, however many times bytes arrived |
| `Closed(c)` | `c` ended: the other side closed it, it failed, `connect` couldn't open it, or `close` finished. It's the last event for `c` |

What arrived before a `Closed` can still be read until the next poll. After that, `read`, `write` and `close` on the connection do nothing, and a new connection can get its slot. A `Conn` kept past then never names the new connection: the id holds a generation as well as the slot, so the runtime ignores the old one.

Bytes written before `Connected` wait until the connection opens. Bytes written to a connection that has closed are dropped.

## Errors

`Net.new`, `listen` and `connect` fail with a `NetError` ([errors](../reference/errors.md)):

```calx
import std.net.{Net, NetError}

fn describe(e: NetError) -> String {
    return match e {
        AddressInUse => String("address in use"),
        BadAddress => String("bad address"),
        Refused => String("refused"),
        Os(code) => "system error {code}",
    }
}

fn main() {
    let net = Net.new()!
    let port = net.listen("127.0.0.1", 0)!
    match net.listen("127.0.0.1", port) {
        ok(_) => println("listened twice"),
        err(e) => println(describe(e)),    // out: address in use
    }
    match net.connect("not an address", 25565) {
        ok(_) => println("connecting"),
        err(e) => println(describe(e)),    // out: bad address
    }
}
```

A connection that can't be opened, such as one to a port where nothing listens, isn't an error from `connect`: it's a `Closed` event, since finding out takes a round trip.

`connect` and `listen` take a host name as well as an address. A host with a character no name or address has, such as a space, is a `BadAddress` at once. A name is looked up before they return, so it waits for the name server, and only an address never waits.

## Threads

Only the thread that made a `Net` can use it, and never inside a [`parallel for`](../reference/parallel.md), which panics. Calcite runs the network on its own thread and talks to the tick loop through queues ([spec](../CALX_SPEC.md#actors)).

A `Net` lasts until the program ends. It has no `close` of its own until Calx has destructors, so that a copy can never name a `Net` that was freed.

## How it works

The runtime keeps each connection's bytes: what arrived and hasn't been read, and what was written and hasn't been sent. `read` copies out of the first, and `write` appends to the second, so a program never lends a buffer to the system and gets it back later.

| Platform | Source |
| --- | --- |
| Windows | An I/O completion port: `AcceptEx`, `ConnectEx`, `WSARecv` and `WSASend`, gathered with `GetQueuedCompletionStatusEx` |
| Linux | Edge-triggered `epoll`, with `accept4` and non-blocking `recv` and `send` |

Both read up to 16 KB at a time per connection. A listening socket on Windows uses `SO_EXCLUSIVEADDRUSE`, so a second listener on the same port fails with `AddressInUse` on both platforms.

A program that doesn't import `std.net` doesn't link it.

`std.net` is written in Calx: [std/net.calx](../../std/net.calx). The sockets are in the runtime, [runtime/calx_net.c](../../runtime/calx_net.c).
