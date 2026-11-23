---
title: std.http.websocket
description: "A WebSocket client: ws:// and wss:// with the system's TLS, text and binary messages, pings and closes answered on a thread of its own, and pings of its own that notice a dead connection."
---

# `std.http.websocket`

A WebSocket client (RFC 6455), for talking to a server that pushes messages as they happen, as Python's `websockets` and `websocket-client` do: a browser's DevTools protocol, a chat or a live dashboard's feed. It connects to `ws://` and `wss://` URLs with the system's own TLS, so certificates are checked as [`std.http`](http.md)'s are.

Each connection runs on a thread of its own. It answers the server's pings and its close by itself, checks that text is UTF-8, and keeps the messages that come until the program takes them, so a program that's busy loses none. Given `ping_interval_ms`, it also pings a server that has gone quiet, and ends the connection if the server stays quiet, so a connection that died without a word is noticed: see [A dead connection](#a-dead-connection). It uses no [`std.net`](net.md#threads) `Net`, so a program can open one on any thread, beside a [server](http-server.md) on another thread of the same program, or in a [library](../reference/libraries.md).

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

A thread that waits on the socket, and also sends what other threads queue for it, can wait as long as it likes rather than looking every few milliseconds: they wake its `receive` with the connection's `Waker` once they've queued something ([below](#waking-a-receive)).

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
| `connect(url: String, headers: List<Header> = [], timeout_ms: u32 = 30000, limit: usize = 67108864, ping_interval_ms: u32 = 0, ping_timeout_ms: u32 = 20000) -> WebSocket ! Error` | The connection to `url`, `ws://` or `wss://`, once it's open, as Python's `websockets.connect` gives it: `open`, then `wait_open`. The headers go with the handshake |
| `open(url, headers = [], timeout_ms = 30000, limit = 67108864, ping_interval_ms = 0, ping_timeout_ms = 20000) -> WebSocket` | The connection, at once, while it opens on its own thread. Opening may take `timeout_ms`, and a message from the server may be `limit` bytes long, 64 MiB unless it says. With `ping_interval_ms`, it pings the server once nothing has come for that long, and ends if nothing comes for `ping_timeout_ms` after: see [A dead connection](#a-dead-connection) |
| `ws.send_text(text: String) ! Error`, `ws.send_binary(bytes: List<u8>) ! Error` | Sends a message, or keeps it to send once the connection is open. Fails with what the connection ended with, if it has |
| `ws.receive(timeout_ms: u32) -> Message? ! Error` | The next message, waiting up to `timeout_ms` for one, or `none` if none came, or a `Waker` woke it first; with 0 it doesn't wait. Once the connection has ended and every message before the end is taken, it fails with what it ended with |
| `ws.waker() -> Waker` | A `Waker` another thread can call `wake()` on, so the `receive` waiting gives `none` at once, after the messages that have come ([below](#waking-a-receive)) |
| `Waker` | Wakes one connection's `receive`, from any thread. It can be copied and sent to other threads, and once the connection is closed a wake does nothing |
| `ws.wait_open() ! Error` | Waits while the connection opens, and fails with why it didn't |
| `ws.is_open() -> bool` | Whether the handshake is done and the connection hasn't closed |
| `ws.idle_ms() -> u64` | The milliseconds since anything last came from the server, a message, a ping, a pong or part of one, or since it opened if nothing has; 0 while it opens |
| `ws.protocol() -> String?` | The subprotocol the server chose from those a `Sec-WebSocket-Protocol` header offered |
| `ws.close(own self, code: u16 = 1000, reason: String = "")` | Closes the connection with `code` and `reason`, waits for the server's answer, for as long as the timeout it was opened with, up to 10 seconds, and lets it go. A reason has room for 123 bytes, and a longer one is cut at the last whole rune that fits |
| `Message` | `Text(String)`, which the connection has checked is UTF-8, or `Binary(List<u8>)`: a whole message, its frames joined |
| `Error` | Why it didn't open, or what it ended with: `BadUrl`, `Resolve`, `Connect`, `Tls`, `Timeout`, `Refused(status)`, `Protocol`, `TooLong`, `Closed(code, reason)`, `Unavailable` and `Os(code)`. `{e}` says it in words |
| `WebSocket` | A connection: opening, open, or closed and keeping what it ended with. It's a [handle](../reference/ownership.md#handles), so it can't be copied. It's open until `close`, the server closes it, or the program ends |

`open` and `connect` panic if the timeout is 0, `ping_interval_ms` isn't 0 and `ping_timeout_ms` is, or a header's name isn't a token, its value has a line break, or it's one the handshake sets itself: Host, Connection, Upgrade, Content-Length, Transfer-Encoding, or Sec-WebSocket-Key, -Version or -Extensions. Headers such as `Authorization`, `Cookie`, `Origin` and `Sec-WebSocket-Protocol` go as they're given.

## A dead connection

A connection can die without a word: a laptop sleeps, or its Wi-Fi goes, and no FIN reaches either side, so the socket looks open for minutes and `receive` just gives `none`. A connection that only gets the server's pings can't tell, as the client answers those itself and never shows them.

Given `ping_interval_ms`, the connection's thread notices. Once nothing has come from the server for `ping_interval_ms`, it pings the server, and if nothing comes for `ping_timeout_ms` after the ping, it closes with 1011 `keepalive ping timeout`, as far as the connection takes it, and ends: `receive` gives the messages that came before, then fails with `Timeout`. Anything from the server counts, its pong, a ping of its own, a message or part of one, so a connection is dead after `ping_interval_ms + ping_timeout_ms` with nothing at all from the server, and a busy one is never pinged. A quiet one whose server answers its pings stays open as long as it likes. It's off unless asked, as `ping_interval_ms` is 0 to start with, and Python's `websockets.connect(url, ping_interval=20, ping_timeout=25)` is this:

```
// Dead after 45 s with nothing from the server, which pings every 20 s itself.
var ws = open(url, ping_interval_ms: 20000, ping_timeout_ms: 25000)
loop {
    match ws.receive(0) {
        ok(some(m)) => handle(m),
        ok(none) => {},
        err(e) => {
            // Timeout when it went dead, Closed when the server closed it: open it again.
            ws.close()
            ws = open(url, ping_interval_ms: 20000, ping_timeout_ms: 25000)
        },
    }
    // The rest of the loop's work.
}
```

A program with a rule of its own can keep it with `ws.idle_ms()`, the time since anything last came from the server, as `ws.idle_ms() > 45000`, with or without the pings.

## Waking a receive

A thread that loops on `receive(10)` and then sends what other threads queued for it hears of each only at the timeout, up to 10 ms later, or about 15.6 ms on Windows, whose timer is that coarse, and wakes about 64 times a second while it's idle. With the connection's `Waker`, the others wake it once they've queued something, so it can wait for seconds, as a selector's self-pipe or asyncio's `call_soon_threadsafe` does in Python:

```
let outbox = Receiver<String>()
let w = ws.waker()
// Another thread: outbox.send(text), then w.wake().
loop {
    while let text = outbox.next() {
        try ws.send_text(text)
    }
    if let m = try ws.receive(60000) {
        handle(m)
    }
}
```

A wake makes the `receive` that's waiting give `none` once the messages that have come are taken, or makes the next one give it if none is waiting. Wakes that come before a `receive` are one, and every copy of a `Waker` wakes the same connection.

## How a connection ends

| `Error` | When |
| --- | --- |
| `Closed(code, reason)` | The server closed the connection: `1000` normally, `1001` when it's going away, or a code of its own from 3000 to 4999, with its reason. `Closed(1006, "")` is a connection that ended without a close, as when the server or the network went down |
| `Protocol` | The server broke the protocol: its answer to the handshake isn't a WebSocket's, or a frame isn't one RFC 6455 allows, such as a masked frame or a control frame in pieces, or text isn't UTF-8. The connection closes, with 1002, or 1007 for text that isn't UTF-8 |
| `TooLong` | A message is longer than `limit`. The connection closes with 1009 |
| `Refused(status)` | The server answered the handshake with an HTTP status rather than switching, as `404` for a path that has no WebSocket, or `403` for an origin it turns away |
| `Timeout` | It didn't open in time, or the server didn't answer `close` in time, or, with `ping_interval_ms`, nothing came from the server for `ping_timeout_ms` after a ping: the connection [died](#a-dead-connection), and the client closed it with 1011 |
| `BadUrl`, `Resolve`, `Connect`, `Tls`, `Unavailable`, `Os` | It didn't open, as with [`std.http`](http.md#errors) |

The messages that came before the end are taken first, and then `receive` fails with how it ended, and so does each call after.

## What's sent

The handshake is a GET with `Upgrade: websocket`, a random `Sec-WebSocket-Key`, version 13, `User-Agent: Calx` unless the headers give one, and the headers given. Like `std.http`, it goes through no proxy and follows no redirect. It offers no extensions, so messages aren't compressed, and it sends no `Origin`, as a program isn't a page: a server that wants one, as Chrome's DevTools turn away origins they weren't told of, gets the one given in the headers.

Each frame the client sends is masked by four random bytes from the system, as RFC 6455 has it. A message goes as one frame, and pings from the server are answered with the same bytes. The client's own pings, with `ping_interval_ms`, carry a 4-byte count, 1 for the first.

## How it's built

[std/http/websocket.calx](../../std/http/websocket.calx) is the API, and the connection is in the runtime, in [runtime/calx_http.c](../../runtime/calx_http.c), beside `std.http`'s requests. The handshake, the frames and the pings are written there, the same on every system, over a TCP and TLS connection each system makes its own way. The connection's thread does all the reading and writing, as TLS allows one thread at a time on a connection, and the program's sends wake it.

| Platform | The connection |
| --- | --- |
| Windows | Winsock, and for `wss://` Schannel, the system's TLS, loaded from secur32.dll when the first one opens: TLS 1.3 from Windows 10 1809 and 11, and 1.2 before, with the certificate checked against the roots Windows trusts and the host, as WinHTTP checks it. WinHTTP's own WebSocket API isn't used, as it can't send a ping, never shows the program one, and takes no receive timeout once it's open, so with it a dead connection can't be told from a quiet one. The program's sends wake the thread with an event |
| Linux, macOS | libcurl, loaded when the first connection opens, in its connect-only mode, which makes the TCP and TLS connection with the system's certificate checks. libcurl's own WebSocket API isn't used, as it's missing from most systems' builds, Ubuntu's among them. The program's sends wake the thread through a pipe |
| iOS | Network.framework, which makes the TCP connection, and for `wss://` the TLS one with the system's certificate checks. URLSession's own WebSocket task isn't used, as it hides the frames, the pings and the close's code. What comes is kept until the connection's thread reads it, and that and the program's sends wake the thread through pipes |

## Checked

[tests/std/http_websocket.calx](../../tests/std/http_websocket.calx) runs a WebSocket server of its own, written on [`std.net`](net.md), and checks each part against it: the handshake with headers and a subprotocol, a frame in the same packet as the handshake's answer, both kinds of message both ways, a message in three frames with a ping between them, frames of each length's form, closes from either side and the codes the client sends, a connection dropped without a close, what breaks the protocol, a message past the limit, a refusal, a wrong answer to the key, messages sent while the connection opens, a quiet connection that stays open while the server answers its pings, and one that ends with `Timeout` and a 1011 close within the ping interval and timeout once the server stops answering without closing. CI runs it on Windows, Linux and macOS, all over `ws://`, so `wss://` was checked by hand: on Windows 11, against `wss://echo.websocket.org` over TLS 1.3, with text, a 100 000-byte text message and a 70 000-byte binary one echoed, six quiet seconds kept open by pings each second, and the handshake failing with `Tls` for `self-signed.badssl.com`, `expired.badssl.com`, `wrong.host.badssl.com` and `untrusted-root.badssl.com`. On the iOS simulator, [tests/std/http_ios.calx](../../tests/std/http_ios.calx) runs the same tests, and `wss://echo.websocket.org` echoed. On Linux it also talked to `wss://echo.websocket.org` and drove headless Chrome 157 over its DevTools protocol: a page set up with `Runtime.evaluate`, a real click hit-tested at its coordinates with `Input.dispatchMouseEvent`, the page's console events, and a screenshot from `Page.captureScreenshot`.
