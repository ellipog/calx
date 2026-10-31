---
title: std.http
description: HTTP and HTTPS requests on the system's own client, WinHTTP on Windows and libcurl on Linux, each on a thread of its own so a server's loop goes on while it waits.
---

# `std.http`

GET and POST over HTTP and HTTPS. An online-mode server uses it to ask Mojang's session server whether a player who's logging in has joined.

Requests go through the system's own client, so the system checks certificates against the roots it trusts, as browsers do: WinHTTP on Windows, and libcurl on Linux.

Each request runs on a thread of its own. `get` and `post` return at once, `done` says when the request has finished, and `wait` takes the response. A server's loop asks `done` each tick and goes on with its work until then, as a game server's login might wait on an authentication service.

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
| `get(url: String, headers: List<Header>, timeout_ms: u32, limit: usize) -> Request` | Starts a GET. The whole request may take `timeout_ms` milliseconds, and the body may be at most `limit` bytes |
| `post(url: String, headers: List<Header>, body: List<u8>, timeout_ms: u32, limit: usize) -> Request` | Starts a POST of `body`. Give a `Content-Type` header for it |
| `r.done() -> bool` | Whether the request has finished, so `wait` won't wait |
| `r.wait() -> Response ! Error` | Waits for it to finish and takes the response. Panics if it was taken already, by `r` or a copy of it |
| `Header { name: String, value: String }` | A header line |
| `resp.status: u32` | The status code: 200, or 204 when the session server says a player hasn't joined |
| `resp.headers: List<Header>` | The response's header lines, in the order they came, with each value trimmed |
| `resp.body: List<u8>` | The body |
| `resp.header(name: String) -> String?` | The first header named `name`, ignoring case, as HTTP compares names |

`get` and `post` panic if the timeout is 0, or if a header is one that `std.http` writes itself (`Host`, `Content-Length`, `Transfer-Encoding` or `Connection`). They also panic if a header's name has a byte HTTP doesn't allow in names, or its value has a line break or a zero byte. Those would let a value a player chose start a header of its own.

Why a request got no response, as `Error`:

| Error | When |
| --- | --- |
| `BadUrl` | The URL isn't `http://` or `https://`, then a host, then an optional port and path. It must be printable ASCII, with anything else percent-encoded, and have no user name before the host |
| `Resolve` | No name server knows the host |
| `Connect` | Nothing answered at the host and port |
| `Tls` | The TLS handshake failed, or the system doesn't trust the server's certificate |
| `Timeout` | The response didn't come within the time |
| `Protocol` | The server closed the connection early, or what it sent isn't HTTP |
| `TooLong` | The body is longer than the limit |
| `Unavailable` | There's no HTTP client on the system: on Linux, libcurl isn't installed |
| `Os(code)` | Something else, with the system's error code |

## What's sent

Both clients are set up to send the same request:

- HTTP/1.1, with a `User-Agent` of `Calx` unless a header gives another.
- No `Accept` or `Accept-Encoding` header unless the caller gives one, so bodies come back as they are, not compressed.
- No proxy.
- Redirects are followed up to 10 times, but never from HTTPS to HTTP.
- The fragment, after `#`, isn't sent, as Java and browsers don't send it.

Header values that aren't ASCII are read as Latin-1, as Java reads them, and given as UTF-8.

## Checked

The [tests](../../tests/std/http.calx) run a server on [`std.net`](net.md) in the same program, and make requests to it on loopback. They check status codes, headers in both directions, POST bodies, a redirect, an empty 204, a body over the limit, a server that never answers, and URLs that aren't HTTP. They give the same results on Windows and Linux.

These were also run against the real services, with the same results on both systems:

- Mojang's `hasJoined`, for a player who hadn't joined, gave 204.
- `api.minecraftservices.com/publickeys` gave 200 and its JSON.
- An expired certificate and a self-signed one gave `Tls`.
- A host that doesn't exist gave `Resolve`.

## How it's built

[runtime/calx_http.c](../../runtime/calx_http.c), linked only into programs that call it.

| Part | How |
| --- | --- |
| Windows | WinHTTP, which comes with Windows. The time left is set again before each step, so the timeout is for the whole request |
| Linux | libcurl, found with `dlopen` when the first request starts, so a program that makes none doesn't need it installed. `libcurl.so.4` is in every major distribution |
| Threads | One for each request, joined by `wait`. Requests are slots in a table, named by an index and a generation, so a response that was taken can't be taken again |

`std.http` is written in Calx over those runtime calls: [std/http.calx](../../std/http.calx).
