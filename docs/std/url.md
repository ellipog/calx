---
title: std.url
description: Percent-encoding and form bodies as Python's urllib.parse gives them, and URLs taken apart, put back together and joined as RFC 3986 says - encode, decode, form_encode, form_decode, Url.
---

# `std.url`

URLs as text. Percent-encoding and decoding give what Python's `urllib.parse` `quote` and `unquote` give, form bodies and queries are written as `urlencode` writes them and read as `parse_qsl` reads them, and a `Url` is a URL taken apart into its parts, put back together, and joined with a relative reference as RFC 3986 says and `urljoin` mostly does.

```calx
import std.url.{Url, encode, decode, form_encode}

fn main() ! String {
    println(encode("caves & cliffs/1.18"))    // out: caves%20%26%20cliffs/1.18
    println(decode("%C3%A9t%C3%A9"))          // out: été
    println(form_encode([(String("q"), String("iron golem")), (String("page"), String("2"))]))    // out: q=iron+golem&page=2

    let url = try Url.parse("https://Example.com:8443/wiki/Stone?action=raw#top")
    println("{url.host.copy() ?? "-"} {url.port ?? 443} {url.path}")    // out: example.com 8443 /wiki/Stone
    println("{try url.join("../Dirt")}")      // out: https://example.com:8443/Dirt
    for (name, value) in url.query_pairs() {
        println("{name} = {value}")           // out: action = raw
    }
}
```

## Encoding

| Function | Gives |
| --- | --- |
| `encode(text, safe = "/")` | The text with each byte of its UTF-8 as `%XX` in uppercase hex, but for letters, digits, `_`, `.`, `-` and `~`, and the ASCII characters in `safe`. Python's `quote` |
| `encode_plus(text, safe = "")` | The same with a space as `+` and `/` encoded too, for forms and queries. Python's `quote_plus` |
| `decode(text)` | The text with each `%XX` as its byte again. A `%` without two hex digits after it stays, and bytes that aren't UTF-8 become U+FFFD. Python's `unquote` |
| `decode_plus(text)` | The same with each `+` a space first. Python's `unquote_plus` |
| `form_encode(pairs: List<(String, String)>)` | Each name `=` its value, both through `encode_plus`, joined with `&`. Python's `urlencode` |
| `form_decode(text) -> List<(String, String)>` | The names and values between the `&`s, each split at its first `=` and decoded with `decode_plus`, in order. A part without `=` is a name with an empty value; empty parts are skipped. Python's `parse_qsl(text, keep_blank_values=True)` |

```calx
import std.url.{encode, encode_plus, decode, form_decode}

fn main() {
    println(encode("a/b c", safe: ""))      // out: a%2Fb%20c
    println(encode_plus("a/b c"))           // out: a%2Fb+c
    println(decode("100% %41 %E2%82"))      // out: 100% A �
    println("{form_decode("a=1&b&c=x+y&&d=1=2")}")    // out: [(a, 1), (b, ), (c, x y), (d, 1=2)]
}
```

- `decode` reads bad UTF-8 as Python does, with [`decode_lossy_unicode`](text-utf8.md): one U+FFFD for each longest piece that could still start a character.
- What isn't ASCII in `safe` is ignored, as in Python, since such characters are always encoded.

## `Url`

`Url.parse(text) -> Url ! String` takes a URL apart into its fields. Nothing is decoded, so `{url}` gives back the text it came from, but for the scheme and host, which are lowercased, as Python's `urlsplit` lowercases them.

| Field | Holds |
| --- | --- |
| `scheme: String` | `https`, in lowercase |
| `user: String?`, `password: String?` | What's before the host's `@`, split at the first `:`, still percent-encoded |
| `host: String?` | The host, lowercased, without the brackets around an IPv6 address. `none` when there's no `//` authority, as in `mailto:a@b`; empty in `file:///etc` |
| `port: u16?` | The port, if there's one after the host |
| `path: String` | The path, still percent-encoded: `/a/b`, or empty |
| `query: String?` | What follows the `?`, still encoded, or `none` when there's no `?`. `?` with nothing after it is `some("")` |
| `fragment: String?` | What follows the `#`, the same way |

| Method | Gives |
| --- | --- |
| `Url.parse(text)` | The URL's parts, or why it isn't one: it has no scheme, its port isn't a number up to 65535, or a bracket is out of place |
| `url.join(reference) -> Url ! String` | The URL a reference names when read from `url`, as RFC 3986's section 5.2 resolves it: `..` and `.` are taken out, and the parts the reference doesn't have come from `url` |
| `url.query_pairs()` | The query's names and values, as `form_decode` reads them; none if there's no query |
| `{url}` | The parts put back together, as RFC 3986's section 5.3 joins them, with brackets around a host that has a `:` |

The fields are `pub`, so a `Url` can be changed and written out again:

```calx
import std.url.{Url}

fn main() ! String {
    var url = try Url.parse("http://[::1]:25565/status")
    println("{url.host.copy() ?? "-"} {url.port ?? 0}")    // out: ::1 25565
    url.scheme = String("https")
    url.port = none
    url.query = some(String("full=1"))
    println("{url}")                                       // out: https://[::1]/status?full=1
    match Url.parse("http://h:99999/") {
        ok(_) => println("parsed"),
        err(why) => println(why),                          // out: the port 99999 is more than 65535
    }
}
```

`join` is RFC 3986's, which `urljoin` follows but for a few corners, where this gives what the RFC and browsers give:

| Base, reference | `join` | Python's `urljoin` |
| --- | --- | --- |
| `http://a/b#f`, `""` | `http://a/b`: the reference has no fragment | `http://a/b#f`, the base as it is |
| `http://a/b`, `//g/./x` | `http://g/x` | `http://g/./x`, with the dots left in |
| `http://a/b//c/d`, `e` | `http://a/b//c/e` | `http://a/b/c/e`, with the empty segment dropped |
| `http://a/b/c`, `http:g` | `http:g`, a reference with a scheme | `http://a/b/g`, read as relative since the scheme is the base's |

`Url.parse` is also stricter than `urlsplit` in two ways: text after an IPv6 address's `]` must be a port, where Python ignores it, and it doesn't take the spaces, tabs and newlines out that Python strips.

## How it's built

[std/url.calx](../../std/url.calx), in Calx, written from RFC 3986 and checked against Python 3.14's `urllib.parse`: 5,106 random cases of quoting, unquoting, forms, joins and parts, with none different but the corners above. The URL is split by offsets in one pass, so the only text made is the parts', and a path is only taken apart for its `.` and `..` when it has one.

## Speed

On our benchmark, an i9-13900K with other work running, so rough, the best of five runs, against Python 3.14:

| Work | Calx | Python | |
| --- | --- | --- | --- |
| `encode`, 1 MB of text | 0.8 ms | 12.7 ms | 16× |
| `decode` of that | 0.7 ms | 39 ms | 57× |
| `form_encode`, 20,000 pairs | 0.9 ms | 19.6 ms | 23× |
| `form_decode` of that | 1.2 ms | 29 ms | 24× |
| `join`, 12,000 references | 1.6 ms | 30.7 ms | 20× |
| `Url.parse` and its host and port, 12,000 URLs | 1.5 ms | 32.9 ms | 22× |
