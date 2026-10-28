---
title: std.net
description: TCP connections driven by events, on IOCP on Windows and epoll on Linux, with the buffers kept by the runtime.
---

# `std.net`

TCP servers and clients. A `Net` holds listening sockets and connections, and `poll` reports what happened to them as events. Nothing else waits for the network: `write` queues bytes and returns, and `read` takes bytes that have already arrived. Another thread can wake a poll with a `Waker`.

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
                Woken => {}
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
| `net.write(conn: Conn, bytes: List<u8>)` | Queues the bytes to send. They go to the system at the next poll or `flush`, with everything else queued on the connection since the last, then as soon as the socket takes them |
| `net.flush()` | Sends what was queued on every connection now, rather than at the next poll |
| `net.close(conn: Conn)` | Sends what was queued on the connection, then closes it |
| `net.waker() -> Waker ! NetError` | A `Waker` for the `Net`'s polls |
| `Waker` | Wakes a `Net`'s poll from another thread. It's `Copy` and `Send`, and every copy wakes the same `Net` |
| `waker.wake()` | Makes the poll waiting report `Woken`, or the next poll if none is waiting. Any thread can call it, except inside a `parallel for` |
| `net.peer(conn: Conn) -> Address?` | The address at the connection's other end, as it was when it opened. It stays until the poll after the connection's `Closed`, so the `Closed` can be logged with it, and is `none` for a connection that never opened |
| `Conn` | A connection. It's `Copy` |
| `conn.slot() -> usize` | A small number for the connection, counting from 0, for indexing what the program keeps per connection |
| `conn.id() -> u32`, `Conn.from_id(id: u32) -> Conn` | The connection's number, its slot and how many connections had the slot before, which no two of a `Net`'s connections share at once, and the connection with a number, for a log of connections that's replayed. A number no connection has names none: reading it gives nothing, and writing to it or closing it does nothing |
| `Event` | `Accepted(Conn)`, `Connected(Conn)`, `Received(Conn)`, `Closed(Conn)` or `Woken` |
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
| `Woken` | A `Waker` woke the poll. However many wakes came before the poll reported one, it reports one `Woken` |

What arrived before a `Closed` can still be read until the next poll. After that, `read`, `write` and `close` on the connection do nothing, and a new connection can get its slot. A `Conn` kept past then never names the new connection: the id holds a generation as well as the slot, so the runtime ignores the old one.

Bytes written before `Connected` wait until the connection opens. Bytes written to a connection that has closed are dropped.

Writes wait for the next poll so that a program that writes many times between polls, such as a server sending a tick's packets, hands the system each connection's bytes once, in one send. `flush` sends them sooner, and `close` sends the connection's first.

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

Only the thread that made a `Net` can use it, and never inside a [`parallel for`](../reference/parallel.md), which panics. Other threads talk to it through [queues](thread.md), and a `Waker` lets them say they've queued something, so the thread polling needn't wait out its timeout to find it:

```calx
import std.net.{Event, Net, Waker}
import std.thread.{Task}

// Works something out on its own thread, and wakes the poll once it's queued.
struct Worker {
    out: Sender<i64>
    waker: Waker
}

impl Task for Worker {
    fn run(own self) {
        var out = self.out
        var sum: i64 = 0
        for i in 1i64..1001 {
            sum += i
        }
        out.send(sum)
        self.waker.wake()
    }
}

fn main() {
    let net = Net.new()!
    var results = Receiver<i64>()
    _ = unsafe { Thread.spawn(Worker { out: results.sender(), waker: net.waker()! }) }
    var events = List<Event>()
    var done = false
    while not done {
        net.poll(10000, mut events)    // returns once woken, not after 10 seconds
        while let sum = results.next() {
            println("{sum}")    // out: 500500
            done = true
        }
    }
}
```

Calcite polls on its tick thread and encodes what it sends, compressing and encrypting it, on threads of its own, which wake the poll with each batch they finish.

A `Net` lasts until the program ends. It has no `close` of its own until Calx has destructors, so that a copy can never name a `Net` that was freed.

## How it works

The runtime keeps each connection's bytes: what arrived and hasn't been read, and what was written and hasn't been sent. `read` copies out of the first, and `write` appends to the second, so a program never lends a buffer to the system and gets it back later.

| Platform | Source |
| --- | --- |
| Windows | An I/O completion port: `AcceptEx`, `ConnectEx`, `WSARecv` and `WSASend`, gathered with `GetQueuedCompletionStatusEx` |
| Linux | Edge-triggered `epoll`, with `accept4` and non-blocking `recv` and `send` |

What's written is queued, and handed to the system at the next poll or flush: one `WSASend` or `send` a connection, however many writes there were. A wake is a completion posted to the port with `PostQueuedCompletionStatus` on Windows, and a write to an `eventfd` that epoll watches on Linux, made with the first `waker`. A flag set and cleared atomically makes the wakes before a poll one.

Both read up to 16 KB at a time per connection. A listening socket on Windows uses `SO_EXCLUSIVEADDRUSE`, so a second listener on the same port fails with `AddressInUse` on both platforms.

A program that doesn't import `std.net` doesn't link it.

`std.net` is written in Calx: [std/net.calx](../../std/net.calx). The sockets are in the runtime, [runtime/calx_net.c](../../runtime/calx_net.c).
