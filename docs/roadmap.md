---
title: Roadmap
description: Where Calx is - what's built and tested, what's designed but not built, and what's planned after that.
---

# Roadmap

Calx is pre-release: there are no releases yet, and the language can still change. You build the compiler from source ([Install Calx](guide/install.md)). The work is tracked in the open, on the [Calx board](https://acta.aaenz.no/aaen-studios/clx), and this page sums it up.

Calx's first large program is [Calcite](https://github.com/ellipog/calcite), a Minecraft server, so Calcite's needs decide what gets built first. Every feature is designed for programs in general, though, and the reference describes them that way.

## Built

Everything in the [reference](README.md#reference) and the [standard library](README.md#standard-library) is built, and every example on those pages is compiled and run on Windows and Linux on every push:

- The types: integers and floats with defined results on every platform, structs, enums, tuples, optionals, error values, generics and interfaces.
- Memory safety without a garbage collector: ownership, views, arenas and `Box<T>`, with `unsafe` code and C functions for what the checker can't see.
- `parallel for`, whose capture check gives the same result with any number of threads, and messages between actors, delivered in a fixed order.
- Modules, libraries that a program opens while it runs, and attributes that libraries write in Calx, which generate code while the program compiles.
- Lists, deques, priority queues, maps and sets, and the [Java-compatible](README.md#java-compatibility) hash tables.
- Standard library modules for input and output, files, the network, threads, time, math, hashing, compression and the terminal.
- The `calx` command, with cached builds that redo only what changed, and the [VS Code extension](reference/editors.md).

## Designed, not built

The [spec](CALX_SPEC.md) has the design of each of these:

- **Handles**: `Handle<T>`, a checked reference to an object in a slot map, which says when the object is gone instead of reading freed memory ([spec](CALX_SPEC.md#handles)).
- **Shared data**: `Shared<T>`, for data that many iterations of a `parallel for` read at once ([spec](CALX_SPEC.md#memory-model)).
- **More parallel loops**: a `parallel for` over the values of a slot map or a map, and on a pool the program picks ([spec](CALX_SPEC.md#parallel-loops)).
- **Values of different types together**: `dyn` interfaces, associated types, and interfaces for the operators ([spec](CALX_SPEC.md#interfaces-and-generics)).
- **`java.ArrayList`**, a list that grows as Java's does, for code that must match Java's capacities ([spec](CALX_SPEC.md#collections)).
- **Plugins that fail alone**: a plugin's panic stops only that plugin, and its memory is all freed when it stops ([spec](CALX_SPEC.md#arenas-and-plugin-memory)).

## Later

- **Plugins as WebAssembly**, built from the same source, for hosts that run code they don't trust.
- **A language server**, so editors other than VS Code get errors, completion and go-to-definition, and `calx` commands for building and testing plugins.
- **Limits on plugins' time**: checks the compiler puts in plugin builds, so a plugin that runs too long can be stopped.
- **Quarry**, a package manager for Calx libraries, which are called stones.

When a feature is built, its section moves from the spec to the reference, and this page moves it up.
