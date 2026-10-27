---
title: Calx documentation
description: Index of the Calx docs, the page order, and the rules for writing them.
---

# Calx documentation

These pages are the source for the Calx docs website. Until the site exists, read them here.

## Pages

In reading order:

### Reference

What's built, one page per feature:

1. [The calx command](reference/command.md): check, build, run, emit-c, and how builds stay fast
2. [Syntax](reference/syntax.md): files, comments, names, keywords, literals and operators
3. [Types](reference/types.md): the primitive types, arrays, structs and unit
4. [Variables](reference/variables.md): let, var, constants, assignment, unused names, no shadowing
5. [Integers](reference/integers.md): wrapping, division, shifts and constant expressions, as in Java
6. [Floats](reference/floats.md): IEEE 754 arithmetic and printing, as in Java
7. [Conversions](reference/conversions.md): exact `T(x)` and the named lossy conversions
8. [Control flow](reference/control-flow.md): if, while, for, loop, break and continue
9. [Functions](reference/functions.md): parameter modes, named arguments, returns, evaluation order
10. [Structs](reference/structs.md): fields, defaults, literals and methods
11. [Printing and panics](reference/printing.md): print, println, assert and panic
12. [Strictness](reference/strictness.md): the rules that make the checker stricter than Rust's

### Design

1. [Calx spec](CALX_SPEC.md): every feature that is designed but not built yet
2. [Tick-loop benchmark](benchmarks.md): the workload behind the Phase 3 performance gate

The guide starts once enough of the language is built to write a real program.

## How the docs are organized

| Section | Folder | For | Normative? |
| --- | --- | --- | --- |
| Guide | `guide/` | Learning Calx, in reading order, as tutorials | No |
| Reference | `reference/` | One page per shipped feature: syntax, meaning, examples, errors | Yes |
| Spec | `CALX_SPEC.md` | Features that are designed but not built yet | Yes, until the feature ships |

A feature lives in exactly one normative place. When it ships, its spec section moves to a reference page, and a link is left behind in the spec.

## Rules

- **Docs ship with the feature.** A change to the language, the compiler's behavior, the standard library or the runtime updates its pages in the same change. A feature without a reference page isn't done.
- **Examples are tests.** From Phase 3, every Calx code block in these docs is compiled by the compiler test suite, and run if it shows output. A block that should fail to compile is marked with the error it must produce.
- **One topic per page.** Each page starts with front matter giving its `title` and `description`, so a site generator can build the website from these files unchanged.
- **Relative links only.** Link to other pages by relative path, such as `reference/integers.md`, so the links work both here and on the site.
- **Page order lives here.** When you add a page, add it to the list above.
