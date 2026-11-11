---
title: Roadmap
description: Where Calx is - what's built and tested, what's designed but not built, and what's planned after that.
---

# Roadmap

Calx is pre-release: there's no stable release yet, and the language can still change. One command installs the newest nightly build ([Install Calx](guide/install.md)). The board Calx is built from is on [calxlang.com/roadmap](https://calxlang.com/roadmap/), where you can vote for what you'd like sooner, suggest ideas and report problems. This page sums it up by feature, and the site shows the summary under the board. When a feature is built, its section moves from the spec to the reference, and this page moves it up.

Calx's first large program is [Calcite](https://github.com/ellipog/calcite), a Minecraft server, so Calcite's needs decide what gets built first. Every feature is designed for programs in general, though, and the reference describes them that way.

## Built

Everything in the [reference](README.md#reference) and the [standard library](README.md#standard-library) is built, and every example on those pages is compiled and run on Windows and Linux on every push:

- The types: integers and floats with defined results on every platform, structs, enums, tuples, optionals, error values, generics, and interfaces, with operators, indexing, associated types, and `dyn` values of any type that has one.
- Memory safety without a garbage collector: ownership, views, arenas, `Box<T>` and `Shared<T>`, with `unsafe` code and C functions for what the checker can't see.
- `parallel for` over ranges, lists, slot maps and maps, on the main pool or one the program makes, whose capture check gives the same result with any number of threads, and messages between actors, delivered in a fixed order.
- Modules, libraries that a program opens while it runs, and attributes that libraries write in Calx, which generate code while the program compiles.
- Lists, deques, priority queues, maps and sets, the [Java-compatible](README.md#java-compatibility) hash tables, and slot maps, whose handles say when a value is gone instead of reading freed memory.
- Standard library modules for input and output, files, the network, threads, time, math, hashing, compression and the terminal.
- The `calx` command, with cached builds that redo only what changed, and the [VS Code extension](reference/editors.md).

## Designed, not built

The [spec](CALX_SPEC.md) has the design of each of these:

- **Plugins that fail alone**: a plugin's panic stops only that plugin, and its memory is all freed when it stops ([spec](CALX_SPEC.md#arenas-and-plugin-memory)).

## Later

- **Plugins as WebAssembly**, built from the same source, for hosts that run code they don't trust.
- **A language server**, so editors other than VS Code get errors, completion and go-to-definition, and `calx` commands for building and testing plugins.
- **Limits on plugins' time**: checks the compiler puts in plugin builds, so a plugin that runs too long can be stopped.
- **Quarry**, a package manager for Calx libraries, which are called stones.
