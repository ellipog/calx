---
title: std.meta
description: Declarations as data for attributes. StructDecl, EnumDecl and FnDecl, their fields, variants, parameters and types, and Generated, the code an attribute gives back.
---

# `std.meta`

What an [attribute](../reference/attributes.md) reads and returns. The compiler gives an attribute the declaration it's on as one of three types, `StructDecl`, `EnumDecl` or `FnDecl`, and the attribute returns a `Generated` with the code to add (D124).

```calx
// file: main.calx
import gen.{variants, logged}

@variants
enum Shape {
    Circle(f64)
    Rect { w: f64, h: f64 }
    Dot
}

@logged
fn twice(n: i32) -> i32 {
    return n * 2
}

fn main() {
    println(shape_variants())    // out: Circle(f64), Rect { w: f64, h: f64 }, Dot
    println("{twice_logged(21)}")
    // out: twice(n: i32) -> i32
    // out: 42
}

// file: gen.calx
import std.meta.{EnumDecl, FnDecl, Generated, TypeKind}

/// A function that lists the variants, as they're written.
@attribute
pub fn variants(decl: EnumDecl) -> Generated {
    var names = String()
    for v in decl.variants {
        if not names.is_empty() {
            names.push_str(", ")
        }
        names.push_str(v.name)
        if v.payload.len() > 0 {
            names.push_str("({v.payload[0].text})")
        }
        if v.fields.len() > 0 {
            names.push_str(#" { "#)
            for i in 0..v.fields.len() {
                if i > 0 {
                    names.push_str(", ")
                }
                names.push_str("{v.fields[i].name}: {v.fields[i].ty.text}")
            }
            names.push_str(#" }"#)
        }
    }
    var g = Generated.new()
    // The code holds a raw string itself, so this one takes two `#`s.
    g.add(##"
        fn ##{decl.name.to_lower_root()}_variants() -> String {
            return #"##{names}"#
        }
        "##)
    return g
}

/// A version of the function that says it's called.
@attribute
pub fn logged(decl: FnDecl) -> Generated {
    var params = String()
    var args = String()
    for p in decl.params {
        if not params.is_empty() {
            params.push_str(", ")
            args.push_str(", ")
        }
        params.push_str("{p.name}: {p.ty.text}")
        args.push_str(p.name)
    }
    let result = if decl.result.kind == TypeKind.Tuple { String() } else { " -> {decl.result.text}" }
    var g = Generated.new()
    g.add(#"
        fn #{decl.name}_logged(#{params})#{result} {
            println("#{decl.name}(#{params})#{result}")
            return #{decl.name}(#{args})
        }
        "#)
    return g
}
```

Everything is as the source writes it. Types, defaults and values are text, and nothing is resolved: a field of type `VarInt` says `VarInt`, whatever that names.

## Declarations

| Type | Fields |
| --- | --- |
| `StructDecl` | `name`, `is_pub`, `generics`, the names of its generic parameters, `fields: List<Field>`, `doc` |
| `EnumDecl` | `name`, `is_pub`, `generics`, `variants: List<Variant>`, `repr`, such as `"u8"` for `enum Flag: u8` or `""`, `doc` |
| `FnDecl` | `name`, `is_pub`, `is_unsafe`, `generics`, `params: List<Param>`, `result: TypeRef`, `error: TypeRef`, `can_fail`, `doc` |

`doc` is the declaration's `///` lines, joined with newlines, without the slashes and the space after them.

| Type | Fields |
| --- | --- |
| `Field` | `name`, `ty: TypeRef`, `is_pub`, `default`, its default value as written or `""`, `attributes: List<AttributeUse>`, `doc` |
| `Variant` | `name`, `payload: List<TypeRef>`, the types of `Circle(f64)`, `fields: List<Field>`, the fields of `Rect { w: f64, h: f64 }`, `value`, such as `"1"` for `OnGround = 1` or `""`, `doc` |
| `Param` | `name`, `mode: Mode`, `is_self`, `ty: TypeRef` |
| `AttributeUse` | `name`, `args: List<String>`, each argument as written, such as `255` or `"hp"` |

- `Mode` is `Read`, `Mut` or `Own`.
- A function that returns nothing has a `result` of kind `Tuple`, with no `args`. One that can't fail has an `error` like that too.
- A bare `self` parameter has `is_self` set and a type of kind `SelfType`. `self: Point` has the type as written.
- `AttributeUse` holds the attributes on a field that the declaration's attribute lists as the ones it reads ([attributes on fields](../reference/attributes.md#attributes-on-fields)).

## Types

A `TypeRef` is a type as it's written:

| Field | Holds |
| --- | --- |
| `kind: TypeKind` | Which kind of type it is |
| `text` | The whole type: `List<VarInt>` |
| `name` | A named type's name, such as `List` or `std.c.int`, or a `dyn` type's interface |
| `args: List<TypeRef>` | The types inside it |
| `len` | An array's length, as written |

| `TypeKind` | Written | Has |
| --- | --- | --- |
| `Named` | `i32`, `Point`, `List<T>` | `name`, and `args` when it has any |
| `Optional` | `T?` | `args[0]` |
| `Array` | `[T; N]` | `args[0]` and `len` |
| `Tuple` | `(A, B)` and `()` | `args` |
| `Function` | `fn(A, B) -> R` | The parameters as `args`. The rest is in `text` |
| `Pointer` | `*T` and `*const T` | `args[0]` |
| `Dyn` | `dyn Shape` | `name` |
| `SelfType` | `Self` | Nothing |

## Generated

| Item | Does |
| --- | --- |
| `Generated.new()` | No code yet |
| `g.add(code: own String)` | Adds one or more items, written as Calx: functions, `impl`s, structs, enums, constants and imports |

The compiler adds the items to the declaration's module and checks them with the rest ([the code it generates](../reference/attributes.md#the-code-it-generates)).

## The rest

`Request`, `answer` and `refuse` are for the program the compiler builds to run a program's attributes: it reads the declarations from its standard input with `Request`, and gives each use its answer. An attribute doesn't need them.
