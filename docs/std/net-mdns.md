---
title: std.net.mdns
description: Services announced and found on the local network with multicast DNS and DNS-SD, RFC 6762 and RFC 6763, as Python's zeroconf and Apple's Bonjour do, beside the system's own responder.
---

# `std.net.mdns`

A program's services announced on the local network, and other programs' found there, with multicast DNS and DNS-SD (RFC 6762 and RFC 6763), as Python's `zeroconf` and Apple's Bonjour do. A desktop app announces itself as `_loom._tcp` with its id in a TXT record, and a phone on the same Wi-Fi finds it by that, whatever address the PC has today. Bonjour's `dns-sd -B _loom._tcp`, avahi's `avahi-browse` and Windows' own mDNS find what it announces, and it finds what they announce: it shares UDP port 5353 with the system's own responder rather than replacing it.

```calx
// firewall: answers mDNS on every address
import std.net.mdns.{Mdns, MdnsEvent, ServiceInfo}
import std.random.{Random}

fn main() {
    // A type of its own each run, so another copy of this example running at once isn't found.
    var r = Random.system()
    let kind = "_demo{r.below(100000)}._tcp"
    var m = Mdns.new()!
    let info = ServiceInfo.new(kind.copy(), "Living Room PC", 8443).with_property("id", "7f3a")
    println("announced {m.register(info)!}")    // out: announced Living Room PC
    m.browse(kind)!
    while let e = m.next_event(5000)! {
        if let MdnsEvent.Added(s) = e {
            println("found {s.name} on port {s.port}, id {s.property("id") ?? String()}")    // out: found Living Room PC on port 8443, id 7f3a
            break
        }
    }
    m.close()
}
```

An `Mdns` is a responder and a browser at once, as zeroconf's `Zeroconf()` is, on a [`Net`](net.md) of its own. `register` announces a service and returns once it's announced, `browse` starts finding a type's services, and `next_event` gives what was found, changed and gone. It answers the network's questions about this program's services only while it's in `next_event`, as zeroconf's thread answers them, so a program that announces keeps calling it: on [a thread of its own](#on-a-thread-of-its-own) if the program has other work.

| Python's `zeroconf` | Calx |
| --- | --- |
| `zc = Zeroconf()` | `var m = Mdns.new()!` |
| `ServiceInfo("_loom._tcp.local.", "PC._loom._tcp.local.", port=8443, properties={"id": "7f3a"})` | `ServiceInfo.new("_loom._tcp", "PC", 8443).with_property("id", "7f3a")`: the type and the instance name alone, without `.local.` |
| `zc.register_service(info, allow_name_change=True)` | `m.register(info)!`, which gives the name it got, `PC (2)` if another host has `PC` |
| `zc.register_service(info)`, which raises `NonUniqueNameException` | `m.register(info, allow_name_change: false)`, which fails with `MdnsError.NameTaken` |
| `zc.update_service(info)` | `m.update(info)`, `false` if it isn't registered |
| `zc.unregister_service(info)` | `m.unregister(kind, name)`, which says goodbye |
| `ServiceBrowser(zc, "_loom._tcp.local.", listener)` | `m.browse("_loom._tcp")!`, then `m.next_event()` in a loop |
| `listener.add_service`, `update_service` and `remove_service` | `MdnsEvent.Added(info)`, `Updated(info)` and `Removed(info)`, with what's known of it already resolved |
| `zc.get_service_info(type_, name, timeout=3000)` | `m.resolve(kind, name)`, `none` if no answer came in 3 seconds |
| A `ServiceBrowser` and a `sleep(3)`, to list what's there | `find("_loom._tcp")!`, the services found in 3 seconds, by name |
| `info.server`, `info.port` | `info.host`, `info.port` |
| `info.parsed_addresses()` | `info.addresses`, as [`Ip`](net.md#addresses)s, IPv4 and IPv6 |
| `info.properties[b"id"]`, bytes | `info.property("id")`, a `String?`, its key matched without regard to case |
| `zc.close()` | `m.close()`, which says goodbye for each service still registered |
| `BadTypeInNameException` | `MdnsError.BadName(why)` |

`register` renames a service whose name is taken unless it's told not to, as Bonjour does, where zeroconf raises unless it's told it may: two copies of an app on one network then both announce, as `Living Room PC` and `Living Room PC (2)`, rather than the second failing.

## Finding a service

A program that looks once, such as a command-line tool, takes `find`, which browses for a while, then closes. One that keeps looking, such as a phone app watching for the PC to come back with another address, browses and handles the events:

```
import std.net.mdns.{Mdns, MdnsEvent, find}

fn main() {
    for s in find("_loom._tcp")! {
        println("{s.name} at {s.host}:{s.port}, id {s.property("id") ?? String("?")}, {s.addresses}")
    }

    var m = Mdns.new()!
    m.browse("_loom._tcp")!
    loop {
        match m.next_event()! {
            some(MdnsEvent.Added(s)) => println("{s.name} is at {s.addresses}"),
            some(MdnsEvent.Updated(s)) => println("{s.name} moved to {s.addresses}"),    // a new address, port or TXT
            some(MdnsEvent.Removed(s)) => println("{s.name} went"),
            _ => {}
        }
    }
}
```

- **Resolved when it's found.** `Added` comes when the service's SRV and TXT records and its host's addresses are all known, not when its name is first seen: what zeroconf's `add_service` and `get_service_info` give together. One whose SRV or addresses never come isn't added.
- **Changed while it's found.** `Updated` comes when its port, properties or addresses change, as when the PC joins another network, and `Removed` when it says goodbye or its records run out without being renewed: two minutes for a host's addresses and its SRV, and 75 minutes for its TXT and its name, as RFC 6762 §10 advises. Records are asked for again at 80, 85, 90 and 95% of their life, so a service that's still there doesn't go.
- **Asked for less and less.** A browse asks at once, then after 1, 2, 4 seconds and so on, to an hour apart (§5.2), with what it knows already in the question, so the services it has don't answer again (§7.1).
- **Addresses without scopes.** An IPv6 link-local address comes without the interface it was heard on, since the same service is heard on each interface: to connect to `fe80::...`, give it the scope of the interface it's on, from [`interfaces`](net.md#interfaces).

## Announcing a service

`register` takes about a second: it asks three times, 250 ms apart, whether another host has a service of the same name and type (§8.1), then announces it, and announces it twice more, one and three seconds after that, while the program calls `next_event` (§8.3). From then on it answers each question about the service, with its PTR, SRV, TXT and address records, and an NSEC record for those it doesn't have (§6.1), so a browser needn't wait for an answer that won't come.

- **A host of its own.** Its services are on `Living-Room-PC-calx.local`, the machine's name with `-calx` after it, or the host `Mdns.new` is given. The system's own responder already announces `Living-Room-PC.local` with the addresses it chooses, so the same name with other addresses would be a conflict that renames the machine.
- **The machine's addresses.** It announces every IPv4 and IPv6 address of every interface with multicast but the loopback, and looks at them again every 5 seconds: when they change, it announces the new ones, so a browser's `Updated` follows the PC to its new network.
- **Changed and gone.** `update` announces a service again with its new port, properties or addresses, and `unregister` and `close` say goodbye, twice, with the records' time to live at 0, so browsers forget it at once (§10.1), rather than when its records run out.
- **Answering as the RFC asks.** A question asked to be answered unicast (QU) is, and one from a port other than 5353, a legacy resolver's such as `dig -p 5353 @224.0.0.251`, gets its answer back unicast with its id and question, and lives at most 10 seconds (§6.7). An answer that many hosts may give is sent 20 to 120 ms after the question (§6), and none goes past the local link: their IP TTL is 255, as §11 checks.

### Names taken

A service's name is its type and instance name. While `register` probes, another host that has the name, or that's probing for it at the same moment and wins the tie (§8.2), makes it pick `Name (2)`, then `Name (3)`, and probe again, as Bonjour does; without `allow_name_change`, `register` fails with `NameTaken`. A host that claims the name after it was announced makes it probe again for the same name, as §9 says, since the other may have been the same service seen twice. If it loses, it's announced under a new name, and `next_event` gives `Renamed(kind, old, new)`, so the program can show it.

## On a thread of its own

An `Mdns` is one thread's, as its `Net` is, so a program with other work runs it on a thread of its own, made there, and sends it what to do. This one looks for a message each second; one that mustn't wait sends its `waker()` to the other thread first, which wakes `next_event` at once, as [a `Net`'s](net.md#threads) does:

```
import std.net.mdns.{Mdns, ServiceInfo}
import std.thread.{Task}

struct Announcer {
    id: String
    port: u16
    stop: Receiver<bool>
}

impl Task for Announcer {
    fn run(own self) {
        var stop = self.stop
        var m = Mdns.new()!
        let info = ServiceInfo.new("_loom._tcp", "Living Room PC", self.port).with_property("id", self.id)
        println("announced as {m.register(info)!}")
        while stop.next() == none {
            _ = m.next_event(1000)!    // answers questions while it waits
        }
        m.close()    // says goodbye
    }
}
```

[tests/std/net_mdns.calx](../../tests/std/net_mdns.calx) does this, with a browser on the main thread that sees the service come, change and go.

## Items

| Item | What it is |
| --- | --- |
| `PORT` | 5353, mDNS's port |
| `Mdns.new(port = PORT, host = "")` | A responder and browser on `port`, every interface with multicast, IPv4 and IPv6, with its host `host` in `.local`, or the machine's name with `-calx` after it. Fails with `Net` if neither IPv4 nor IPv6 can be had |
| `m.register(info, allow_name_change = true) -> String ! MdnsError` | Probes, then announces `info`, and gives its name. `BadName` for a type or name DNS-SD can't carry, `NameTaken` for a name another host has, without `allow_name_change` |
| `m.update(info) -> bool` | Announces a registered service again with what `info` has now |
| `m.unregister(kind, name) -> bool` | Says goodbye for a registered service |
| `m.browse(kind) ! MdnsError` | Starts finding services of type `kind` |
| `m.stop_browsing(kind)` | Stops, and forgets what it found, without `Removed` |
| `m.services() -> List<ServiceInfo>` | The services of the types it browses that it knows of now |
| `m.resolve(kind, name, timeout_ms = 3000) -> ServiceInfo? ! MdnsError` | One service, from what it knows, or by asking |
| `m.next_event(timeout_ms = -1) -> MdnsEvent? ! MdnsError` | The next thing that happened, answering questions meanwhile, or `none` after `timeout_ms`; a negative one waits until there is one |
| `m.host()`, `m.addresses()` | The host its services are on, and the addresses it announces for it |
| `m.waker() -> Waker ! NetError` | Wakes `next_event` from another thread |
| `m.close()` | Says goodbye for every service, then closes its sockets |
| `find(kind, timeout_ms = 3000) -> List<ServiceInfo> ! MdnsError` | The services of type `kind` found in `timeout_ms`, by name |
| `ServiceInfo.new(kind, name, port)` | A service on this machine with no properties |
| `info.with_property(name, value)` | The service with a TXT property set |
| `info.property(name) -> String?` | A TXT property's value, its key matched without regard to ASCII case |
| `info.full_name()` | Its whole name as DNS writes it: `Living Room PC._loom._tcp.local.` |
| `ServiceInfo`'s fields | `kind`, `name`, `port`, `properties` (in order), `host`, `addresses`, `priority`, `weight` |
| `MdnsEvent` | `Added(info)`, `Updated(info)`, `Removed(info)`, `Renamed(kind, old, new)` |
| `MdnsError` | `Net(NetError)`, `BadName(why)`, `NameTaken(name)` |

A type is an underscore and 1 to 15 letters, digits and hyphens, then `_tcp` or `_udp`, as RFC 6763 §7 says: `_loom._tcp`. An instance name is 1 to 63 bytes of any UTF-8, spaces and dots too. A property's key and value together are at most 255 bytes, and a key has no `=`.

## How it works

It has two UDP sockets on port 5353, one for IPv4 and one for IPv6, bound with [`reuse`](net.md#udp), so the system's own responder and other programs' keep theirs: Windows' own mDNS, Bonjour's mDNSResponder on macOS and avahi on Linux. Each joins mDNS's group, `224.0.0.251` or `ff02::fb`, on every interface that's up and has multicast, the loopback too where it has multicast, as macOS's has, and joins it again on one that comes up later. Over the loopback, programs on one machine find each other with no network, and on macOS without the [permission for the local network](net.md#multicast) a program may not have. A machine with no IPv6 has only the IPv4 one. What it sends goes to the group from port 5353, on each interface, with the multicast loopback on, so other programs on this machine hear it as the network does.

Its records are cached as §10 says: a record with the cache-flush bit replaces those of its name and type older than a second (§10.2), at most 4096 are kept, and one whose time to live is 0 goes a second later. Names are compared without regard to ASCII case, and written compressed, as RFC 1035 writes them.

What it doesn't do: it has no records but a service's and its host's, so it can't be a general mDNS name server, as avahi's `avahi-publish -a` is, and it doesn't relay other hosts' answers, as a Bonjour sleep proxy does. A network that drops multicast, as some guest Wi-Fi does, carries no mDNS at all, whoever's.

On macOS, a program reaches other machines only with the permission for the local network (TN3179), which a program run from Terminal has: an app asks for it with `NSLocalNetworkUsageDescription` in its `Info.plist`, and lists the types it uses in `NSBonjourServices`, such as `_loom._tcp`. Without it, it hears and is heard only on this machine, over the loopback.
