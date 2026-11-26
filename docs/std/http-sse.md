---
title: std.http.sse
description: Server-sent events read as they come, as a browser's EventSource reads them, with multi-line data, IDs and retry times, and reconnects that send Last-Event-ID.
---

# `std.http.sse`

Server-sent events (the HTML standard's `text/event-stream`) read as they come, as a browser's `EventSource` and Python's `httpx_sse` read them: a live feed, a server's notifications, or an MCP server's answer to `subscriptions/listen`. It reads over a [`std.http` stream](http.md#reading-a-response-as-it-comes), with the system's own TLS.

`connect` GETs the URL and waits for the server to answer with an event stream; `next` gives each event as it comes, and `none` once the server ends the stream. A script that follows a feed:

```calx
import std.env.{args}
import std.http.sse.{connect}

fn main() {
    let a = args()
    if a.is_empty() {
        println("give the URL of an event stream, such as https://stream.wikimedia.org/v2/stream/recentchange")    // out: give the URL of an event stream, such as https://stream.wikimedia.org/v2/stream/recentchange
        return
    }
    var es = match connect(a[0].copy()) {
        ok(es) => es,
        err(e) => {
            println("no events: {e}")
            return
        }
    }
    for _ in 0..3 {
        match es.next() {
            ok(some(ev)) => println("{ev.event} {ev.id}: {ev.data.len()} bytes"),
            ok(none) => break,
            err(e) => {
                println("ended: {e}")
                break
            }
        }
    }
    es.close()
}
```

A stream that ends, or breaks, is picked up again with `reconnect`, which waits the retry time, then asks again with the last event's ID in `Last-Event-ID`, so the server can go on from there, as a browser does:

```
var es = try connect(url)
loop {
    match es.next() {
        ok(some(ev)) => handle(ev),
        ok(none) => try es.reconnect(),     // the server ended it
        err(_) => try es.reconnect(),       // it broke, or read_timeout_ms passed
    }
}
```

`connect_post` POSTs a body and reads the events it's answered with, as an MCP client's `subscriptions/listen` is answered. A server that may answer with JSON instead gives it in `NotEvents`, with its headers:

```
let listen = Header { name: "Content-Type", value: "application/json" }
match connect_post(url, [listen, session], request.to_bytes()) {
    ok(es) => {
        while let ev = try es.next() {
            let msg = try json.parse(ev.data)
            ...    // notifications/tools/list_changed, and the rest
        }
    }
    err(NotEvents(head, body)) => ...    // a JSON answer, or a status that isn't 200
    err(Http(e)) => ...                  // no answer
}
```

## Items

| Item | Gives |
| --- | --- |
| `connect(url: String, headers: List<Header> = [], timeout_ms: u32 = 30000, read_timeout_ms: u32 = 0, version: Version = Version.Http1) -> EventSource ! Error` | GETs `url` and waits for the answer, which must be 200 with a Content-Type of `text/event-stream`. It sends `Accept: text/event-stream` and `Cache-Control: no-cache` unless `headers` has its own. The answer may take `timeout_ms`; with `read_timeout_ms`, `next` fails with `Timeout` once nothing has come for that long. Panics as `std.http.get` does |
| `connect_post(url: String, headers: List<Header>, body: List<u8>, timeout_ms: u32 = 30000, read_timeout_ms: u32 = 0, version: Version = Version.Http1) -> EventSource ! Error` | POSTs `body` and reads the events it's answered with. Give a Content-Type header for the body |
| `es.next() -> Event? ! Error` | The next event, waiting for it, or `none` once the server has ended the stream |
| `es.ready() -> bool ! Error` | Whether `next` would give an event, or say the stream has ended, without waiting |
| `es.wake_on(waker: Waker)` | Wakes the `Net` as events come, for a loop that takes each that's `ready` |
| `es.reconnect() ! Error` | Closes the connection, waits `retry_ms`, and makes the same request again, a POST's body and all, with `Last-Event-ID` if there's been an ID. Events that came and weren't read are dropped |
| `es.last_id() -> String` | The last event's ID, which `reconnect` sends. Empty if there's been none |
| `es.retry_ms() -> u32` | How long `reconnect` waits: 3000 ms, as browsers wait, until a `retry:` line says otherwise |
| `es.head() -> Head` | The status and header lines of the last answer, such as a session ID the server gave |
| `es.close()` | Closes the connection. Dropping it does the same |
| `Event { event: String, data: String, id: String, retry: u32? }` | An event: its type, `message` unless it had an `event:` line; its `data:` lines joined with line feeds; the last event ID by then, as a browser's `lastEventId`; and its `retry:` line's milliseconds, if it had one |

## Errors

| Error | When | `{e}` |
| --- | --- | --- |
| `Http(e)` | No answer came, or the connection ended with an error, or `read_timeout_ms` passed, as [`std.http`'s `Error`](http.md#errors) says | as `e` says it, such as timed out |
| `NotEvents(head, body)` | The answer's status isn't 200 or its Content-Type isn't `text/event-stream`. `body` is its body, up to 16 MiB | the server answered 404 with text/html, not an event stream |

## How the stream is read

As the HTML standard reads one, which browsers follow:

- Lines end with CRLF, LF or CR. A byte order mark at the start is dropped, and bytes that aren't UTF-8 are read as U+FFFD.
- A line that starts with `:` is a comment, as servers send to keep a connection open, and is skipped.
- `field: value`, with one space after the colon dropped, or a field alone with an empty value. `event` sets the type, `data` adds a line of data, `id` sets the last event ID unless the value holds a zero byte, and `retry` sets the retry time if it's all digits. Other fields are skipped.
- A blank line ends an event. One with no `data` line gives no event, but its `id` still sets the last event ID. An event the stream ends in the middle of is dropped.

The server's side is [`std.http.server`](http-server.md#event-streams)'s `stream` and `send_event`.

## Checked

[tests/std/http_stream.calx](../../tests/std/http_stream.calx) serves events from a thread of its own in the same program: a byte order mark, a comment and a retry time, an event of two data lines, one split across chunks with CRLF, one with CR alone and an empty data line, an ID with no data, and an event cut off by the end. It checks each event, the last ID and the retry time, that `reconnect` asks again with `Last-Event-ID: 3`, that the server hears the connection close once the client closes it, and that a JSON answer to `connect_post` comes back in `NotEvents`. It gives the same results on Windows and Linux. Wikimedia's EventStreams feed was followed over TLS, in HTTP/1.1 and HTTP/2, on both systems.

## How it's built

[std/http/sse.calx](../../std/http/sse.calx), in Calx, over [`std.http`'s streams](http.md#reading-a-response-as-it-comes).
