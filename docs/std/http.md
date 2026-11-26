---
title: std.http
description: HTTP and HTTPS requests on the system's own client, WinHTTP on Windows, URLSession on iOS and libcurl on Linux and macOS, off the program's thread so its loop goes on while it waits, in HTTP/1.1 or HTTP/2, over connections kept for the requests after them.
---

# `std.http`

GET and POST over HTTP and HTTPS, in HTTP/1.1 or [HTTP/2](#http2). An online-mode server uses it to ask Mojang's session server whether a player who's logging in has joined.

Requests go through the system's own client, so the system checks certificates against the roots it trusts, as browsers do: WinHTTP on Windows, URLSession on iOS, and libcurl on Linux and macOS. To answer requests rather than make them, there's [std.http.server](http-server.md), to talk to a WebSocket, [std.http.websocket](http-websocket.md), and to read server-sent events, [std.http.sse](http-sse.md), over the [streams](#reading-a-response-as-it-comes) below.

A script that only wants what's at a URL calls `fetch`, which waits for the answer:

```
import std.http.{fetch}
import std.json.{parse}

fn main() ! Error {
    let resp = try fetch("https://piston-meta.mojang.com/mc/game/version_manifest_v2.json")
    let manifest = try parse(String.from_utf8(resp.body) ?? String())
    println("{resp.status} {manifest.pointer("/latest/release")}")    // 200 some("26.3")
}
```

Each request runs off the program's thread. `get` and `post` return at once, `done` says when the request has finished, and `wait` takes the response. A server's loop asks `done` each tick and goes on with its work until then, as a game server's login might wait on an authentication service.

```calx
import std.env.{args}
import std.http.{Header, get}

fn main() {
    let a = args()
    if a.is_empty() {
        println("give a URL, such as https://api.minecraftservices.com/publickeys")    // out: give a URL, such as https://api.minecraftservices.com/publickeys
        return
    }
    let accept = Header { name: "Accept", value: "application/json" }
    var r = get(a[0].copy(), [accept], 5000, 1048576)
    while not r.done() {
        // A server would go on with its tick here.
    }
    match r.wait() {
        ok(resp) => println("{resp.status}, {resp.body.len()} bytes, {resp.header("content-type") ?? "no type"}"),
        err(e) => println("no response: {e}"),
    }
}
```

| Item | Gives |
| --- | --- |
| `get(url: String, headers: List<Header>, timeout_ms: u32, limit: usize, version: Version = Version.Http1) -> Request` | Starts a GET. The whole request may take `timeout_ms` milliseconds, and the body may be at most `limit` bytes. `version` is the HTTP it asks for ([HTTP/2](#http2)) |
| `post(url: String, headers: List<Header>, body: List<u8>, timeout_ms: u32, limit: usize, version: Version = Version.Http1) -> Request` | Starts a POST of `body`. Give a `Content-Type` header for it |
| `fetch(url: String, timeout_ms: u32 = 60000, version: Version = Version.Http1) -> Response ! Error` | GETs `url` and waits for the answer, for scripts, as Python's `requests.get(url)`: no headers of its own and a body of up to 1 GiB. Any status is an answer; only no answer is an error |
| `r.done() -> bool` | Whether the request has finished, so `wait` won't wait |
| `r.wake_on(waker: Waker)` | Wakes the [`std.net.Waker`](net.md)'s `Net`, such as a server's, as the request finishes, or at once if it has ([below](#waking-a-servers-loop)) |
| `r.progress() -> Progress` | How much of the body has come so far, while the request runs or once it's done, for a progress bar. Nothing has come until the headers have, and once the response is taken it's 0 of an unknown length |
| `Progress { received: u64, total: u64? }` | The bytes of the body that have come, and its length in all if the server gave a `Content-Length` |
| `r.wait() -> Response ! Error` | Waits for it to finish and takes the response. Panics if it was taken already, by `r` or a copy of it |
| `Header { name: String, value: String }` | A header line |
| `resp.status: u32` | The status code: 200, or 204 when the session server says a player hasn't joined |
| `resp.headers: List<Header>` | The response's header lines, in the order they came, with each value trimmed. HTTP/2 sends names in lower case, and on Linux and macOS they come so; WinHTTP and URLSession give them in their own case |
| `resp.body: List<u8>` | The body |
| `resp.header(name: String) -> String?` | The first header named `name`, ignoring case, as HTTP compares names |
| `resp.version: String` | The HTTP the response came in, `HTTP/1.1`, `HTTP/2` or `HTTP/1.0`, as httpx's `r.http_version`, and on iOS `HTTP/3` too |
| `resp.reused: bool` | Whether it came over a connection that was open already ([connections](#connections)) |
| `resp.timings: Timings` | How long its steps took ([timings](#timings)) |

A download shows how far it has come by asking `progress` while it waits, as `calx update` draws its progress bar:

```calx
import std.env.{args}
import std.http.{get}
import std.thread.{sleep}

fn main() {
    let a = args()
    if a.is_empty() {
        println("give a URL to download")    // out: give a URL to download
        return
    }
    var r = get(a[0].copy(), [], 600000, 1 << 30)    // 1 GiB at most
    while not r.done() {
        let p = r.progress()
        match p.total {
            some(total) => print("\r{p.received * 100 / (total + 1)}%"),
            none => print("\r{p.received} bytes"),
        }
        unsafe { sleep(100_000_000) }    // a tenth of a second
    }
    match r.wait() {
        ok(resp) => println("\r{resp.body.len()} bytes"),
        err(e) => println("\rno response: {e}"),
    }
}
```

`get` and `post` panic if the timeout is 0, or if a header is one that `std.http` writes itself (`Host`, `Content-Length`, `Transfer-Encoding` or `Connection`). They also panic if a header's name has a byte HTTP doesn't allow in names, or its value has a line break or a zero byte. Those would let a value a player chose start a header of its own.

## Waking a server's loop

A server's loop that also starts requests would otherwise look at `done` after each `next_event(10)`, and hear of an answer up to 10 ms late, or about 15.6 ms on Windows, whose timer is that coarse. Given the server's `Waker`, the request wakes `next_event` as it finishes, so the loop waits as long as it likes, as asyncio's `wait` returns once a request does:

```
var r = get(url, [], 10000, 1 << 20)
r.wake_on(server.waker()!)
loop {
    _ = server.next_event()
    if r.done() {
        handle(r.wait())
        break
    }
}
```

It wakes it from the thread that finished the request, and at once if it had finished already. A later `wake_on` wakes the `Net` it's given instead. [`std.http.server`](http-server.md#clients-requests-and-programs-on-the-same-loop) has a whole loop.

## Reading a response as it comes

`get` and `post` give the body once it has all come, so a response that never ends, such as an event stream, can't be read with them. `stream_get` and `stream_post` start a request whose status and headers can be read first, then its body a part or a line at a time as it comes, until the server ends it or the program closes it, as Python's `httpx.stream` does. A GET of a live feed, which prints its first lines and stops:

```calx
import std.env.{args}
import std.http.{Header, stream_get}

fn main() {
    let a = args()
    if a.is_empty() {
        println("give the URL of a feed, such as https://stream.wikimedia.org/v2/stream/recentchange")    // out: give the URL of a feed, such as https://stream.wikimedia.org/v2/stream/recentchange
        return
    }
    var s = stream_get(a[0].copy(), [Header { name: "Accept", value: "text/event-stream" }])
    match s.head() {
        ok(head) => println("{head.status} {head.header("content-type") ?? "no type"}"),
        err(e) => {
            println("no response: {e}")
            return
        }
    }
    for _ in 0..5 {
        match s.read_line() {
            ok(some(line)) => println(line),
            ok(none) => break,
            err(e) => {
                println("ended: {e}")
                break
            }
        }
    }
    s.close()    // the server hears the connection close
}
```

A POST is the same, with its body, as an MCP client's `subscriptions/listen` is answered with an event stream:

```
var s = stream_post(url, [Header { name: "Content-Type", value: "application/json" }], request.to_bytes())
let head = try s.head()
while let line = try s.read_line() {
    ...
}
```

| Item | Gives |
| --- | --- |
| `stream_get(url: String, headers: List<Header> = [], timeout_ms: u32 = 30000, read_timeout_ms: u32 = 0, version: Version = Version.Http1) -> Stream` | Starts a GET whose body is read as it comes, and gives it at once, while it connects on its own thread. The status and headers may take `timeout_ms` to come; after them there's no limit on how long the body takes or how long it is. With `read_timeout_ms`, a read that waits that long with nothing coming fails with `Timeout`, as httpx's read timeout does, and 0 waits as long as it takes. Panics as `get` does |
| `stream_post(url: String, headers: List<Header>, body: List<u8>, timeout_ms: u32 = 30000, read_timeout_ms: u32 = 0, version: Version = Version.Http1) -> Stream` | Starts a POST of `body` whose answer is read as it comes |
| `s.head() -> Head ! Error` | Waits for the status and header lines and gives them, or fails with why no response came: `Timeout` if they didn't come in `timeout_ms` |
| `Head { status: u32, headers: List<Header>, version: String }`, `head.header(name) -> String?` | As a `Response` has them. On iOS, `version` is `HTTP/1.1` whichever it was, as URLSession says only at the end |
| `s.read() -> List<u8>? ! Error` | All of the body that has come and hasn't been given, waiting for some, or `none` once it has ended |
| `s.read_line() -> String? ! Error` | The next line, without the LF, CRLF or CR that ends it, waiting until it has all come, or `none` once the body has ended. A last line with no end is given as it is. Bytes that aren't UTF-8 are read as U+FFFD |
| `s.ready() -> bool` | Whether `read` would give something, or say the body has ended, without waiting |
| `s.line_ready() -> bool ! Error` | Whether `read_line` would give a line, or say the body has ended, without waiting |
| `s.wake_on(waker: Waker)` | Wakes the `Net` as the headers come, each time some of the body does, and as it ends, for a loop that reads what's `ready` |
| `s.close()` | Closes the connection, whether or not the body has ended, so the server hears the program has stopped. Dropping the stream does the same |

- A read fails with `Protocol` if the connection ended before the body did, as when the server went away. After a `Timeout` from `read_timeout_ms`, it can be read again.
- A stream keeps up to 1 MiB of the body that the program hasn't read, then stops reading from the server until it has, so the server waits for a program that reads slowly, as TCP's window makes it: on Windows its thread waits, and on Linux and macOS libcurl pauses the transfer. URLSession can't make a server wait, so on iOS the body is kept as it comes.
- An HTTP/1.1 stream's connection isn't kept for other requests once it's closed, so closing it always closes the connection, rather than leaving the system's client to read on to the end of a body that may have none.

## HTTP/2

A request is HTTP/1.1 unless its `version` asks for HTTP/2, as Python's `httpx.Client(http2=True)` does. With `Version.Http2`, the client offers HTTP/2 in the TLS handshake (ALPN), and the server chooses: HTTP/2 if it takes it, HTTP/1.1 if it doesn't. `resp.version` says which it was. Several HTTP/2 requests to a host go over one connection at once, each a stream of its own, so the second doesn't wait for the first:

```calx
import std.env.{args}
import std.http.{Header, Request, Version, get}

fn show(r: mut Request) {
    match r.wait() {
        ok(resp) => println("{resp.status} {resp.version} reused={resp.reused} {resp.body.len()} bytes in {resp.timings.total_ns / 1000000} ms"),
        err(e) => println("no response: {e}"),
    }
}

fn main() {
    let a = args()
    if a.is_empty() {
        println("give an https:// URL")    // out: give an https:// URL
        return
    }
    let url = a[0].copy()
    let start = Header { name: "Range", value: "bytes=0-1023" }
    let end = Header { name: "Range", value: "bytes=-1024" }
    var first = get(url, [start], 30000, 1 << 20, version: Version.Http2)
    var last = get(url, [end], 30000, 1 << 20, version: Version.Http2)
    show(mut first)    // 206 HTTP/2 reused=false 1024 bytes in 61 ms
    show(mut last)     // 206 HTTP/2 reused=true 1024 bytes in 58 ms
}
```

| `Version` | Asks for | Python |
| --- | --- | --- |
| `Http1` | HTTP/1.1, the default | `requests`, and `httpx` without `http2` |
| `Http2` | HTTP/2 if the server takes it in the TLS handshake, and HTTP/1.1 if it doesn't, or the URL is `http://`, or the system's client has no HTTP/2 | `httpx.Client(http2=True)` |
| `Http2Only` | HTTP/2 and nothing else. Over `http://` that's h2c, with prior knowledge and no `Upgrade`, and over `https://` a server that doesn't take HTTP/2 is an error | `httpx.Client(http1=False, http2=True)` |

Which systems speak it, through their own clients:

| System | `Http2`, over TLS | `Http2Only` over `http://`, h2c |
| --- | --- | --- |
| Windows 10 1607 and later, and Windows 11 | WinHTTP's HTTP/2 | `Unavailable`: WinHTTP has none |
| Linux | libcurl's, where it's built with nghttp2, as Debian's, Ubuntu's, Fedora's, Arch's and Alpine's are. `curl -V` lists `HTTP2` if so. Without it, `Http2` is HTTP/1.1 and `Http2Only` is `Unavailable` | The same |
| macOS | The system's libcurl, which has HTTP/2 | The same |
| iOS | URLSession's, which picks HTTP/2 itself whenever the server takes it, for `Http1` too, and HTTP/3 where the server offers it | `Unavailable`: URLSession has none |

A server that doesn't take HTTP/2 makes `Http2Only` over `https://` fail: with `Protocol` on Windows, and on Linux and macOS with `Tls`, as libcurl offers only HTTP/2 in the handshake and the server refuses it, or `Protocol` if it answers in HTTP/1.1 all the same. On iOS, where URLSession picks the version, it's `Protocol`. A server that speaks only HTTP/2 answers an HTTP/1.1 request with something that isn't HTTP/1.1, which is `Protocol`; `Version.Http2` is the cure.

HTTP/2 sends header names in lower case. libcurl gives them so, and WinHTTP in its own case, so `resp.header(name)`, which ignores case, finds them on every system.

## Connections

A connection a request opens is kept, and the next request to the same host and port goes over it, as `requests.Session()` and `httpx.Client()` keep theirs: a whole program is one session. `resp.reused` says whether a response came over a connection that was open already. An HTTP/1.1 connection carries one request at a time, so requests at once to a host open one each. An HTTP/2 one carries any number at once, and a request to a host that another is connecting to waits to go over that connection rather than opening its own.

Requests that ask for HTTP/2 never go over a kept HTTP/1.1 connection that an `Http1` request opened, so a server that takes HTTP/2 always gets it from them.

## Timings

`resp.timings` is how long each step of the request took, as curl's `-w '%{time_connect}'` and the like give them: each in nanoseconds from the request's start to the end of the step. A step the request didn't take is 0, as resolving, connecting and TLS are on a kept connection, and TLS is over `http://`.

| Field | Until |
| --- | --- |
| `resolve_ns` | The host's address was known. On Windows, 0 too when the system had it already |
| `connect_ns` | The TCP connection was made |
| `tls_ns` | The TLS handshake was done |
| `first_byte_ns` | The response's first byte came. On Windows, its headers, which are mostly in the first packet |
| `total_ns` | The whole body had come |

They come from libcurl, from URLSession's task metrics on iOS, and from WinHTTP's request times on Windows. A Windows whose WinHTTP doesn't give those gives the total alone, and `reused` is false there.

## Forms and file uploads

`Form` is a `multipart/form-data` body, as an HTML form with a file input sends one, and as Python's requests sends `data=` with `files=` (RFC 7578). `post_form` sends it with its `Content-Type`, as an upload API that wants a JSON field and a file expects:

```calx
import std.http.{Form}

fn main() {
    let jar: List<u8> = [80, 75, 3, 4]
    let form = Form.with_boundary("XyZ").field("metadata", "[1]").file("file", "mod.jar", jar)
    println(form.content_type())    // out: multipart/form-data; boundary=XyZ
    println("{form.body().len()} bytes")    // out: 192 bytes
    // var r = post_form(url, [Header { name: "X-Api-Token", value: token }], form, 60000, 1 << 20)
}
```

| Item | Gives |
| --- | --- |
| `Form.new() -> Form` | An empty form whose boundary is 32 hex digits from the system's random generator, as urllib3 makes it |
| `Form.with_boundary(boundary: String) -> Form` | An empty form with this boundary, for a body that comes out the same each time. Panics unless it's 1 to 70 of RFC 2046's characters, letters, digits and `'()+_,-./:=?` and spaces, not ending in a space |
| `form.field(name: String, value: String) -> Form` | The form with a text field added, with no `Content-Type` of its own |
| `form.file(name: String, filename: String, bytes: List<u8>, content_type: String = "application/octet-stream") -> Form` | The form with a file added, its bytes as they are |
| `form.content_type() -> String` | `multipart/form-data; boundary=` and the boundary |
| `form.body() -> List<u8>` | Every part, then the closing boundary |
| `form.boundary() -> String` | The boundary |
| `post_form(url: String, headers: List<Header>, form: Form, timeout_ms: u32, limit: usize, version: Version = Version.Http1) -> Request` | Starts a POST of the form's body with its `Content-Type`, as `post` does. Panics if `headers` has a `Content-Type` of its own |

- A part is written as it's added, so a file's bytes are copied once into the body, and once more by `post_form`'s request.
- A name or filename is quoted with `"`, carriage return and line feed written `%22`, `%0D` and `%0A`, as browsers and urllib3 write them, so neither can end its line early. Other text, UTF-8 too, goes as it is.

## Errors

Why a request got no response, as `Error`:

| Error | When | `{e}` |
| --- | --- | --- |
| `BadUrl` | The URL isn't `http://` or `https://`, then a host, then an optional port and path. It must be printable ASCII, with anything else percent-encoded, and have no user name before the host | not a valid URL |
| `Resolve` | No name server knows the host | can't find the host |
| `Connect` | Nothing answered at the host and port | can't connect: nothing answered at the host and port |
| `Tls` | The TLS handshake failed, or the system doesn't trust the server's certificate | the TLS handshake failed |
| `Timeout` | The response didn't come within the time | timed out |
| `Protocol` | The server closed the connection early, or what it sent isn't HTTP/1.1 or HTTP/2: an HTTP/2-only server's answer to HTTP/1.1, or something that isn't HTTP at all, such as an SSH server on the port. With `Http2Only`, a server that doesn't take HTTP/2 | the response ended early, or the server didn't answer in HTTP/1.1 or HTTP/2 |
| `TooLong` | The body is longer than the limit | the body is longer than the limit |
| `Unavailable` | There's no HTTP client on the system that can make the request: on Linux, libcurl isn't installed, or for `Http2Only`, it's built without HTTP/2; on Windows and iOS, `Http2Only` over `http://` | there's no HTTP client on this system for the request; on Linux, install libcurl |
| `Os(code)` | Something else, with the system's error code | system error 7 |

## What's sent

Both clients are set up to send the same request:

- HTTP/1.1, or HTTP/2 when `version` asks for it, with a `User-Agent` of `Calx` unless a header gives another.
- No `Accept` or `Accept-Encoding` header unless the caller gives one, so bodies come back as they are, not compressed. On iOS, URLSession adds `Accept: */*` and an `Accept-Language` of its own where the caller gives none, and `Accept-Encoding` is `identity`, so bodies come back as they are there too.
- No proxy.
- Redirects are followed up to 10 times, but never from HTTPS to HTTP.
- The fragment, after `#`, isn't sent, as Java and browsers don't send it.

Header values that aren't ASCII are read as Latin-1, as Java reads them, and given as UTF-8.

## Checked

The [tests](../../tests/std/http.calx) run a server on [`std.net`](net.md) in the same program, and make requests to it on loopback. They check status codes, headers in both directions, POST bodies, a redirect, an empty 204, a body over the limit, a server that never answers, one that doesn't answer in HTTP, and URLs that aren't HTTP. They give the same results on Windows and Linux.

[Others](../../tests/std/http_version.calx) check kept connections and timings over HTTP/1.1, that `Http2` is HTTP/1.1 over `http://`, and, with a server that speaks enough HTTP/2 to answer a GET, that two `Http2Only` requests go over one h2c connection at once, the server answering neither until both have come. WinHTTP has no h2c, so on Windows that part checks that the requests are `Unavailable`, and so does it on iOS.

[tests/std/http_stream.calx](../../tests/std/http_stream.calx) checks streams against a server in the same program: a chunked body whose lines are split across chunks and end in CRLF, CR or LF, with a CRLF whose LF comes in the next chunk; a body with a Content-Length read as its parts come, with `ready`; a POST answered with a stream, which the client closes while the server is still sending, and the server hears the connection close; 8 MiB sent at once, which comes whole and in order while the stream holds 1 MiB of it at a time; headers that never come, which time out; and [server-sent events](http-sse.md). It gives the same results on Windows and Linux. Against the real services, Wikimedia's EventStreams feed was followed over TLS, in HTTP/1.1 and HTTP/2, and a POST to httpbin was read as it came, on both systems.

[tests/std/http_ios.calx](../../tests/std/http_ios.calx) builds std.http's and std.net's tests for the iOS simulator and runs them there, on a Mac with Xcode. Each gives what it gives on the other systems, except for the `Accept` URLSession adds. From the simulator, `https://example.com` gave 200 over HTTP/2 with its timings, and `self-signed.badssl.com` gave `Tls`.

These were also run against the real services, with the same results on both systems:

- Mojang's `hasJoined`, for a player who hadn't joined, gave 204.
- `api.minecraftservices.com/publickeys` gave 200 and its JSON.
- An expired certificate and a self-signed one gave `Tls`.
- A host that doesn't exist gave `Resolve`.
- With `Http2`, a server that takes HTTP/2 gave `HTTP/2`, and two more requests at once went over its connection; one that doesn't, such as `info.cern.ch`, gave `HTTP/1.1`.

## How it's built

[runtime/calx_http.c](../../runtime/calx_http.c), linked only into programs that call it.

| Part | How |
| --- | --- |
| Windows | WinHTTP, which comes with Windows. The time left is set again before each step, so the timeout is for the whole request. WinHTTP shares kept connections between all of a program's requests, and one that offers HTTP/2 would go over a kept HTTP/1.1 connection, so it requires HTTP/2, unless the host is known not to take it. A server that doesn't fails it, and it's made again in HTTP/1.1, once for the host. While one HTTP/2 request is opening a connection to a host, the others wait for it, as libcurl's `CURLOPT_PIPEWAIT` has them wait |
| Linux and macOS | libcurl, found with `dlopen` when the first request starts, so a program that makes none doesn't need it installed. `libcurl.so.4` is in every major distribution, and every macOS has it |
| iOS | URLSession, which iOS has instead of libcurl, on one ephemeral session with no proxy, cookies or cache. Each request is a data task, and the session's delegate, on a queue of its own, gathers the response, keeps to the limit and the redirect rules, and finishes the request. That part of the runtime is Objective-C and links Foundation by itself, so an app adds nothing. The app's App Transport Security applies, so a plain `http://` request to a host by name, not an IP address or a `.local` name, needs an exception in its `Info.plist` |
| Threads | On Windows one for each request, joined by `wait`. On Linux and macOS, three for all of them, each with a libcurl multi handle and its connections: one for HTTP/1.1 requests, one for `Http2` over TLS and one for `Http2Only`, so an HTTP/2 request never goes over a kept HTTP/1.1 connection. A libcurl older than 7.28, which has no `curl_multi_wait`, gets one for each request, with no kept connections. Requests are slots in a table, named by an index and a generation, so a response that was taken can't be taken again |

`std.http` is written in Calx over those runtime calls: [std/http.calx](../../std/http.calx).
