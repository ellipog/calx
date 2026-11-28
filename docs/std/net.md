---
title: std.net
description: TCP connections and UDP sockets driven by events, on IOCP on Windows, epoll on Linux and kqueue on macOS, with the buffers kept by the runtime, multicast, IP addresses and the machine's interfaces.
---

# `std.net`

TCP servers and clients, and [UDP sockets](#udp). A `Net` holds listening sockets, connections and UDP sockets, and `poll` reports what happened to them as events. Nothing else waits for the network: `write` queues bytes and returns, and `read` takes bytes that have already arrived. Another thread can wake a poll with a `Waker`, and so can a signal, so a server [stops cleanly](#stopping-at-a-signal) when it's asked to. Services on the local network are announced and found with mDNS by [`std.net.mdns`](net-mdns.md), which runs on a `Net` of its own.

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
                Datagram(_) => {}
            }
        }
    }
    println("client got {String.from_utf8(got) ?? String("?")}")    // out: client got hi
}
```

## Items

| Item | Gives |
| --- | --- |
| `Net` | Listening sockets, connections and their events. It's `Copy`: copies are the same `Net`. It's one thread's at a time, and each thread that polls has its own ([threads](#threads)) |
| `Net.new() -> Net ! NetError` | A new, empty `Net` |
| `net.listen(host: String, port: u16) -> u16 ! NetError` | Listens on `host` and `port`, and returns the port, which the system picks when `port` is 0. An empty host listens on every address, IPv4 and IPv6, with one IPv6 socket that takes IPv4 clients too; where the machine has no IPv6, on every IPv4 address |
| `net.connect(host: String, port: u16) -> Conn ! NetError` | Starts opening a connection. A poll reports `Connected` once it's open, or `Closed` if it couldn't be |
| `net.listen_local(name: String, allow: List<String> = []) ! NetError` | Listens on the [local socket](#local-sockets) `name`, which the confinements in `allow` may connect to too |
| `net.connect_local(name: String) -> Conn ! NetError` | Opens a connection to the local socket `name`. A poll reports `Connected`; `Refused` if nothing listens there |
| `net.poll(timeout_ms: i64, events: mut List<Event>)` | Waits up to `timeout_ms` milliseconds for something to happen, or forever if it's negative, and replaces what's in `events` with what happened. It returns as soon as there's an event, so `events` is empty only once the time is up: the runtime's own work, such as finishing a send or a close, doesn't end the wait |
| `net.read(conn: Conn, buf: mut List<u8>) -> usize` | Moves every byte that has arrived on the connection onto the end of `buf`, and returns how many there were |
| `net.write(conn: Conn, bytes: List<u8>)` | Queues the bytes to send. They go to the system at the next poll or `flush`, with everything else queued on the connection since the last, then as soon as the socket takes them |
| `net.send_file(conn: Conn, file: File, at: u64, n: u64) -> bool` | Queues `n` bytes of the open [`File`](fs.md), from byte `at`, after what's queued, files too. The system sends them from the file, without them passing through the program ([below](#sending-files)). `false` if the connection is closing, or the file can't be opened again |
| `net.flush()` | Sends what was queued on every connection now, rather than at the next poll |
| `net.queued(conn: Conn) -> usize` | How many bytes queued on the connection the system hasn't taken yet, a file's too, so something big can be sent in parts, the next queued once most of the last has gone. 0 once it's closed |
| `net.close(conn: Conn)` | Sends what was queued on the connection, then closes it |
| `net.drain(timeout_ms: i64) -> bool` | Waits up to `timeout_ms` milliseconds, or forever if it's negative, until what was written has gone and what was closed has closed, for a program about to end, as asyncio's `drain()` and `wait_closed()` do. `false` if the time ran out ([below](#stopping-at-a-signal)) |
| `net.wake_on_signal() ! NetError` | Wakes the poll at each signal the program handles with [`std.signal`](signal.md), which reports `Woken` ([below](#stopping-at-a-signal)) |
| `net.signalled() -> bool` | Whether a signal has woken the `Net` since `wake_on_signal`, or since `signalled` last said so, which it then forgets. Each `Net` hears of each signal, so loops on several threads each stop at one, though only one thread `take`s it ([below](#stopping-at-a-signal)) |
| `net.waker() -> Waker ! NetError` | A `Waker` for the `Net`'s polls |
| `Waker` | Wakes a `Net`'s poll from another thread. It's `Copy` and `Send`, and every copy wakes the same `Net`. A [WebSocket](http-websocket.md#waking-a-servers-loop), a [request](http.md#waking-a-servers-loop) and a [child](process.md#waking-a-servers-loop) wake one with `wake_on` when something comes |
| `waker.wake()` | Makes the poll waiting report `Woken`, or the next poll if none is waiting. Any thread can call it, except inside a `parallel for` |
| `net.peer(conn: Conn) -> Address?` | The address at the connection's other end, as it was when it opened. It stays until the poll after the connection's `Closed`, so the `Closed` can be logged with it, and is `none` for a connection that never opened |
| `Conn` | A connection. It's `Copy`, and two are equal when their `id`s are. It's the `Net`'s that opened or accepted it, and another `Net` given it panics ([threads](#threads)) |
| `conn.slot() -> usize` | A small number for the connection, counting from 0, for indexing what the program keeps per connection |
| `conn.id() -> u32`, `Conn.from_id(id: u32) -> Conn` | The connection's number, its slot and how many connections had the slot before, which no two of a `Net`'s connections share at once, and the connection with a number, in whichever `Net` it's given to, for a log of connections that's replayed. A number no connection has names none: reading it gives nothing, and writing to it or closing it does nothing |
| `Event` | `Accepted(Conn)`, `Connected(Conn)`, `Received(Conn)`, `Closed(Conn)`, `Woken`, or `Datagram(Udp)` |
| `net.bind_udp(host: String, port: u16, reuse: bool = false) -> Udp ! NetError` | A UDP socket bound to `host` and `port`, the system's choice of port for 0 ([UDP](#udp)) |
| `Udp` | A UDP socket. It's `Copy`, and copies are the same socket |
| `udp.send_to(bytes: List<u8>, to: Address) ! NetError` | Sends one datagram, at once, or as soon as the system has room |
| `udp.receive_from() -> Datagram?` | The next datagram that has arrived, or `none` once each has been taken |
| `udp.port() -> u16`, `udp.id() -> u32` | The port it's bound to, 0 once it's closed, and its number in its `Net` |
| `udp.join_multicast(group: Ip, index: u32 = 0) ! NetError`, `leave_multicast` | Joins or leaves a multicast group on the interface with that index, or the system's choice for 0 ([multicast](#multicast)) |
| `udp.set_multicast_ttl(hops: u8)`, `set_multicast_loopback(on: bool)`, `set_multicast_interface(index: u32)` | How far its multicast datagrams go, whether they come back to this machine, and which interface they leave on. Each `! NetError` |
| `udp.set_ttl(hops: u8)`, `set_broadcast(on: bool)` | How far its other datagrams go, and whether it may send to a broadcast address. Each `! NetError` |
| `udp.close()` | Closes it. What waited, either way, is dropped |
| `Datagram` | `bytes: List<u8>`, and `from: Address`, where it came from |
| `NetError` | `AddressInUse`, `BadAddress`, `Refused`, or `Os(i32)` with the system's error code |
| `Stream.new(net: Net, conn: Conn) -> Stream` | The connection as a [`std.io`](io.md#reader-and-writer) `Reader` and `Writer` that wait, as a blocking socket does ([a connection as a stream](#a-connection-as-a-stream)) |
| `Ip` | `V4([u8; 4])`, or `V6([u8; 16], u32)` with its scope, the interface a link-local address is on, or 0 |
| `Ip.parse(text: String) -> Ip?` | The address `text` writes, as Python's `ipaddress.ip_address` reads it: `192.168.1.20`, `::1`, `fe80::1%3`. `none` for anything else, a host name too |
| `ip.to_string() -> String` | The address as RFC 5952 writes it: `127.0.0.1`, `::1`, `2001:db8::8:800:200c:417a`, `fe80::1%3` |
| `ip.is_v4()`, `is_loopback()`, `is_link_local()`, `is_multicast()` | What kind of address it is: `127.0.0.0/8` and `::1` are loopback, `169.254.0.0/16` and `fe80::/10` link-local, `224.0.0.0/4` and `ff00::/8` multicast |
| `Address` | `ip: Ip` and `port: u16` |
| `address.to_string() -> String` | `127.0.0.1:25565`, or `[::1]:25565` |
| `interfaces() -> List<Interface> ! NetError` | The machine's network interfaces ([interfaces](#interfaces)) |
| `Interface` | `name: String`, `index: u32`, `addresses: List<Ip>`, `prefixes: List<u8>`, and `up`, `loopback` and `multicast`, each a `bool` |
| `host_name() -> String` | The machine's name, as Python's `socket.gethostname()` gives it |

## Events

| Event | Means |
| --- | --- |
| `Accepted(c)` | A listening socket accepted the new connection `c` |
| `Connected(c)` | The connection `c` from `connect` is open |
| `Received(c)` | Bytes arrived on `c`, and `read` takes them. A connection gets at most one `Received` per poll, however many times bytes arrived |
| `Closed(c)` | `c` ended: the other side closed it, it failed, `connect` couldn't open it, or `close` finished. It's the last event for `c` |
| `Woken` | A `Waker` woke the poll, or a signal did after `wake_on_signal`. However many wakes came before the poll reported one, it reports one `Woken` |
| `Datagram(u)` | Datagrams arrived on the UDP socket `u`, and `receive_from` takes them, one at a time. A socket gets at most one `Datagram` per poll, however many came |

What arrived before a `Closed` can still be read until the next poll. After that, `read`, `write` and `close` on the connection do nothing, and a new connection can get its slot. A `Conn` kept past then never names the new connection: the id holds a generation as well as the slot, so the runtime ignores the old one.

Bytes written before `Connected` wait until the connection opens. Bytes written to a connection that has closed are dropped.

Writes wait for the next poll so that a program that writes many times between polls, such as a server sending a tick's packets, hands the system each connection's bytes once, in one send. `flush` sends them sooner, and `close` sends the connection's first.

A `close` is graceful: the other side gets every byte queued before it, then the end of the stream, not a reset. On Windows that means cancelling the receive the runtime keeps pending, and closing the socket once it comes back, since closing it with a receive pending resets the connection.

## Sending files

`send_file` sends part of a file the way Python's `socket.sendfile` and Java's `FileChannel.transferTo` do: the system copies it from the file to the connection, so the program neither reads it nor holds it in memory, however big it is.

```calx
import std.fs.{open_read, remove_dir_all, temp_dir, write}
import std.net.{Conn, Event, Net}

// A file sent after a line saying how long it is, then the connection closed once it's gone.
fn send_whole(net: Net, c: Conn, path: String) {
    let f = open_read(path)!
    let size = f.metadata()!.size
    net.write(c, "{size}\n".to_bytes())
    if not net.send_file(c, f, 0, size) {
        net.write(c, f.read_part(0, usize.wrap(size))!)
    }
    f.close()!
    net.close(c)
}

// The program sends a file to itself.
fn main() {
    let dir = temp_dir()!
    write("{dir}/hello.txt", "Hello from a file".to_bytes())!
    let net = Net.new()!
    let port = net.listen("127.0.0.1", 0)!
    let client = net.connect("127.0.0.1", port)!
    var got = List<u8>()
    var events = List<Event>()
    var done = false
    while not done {
        net.poll(1000, mut events)
        for e in events {
            match e {
                Accepted(c) => send_whole(net, c, "{dir}/hello.txt"),
                Received(c) => {
                    _ = net.read(c, mut got)
                },
                Closed(c) => done = c.id() == client.id(),
                _ => {},
            }
        }
    }
    let lines = (String.from_utf8(got) ?? String()).lines()
    println("{lines[0]} bytes: {lines[1]}")    // out: 17 bytes: Hello from a file
    remove_dir_all(dir)!
}
```

- **In order.** The file goes after what was written before it, and what's written while it goes waits in the runtime and follows it.
- **Queued like writes.** Files queue behind each other on a connection, each after what was queued before it, and what's written between them goes between them. [`queued`](#items) counts what's left of each.
- **Its own handle.** The runtime opens the file again for itself, so the program can close the `File` as soon as `send_file` returns, and can send one `File` to many connections at once.
- **A short file.** The length is the program's to know. If the file ends before `at + n`, the connection closes where it ends, since the other side would wait for the rest forever.
- **Never stuck behind a slow client.** Windows for desktops sends only two files at once in the whole system. While both are slow, the runtime reads the others 256 KiB at a time and sends that, so it holds a part per connection but no one waits. See [How it works](#how-it-works).

## A connection as a stream

A `Stream` is a connection read and written in order, waiting as a blocking socket does, so code that takes a [`std.io`](io.md#reader-and-writer) `Reader` or `Writer` takes a connection, as Python's `socket.makefile("rwb")` makes a file of one: [`std.tar`](tar.md#a-piece-at-a-time)'s `StreamWriter` sending folders to another server through a [`std.zstd`](zstd.md#a-piece-at-a-time) `Writer`, and its `StreamReader` taking them at the other end, a piece at a time.

```calx
import std.net.{Event, Net, Stream}

fn main() {
    let net = Net.new()!
    let port = net.listen("127.0.0.1", 0)!
    let conn = net.connect("127.0.0.1", port)!
    // The other end, once it's accepted, before a stream polls this Net.
    var events = List<Event>()
    var accepted = List<Stream>()
    while accepted.is_empty() {
        net.poll(1000, mut events)
        for e in events {
            match e {
                Accepted(c) => accepted.push(Stream.new(net, c)),
                _ => {},
            }
        }
    }
    var sent = Stream.new(net, conn)
    sent.write("over the wire".to_bytes())!
    sent.flush()!
    var got = List<u8>()
    while got.len() < 13 {
        _ = accepted[0].read(mut got, 100)!
    }
    println(String.from_utf8(got)!)    // out: over the wire
    println("{sent.close(1000)}")    // out: true
}
```

| Item | Does |
| --- | --- |
| `Stream.new(net: Net, conn: Conn) -> Stream` | The connection, accepted or from `connect`, as a stream. One from `connect` that isn't open yet is waited for |
| `s.read(buf: mut List<u8>, n: usize) -> usize ! String` | Up to `n` bytes onto the end of `buf`, waiting until some arrive; 0 once the connection has closed and they've all been read |
| `s.write(bytes: List<u8>) ! String` | Queues the bytes; once more than 4 MiB is queued, waits until it has gone to the system. Fails once the connection has closed |
| `s.flush() ! String` | Waits until what's queued has gone to the system |
| `s.close(timeout_ms: i64 = -1) -> bool` | Sends what's queued and closes the connection, then waits up to `timeout_ms` for it to go, as `drain` does |
| `s.conn() -> Conn` | The connection |

- **Its `Net` is its own while it's used.** A stream polls its `Net` while it waits, and passes over what those polls say of other connections, so a thread makes a `Net` for the transfer, as a blocking socket is one thread's. Its polls, a flush's too, take every event, an `Accepted` among them, so two streams share a `Net` in one thread only once the connections are open, and while neither waits for the other, as above.
- **Memory stays bounded both ways.** A write waits once 4 MiB is queued, and the runtime reads a connection only while its `Net` is polled, which a stream does only when it has nothing left to give: a slow reader holds the sender back through TCP, rather than the bytes piling up in either.

## Local sockets

A local socket reaches a program on the same machine by name rather than by port, as Python's `socket.AF_UNIX` does, and a program [confined](process.md#confining-it) by `std.process` can reach one, where it can't reach `127.0.0.1` on Windows (CLX-480). `listen_local` listens on one, each connection is reported as `Accepted`, and it's read, written and closed as a TCP connection is, in the same loop. `connect_local` opens one.

```
let net = Net.new()!
net.listen_local("tool-a_1", ["agent-a_1"])!   // a program confined as agent-a_1 may connect
```

- **A name** is 1 to 100 letters, digits, `.`, `-` and `_`. On Windows it's the named pipe `\\.\pipe\NAME`. On Linux and macOS it's the Unix socket `/tmp/calx-local-UID/NAME`, in a folder only this user may use, and a name with a `/` in it is the socket's own path.
- **Who may connect:** this user's programs. On Windows a confined program may only if its confinement is in `allow`: the pipe's ACL lets that AppContainer read and write it, but not make instances of it, so it can't pose as the listener. On Linux and macOS any of this user's programs may, confined or not, since neither Landlock nor the macOS sandbox can tell one confinement from another there, so a server that serves several confinements checks who's asking itself, such as with a token each is given.
- **One listener a name:** a second fails with `AddressInUse`. On Linux and macOS a socket file a program that has ended left behind is taken over.
- **A connection's `peer`** is `none`.

## UDP

A UDP socket sends and receives datagrams, each whole or not at all, as Python's `socket(AF_INET, SOCK_DGRAM)` does, in the same loop as the `Net`'s connections: `bind_udp` opens one, its poll reports `Datagram` when some arrive, and `receive_from` takes each, with the address it came from, which `send_to` can answer:

```calx
import std.net.{Address, Event, Ip, Net}

fn main() {
    let net = Net.new()!
    let server = net.bind_udp("127.0.0.1", 0)!    // 0: the system picks a free port
    let client = net.bind_udp("127.0.0.1", 0)!
    client.send_to("ping".to_bytes(), Address { ip: Ip.V4([127, 0, 0, 1]), port: server.port() })!
    var events = List<Event>()
    var done = false
    while not done {
        net.poll(1000, mut events)
        for e in events {
            match e {
                Datagram(u) => {
                    while let d = u.receive_from() {
                        if u == server {
                            server.send_to("pong".to_bytes(), d.from)!    // back where it came from
                        } else {
                            println(String.from_utf8(d.bytes) ?? String("?"))    // out: pong
                            done = true
                        }
                    }
                }
                _ => {}
            }
        }
    }
}
```

- **Sent at once.** Each `send_to` is one datagram, so there's nothing to gather for the next poll, as `write` gathers a connection's bytes: it goes to the system there and then. If the system has no room for it, which is rare, it waits in the runtime, up to 1 MiB of them, and goes as soon as there's room, before any sent after it, as asyncio's `DatagramTransport` keeps them.
- **Lost without a word.** A datagram can be lost on the way, or arrive twice, or out of order, as UDP's are, and nothing tells the sender. What `send_to` fails at is what the system refuses there and then: an address of the other family, `BadAddress`, or a network it has no route to, with the system's code.
- **Kept until taken.** What arrives waits for `receive_from`, up to 1 MiB of datagrams a socket, past which more are dropped, as a socket's buffer drops them when it's full. A datagram that a datagram sent earlier was refused by, an ICMP "port unreachable", isn't an error on the next receive, as Windows would make it: the runtime turns that off.
- **Bound where it's told.** An empty host binds every address, IPv4 and IPv6, with one socket that takes both, as `listen` does; `0.0.0.0` binds every IPv4 address, and `::` every IPv6 address alone. A second socket on a port fails with `AddressInUse`, unless both ask for `reuse`, as two programs that each answer multicast on one port do ([below](#multicast)).

### Multicast

A socket that joins a multicast group gets the datagrams sent to it, as Python's `IP_ADD_MEMBERSHIP` and `IPV6_JOIN_GROUP` give them. Several sockets, in one program or many, can bind the same port with `reuse` and each get every datagram sent to the group there, which is how [mDNS](net-mdns.md) shares port 5353 with the system's own responder:

```calx
// firewall: joins a multicast group on every address
import std.net.{Address, Event, Ip, Net, interfaces}

fn main() {
    // This machine's own loopback interface where it carries multicast, as macOS's does, and the
    // system's choice, 0, where it doesn't.
    var index: u32 = 0
    for i in interfaces()! {
        if i.loopback and i.multicast and i.up {
            index = i.index
        }
    }
    let net = Net.new()!
    let group = Ip.parse("239.255.0.1")!
    let listener = net.bind_udp("0.0.0.0", 0, reuse: true)!
    listener.join_multicast(group, index)!
    let sender = net.bind_udp("0.0.0.0", 0)!
    sender.set_multicast_interface(index)!
    sender.set_multicast_ttl(1)!    // the local network only, which is where it starts
    sender.send_to("hello, group".to_bytes(), Address { ip: group, port: listener.port() })!
    var events = List<Event>()
    var heard = false
    while not heard {
        net.poll(2000, mut events)
        for e in events {
            if let Event.Datagram(u) = e {
                while let d = u.receive_from() {
                    println(String.from_utf8(d.bytes) ?? String("?"))    // out: hello, group
                    heard = true
                }
            }
        }
    }
}
```

| Option | Python's | Does |
| --- | --- | --- |
| `join_multicast(group, index)`, `leave_multicast(group, index)` | `IP_ADD_MEMBERSHIP`, `IPV6_JOIN_GROUP` and their leaves | Joins or leaves `group` on the interface whose index `index` is, or the system's choice for 0. An IPv4 group needs an IPv4 socket, or one bound to every address, and an IPv6 group an IPv6 one |
| `set_multicast_ttl(hops)` | `IP_MULTICAST_TTL`, `IPV6_MULTICAST_HOPS` | How many routers its datagrams to a group may cross: 1 to start with, the local network |
| `set_multicast_loopback(on)` | `IP_MULTICAST_LOOP`, `IPV6_MULTICAST_LOOP` | Whether they come back to the sockets on this machine that joined the group, this one too: they do to start with |
| `set_multicast_interface(index)` | `IP_MULTICAST_IF`, `IPV6_MULTICAST_IF` | The interface they leave on, by index, or the system's choice for 0. A datagram waiting for room leaves on the one it was sent on |
| `set_ttl(hops)` | `IP_TTL`, `IPV6_UNICAST_HOPS` | How many routers its other datagrams may cross |
| `set_broadcast(on)` | `SO_BROADCAST` | Whether it may send to a broadcast address, such as `255.255.255.255` |

An interface is named by its index, which [`interfaces`](#interfaces) gives, as the systems name it in multicast: Windows takes an IPv4 interface's index where an address would go, and Linux and macOS take it as it is.

On macOS a program needs the person's permission to reach the local network, as Apple's TN3179 says, multicast included: without it, a send to the network fails with `system error 65`, "no route to host". A program run from Terminal or over SSH has it, as does one run as root or by launchd as a daemon, and an app asks for it with `NSLocalNetworkUsageDescription` in its `Info.plist`. The loopback interface needs no permission, which is why the example above uses it there, and so do CI's tests, since a runner's programs are none of those.

### Interfaces

`interfaces()` gives the machine's network interfaces, each with its name, its index, its addresses and their prefix lengths, and whether it's up, the loopback, and able to multicast, as Python's `psutil.net_if_addrs()` and `net_if_stats()` give them. They change as networks come and go, so a program that keeps them asks again, as `std.net.mdns` does every 5 seconds:

```calx
import std.net.{host_name, interfaces}

fn main() {
    let list = interfaces()!
    var loopback = false
    for i in list {
        if i.loopback {
            loopback = loopback or i.addresses.any(|a| a.is_loopback())
        }
    }
    println("{loopback} {not host_name().is_empty()}")    // out: true true
}
```

On Windows the name is the one the network settings show, `Wi-Fi` or `Ethernet`, and on Linux and macOS the system's, `eth0` or `en0`. An IPv6 link-local address has its interface's index as its scope.

## Addresses

```calx
import std.net.{Net, Event}

fn main() {
    let net = Net.new()!
    let port = net.listen("127.0.0.1", 0)!
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

`Net.new`, `listen` and `connect` fail with a `NetError` ([errors](../reference/errors.md)), which `{e}` says in words, as Python does: `address already in use`, `connection refused`, or `system error 10060` with the system's code:

```calx
import std.net.{Net}

fn main() {
    let net = Net.new()!
    let port = net.listen("127.0.0.1", 0)!
    match net.listen("127.0.0.1", port) {
        ok(_) => println("listened twice"),
        err(e) => println("{e}"),    // out: address already in use
    }
    match net.connect("not an address", 25565) {
        ok(_) => println("connecting"),
        err(e) => println("{e}"),    // out: not an address, or no name server knows the host
    }
}
```

A connection that can't be opened, such as one to a port where nothing listens, isn't an error from `connect`: it's a `Closed` event, since finding out takes a round trip.

`connect` and `listen` take a host name as well as an address. A host with a character no name or address has, such as a space, is a `BadAddress` at once. A name is looked up before they return, so it waits for the name server, and only an address never waits.

## Threads

A `Net` is an event loop, and it's one thread's at a time. A program with loops on several threads gives each [thread](thread.md) a `Net` of its own, as Python gives each thread its own `asyncio.new_event_loop()`. Everything about a `Net` is its own: its sockets, its buffers, its events, and the IOCP port, epoll or kqueue its poll waits on. So `Net`s on different threads poll at once, and none waits for another, or for anything another holds:

```calx
import std.net.{Event, Net}
import std.thread.{Task}

// A loop on a thread of its own: it listens, connects to itself, and says what came back.
struct Loop {
    word: String
    out: Sender<String>
}

impl Task for Loop {
    fn run(own self) {
        var out = self.out
        let net = Net.new()!
        let client = net.connect("127.0.0.1", net.listen("127.0.0.1", 0)!)!
        var events = List<Event>()
        var got = List<u8>()
        while got.len() < self.word.len() {
            net.poll(5000, mut events)
            for e in events {
                match e {
                    Connected(c) => net.write(c, self.word.to_bytes()),
                    Received(c) => {
                        if c == client {
                            _ = net.read(c, mut got)
                        } else {
                            var bytes = List<u8>()
                            _ = net.read(c, mut bytes)
                            net.write(c, bytes)    // echo it back
                        }
                    }
                    _ => {}
                }
            }
        }
        out.send(String.from_utf8(got) ?? String())
    }
}

fn main() {
    var out = Receiver<String>()
    let a = unsafe { Thread.spawn(Loop { word: String("ping"), out: out.sender() }) }
    let b = unsafe { Thread.spawn(Loop { word: String("pong"), out: out.sender() }) }
    a.join()
    b.join()
    var words = [out.next() ?? String(), out.next() ?? String()]
    words.sort()
    println("{words}")    // out: [ping, pong]
}
```

- **What threads share.** Only what the system has one of for the whole program: its limit on open sockets, and on Windows for desktops the two `TransmitFile` calls it runs at once, whose turns the runtime hands out to every `Net` alike ([below](#how-it-works)).
- **One thread at a time.** A `Net` can move to another thread, as any value that's sent in a message or a task does, and a [`Server`](http-server.md) with it, which is then that thread's. But a `Net` is `Copy`, so two threads could hold one, and two using it at once panic, rather than tear its connections apart. Inside a [`parallel for`](../reference/parallel.md) it panics too.
- **Connections are their `Net`'s.** A `Conn` names a connection by its number in its `Net`, and the first connections of two `Net`s have the same numbers. So a `Conn` from one `Net` given to another panics, rather than read or write that `Net`'s own connection with the number. One made with `Conn.from_id` is taken by whichever `Net` it's given to.
- **Signals.** Each `Net` that `wake_on_signal` was called on is woken by each signal, on whichever thread it's polled, and `signalled` tells each that it was ([below](#stopping-at-a-signal)).
- **In a library.** A [library](../reference/libraries.md#one-runtime) has no runtime of its own, so its `Net`s are the program's runtime's, as the program's own are. A library's threads poll beside the program's, each with its own `Net`, as they would in the program. The threads run the library's code, so they end before the program `close`s it. [tests/library/net_threads.calx](../../tests/library/net_threads.calx) runs a server and a WebSocket client to it on two threads a library starts, while the program polls a `Net` of its own on another.
- **In a static library.** A [static library](../reference/embedding.md) carries its own runtime, which `calx_init` starts, and its `Net`s work the same on any thread, one the library starts or one the host does, a Rust or C app's own. [tests/library/staticlib_net_threads.calx](../../tests/library/staticlib_net_threads.calx) links one into a C program, which runs the server and the client on two of the library's threads while a thread the program starts polls a `Net` of its own.

[std.http.server](http-server.md) and [std.http.websocket](http-websocket.md) follow from that. A `Server` holds a `Net`, so a server on a thread is that thread's, and a WebSocket client needs no `Net`: its connection runs on a thread std.http.websocket starts for it, so it can be opened on any thread, a server's own or another. [tests/thread/net_server_websocket.calx](../../tests/thread/net_server_websocket.calx) runs one of each on two threads of one program, and [tests/thread/net_two_loops.calx](../../tests/thread/net_two_loops.calx) two `Net`s with a burst of traffic each at once, while a third thread waits in a poll.

Other threads talk to a `Net`'s thread through [queues](thread.md), and a `Waker` lets them say they've queued something, so the thread polling needn't wait out its timeout to find it:

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

A server can poll on its main thread and encode what it sends, compressing and encrypting it, on threads of its own, which wake the poll with each batch they finish.

A `Net` lasts until the program ends. It has no `close` of its own until Calx has destructors, so that a copy can never name a `Net` that was freed.

## Stopping at a signal

A server that's asked to stop, by Ctrl+C, by systemd's SIGTERM or by Windows shutting down, should stop taking connections, send what it owes, and close each one, rather than die with bytes still queued. [`std.signal`](signal.md)'s `handle` says the program handles those signals, and `wake_on_signal` makes each one wake the `Net`'s poll, which reports `Woken`, as asyncio's `loop.add_signal_handler` wakes its loop. The program closes its connections, and `drain` waits until what was queued has gone and they have closed, since a program that ends cuts both off:

```calx
import std.net.{Conn, Event, Net}
import std.signal.{Signal, handle, raise, take}

fn main() {
    handle()
    let net = Net.new()!
    net.wake_on_signal()!
    let port = net.listen("127.0.0.1", 0)!
    _ = net.connect("127.0.0.1", port)!
    var events = List<Event>()
    var open = List<u32>()
    var stopping = false
    while not stopping {
        net.poll(1000, mut events)
        for e in events {
            match e {
                Accepted(c) => {
                    open.push(c.id())
                    // As if Ctrl+C had been pressed.
                    _ = raise(Signal.Interrupt)
                }
                Woken => {
                    if let s = take() {
                        println("stopping on {s}")    // out: stopping on interrupt
                        stopping = true
                    }
                }
                _ => {}
            }
        }
    }
    for id in open {
        net.write(Conn.from_id(id), "bye\n".to_bytes())
        net.close(Conn.from_id(id))
    }
    println("{net.drain(5000)}")    // out: true
}
```

- **A signal wakes a poll from the thread that hears of it**, as a `Waker` would, so it comes as `Woken`, one with any other wakes before the poll, and `take` says which signal it was. A `Net` can be woken by signals and `Waker`s both.
- **Loops on several threads** each call `wake_on_signal` on their own `Net`, and each is woken by each signal. But a signal is taken once for the whole program, so a loop that asks `take` or `pending` whether one came can find that another thread took it first, and miss it. Each asks its own `Net` instead: `signalled()` says whether a signal woke that `Net` since it last said so ([tests/thread/net_signal_two.calx](../../tests/thread/net_signal_two.calx)).
- **`drain(timeout_ms)`** waits up to that long, or forever if it's negative, until what was written to every connection has gone to the system, and each connection `close` closed has closed: on Windows that's once the receive the runtime cancels comes back. `false` if the time ran out first. Events that come meanwhile wait for the next poll.
- **[`std.http.server`](http-server.md#stopping-cleanly)** does all of this for a server with `wake_on_signal` and `shutdown`.

## How it works

The runtime keeps each connection's bytes: what arrived and hasn't been read, and what was written and hasn't been sent. `read` copies out of the first, and `write` appends to the second, so a program never lends a buffer to the system and gets it back later.

| Platform | Source |
| --- | --- |
| Windows | An I/O completion port: `AcceptEx`, `ConnectEx`, `WSARecv`, `WSASend`, `TransmitFile`, `ReadFile`, `WSARecvFrom` and `WSASendTo`, gathered with `GetQueuedCompletionStatusEx` |
| Linux | Edge-triggered `epoll`, with `accept4`, and non-blocking `recv`, `send`, `sendfile`, `recvfrom` and `sendto` |
| macOS | A `kqueue` with `EV_CLEAR`, which is edge-triggered too, with the same calls |

What's written is queued, and handed to the system at the next poll or flush: one `WSASend` or `send` a connection, however many writes there were. A wake is a completion posted to the port with `PostQueuedCompletionStatus` on Windows, and a write to an `eventfd` that epoll watches on Linux, made with the first `waker`. A flag set and cleared atomically makes the wakes before a poll one.

A file goes once what was queued before it has gone, at most 1 GiB a call. On Windows the runtime opens it again with `ReOpenFile`, overlapped, so the offset it gives is the one used: a handle copied with `DuplicateHandle` would share the program's file position, which `TransmitFile` reads from, and two connections sent one file at once cut each other short. On Linux a `dup` does, since `sendfile` is given its own offset. What's written while a file goes, and files queued behind it, wait in the runtime and take their turns. On Windows they often have to: `TransmitFile` can say it's done after the other end has every byte and has asked for more. A connection whose other end has gone ends with a `Closed` event, never the program: `send` is given `MSG_NOSIGNAL`, and `sendfile`, which has no such flag, runs with `SIGPIPE` blocked, which it takes if the call raised it.
Windows editions for desktops, rather than servers, run two `TransmitFile` calls at once in the whole system and queue the rest, so a connection queued behind two slow clients would get nothing until they were done. There the runtime takes turns: two calls at a time, each of at most 4 MiB. A connection that finds both turns taken reads 256 KiB of its file with an overlapped `ReadFile`, sends that with `WSASend`, and asks for a turn again before the next part. It takes a third turn anyway when the last call took at most 50 ms and ended in the last 100 ms, since a call that short won't wait long. Editions for servers and Linux never use parts. `CALX_NET_FILE_PARTS=1` in the environment makes Windows send every file in parts, so tests can run that path on any edition.

Each `Net` has its own port, epoll or kqueue, and its own slots, buffers and events, and the TransmitFile turns, counted atomically, are all the `Net`s share. Each call of a `Net`'s holds it with a flag that's set and cleared atomically, which is how a second thread in it at once is found and panics, and which also hands what one thread did to the next to hold it. A `Waker`'s wake holds nothing, since any thread may wake a `Net`. A `Conn` holds its `Net`'s number, which std.net checks. The `Net`s a signal wakes are in a list that's locked while one is added, so two threads asking at once each get their place.

Both read up to 16 KB at a time per connection. A listening socket on Windows uses `SO_EXCLUSIVEADDRUSE`, so a second listener on the same port fails with `AddressInUse` on both platforms.

A UDP socket is a slot of the same `Net` as its connections. On Windows it keeps an overlapped `WSARecvFrom` pending into a buffer of 64 KiB, and on Linux and macOS it reads with `recvfrom` until the system has nothing left, each datagram kept with its length and where it came from until `receive_from` takes it. A send is a `sendto` there and then, on a socket that doesn't block; one the system has no room for waits, with where it goes and the multicast interface it was sent on, and goes when epoll or kqueue says there's room, or on Windows by an overlapped `WSASendTo` at a time. On Windows the socket has `SIO_UDP_CONNRESET` and `SIO_UDP_NETRESET` off, so an ICMP error that a send brought back doesn't end the next receive, and `reuse` is `SO_REUSEADDR` where a socket without it has `SO_EXCLUSIVEADDRUSE`. On macOS `reuse` is `SO_REUSEPORT` too, as sharing a port there needs. On Linux a socket has `IP_MULTICAST_ALL` off, so it gets only the groups it joined, as on the other systems, rather than every group any socket on the machine joined. An interface's addresses come from `GetAdaptersAddresses` on Windows and `getifaddrs` elsewhere.

A program that doesn't import `std.net` doesn't link it.

`std.net` is written in Calx: [std/net.calx](../../std/net.calx). The sockets are in the runtime, [runtime/calx_net.c](../../runtime/calx_net.c).
