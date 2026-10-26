# Calx spec v0

Status: skeleton. Fill each section from what the five sample addons (`calcite/plugins/samples/`) and the three Calcite sketches (tick loop, region actor, packet handler) needed. The "Must" lists are requirements taken from the build plan. They say what a section has to settle, not how.

## Types

TBD

## Integer and float semantics

TBD

Must match Java exactly. Each of these is undefined behavior or implementation-defined in C, so the spec has to say what Calx does and the emitter has to produce C that does it:

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

## Region actors and messages

TBD

Must:

- Be the only concurrency model
- Give identical results with one region thread and with many. That means message delivery order can't depend on scheduling (for example, deliver at tick boundaries in a fixed order such as sender region, then sequence number), and region split and merge decisions depend only on world state, never on timing
- Enforce region scoping at compile time, for Calcite and for native plugins

## FFI

TBD: `extern`, C ABI, what trusted plugins unlock

## Codegen hooks

TBD: packet declarations, generated data tables, `#line` directives

## Non-goals

TBD: what Calx leaves out
