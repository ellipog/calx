---
title: std.mail
description: Messages between actors, such as regions, collected at the barrier and delivered in order of sender id, then of sending, with any number of threads.
---

# `std.mail`

Messages between actors, delivered in a fixed order (D20, D115). An actor is a struct that a [`parallel for`](../reference/parallel.md) runs, such as a region. It keeps an `Outbox` for what it sends and an `Inbox` for what it's sent. At the barrier between ticks, one thread collects every outbox into a `Mail` in ascending sender id, and hands each letter to the inbox it's for. So every inbox gets its messages in order of sender id, then in the order each sender sent them, with one thread or many.

```calx
import std.mail.{Inbox, Mail, Outbox}

enum Msg {
    Add(i64)
    Note(String)
}

struct Region {
    id: u32
    total: i64
    notes: List<String>
    inbox: Inbox<Msg>
    outbox: Outbox<u32, Msg>
}

// A region's tick: what it was sent last tick, then what it sends.
fn tick(r: mut Region, count: u32) {
    while let (from, m) = r.inbox.next() {
        match m {
            Msg.Add(x) => r.total += x,
            Msg.Note(s) => r.notes.push("{from}:{s}"),
        }
    }
    r.outbox.send((r.id + 1) % count, Msg.Add(i64(r.id) + 1))
    r.outbox.send(0, Msg.Note("hi"))
}

// The barrier: every outbox in ascending sender id, then each letter to its inbox.
fn exchange(regions: mut List<Region>, mail: mut Mail<u32, Msg>) {
    for k in 0usize..regions.len() {
        mail.collect(regions[k].id, mut regions[k].outbox)
    }
    while let l = mail.next() {
        regions[usize(l.to)].inbox.deliver(l.from, l.message)
    }
}

fn main() {
    var regions: List<Region> = []
    for id in 0u32..3 {
        regions.push(Region { id: id, total: 0, notes: [], inbox: Inbox.new(), outbox: Outbox.new() })
    }
    var mail: Mail<u32, Msg> = Mail.new()
    for _ in 0..2 {
        parallel for k in 0usize..regions.len() {
            tick(mut regions[k], 3)
        }
        exchange(mut regions, mut mail)
    }
    println("{regions[0].total} {regions[1].total} {regions[0].notes}")    // out: 3 1 [0:hi, 1:hi, 2:hi]
}
```

Spawning an actor is putting it in the collection the `parallel for` goes over, and its id is whatever the program gives it. A game server might make its global actor 0 and number its regions from 1, in an order that's a function of the world, so the delivery order is too (D22).

## Items

| Item | Does |
| --- | --- |
| `Outbox<D, M>` | What an actor sends: messages of type `M` to destinations of type `D`, in the order sent |
| `Outbox.new()` | An empty outbox |
| `o.send(to: own D, message: own M)` | Sends `message` to `to`. Both move into the outbox until the barrier collects them |
| `Mail<D, M>` | The letters of every outbox at a barrier, in delivery order |
| `Mail.new()` | Empty mail, ready for a round |
| `mail.collect(from: u32, outbox: mut Outbox<D, M>)` | Takes every message in `outbox`, as sent by `from`, and leaves it empty. Panics unless `from` is greater than the sender collected before it this round |
| `mail.next() -> Letter<D, M>?` | The next letter in delivery order, or `none` once every letter is taken, which ends the round |
| `Letter<D, M>` | A message on its way, with the fields `from: u32`, `to: D` and `message: M` |
| `Inbox<M>` | What an actor has been sent: messages and their senders, in delivery order |
| `Inbox.new()` | An empty inbox |
| `i.deliver(from: u32, message: own M)` | Puts `message`, from `from`, after the others |
| `i.next() -> (u32, M)?` | The next message and its sender, or `none` when there are no more |
| `len()`, `is_empty()` | On all three: how many messages they hold, and whether they hold none |

`D` and `M` are `Send`: values that can go from one region to another. Every type is, except one that is or holds a pointer or an arena list, and generic code that puts its own `T` in a message gives `T` the bound `Send`.

## The order is checked

The order can't depend on which thread ran which actor, so `collect` panics when senders come out of order, or one comes twice in a round, and names the line that called it ([`@track_caller`](../reference/printing.md#panics-that-name-the-caller)):

```calx
import std.mail.{Mail, Outbox}

fn main() {
    var a: Outbox<u32, i32> = Outbox.new()
    var b: Outbox<u32, i32> = Outbox.new()
    a.send(1, 10)
    var mail: Mail<u32, i32> = Mail.new()
    mail.collect(2, mut a)
    mail.collect(1, mut b)    // panic: mail from 1 is collected after mail from 2
}
```

A round ends when `next` returns `none`, so the next tick's barrier can start again from the lowest id.

## Why there's no `actor` keyword

Early designs had `actor struct` and a `send dest msg` statement that appended to the current actor's outbox. Once `parallel for` over owned values became the only parallelism (D49), an outbox the actor holds as a field did the same work without hidden state. A function that sends takes what it sends from, and the capture check keeps every outbox to its own iteration. So `actor` and `send` are ordinary names (D115).

`std.mail` is written in Calx: [std/mail.calx](../../std/mail.calx). Each type is a [`Deque`](../reference/deques.md) inside, so sending, collecting, delivering and taking are each a push or a pop.
