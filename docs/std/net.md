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
| `net.listen(host: String, port: u16) -> u16 ! NetError` | Listens on `host` and `port`, and returns the port, which the system picks when `port` is 0. An empty host listens on every address, IPv4 and IPv6, with one IPv6 socket that takes IPv4 clients too; where the machine has no IPv6, on every IPv4 address |
| `net.connect(host: String, port: u16) -> Conn ! NetError` | Starts opening a connection. A poll reports `Connected` once it's open, or `Closed` if it couldn't be |
| `net.poll(timeout_ms: i64, events: mut List<Event>)` | Waits up to `timeout_ms` milliseconds for something to happen, or forever if it's negative, and replaces what's in `events` with what happened. It returns as soon as there's an event, so `events` is empty only once the time is up: the runtime's own work, such as finishing a send or a close, doesn't end the wait |
| `net.read(conn: Conn, buf: mut List<u8>) -> usize` | Moves every byte that has arrived on the connection onto the end of `buf`, and returns how many there were |
| `net.write(conn: Conn, bytes: List<u8>)` | Queues the bytes to send. They go as soon as the socket takes them |
| `net.close(conn: Conn)` | Closes the connection once what was queued is sent |
| `net.peer(conn: Conn) -> Address?` | The address at the connection's other end, as it was when it opened. It stays until the poll after the connection's `Closed`, so the `Closed` can be logged with it, and is `none` for a connection that never opened |
| `Conn` | A connection. It's `Copy` |
| `conn.slot() -> usize` | A small number for the connection, counting from 0, for indexing what the program keeps per connection |
| `Event` | `Accepted(Conn)`, `Connected(Conn)`, `Received(Conn)` or `Closed(Conn)` |
| `NetError` | `AddressInUse`, `BadAddress`, `Refused`, or `Os(i32)` with the system's error code |
| `Ip` | `V4([u8; 4])`, or `V6([u8; 16], u32)` with its scope, the interface a link-local address is on, or 0 |
| `ip.to_string() -> String` | The address as RFC 5952 writes it: `127.0.0.1`, `::1`, `2001:db8::8:800:200c:417a`, `fe80::1%3` |
| `Address` | `ip: Ip` and `port: u16` |
| `address.to_string() -> String` | `127.0.0.1:25565`, or `[::1]:25565` |

## Events

| Event | Means |
| --- | --- |
| `Accepted(c)` | A listening socket accepted the new connection `c` |
| `Connected(c)` | The connection `c` from `connect` is open |
| `Received(c)` | Bytes arrived on `c`, and `read` takes them. A connection gets at most one `Received` per poll, however many times bytes arrived |
| `Closed(c)` | `c` ended: the other side closed it, it failed, `connect` couldn't open it, or `close` finished. It's the last event for `c` |

What arrived before a `Closed` can still be read until the next poll. After that, `read`, `write` and `close` on the connection do nothing, and a new connection can get its slot. A `Conn` kept past then never names the new connection: the id holds a generation as well as the slot, so the runtime ignores the old one.

Bytes written before `Connected` wait until the connection opens. Bytes written to a connection that has closed are dropped.

A `close` is graceful: the other side gets every byte queued before it, then the end of the stream, not a reset. On Windows that means cancelling the receive the runtime keeps pending, and closing the socket once it comes back, since closing it with a receive pending resets the connection.

## Addresses

```calx
import std.net.{Net, Event}

fn main() {
    let net = Net.new()!
    let port = net.listen("", 0)!    // every address
    _ = net.connect("127.0.0.1", port)!
    var events = List<Event>()
    var seen = false
    while not seen {
        net.poll(1000, mut events)
        for e in events {
            match e {
                Accepted(c) => {
                    if let a = net.peer(c) {
                        println("from {a.ip.to_string()}")    // out: from 127.0.0.1
                    }
                    seen = true
                }
                _ => {}
            }
        }
    }
}
```

An IPv4 client of a socket that listens on IPv6 too, as one on every address does, shows as IPv4, not as the IPv4-mapped `::ffff:127.0.0.1`, as Java shows it.

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
