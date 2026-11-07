---
title: std.json
description: JSON text parsed into a Json tree with the line and column of any mistake, written back compact or indented, and @json, which reads and writes a struct from its fields' types.
---

# `std.json`

JSON as RFC 8259 defines it: text parsed into a tree of `Json` values, with the line and column of what's wrong when it isn't JSON, values written back as compact or indented text, and `@json`, which writes a struct's `to_json` and `from_json` from its fields' types, as serde's derives and Python's dataclasses do.

```calx
import std.json.{json, Json, JsonError, parse_json, pretty_json}

@json
struct Server {
    name: String
    @json_key("maxPlayers")
    max_players: u32
    motd: String?
    tags: List<String>
    port: u16 = 25565
}

fn main() ! JsonError {
    let s = try Server.from_json_text(#"{"name": "lobby", "maxPlayers": 50, "tags": ["survival"]}"#)
    println("{s.name} {s.max_players} {s.motd} {s.tags} {s.port}")    // out: lobby 50 none [survival] 25565
    println(s.to_json_text())    // out: {"name":"lobby","maxPlayers":50,"motd":null,"tags":["survival"],"port":25565}

    let tree = try parse_json(#"{"id": 7, "pos": [1.5, -2]}"#)
    println("{tree.get("id")} {(tree.get("pos") ?? Json.Null).at(1)}")    // out: some(7) some(-2)
    println(pretty_json(tree, 2))
    // out: {
    // out:   "id": 7,
    // out:   "pos": [
    // out:     1.5,
    // out:     -2
    // out:   ]
    // out: }
}
```

## Items

| Item | Gives |
| --- | --- |
| `Json` | A JSON value. See [Values](#values) |
| `JsonError` | Why text isn't JSON, or a value isn't what `from_json` wants: `message`, and the `line` and `column` in the text, from 1, or 0 for an error in a value. `{e}` is `line 3, column 5: ...`, or the message alone |
| `parse_json(text: String) -> Json ! JsonError` | The value in `text`, with nothing but white space around it |
| `{v}` | The value as compact JSON, with no white space: what JavaScript's `JSON.stringify(v)` writes |
| `pretty_json(value: Json, indent: usize) -> String` | The value with each element and member on a line of its own, `indent` spaces a level, as Python's `json.dumps(v, indent=2)` writes it. See [Writing](#writing) |
| `v.get(key: String) -> Json?` | A copy of member `key`'s value, if `v` is an object that has one |
| `v.at(i: usize) -> Json?` | A copy of element `i`, if `v` is an array that long |
| `v.as_str()`, `v.as_bool()` | The text or the bool, if `v` is one |
| `v.as_i64() -> i64?`, `v.as_u64() -> u64?` | The number, if `v` is an integer that fits |
| `v.as_f64() -> f64?` | Any number, as the nearest `f64` |
| `v.is_null() -> bool` | Whether `v` is `null` |
| `v.len() -> usize` | An array's elements or an object's members, and 0 for anything else |
| `v.keys() -> List<String>` | An object's keys, in order |
| `v.kind() -> String` | `null`, `a bool`, `a number`, `a string`, `an array` or `an object`, for messages |
| `@json` | On a struct: its `to_json`, `to_json_text`, `from_json` and `from_json_text`. See [`@json`](#json) |

## Values

```
pub enum Json {
    Null
    Bool(bool)
    Int(i64)                          // an integer that fits an i64
    UInt(u64)                         // one past that, up to 2^64 - 1
    Float(f64)                        // any other number
    Str(String)
    Array(List<Json>)
    Object(LinkedMap<String, Json>)   // in the text's order
}
```

- **Integers are exact.** A number written without a fraction or an exponent is an `Int` if it fits an `i64` and a `UInt` if it fits a `u64`, so an id comes through to the last digit, as it does with serde_json. Anything else is a `Float`: `1.0`, `1e3`, and an integer too big for either, which is the nearest `f64`. JavaScript reads every number as an `f64`, so an integer past 2^53 doesn't survive a trip through a browser; that's JavaScript's limit, not JSON's.
- **Objects keep their order.** An object's members are a [`LinkedMap`](../reference/linked-maps.md), in the order the text had them, so a file read and written back keeps its order, and `m[key]` finds one at once. A key written twice keeps its first place and its last value, as in Python and JavaScript.
- **The grammar is JSON's, no more:** no comments, no commas after the last element, no single quotes, no `NaN` or `Infinity`, no leading zeros or `+`. Arrays and objects may nest 512 deep, so text from anyone can't run the stack out.

`get` and `at` copy the value, which suits a number or a short string. A `match` reads a big one in place:

```calx
import std.json.{Json, parse_json}

fn main() {
    let j = parse_json(#"{"players": [{"name": "alex"}, {"name": "sam"}], "max": 18446744073709551615}"#)!
    if let Json.Object(members) = j {
        if let Json.Array(players) = members["players"] {
            for p in players {
                println("{p.get("name") ?? Json.Null}")
            }
        }
        // out: "alex"
        // out: "sam"
        println("{members["max"].as_u64()} {members["max"].as_i64()} {members["max"].kind()}")    // out: some(18446744073709551615) none a number
    }
}
```

## Writing

- `{v}` writes compact JSON, and `pretty_json(v, indent)` writes each element and member on a line of its own, with `: ` after a key. An empty array or object stays `[]` or `{}`. An `indent` of 0 still puts each on a line of its own, without indenting it, as Python's `indent=0` does.
- A string escapes `"`, `\` and the control characters, as `\n`, `\t` and the like or `\u` and four hex digits, and writes everything else as itself, so text that isn't ASCII stays UTF-8, as Python's `ensure_ascii=False` and serde_json write it.
- A float is written with the fewest digits that read back as the same `f64`, as `{x}` prints it, and with `.0` if it has no fraction, so it reads back as a `Float`. JSON has no `NaN` or infinity, and they're written as `null`, as JavaScript writes them.

```calx
import std.json.{Json, pretty_json}

fn main() {
    var m = LinkedMap<String, Json>()
    m.insert("text", Json.Str("tab\tquote\" é"))
    m.insert("nums", Json.Array([Json.Float(0.1), Json.Float(2.0), Json.Float(0.0 / 0.0), Json.Int(-3)]))
    m.insert("empty", Json.Object(LinkedMap<String, Json>()))
    let v = Json.Object(m)
    println("{v}")    // out: {"text":"tab\tquote\" é","nums":[0.1,2.0,null,-3],"empty":{}}
    println(pretty_json(v, 0))
    // out: {
    // out: "text": "tab\tquote\" é",
    // out: "nums": [
    // out: 0.1,
    // out: 2.0,
    // out: null,
    // out: -3
    // out: ],
    // out: "empty": {}
    // out: }
}
```

## Errors

What isn't JSON is a `JsonError` with the line and the column, both from 1, the column counted in runes, and a message that says what was found and what should have been there:

```calx
import std.json.{parse_json}

fn main() {
    for text in ["[1, 2,]", "\{\"a\" 1\}", "[01]", "\"tab\there\"", "\{\n  \"a\": [1,\n    nul]\n\}"] {
        match parse_json(String(text)) {
            ok(v) => println("{v}"),
            err(e) => println("{e}"),
        }
    }
    // out: line 1, column 7: `]` where a value should be
    // out: line 1, column 6: `1` after a key, where `:` should be
    // out: line 1, column 2: a number that starts with `0` and goes on, which JSON doesn't allow
    // out: line 1, column 5: a control character in a string, which JSON writes as an escape, such as `\n`
    // out: line 3, column 5: `n` where a value should be
}
```

## `@json`

`@json` on a struct writes four functions for it, from its fields' types:

| Function | Does |
| --- | --- |
| `s.to_json() -> Json` | The struct as an object with a member for each field, in the fields' order |
| `s.to_json_text() -> String` | That, as compact text |
| `T.from_json(value: Json) -> T ! JsonError` | The struct an object describes. Members it has no field for are passed over |
| `T.from_json_text(text: String) -> T ! JsonError` | The struct JSON text describes |

The fields can be of these types:

| Field | In JSON |
| --- | --- |
| `bool` | `true` or `false` |
| `i8` to `i64`, `u8` to `u64`, `usize` | An integer, checked against the type's range. `1.0` isn't one |
| `f64`, `f32` | Any number, an integer too |
| `String` | A string |
| `Json` | Anything, as it is |
| `T?` | `null` for `none`, or what `T` is |
| `List<T>` | An array of what `T` is |
| `Map<String, T>`, `LinkedMap<String, T>`, `SortedMap<String, T>` | An object whose members are what `T` is, written in the map's order |
| A struct with `@json` of its own | What it is, from its own module too, once it's imported |

- **A member that isn't there** is `none` for an optional field, and the field's default if it has one, as `port` has above. For any other field it's an error, `` `Server.name` is missing ``.
- **`@json_key("...")` on a field** names its member in JSON, for the names a format already has, such as `maxPlayers`. A field is written under its own name otherwise.
- **An error names what's wrong** as the struct's name and the member's key, with `[]` for an element of a list or a map: `` `Server.tags[]` should be a string, not a number ``.
- `@json` refuses a generic struct, a map whose keys aren't `String`, and a field of any other type, such as a tuple, with why, at the `@json`. A struct field whose type has no `@json` of its own is an error in the code `@json` writes, that the type has no `to_json`; give that type `@json` too.
- `calx expand` prints the code `@json` writes, which is ordinary Calx.

```calx
import std.json.{json, JsonError}

@json
struct Pos {
    x: f64
    z: f64
}

@json
struct Player {
    name: String
    level: u8
    pos: Pos
    home: Pos?
    inventory: SortedMap<String, u32>
}

fn read(text: String) {
    match Player.from_json_text(text) {
        ok(p) => println("ok {p.to_json_text()}"),
        err(e) => println("{e}"),
    }
}

fn main() {
    read(#"{"name": "alex", "level": 7, "pos": {"x": 1, "z": -2.5}, "inventory": {"stone": 64, "apple": 3}, "extra": true}"#)
    // out: ok {"name":"alex","level":7,"pos":{"x":1.0,"z":-2.5},"home":null,"inventory":{"apple":3,"stone":64}}
    read(#"{"name": "alex", "level": 300}"#)    // out: `Player.level` should be from 0 to 255, not 300
    read(#"{"name": "alex", "level": 7, "pos": {"x": 1}}"#)    // out: `Pos.z` is missing
    read(#"{"name": "alex", "level": 7, "pos": {"x": 1, "z": 2}, "inventory": {"stone": -1}}"#)    // out: `Player.inventory[]` should be from 0 to 4294967295, not -1
    read(#"{"name": "alex", "level": 7,}"#)    // out: line 1, column 29: `}` in an object, where a key in double quotes should be
}
```

## How it works

`std.json` is written in Calx: [std/json.calx](../../std/json.calx).

- The parser goes through the text's bytes once, by recursive descent. A string is copied a run at a time between its escapes, and a number is checked against JSON's grammar as its digits add up in a `u64`; one with a fraction or an exponent, or too big, is read with `f64.parse`. A line and a column are counted only when there's an error to report, so text that parses pays nothing for them.
- `\u` escapes are joined as UTF-16 surrogate pairs, and half a pair alone is an error, since a `String` holds only valid UTF-8.
- The writer adds to one `String`, and copies a string a run at a time between the characters it escapes.
- `@json` is an [attribute](../reference/attributes.md), written in Calx with [`std.meta`](meta.md). The code it writes reads through the tree: `from_json_text` parses the text first, then reads the struct from the `Json` value, a field at a time.
