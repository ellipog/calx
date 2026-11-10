---
title: std.json.schema
description: JSON Schema checked as Python's jsonschema 4.26 checks it - drafts 3, 4, 6, 7, 2019-09 and 2020-12, with the same errors, messages, paths and order, and $refs found as the referencing library finds them - Validator, Registry, check_schema, best_match.
---

# `std.json.schema`

JSON Schema, checked as Python's [jsonschema](https://python-jsonschema.readthedocs.io/) 4.26 checks it. A `Validator` for any of drafts 3, 4, 6, 7, 2019-09 and 2020-12 gives the errors `Draft202012Validator(schema).iter_errors(instance)` gives: the same ones, in the same order, with the same messages, paths and values. `$ref`, `$dynamicRef` and `$recursiveRef` are found as the referencing library finds them, in the schema, a `Registry` of others, and the drafts' meta-schemas, which are built in.

```calx
import std.json
import std.json.{JsonError}
import std.json.schema.{Draft, Validator}

fn main() ! JsonError {
    let schema = try json.parse(#"{"type": "object", "required": ["name"], "properties": {"name": {"type": "string"}, "count": {"type": "integer", "minimum": 1, "maximum": 64}}}"#)
    var v = Validator.new(schema, Draft.Draft202012)
    let item = try json.parse(#"{"count": 65}"#)
    match v.iter_errors(item) {
        ok(errors) => {
            for e in errors {
                println("{e.json_path()}: {e.message}")
            }
        },
        err(f) => println("{f}"),
    }
    // out: $: 'name' is a required property
    // out: $.count: 65 is greater than the maximum of 64
}
```

## Validators

| Calx | Python | Does |
| --- | --- | --- |
| `Validator.new(schema, draft)` | `Draft7Validator(schema)` and the others | A validator for `schema` under `draft`'s keywords and types |
| `Validator.with_registry(schema, draft, registry)` | `Draft7Validator(schema, registry=registry)` | One whose `$ref`s can find the schemas in `registry` too |
| `v.with_formats()` | `format_checker=FormatChecker()` | The same validator, checking `format` too. Only the `regex` format is checked so far |
| `v.iter_errors(instance) -> List<ValidationError> ! SchemaFault` | `list(v.iter_errors(instance))` | Every error, in Python's order |
| `v.is_valid(instance) -> bool ! SchemaFault` | `v.is_valid(instance)` | Whether there are none. It stops at the first and makes no messages |
| `v.validate(instance) -> ValidationError? ! SchemaFault` | `v.validate(instance)` | The first error, which Python raises, or `none` |
| `Draft` | the validator classes | `Draft3`, `Draft4`, `Draft6`, `Draft7`, `Draft201909` and `Draft202012` |

A validator keeps what it has worked out between calls, such as the regexes it has compiled and the meta-schemas it has read, so one made once and used for many values is the fast way, as in Python.

## Errors

A `ValidationError` has what Python's has, under the same names but one:

| Field | Python's | Is |
| --- | --- | --- |
| `message` | `message` | What's wrong, with values as Python's `repr` writes them: `'name' is a required property` |
| `keyword` | `validator` | The keyword that failed, or `none` for a `false` schema |
| `validator_value`, `instance`, `schema` | the same | The keyword's value, the value that failed, and the schema the keyword is in, as `Json` |
| `path`, `absolute_path` | the same | Where the value is, as `PathPart`s: `Key(name)` or `Index(i)`. `path` is from the value checked, or for an error in another's `context`, from that error's value |
| `schema_path`, `absolute_schema_path` | the same | The way to the keyword in the schema |
| `context` | `context` | For `anyOf`, `oneOf` and draft 3's `type`, the errors of each schema tried |
| `schema_error` | `isinstance(e, SchemaError)` | Whether `check_schema` found it, in a schema |

`e.json_path()` is `e.json_path`, such as `$.tags[1]`, and `"{e}"` is Python's `str(e)`, with the schema and the value pretty-printed under it. `best_match(errors)` picks the error most likely to say what's wrong, as jsonschema's `best_match` does: one near the root, and inside an `anyOf` or `oneOf`, the one in its context that stands out.

```calx
import std.json
import std.json.{JsonError}
import std.json.schema.{Draft, Validator, best_match}

fn main() ! JsonError {
    let schema = try json.parse(#"{"properties": {"size": {"anyOf": [{"type": "integer"}, {"enum": ["S", "M", "L"]}]}}}"#)
    var v = Validator.new(schema, Draft.Draft7)
    match v.iter_errors(try json.parse(#"{"size": "XL"}"#)) {
        ok(errors) => {
            let best = best_match(errors)!
            println(best.message)                 // out: 'XL' is not valid under any of the given schemas
            println(best.context[1].message)      // out: 'XL' is not one of ['S', 'M', 'L']
            println("{best}".split("\n")[2])      // out: Failed validating 'anyOf' in schema['properties']['size']:
        },
        err(f) => println("{f}"),
    }
}
```

## Schemas of schemas

| Calx | Python | Does |
| --- | --- | --- |
| `check_schema(schema, draft) -> ValidationError? ! SchemaFault` | `Draft7Validator.check_schema(schema)` | The first thing wrong with `schema`, checked against the draft's meta-schema with its formats, with `schema_error` set |
| `SchemaChecker.new(draft)`, then `checker.check(schema)` | `Draft7Validator(Draft7Validator.META_SCHEMA, format_checker=...)` | The same, with the meta-schema read once, for checking many schemas |
| `validator_for(schema, default) -> Draft ! SchemaFault` | `validator_for(schema, default=...)` | The draft `schema`'s `$schema` names, or `default` |
| `validate(instance, schema) -> ValidationError? ! SchemaFault` | `jsonschema.validate(instance, schema)` | Checks the schema against its meta-schema, then the value against the schema, under the draft `$schema` names or 2020-12: the schema's first error, or the value's `best_match`, or `none` |

## `$ref` and registries

A validator finds `$ref`s in its schema and in the drafts' meta-schemas, which come with it. Others are added to a `Registry`, as `Registry().with_resource(uri, Resource.from_contents(contents, default_specification=...))` adds them in Python. Nothing is fetched over the network, as jsonschema doesn't fetch since 4.18.

```calx
import std.json
import std.json.{JsonError}
import std.json.schema.{Draft, Registry, Validator}

fn main() ! JsonError {
    var reg = Registry.new()
    reg.add("https://example.com/pos.json", try json.parse(#"{"type": "object", "properties": {"y": {"type": "integer", "minimum": -64, "maximum": 320}}}"#), Draft.Draft7)
    var v = Validator.with_registry(try json.parse(#"{"items": {"$ref": "https://example.com/pos.json"}}"#), Draft.Draft7, reg)
    match v.iter_errors(try json.parse(#"[{"y": 400}]"#)) {
        ok(errors) => println("{errors[0].json_path()} {errors[0].message}"),    // out: $[0].y 400 is greater than the maximum of 320
        err(f) => println("{f}"),
    }
}
```

| Calx | Python |
| --- | --- |
| `Registry.new()` | `Registry()` |
| `reg.add(uri, contents, default)` | `registry.with_resource(uri, Resource.from_contents(contents, default_specification=d))`. The draft is the one `contents`' `$schema` names, or `default` |

## Faults

What Python raises instead of giving errors comes back as a `SchemaFault`, whose `kind` is the exception's class and whose `message` is its `str`:

| `kind` | When |
| --- | --- |
| `Unresolvable`, `PointerToNowhere`, `NoSuchAnchor`, `InvalidAnchor`, `NoSuchResource` | A `$ref` that leads nowhere: `Unresolvable: other.json` |
| `UnknownType` | A `type` the draft doesn't have |
| `PatternError` | A `pattern` or `patternProperties` key that isn't a regex in Python's `re` |
| `TypeError`, `AttributeError`, `ValueError`, `ZeroDivisionError` | A schema too broken to read, such as a `minLength` that's a string, with Python 3.14's message for it |

## Where it differs

- Integers past the range of a `u64` are floats in [`std.json`](json.md), so a message shows `1e+20` where Python shows `100000000000000000000`.
- Only the `regex` format is checked by `with_formats`: the others Python checks, such as `date`, `email` and `ipv4`, pass.
- Two of Python's results depend on the order of a set, which changes from run to run: the order of the unexpected names in `additionalProperties`' message from `check_schema`, and which broken resource a registry's crawl stops at first. Here each is one of the orders Python gives.
- `best_match` never fails. Python's raises `TypeError: unhashable type: 'dict'` when an error's schema is a draft 3 one whose `type` holds a schema, since it asks its type checker about that schema as if it were a type's name.

## How it's built

[std/json/schema.calx](../../std/json/schema.calx), in Calx, written from jsonschema's and referencing's behaviour and checked against them: every draft's tests in the [JSON-Schema-Test-Suite](https://github.com/json-schema-org/JSON-Schema-Test-Suite), and 5,400 more of schemas broken on purpose, references across registries and real configuration files, 16,810 results in all, with the errors, their messages, paths, values and contexts, `best_match`, `check_schema` and the faults compared. All are the same but the integers past `u64` above.

A schema is read once into a table of nodes, each object's keywords numbered, so a check never looks a keyword up by name. The meta-schemas are read and crawled only when a `$ref` reaches them, and a `$ref` is resolved once for each place it's reached from, then remembered for the rest of the check.

## Speed

On our benchmark, an i9-13900K's performance cores, the best of five runs, against jsonschema 4.26 on Python 3.14:

| Work | Calx | Python | |
| --- | --- | --- | --- |
| `is_valid` of 20,000 entries against a 2020-12 schema with `$ref`s, `pattern`, `enum`, `anyOf` and `uniqueItems` | 17.1 ms | 1,270 ms | 74× |
| `iter_errors` of 20,000 such entries, 2,000 of them wrong | 19.9 ms | 1,281 ms | 64× |
| The same for 2,000 entries, then `str` of each of the 200 errors | 2.4 ms | 135 ms | 56× |
| 546 real configuration files against 5 draft 7 schemas that `$ref` each other through a registry, the errors sorted by path | 7.2 ms | 147 ms | 20× |
| The test suite's 464 draft 2020-12 schemas, a validator made for each, and its 2,374 values | 4.2 ms | 25.7 ms | 6.2× |
| `check_schema` of the suite's 324 draft 7 schemas | 7.2 ms | 13.7 ms | 1.9× |
| The same with one `SchemaChecker`, against Python's one meta-schema validator | 3.1 ms | 10.8 ms | 3.5× |

`check_schema` reads the meta-schema from its text on every call, where Python's validator classes keep theirs parsed, so a `SchemaChecker` kept for many schemas is the fast way.
