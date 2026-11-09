---
title: std.http.server
description: A small HTTP/1.1 server for scripts, as Python's http.server is, which hands a program each request in turn on std.net's event loop and serves a folder as python -m http.server does.
---

# `std.http.server`

A small HTTP/1.1 server for scripts, as Python's `http.server` is. A `Server` takes requests from as many connections as come and hands them to the program one at a time, as `next` gives them. The program answers each with `respond`. `serve_dir` serves a folder's files, as `python -m http.server` does, and [`calx serve`](../reference/command.md#serving-a-folder) runs it from the command line.

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

`Header` is [std.http](http.md)'s, `{ name: String, value: String }`.

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

## Where it differs from Python

- **Loopback.** It listens on `127.0.0.1` unless it's given another address, where `python -m http.server` listens on every address.
- **Threads.** A program's own loop gets requests one at a time, in the thread that calls `next`, so a slow answer holds up the rest. `serve_dir` reads files on up to 8 worker threads. Python's `ThreadingHTTPServer`, which `python -m http.server` runs, gives each connection a thread.
- **Kept open.** It speaks HTTP/1.1 and keeps connections open. Python's handler speaks HTTP/1.0 unless it's set otherwise, and closes each connection after one answer.
- **Types.** They come from a table of 52 extensions, the same on every machine, and text types are given with `; charset=utf-8`. Python's come from `mimetypes`, which on Windows also reads the registry, so they can differ from machine to machine.
- **Whole files from `from_dir`.** `from_dir` reads a file whole, so a program's own loop needs a file's size in memory to send it. `serve_dir` has the system send files of 64 KiB or more from the file.
- **No HTTPS.** Python 3.14's `--tls-cert` has no match here yet.
- **A zero byte.** A path with `%00` in it gets `404`. Python's handler raises an error at it and closes the connection without an answer.
- **Pages.** Error pages and folder listings are its own short HTML, with the same statuses, links and names as Python's.

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

## Speed

These numbers are from our benchmark, so take them as rough:

- **Machine:** an i9-13900K with other work running, the server pinned to its performance cores and the load tool to its efficiency cores, the better of two rounds taken in turn.
- **Load:** 16 connections kept open at once, from a small load tool written in Rust, on loopback, for 5 seconds.
- **Requests:** files served from a folder by `serve`, with logging off.
- **Comparisons:**
  - Python 3.14's `ThreadingHTTPServer` with `SimpleHTTPRequestHandler`, set to HTTP/1.1 so connections stay open, and logging off;
  - Rust 1.91 with axum 0.8.9 and tower-http 0.6.11's `ServeDir`, built for release on tokio's multi-thread runtime with 8 threads.

| Requests a second | Calx | Calx before | Python | Rust |
| --- | --- | --- | --- | --- |
| A 1 KB file | 40,400 | 40,900 | 3,670 | 20,600 |
| A 1 MB file | 8,400 | 2,660 | 3,300 | 5,440 |
| A 64 MB file | 64 | 34 | 86 | 76 |
| Most memory, all three | 36 MB | 157 MB | 61 MB | 14 MB |

- *Calx before* is `serve` before it sent files with `send_file`: workers read files up to 4 MiB whole, and the event loop's thread read bigger ones in parts and copied them into each connection's queue. Sending from the file is 3 times as fast for the 1 MB file, and takes a quarter of the memory.
- A small file costs its open, its look and its close, which on Windows take most of the time. Each file is opened once, and asked its size and time.
- The 64 MB file is slower than in Python and Rust, which read and send it on 16 threads and 8. Windows editions for desktops run only two `TransmitFile` calls at once, and queue the rest. This row also varied the most between runs, from 50 to 76 for Calx.
