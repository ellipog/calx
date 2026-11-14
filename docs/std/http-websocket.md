---
title: std.http.websocket
description: "A WebSocket client: ws:// and wss:// on the system's HTTP client, text and binary messages, pings and closes answered on a thread of its own."
---

# `std.http.websocket`

A WebSocket client (RFC 6455), for talking to a server that pushes messages as they happen, as Python's `websockets` and `websocket-client` do: a browser's DevTools protocol, a chat or a live dashboard's feed. It connects to `ws://` and `wss://` URLs through the system's own HTTP client, so TLS certificates are checked as [`std.http`](http.md)'s are.

Each connection runs on a thread of its own. It answers the server's pings and its close by itself, checks that text is UTF-8, and keeps the messages that come until the program takes them, so a program that's busy loses none.

The server's side is [std.http.server](http-server.md#websockets)'s `upgrade`, which makes a request's connection a WebSocket, with the same `Message`.

```
import std.http.websocket.{Message, connect}
import std.json

fn main() ! Error {
    // Headless Chrome, started with --remote-debugging-port=9222; the page's URL is in
    // http://127.0.0.1:9222/json/list.
    let ws = try connect("ws://127.0.0.1:9222/devtools/page/5BD479C5C2E9A38E9DAA47816168065C")
    try ws.send_text(#"{"id":1,"method":"Runtime.evaluate","params":{"expression":"document.title"}}"#)
    while let Message.Text(t) = try ws.receive(5000) {
        let reply = try json.parse(t)
        if reply.get("id") != none {
            println("{reply.pointer("/result/result/value")}")    // some("Example Domain")
            break
        }
    }
    ws.close()
}
```

A program with a loop of its own calls `open`, which returns at once while the connection opens, as [`std.http.get`](http.md) returns a request. What it sends meanwhile goes once the connection is open, and `receive(0)` takes a message if one has come, without waiting:

```
let feed = open("wss://example.com/feed", [Header { name: "Authorization", value: token }])
loop {
    while let m = try feed.receive(0) {
        // ...
    }
    // The rest of the loop's work.
}
```

What fails says why, and the URL needn't be reachable to see that:

```calx
import std.http.websocket.{Error, connect}

fn main() {
    match connect("http://example.com/") {
        ok(ws) => ws.close(),
        err(e) => println("{e} {e == Error.BadUrl}"),    // out: not a valid WebSocket URL true
    }
}
```

## Items

| Item | Gives |
| --- | --- |
| `connect(url: String, headers: List<Header> = [], timeout_ms: u32 = 30000, limit: usize = 67108864) -> WebSocket ! Error` | The connection to `url`, `ws://` or `wss://`, once it's open, as Python's `websockets.connect` gives it: `open`, then `wait_open`. The headers go with the handshake |
| `open(url, headers = [], timeout_ms = 30000, limit = 67108864) -> WebSocket` | The connection, at once, while it opens on its own thread. Opening may take `timeout_ms`, and a message from the server may be `limit` bytes long, 64 MiB unless it says |
| `ws.send_text(text: String) ! Error`, `ws.send_binary(bytes: List<u8>) ! Error` | Sends a message, or keeps it to send once the connection is open. Fails with what the connection ended with, if it has |
| `ws.receive(timeout_ms: u32) -> Message? ! Error` | The next message, waiting up to `timeout_ms` for one, or `none` if none came; with 0 it doesn't wait. Once the connection has ended and every message before the end is taken, it fails with what it ended with |
| `ws.wait_open() ! Error` | Waits while the connection opens, and fails with why it didn't |
| `ws.is_open() -> bool` | Whether the handshake is done and the connection hasn't closed |
| `ws.protocol() -> String?` | The subprotocol the server chose from those a `Sec-WebSocket-Protocol` header offered |
| `ws.close(own self, code: u16 = 1000, reason: String = "")` | Closes the connection with `code` and `reason`, waits for the server's answer, for as long as the timeout it was opened with, up to 10 seconds, and lets it go. A reason has room for 123 bytes, and a longer one is cut at the last whole rune that fits |
| `Message` | `Text(String)`, which the connection has checked is UTF-8, or `Binary(List<u8>)`: a whole message, its frames joined |
| `Error` | Why it didn't open, or what it ended with: `BadUrl`, `Resolve`, `Connect`, `Tls`, `Timeout`, `Refused(status)`, `Protocol`, `TooLong`, `Closed(code, reason)`, `Unavailable` and `Os(code)`. `{e}` says it in words |
| `WebSocket` | A connection: opening, open, or closed and keeping what it ended with. It's a [handle](../reference/ownership.md#handles), so it can't be copied. It's open until `close`, the server closes it, or the program ends |

`open` and `connect` panic if the timeout is 0, or a header's name isn't a token, its value has a line break, or it's one the handshake sets itself: Host, Connection, Upgrade, Content-Length, Transfer-Encoding, or Sec-WebSocket-Key, -Version or -Extensions. Headers such as `Authorization`, `Cookie`, `Origin` and `Sec-WebSocket-Protocol` go as they're given.

## How a connection ends

| `Error` | When |
| --- | --- |
| `Closed(code, reason)` | The server closed the connection: `1000` normally, `1001` when it's going away, or a code of its own from 3000 to 4999, with its reason. `Closed(1006, "")` is a connection that ended without a close, as when the server or the network went down |
| `Protocol` | The server broke the protocol: its answer to the handshake isn't a WebSocket's, or a frame isn't one RFC 6455 allows, such as a masked frame or a control frame in pieces, or text isn't UTF-8. The connection closes, with 1002, or 1007 for text that isn't UTF-8 |
| `TooLong` | A message is longer than `limit`. The connection closes with 1009 |
| `Refused(status)` | The server answered the handshake with an HTTP status rather than switching, as `404` for a path that has no WebSocket, or `403` for an origin it turns away |
| `BadUrl`, `Resolve`, `Connect`, `Tls`, `Timeout`, `Unavailable`, `Os` | It didn't open, as with [`std.http`](http.md#errors). `Timeout` is also a server that didn't answer `close` in time |

The messages that came before the end are taken first, and then `receive` fails with how it ended, and so does each call after.

## What's sent

The handshake is a GET with `Upgrade: websocket`, a random `Sec-WebSocket-Key`, version 13, `User-Agent: Calx` unless the headers give one, and the headers given. Like `std.http`, it goes through no proxy and follows no redirect. It offers no extensions, so messages aren't compressed, and it sends no `Origin`, as a program isn't a page: a server that wants one, as Chrome's DevTools turn away origins they weren't told of, gets the one given in the headers.

Each frame the client sends is masked by four random bytes from the system, as RFC 6455 has it. A message goes as one frame, and pings from the server are answered with the same bytes.

## How it's built

[std/http/websocket.calx](../../std/http/websocket.calx) is the API, and the connection is in the runtime, in [runtime/calx_http.c](../../runtime/calx_http.c), beside `std.http`'s requests:

| Platform | Source |
| --- | --- |
| Windows | WinHTTP's WebSocket API, from Windows 8: `WinHttpWebSocketCompleteUpgrade`, `WinHttpWebSocketSend`, `WinHttpWebSocketReceive`, `WinHttpWebSocketShutdown` and `WinHttpWebSocketQueryCloseStatus`. The thread receives, and sends go out from the program's thread under a lock |
| Linux, macOS | libcurl, loaded when the first connection opens, in its connect-only mode, which makes the TCP and TLS connection with the system's certificate checks. The handshake and the frames are written in calx_http.c, since libcurl's own WebSocket API is missing from most systems' builds, Ubuntu's among them. The connection's thread does all the reading and writing, as libcurl and TLS allow one thread at a time on a connection, and the program's sends wake it through a pipe |

## Checked

[tests/std/http_websocket.calx](../../tests/std/http_websocket.calx) runs a WebSocket server of its own, written on [`std.net`](net.md), and checks each part against it: the handshake with headers and a subprotocol, a frame in the same packet as the handshake's answer, both kinds of message both ways, a message in three frames with a ping between them, frames of each length's form, closes from either side and the codes the client sends, a connection dropped without a close, what breaks the protocol, a message past the limit, a refusal, a wrong answer to the key, and messages sent while the connection opens. On Linux it also talked to `wss://echo.websocket.org` and drove headless Chrome 157 over its DevTools protocol: a page set up with `Runtime.evaluate`, a real click hit-tested at its coordinates with `Input.dispatchMouseEvent`, the page's console events, and a screenshot from `Page.captureScreenshot`.
