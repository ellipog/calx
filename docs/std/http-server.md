---
title: std.http.server
description: A small HTTP/1.1 server for scripts, as Python's http.server is, which hands a program each request in turn on std.net's event loop and serves a folder as python -m http.server does.
---

# `std.http.server`

A small HTTP/1.1 server for scripts, as Python's `http.server` is. A `Server` takes requests from as many connections as come and hands them to the program one at a time, as `next` gives them. The program answers each with `respond`. `serve_dir` serves a folder's files, as `python -m http.server` does, and [`calx serve`](../reference/command.md#serving-a-folder) runs it from the command line. It also [pushes to a browser](#pushing-to-a-browser) as things happen: over a WebSocket, as Python's `websockets.serve` does, or as a stream of events, which a browser's `EventSource` reads.

It listens on loopback, `127.0.0.1`, unless it's given another address. So only programs on the same machine reach it, and Windows' firewall doesn't ask about it.

```calx
import std.http.{Header, get}
import std.http.server.{Response, Server}

fn main() ! Error {
    var server = try Server.listen(0)    // loopback, on a port the system picks
    // A request to it from std.http's client, in the same program, to show it answering.
    var r = get("{server.url()}hello?name=Ada", List<Header>(), 5000, 1024)
    while not r.done() {
        if let req = server.next(10) {
            let name = if req.query.starts_with("name=") { req.query[5..].copy() } else { String("you") }
            server.respond(req, Response.text("hello, {name}, from {req.path}"))
        }
    }
    let resp = try r.wait()
    println("{resp.status} {String.from_utf8(resp.body) ?? String()}")    // out: 200 hello, Ada, from /hello
}
```

A script's own server is a loop: it takes each request, works out the response, and sends it. Here notes are posted to `/notes`, and the files in `public` are served for everything else:

```
import std.http.server.{Response, Server, from_dir}

fn main() ! Error {
    var server = try Server.listen(8000)
    println("open {server.url()}")
    var notes = List<String>()
    loop {
        if let req = server.next() {
            if req.path == "/notes" and req.method == "POST" {
                notes.push(req.text() ?? String())
                server.respond(req, Response.text("{notes.len()} notes", 201))
            } else if req.path == "/notes" {
                server.respond(req, Response.text(notes.join("\n")))
            } else {
                server.respond(req, from_dir("public", req))
            }
        }
    }
}
```

## Items

| Item | Gives |
| --- | --- |
| `Server.listen(port: u16, host: String = "127.0.0.1") -> Server ! NetError` | A server listening on `port` of `host`. 0 lets the system pick a port, and `""` listens on every address, IPv4 and IPv6, as `python -m http.server` does |
| `server.port: u16`, `server.host: String` | The port and address it listens on |
| `server.max_body: usize` | The longest body it takes, in bytes: 64 MB to start with. A longer one gets `413 Content Too Large` |
| `server.log: bool` | Whether `serve` prints where it's serving and logs each request to standard error, as Python does: `true` to start with |
| `server.url() -> String` | The address to open in a browser: `http://127.0.0.1:8000/` |
| `server.next(timeout_ms: i64 = -1) -> Request?` | The next request, from whichever connection has one, waiting up to `timeout_ms` milliseconds for it, or forever if it's negative. `none` if none came in that time |
| `server.respond(req: Request, res: Response)` | Sends `res` as the answer to `req` |
| `server.serve(dir: String)` | The folder `dir` served on this server until the program is stopped, as `serve_dir` serves it, with its files opened on worker threads and big ones sent by the system from the file ([below](#serving-a-folder)) |
| `server.wake_on_signal() ! NetError` | Stops `next`, `next_event` and `serve` at each signal the program handles with [`std.signal`](signal.md): the first two give `none` while one is pending, and once after each, whichever thread takes it, and `serve` returns ([below](#stopping-cleanly)) |
| `server.waker() -> Waker ! NetError` | A [`std.net.Waker`](net.md) another thread can call `wake()` on, so `next` or `next_event` gives `none` at once, after what's ready, rather than at its timeout ([below](#waking-the-loop-from-another-thread)). WebSocket clients, requests and programs the loop started wake it with it too ([below](#clients-requests-and-programs-on-the-same-loop)) |
| `server.shutdown(timeout_ms: i64 = 5000)` | Closes every connection cleanly, for a server that's stopping, waiting up to `timeout_ms` for what each was sent to go, WebSockets to close with 1001 and files to finish ([below](#stopping-cleanly)) |
| `Request` | `method`, `GET` or `POST` or another, as it was sent. `target`, the path and query still percent-encoded: `/find?q=a%20b`. `path`, decoded, without the query: `/find`. `query`, what's after the `?`, as it was sent. `version`, `HTTP/1.1` or `HTTP/1.0`. `headers: List<Header>`, in the order they came. `body: List<u8>`. And `client`, the address it came from |
| `req.header(name: String) -> String?` | The first header named `name`, in any case |
| `req.text() -> String?` | The body as text, or `none` if it isn't UTF-8 |
| `Response` | `status: u16`, `headers: List<Header>` and `body: List<u8>` |
| `Response.new(status: u16, type_name: String, body: List<u8>) -> Response` | A response with `body`, whose `Content-Type` is `type_name` |
| `Response.text(body: String, status: u16 = 200)`, `html`, `json` | A response with `body` as `text/plain; charset=utf-8`, `text/html; charset=utf-8` or `application/json` |
| `Response.error(status: u16, message: String = "")` | A short page saying what went wrong, `404 Not Found`, with `message` under it, escaped |
| `Response.redirect(location: String, status: u16 = 302)` | A redirect to `location` |
| `res.with_header(name: String, value: String) -> Response` | The response with one more header |
| `from_dir(dir: String, req: Request) -> Response` | What Python's `SimpleHTTPRequestHandler` answers `req` with from the folder `dir` ([below](#serving-a-folder)) |
| `serve_dir(dir: String, port: u16 = 8000, host: String = "127.0.0.1") ! NetError` | The folder `dir` served until the program is stopped, as `python -m http.server` serves the folder it's started in, with each request logged to standard error as Python logs it |
| `content_type(name: String) -> String` | The `Content-Type` of a file from its name's extension: `text/html; charset=utf-8` for `.html`, `image/png` for `.png`, and `application/octet-stream` for one it doesn't know |
| `reason(status: u16) -> str` | The reason phrase for a status, as RFC 9110 names it: `Not Found` for 404, or `""` for one it doesn't name |

For [pushing to a browser](#pushing-to-a-browser):

| Item | Gives |
| --- | --- |
| `server.next_event(timeout_ms: i64 = -1) -> ServerEvent?` | The next thing that happens, waiting as `next` does: a request, a WebSocket's message or close, or an event stream's end |
| `ServerEvent` | `Request(Request)`, `Message(ServerSocket, Message)`, `Closed(ServerSocket, u16, String)`, with the code and reason it ended with, or `Ended(EventStream)` |
| `server.upgrade(req: Request, protocols: List<String> = [], origins: List<String> = []) -> ServerSocket ! UpgradeError` | `req` answered by making its connection a WebSocket, with the first of `protocols` the request offers. A page from another site is turned away unless its origin is one of `origins` ([below](#websockets)) |
| `server.send_text(s: ServerSocket, text: String) -> bool`, `send_binary(s, bytes: List<u8>) -> bool` | Sends a message. `false` if the socket has closed or is closing, so nothing was sent |
| `server.close_socket(s: ServerSocket, code: u16 = 1000, why: String = "")` | Closes the socket with `code` and the reason `why`, once the client answers, or after 5 seconds |
| `server.is_open(s: ServerSocket) -> bool`, `server.protocol(s) -> String?` | Whether neither side has closed the socket, and the subprotocol `upgrade` chose |
| `server.max_message: usize` | The longest message it takes from a WebSocket: 64 MB to start with. A longer one closes the socket with 1009 |
| `server.ping_interval_ms: u64`, `server.ping_timeout_ms: u64` | How often it pings each open WebSocket, and how long a ping waits for its pong before the socket is closed with 1011: 20 seconds each to start with, as Python's `websockets.serve` does. 0 turns pings off, or waits forever ([below](#keeping-sockets-alive)) |
| `server.stream(req: Request, headers: List<Header> = []) -> EventStream` | `req` answered with a stream of events, `text/event-stream`, kept open for `send_event` ([below](#event-streams)) |
| `server.send_event(s: EventStream, data: String, event: String = "", id: String = "") -> bool` | Sends an event: `data`, and the `event` type and `id` if they aren't empty. `false` if the stream has ended |
| `server.end_stream(s: EventStream)` | Ends the stream, closing its connection |
| `ServerSocket`, `EventStream` | A WebSocket or an event stream. Each can be copied, compared and kept in a list, and `s.id() -> u32` is a key for a map |
| `UpgradeError` | Why `upgrade` didn't make a WebSocket: `NotWebSocket`, `Version`, `Origin(String)` or `Gone`. `{e}` says it in words |

`Header` is [std.http](http.md)'s, `{ name: String, value: String }`, and `Message` is [std.http.websocket](http-websocket.md)'s, `Text(String)` or `Binary(List<u8>)`.

## How requests come

The server runs on [std.net](net.md)'s event loop, in the thread that calls `next`. `next` reads what has come on every connection, and hands out each request once all of it is there.

- **One answer each.** A request gets one `respond`. Until it does, the next request on the same connection waits, while other connections' requests still come. A program can keep a request and answer it later, after `next` has given others.
- **Kept open.** An HTTP/1.1 connection stays open for the next request, as browsers expect. It's closed after the answer if the request says `Connection: close`, if it came by HTTP/1.0 without `keep-alive`, or if the response has a `Connection: close` header. Requests sent one after another without waiting, as pipelining does, are answered in order.
- **Bodies.** A body comes by `Content-Length` or in chunks, and the request's `body` has all of it, with chunked trailers left out. An HTTP/1.1 request that sends `Expect: 100-continue` is told `100 Continue` once its headers are in, so the client sends the body.
- **What `respond` adds.** It adds `Content-Length` unless the response has one or the status has no body (1xx, 204 and 304). It also adds `Date` and `Server: Calx`. A `HEAD` request gets the headers without the body. Line breaks in a header's name or value are taken out, so a value from a request can't start a header of its own.

Some requests are answered by the server itself, without coming from `next`, and their connections are closed:

| Status | When |
| --- | --- |
| `400 Bad Request` | The request line isn't UTF-8, isn't a method, a target and `HTTP/` with two numbers, or is HTTP/0.9. Also a header line with no `:`, a `Content-Length` that isn't a number, or chunks that aren't written right |
| `413 Content Too Large` | The body is longer than `max_body` |
| `414 URI Too Long` | The request line is longer than 64 KB |
| `431 Request Header Fields Too Large` | A header line is longer than 64 KB, or there are more than 100 |
| `505 HTTP Version Not Supported` | HTTP/2 or later, which this server doesn't speak |

## Waking the loop from another thread

A loop that also takes what worker threads send it, through a `Receiver`, would otherwise hear of each only at `next_event`'s timeout: up to 10 ms with `next_event(10)`, and about 15.6 ms on Windows, whose timer is that coarse. A worker that wakes the server after each send is heard at once, so the loop can wait as long as it likes, as Python's asyncio hears `loop.call_soon_threadsafe`:

```calx
import std.http.server.{Server}
import std.net.{Waker}
import std.thread.{Task}

struct Worker {
    out: Sender<String>
    waker: Waker
}

impl Task for Worker {
    fn run(own self) {
        var out = self.out
        out.send("done")
        self.waker.wake()
    }
}

fn main() ! Error {
    var server = try Server.listen(0)
    var results = Receiver<String>()
    _ = unsafe { Thread.spawn(Worker { out: results.sender(), waker: try server.waker() }) }
    loop {
        // Waits until a request or a message comes, or the worker wakes it.
        _ = server.next_event()
        if let r = results.next() {
            println(r)    // out: done
            break
        }
    }
}
```

A wake makes `next` or `next_event` give `none` once what's ready has been given, or makes the next call give it if none is waiting. Wakes that come before a call are one, and every copy of a `Waker` wakes the same server.

### Clients, requests and programs on the same loop

A loop that serves and also reads WebSocket clients it opened, waits for requests it started, or talks to a program it started, gives each of them the server's `Waker` with `wake_on`. Each then wakes `next_event` when something comes, so the loop needn't look at them every few milliseconds, as Python's `selectors` watches every socket and pipe it's given:

| Item | Wakes the server |
| --- | --- |
| [`ws.wake_on(waker)`](http-websocket.md#waking-a-servers-loop) | Each time a message comes, and as the connection opens or ends |
| [`r.wake_on(waker)`](http.md#waking-a-servers-loop) | As the request finishes |
| [`child.wake_on(waker)`](process.md#waking-a-servers-loop) | Each time output or errors come, as its output ends, and as it exits |

Here the server answers its own request, on one thread:

```calx
import std.http.{get}
import std.http.server.{Response, Server}

fn main() ! Error {
    var server = try Server.listen(0)
    server.log = false
    var r = get("http://127.0.0.1:{server.port}/hello", [], 10000, 1 << 20)
    r.wake_on(try server.waker())
    while not r.done() {
        // Waits for a request, or for `r` to finish.
        if let Request(req) = server.next_event() {
            server.respond(req, Response.text("hi"))
        }
    }
    println(String.from_utf8(r.wait()!.body) ?? "")    // out: hi
}
```

Each wakes it at once if what it says has already come. Wakes that come before a call are one, so after `next_event` gives `none`, the loop takes everything that's there: every message with `receive(0)`, every line with `read_line(0)`.

## Pushing to a browser

A page that shows what's happening, such as a server's load or a build's progress, needn't ask again and again. It opens a connection that stays open, and the server sends on it as things happen. `next_event` gives what comes back, beside the requests, so one loop does both:

```calx
import std.http.{Header, get}
import std.http.server.{Server}
import std.http.websocket.{Message, open}

fn main() ! Error {
    var server = try Server.listen(0)
    // A browser's EventSource, played by std.http's client, which takes the whole stream.
    var r = get("{server.url()}ticks", List<Header>(), 5000, 1024)
    while not r.done() {
        if let req = server.next(10) {
            let s = server.stream(req)
            for i in 1..4 {
                _ = server.send_event(s, "tick {i}")
            }
            server.end_stream(s)
        }
    }
    let resp = try r.wait()
    println((String.from_utf8(resp.body) ?? String()).trim().replace("\n\n", " | "))    // out: data: tick 1 | data: tick 2 | data: tick 3

    // A WebSocket, with std.http.websocket's client on a thread of its own.
    let ws = open("ws://127.0.0.1:{server.port}/live")
    try ws.send_text("ping")
    loop {
        match server.next_event(10) {
            some(Request(req)) => {
                let s = try server.upgrade(req)
                println("{server.is_open(s)}")    // out: true
            },
            some(Message(s, Text(t))) => {
                _ = server.send_text(s, "{t} back")
                server.close_socket(s)
            },
            _ => {},
        }
        if let Message.Text(t) = try ws.receive(0) {
            println(t)    // out: ping back
            break
        }
    }
    ws.close()
}
```

A dashboard keeps the sockets it has, and sends to each in turn. `next_event` waits at most a second, so the numbers go out every second whether or not anything came:

```
import std.http.server.{Response, Server, ServerSocket, from_dir}

fn main() ! Error {
    var server = try Server.listen(8000)
    var live = List<ServerSocket>()
    loop {
        match server.next_event(1000) {
            some(Request(req)) => {
                if req.path == "/live" {
                    match server.upgrade(req) {
                        ok(s) => live.push(s),
                        err(e) => eprintln("{req.client}: {e}"),
                    }
                } else {
                    server.respond(req, from_dir("public", req))
                }
            },
            some(Closed(s, _, _)) => live.retain(|x| x != s),
            _ => {},
        }
        let stats = read_stats()    // the program's own
        for s in live {
            _ = server.send_text(s, stats)
        }
    }
}
```

In the page, `new WebSocket("ws://" + location.host + "/live")` connects, and its `onmessage` gets each message. An event stream is `new EventSource("/live")`, with `onmessage`, or `addEventListener(type, ...)` for events sent with an `event` type.

### WebSockets

`upgrade` answers a WebSocket handshake (RFC 6455) with `101 Switching Protocols`, and from then on the connection carries messages.

- **Messages.** Each comes whole from `next_event`, as `Message(s, Text(t))` or `Message(s, Binary(b))`, its frames joined. Text is checked to be UTF-8. `send_text` and `send_binary` send one as a single frame, unmasked, as a server sends it. A message longer than `max_message` closes the socket.
- **Pings and closes.** The server answers a client's ping with a pong of the same bytes, and a client's close with the same code, then closes the connection. It pings each client too, and closes one that doesn't answer ([below](#keeping-sockets-alive)). `close_socket` sends a close and waits for the client's answer, for 5 seconds at most.
- **The end.** Each socket ends with one `Closed`, which carries the code it ended with. That's the client's, or the server's own when the client broke the protocol, and the client is told so in a close:

| Code | When |
| --- | --- |
| The client's, such as `1000` or `1001` | The client closed it, or answered `close_socket`. `1005` is a close that gave no code |
| `1002` | A frame that RFC 6455 doesn't allow from a client: one that isn't masked, has a reserved bit set or an unknown opcode, or is a control frame in pieces or longer than 125 bytes. Also a message's next frame without its first, a new message before the last one ended, or a close with a code a close can't carry |
| `1007` | Text, or a close's reason, that isn't UTF-8 |
| `1009` | A message longer than `max_message` |
| `1011` | The client didn't answer a ping within `ping_timeout_ms`. Its reason is `keepalive ping timeout` |
| `1006` | The connection ended without a close, or the client didn't answer `close_socket` in 5 seconds |

- **Who may connect.** A page in any browser tab can open a WebSocket to any address, a server on loopback included, and its browser sends the page's `Origin`. So `upgrade` turns away a page from another site with `403 Forbidden`, unless its origin is one of `origins`, compared without case, or `origins` has `"*"`. A request with no `Origin` isn't from a page, and a page served by this server has the server's own address as its origin, so both are let in. An origin of `null`, from a page opened as a file, is another site's.
- **Handshakes it turns away.** A request that isn't a WebSocket handshake gets `400 Bad Request`, and one for a version other than 13 gets `426 Upgrade Required`, with `Sec-WebSocket-Version: 13`. `upgrade` fails with why in each case, so the program can log it.
- **Subprotocols.** The client offers them in `Sec-WebSocket-Protocol`. `upgrade` takes the first of `protocols` that the client offered, in the server's order, and `protocol(s)` says which. With none offered or none in common, there's none.
- **No extensions.** It agrees to none, so messages aren't compressed.

### Keeping sockets alive

A client on a phone or a laptop can go without a close: the laptop sleeps, or the Wi-Fi drops. Nothing comes to say so, and without pings the server would keep the socket, and whatever the program queues for it, until the system notices, which can take minutes or hours. So the server pings each open WebSocket every `ping_interval_ms`, and closes one whose client hasn't answered within `ping_timeout_ms`, as Python's `websockets.serve(ping_interval=20, ping_timeout=20)` does:

- **When.** A socket's first ping goes out `ping_interval_ms` after `upgrade`, and each one after that goes `ping_interval_ms` after the last was answered. Each ping carries 4 bytes, and only a pong with the same bytes answers it. A browser answers pings by itself, and so does [std.http.websocket](http-websocket.md)'s client.
- **No answer.** A ping that has waited `ping_timeout_ms` closes the socket: the server sends a close with `1011` and `keepalive ping timeout`, in case the client is still there, closes the connection at once, and `next_event` gives the socket's `Closed` with that code and reason.
- **In the loop.** Pings go out and time out while `next` or `next_event` waits, which wake for them, so a loop that calls either often keeps its sockets alive. A pong that came while the program was busy counts, since the server reads what came before it looks at the time.
- **Setting them.** Both are 20 seconds to start with, and can be changed at any time; the next poll goes by the new values. A `ping_interval_ms` of 0 sends no pings, and a `ping_timeout_ms` of 0 keeps pinging but never closes a socket for not answering.

A server for clients that may sleep for a while, with pings every 20 seconds and a minute to answer:

```
var server = try Server.listen(8000)
server.ping_interval_ms = 20000
server.ping_timeout_ms = 60000
```

### Event streams

`stream` answers a request with `200 OK` and `Content-Type: text/event-stream`, with no length, and the connection stays open for events until `end_stream` or the browser goes. The connection is the stream's alone, and closes when it ends.

- **Events.** `send_event(s, data)` sends `data` as `data:` lines, one for each line of it, which the browser joins with `\n`. With `event`, the event has that type, which `addEventListener` listens for, and with `id`, the browser keeps that id and sends it as `Last-Event-ID` when it connects again, so the server can send what it missed.
- **The end.** Each stream ends with one `Ended` from `next_event`: when the browser closes the connection, or `end_stream` ends it. `send_event` gives `false` after.
- **Headers.** `stream` sends `Cache-Control: no-cache` and the headers it's given. A `HEAD` request gets the head, and its stream ends at once.

An event stream goes one way, from the server, over plain HTTP, and a browser connects again by itself when it's cut off. A WebSocket goes both ways. A browser keeps at most six HTTP/1.1 connections to one server, so a page should open one stream, not one for each thing it shows.

## Serving a folder

`from_dir(dir, req)` answers as Python's `SimpleHTTPRequestHandler` does:

- **The file.** The request's path is decoded and found under `dir`. A `.` is skipped, a `..` goes up a folder but never above `dir`, and on Windows a part with `\` or `:` is left out, so nothing outside `dir` is served. A path with a zero byte gets `404`.
- **A folder.** A request for a folder without a `/` at its end is redirected to the path with one, with `301 Moved Permanently`, as Python does, so links in the page work. Then its `index.html` or `index.htm` is served, if it has one. Otherwise it gets a page listing what's in it, sorted by name without case, with a `/` after each folder's name and an `@` after each link's.
- **Headers.** A file comes with its `Content-Type`, from `content_type`, and its `Last-Modified` time. It gets `304 Not Modified` if the request's `If-Modified-Since` is the same or later, unless the request has an `If-None-Match`.
- **Errors.** A file or folder that isn't there gets `404 File not found`. A method other than `GET` and `HEAD` gets `501`.

`serve_dir`, and `serve` on a server of the program's own, answer as `from_dir` does, but not one at a time:

- **Worker threads.** Files are opened, looked at and read on worker threads, one for each processor up to 8, which hand each answer back to the event loop. So many requests are answered at once, and a slow disk holds up only the requests waiting on it.
- **Big files from the file.** A file of 64 KiB or more isn't read. Its headers go first, then the system sends its bytes from the file, with [`Net.send_file`](net.md#sending-files), so the memory a download takes doesn't grow with the file. A request that comes behind it on the same connection is answered after it, its file queued behind the first. A `HEAD` request gets the file's length without its bytes being read, from `from_dir` too.

They print a first line saying where, and log each request to standard error as Python's `log_message` does, unless `log` is `false`:

```text
Serving HTTP on 127.0.0.1 port 8000 (http://127.0.0.1:8000/) ...
127.0.0.1 - - [08/Nov/2026 12:55:25] "GET / HTTP/1.1" 200 -
```

## Stopping cleanly

A server that's asked to stop, by Ctrl+C, by systemd's SIGTERM or by Windows shutting down, should stop taking requests, answer the ones it has, and close its connections, rather than die halfway through an answer. [`std.signal`](signal.md)'s `handle` says the program handles those signals. After `wake_on_signal`, a signal wakes the server's poll, and `next` and `next_event` give `none` while one is pending, so the loop ends. `shutdown` then closes every connection cleanly, as Python's `server_close()` does after `shutdown()`:

```calx
import std.http.{Header, get}
import std.http.server.{Response, Server}
import std.signal.{Signal, handle, raise, take}

fn main() ! Error {
    handle()
    var server = try Server.listen(0)
    try server.wake_on_signal()
    var r = get("{server.url()}hi", List<Header>(), 5000, 1024)
    while let req = server.next() {
        server.respond(req, Response.text("hello"))
        // As if Ctrl+C had been pressed once it had answered.
        _ = raise(Signal.Interrupt)
    }
    println("stopping on {take() ?? Signal.Hangup}")    // out: stopping on interrupt
    server.shutdown()
    let resp = try r.wait()
    println("{resp.status}")    // out: 200
}
```

- **`shutdown(timeout_ms: i64 = 5000)`.** Each connection sends what it was sent before it closes, and a file being sent finishes. A WebSocket is closed with 1001, going away, and waits for the client's close, and an event stream ends. A request handed out and not answered gets no answer, and a connection that comes meanwhile is closed at once. It waits up to `timeout_ms` milliseconds for all of that, or forever if it's negative, then closes what's left, and waits for it to have closed, with [`Net.drain`](net.md#stopping-at-a-signal). The sockets' `Closed` and the streams' `Ended` wait for `next_event`, which gives them once the signal's taken.
- **`serve` and `serve_dir`.** After `wake_on_signal`, a signal stops `serve`: it takes no more requests, waits for its workers to answer those they have, calls `shutdown`, and returns. `serve_dir` doesn't handle signals, so Ctrl+C ends it at once, as it ends `python -m http.server`.
- **Without a signal.** `shutdown` works at any time, for a server a program stops for its own reasons.
- **Servers on several threads.** Each that `wake_on_signal` was called on stops at one signal, though only one thread `take`s it: each server's `Net` hears of it itself, with [`Net.signalled`](net.md#stopping-at-a-signal), so a server doesn't need it to still be pending when its thread looks.

## Where it differs from Python

- **Loopback.** It listens on `127.0.0.1` unless it's given another address, where `python -m http.server` listens on every address.
- **Threads.** A program's own loop gets requests one at a time, in the thread that calls `next`, so a slow answer holds up the rest. `serve_dir` reads files on up to 8 worker threads. Python's `ThreadingHTTPServer`, which `python -m http.server` runs, gives each connection a thread. A server is one thread's at a time, as its [`Net`](net.md#threads) is, so a program with servers on several threads makes one on each, and they serve at once; a server can be made on one thread and moved to another, and a [WebSocket client](http-websocket.md) to it can be on any thread.
- **Kept open.** It speaks HTTP/1.1 and keeps connections open. Python's handler speaks HTTP/1.0 unless it's set otherwise, and closes each connection after one answer.
- **Types.** They come from a table of 52 extensions, the same on every machine, and text types are given with `; charset=utf-8`. Python's come from `mimetypes`, which on Windows also reads the registry, so they can differ from machine to machine.
- **Whole files from `from_dir`.** `from_dir` reads a file whole, so a program's own loop needs a file's size in memory to send it. `serve_dir` has the system send files of 64 KiB or more from the file.
- **No HTTPS.** Python 3.14's `--tls-cert` has no match here yet.
- **A zero byte.** A path with `%00` in it gets `404`. Python's handler raises an error at it and closes the connection without an answer.
- **Pages.** Error pages and folder listings are its own short HTML, with the same statuses, links and names as Python's.
- **WebSockets in the loop.** Python's `websockets.serve` runs a coroutine for each connection. Here each socket's messages come from `next_event`, in the one loop, with the requests.
- **Origins.** `websockets.serve` lets a page from any site connect unless it's given `origins`. `upgrade` lets in only the server's own pages and requests that aren't from a page, unless it's given more, since any page a browser has open can reach a server on loopback.
- **Pings.** Both ping each client every 20 seconds to start with, and close one that doesn't answer within 20 seconds. `ping_interval` and `ping_timeout` are seconds, as floats, and `None` turns them off; here they're `ping_interval_ms` and `ping_timeout_ms`, and 0 turns them off. Both send a close with 1011 to a client that didn't answer, but where a `websockets` handler sees `1006`, as no close came back, `next_event` gives `1011` and `keepalive ping timeout`, so a program can tell a client that stopped answering from one whose connection broke.

## Checked against Python

The [tests](../../tests/std/http_server.calx) send requests to a server in the same program, over [std.net](net.md) on loopback. They check:

- bodies by `Content-Length` and in chunks, and `100 Continue`;
- keep-alive, pipelining, `HEAD` and HTTP/1.0;
- each request it refuses;
- `from_dir` on a folder of its own.

`serve_dir` was also run beside `python -m http.server` 3.14 on the same folder. Both were sent the same 65 requests, and their answers compared:

- **Paths:** files with spaces, `%` and Japanese in their names, folders with and without a `/` and an index, `..`, `//`, `%2f` and `%5c`, `C:/` and missing files.
- **Requests:** `HEAD`, `POST` and `PUT`, `If-Modified-Since` before, after and alongside `If-None-Match`, and broken request lines.
- **Compared:** status, `Location`, `Content-Length`, `Last-Modified`, the type, the body, and each listing's links and names.

They answered the same to all but `/%00`, where Python's handler fails.

[tests/std/http_server_push.calx](../../tests/std/http_server_push.calx) checks the pushes with a client of its own on std.net, which sends what a browser would and frames no browser should:

- **The handshake:** RFC 6455's own example key, whose answer the RFC gives, and the subprotocol chosen.
- **Messages:** echoes, a ping, a message in three frames with a ping between them, and binary messages whose lengths take 16 and 64 bits.
- **Closes:** closes from either side, a client that goes without a close, and a close the client never answers, which ends after 5 seconds.
- **What's turned away:** each frame that breaks the protocol, sent right behind the handshake, a message past `max_message` whole and in parts, and handshakes refused by version and by origin.
- **Event streams:** events with types, ids and lines ending in `\r\n`, `\r` and `\n`, a browser that goes, `end_stream`, and `HEAD`.
- **Its own client:** [std.http.websocket](http-websocket.md)'s client, on a thread of its own, against the server.

On Linux, headless Chrome 157 also loaded a page from the server whose script opened a WebSocket and an `EventSource`, and the two agreed on every point:

- the subprotocol;
- text with `é` in it, a message of 70,000 characters, and binary, both ways;
- the server's close with its code and reason, which Chrome called clean, and Chrome's own close;
- events of several lines, with their types and ids;
- the stream's end when the page closed it;
- a page from `127.0.0.1` turned away at `localhost`, which is another origin.

## Speed

These numbers are from our benchmark, so take them as rough:

- **Machine:** an i9-13900K with other work running, under Windows 11 and under Linux in WSL 2. The server was pinned to the performance cores and the load tool to the efficiency cores. On Linux the same processor numbers were used, though WSL needn't keep them on those cores. Each number is the better of two rounds, taken in turn.
- **Load:** 16 connections kept open at once, from a small load tool written in Rust, on loopback, for 5 seconds a file.
- **Requests:** files served from a folder by `serve`, with logging off.
- **Comparisons**, each with `TCP_NODELAY` set, as Calx sets it, since without it Linux holds back each response 40 ms:
  - Python 3.14's `ThreadingHTTPServer` with `SimpleHTTPRequestHandler`, set to HTTP/1.1 so connections stay open, and logging off;
  - Rust 1.91 with axum 0.8.9 and tower-http 0.6.11's `ServeDir`, built for release on tokio's multi-thread runtime with 8 threads.
- **Memory:** the most the server held at any point across the three files. On Windows that's its working set and its private bytes; on Linux, its resident memory.

| Windows | Calx | Python | Rust |
| --- | --- | --- | --- |
| A 1 KB file, requests a second | 41,000 | 3,620 | 20,200 |
| A 1 MB file | 9,730 | 3,230 | 5,670 |
| A 64 MB file | 83 | 87 | 72 |
| Most memory: working set | 8.2 MB | 59.2 MB | 13.7 MB |
| Most memory: private | 9.4 MB | 53.6 MB | 13.6 MB |

| Linux | Calx | Python | Rust |
| --- | --- | --- | --- |
| A 1 KB file, requests a second | 39,500 | 3,950 | 15,300 |
| A 1 MB file | 15,600 | 2,590 | 3,020 |
| A 64 MB file | 397 | 97 | 58 |
| Most memory: resident | 4.8 MB | 33.8 MB | 23.1 MB |

- **Big files** go from the file to the connection by the system, so they cost Calx no copies and no memory. Python and Rust read each one through buffers of their own. Before `serve` sent files this way, it did 2,660 1 MB requests a second on Windows, and held 157 MB.
- **A small file** costs its open, its look and its close, which on Windows take most of the time. Each file is opened once, and asked its size and time.
- **Many big files at once on Windows for desktops.** Those editions run only two `TransmitFile` calls at once, and queue the rest. So while two clients read slowly, the rest would wait for them. [`send_file`](net.md#sending-files) reads a file a part at a time for those connections and sends that, so none of them waits: the 64 MB row was 59 before it did. Python reads and sends on 16 threads, which is slower on 1 MB files and about as fast on 64 MB ones. Linux's `sendfile` has no such limit.
