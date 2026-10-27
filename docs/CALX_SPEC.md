---
title: Calx spec
description: The design of every Calx feature that isn't built yet.
---

# Calx spec v0

Status: skeleton. Fill each section from what the five sample addons (`calcite/plugins/samples/`) and the three Calcite sketches (tick loop, region actor, packet handler) needed. The "Must" lists are requirements from the build plan and the decision log (`calcite/docs/decisions.md`). They say what each section has to settle, not how.

This file holds features that are designed but not built. When a feature ships, its section moves to a page in [`reference/`](reference/), which becomes the normative text, and a link is left here (D40). Each feature lives in exactly one place.

## Types

TBD

Must:

- Generics and interfaces, monomorphized into C (D26)
- No null: absence is an optional type (D23)
- Handle types for game objects, checked on use (D24)

## Integer and float semantics

TBD

Must match Java exactly. Each of these is undefined or implementation-defined behavior in C, so the spec has to say what Calx does and the emitter has to produce C that does it:

- Signed overflow wraps (two's complement), for `i8`/`i16`/`i32`/`i64`
- Shift counts are masked to 31 (`int`) or 63 (`long`), and `>>>` exists
- `MIN / -1` gives `MIN` and `MIN % -1` gives `0` (both trap on x86 in C)
- Integer division and remainder by zero: Java throws. Calx must pick a behavior
- Float to int conversion saturates, and NaN converts to 0
- `char` is unsigned 16-bit, and `byte` is signed
- Float arithmetic is IEEE 754 binary32/binary64 with no fused multiply-add contraction (`-ffp-contract=off`) and no fast-math
- Transcendentals: StrictMath (fdlibm) ports, plus whatever HotSpot's `Math.*` intrinsics do wherever vanilla relies on them (the rig decides which)

## Memory model

TBD: arenas, ownership

Must:

- Be memory-safe outside `unsafe`: bounds checks (elided when provable), no pointer arithmetic, no unchecked casts (D23)
- Restrict `unsafe` and `extern` to Calcite core and trusted plugins
- Let the emitter prove non-aliasing and emit `restrict` (D26)

## Panics

TBD

Must:

- Stop only the failing plugin instance when a plugin panics, so its region keeps ticking (D25)
- Leave no leaked memory behind when an instance is stopped (arenas make this cheap)

## Strings

TBD

Must:

- Be UTF-8 internally (D28)
- Provide Java's `String.hashCode` (over UTF-16 code units), UTF-16 length for protocol limits, and a Modified UTF-8 codec for NBT

## Collections

TBD

Must:

- Provide Java-compatible `HashMap`, `HashSet` and `LinkedHashMap`, and fastutil-compatible open hash collections, whose iteration order matches Java's exactly (D27)
- Provide fast native collections for everything where order isn't observable

## Region actors and messages

TBD

Must:

- Be the only concurrency model
- Tick in lockstep: every region finishes tick N before any region starts tick N+1 (D19)
- Run each tick as a global phase, then the region phase, then message exchange, with delivery ordered by sender region ID, then send sequence (D20)
- Give identical results for the same input log with one region thread and with many (D21)
- Make region split and merge decisions depend only on world state, never on timing (D22)
- Enforce region scoping at compile time, for Calcite and for native plugins

## Modules and builds

TBD

Must:

- Compile each module to its own C translation unit, so builds are separate and incremental (D26)

## FFI

TBD: `extern`, C ABI, what trusted plugins unlock

## Codegen hooks

TBD: packet declarations, generated data tables, `#line` directives

## Non-goals

TBD: what Calx leaves out
