---
title: std.keyring
description: Passwords and tokens kept in the system's own credential store, Credential Manager, the Keychain or the Secret Service, by service and account, as Python's keyring keeps them.
---

# `std.keyring`

Secrets kept in the system's own credential store, as Python's `keyring` keeps them: Credential Manager on Windows, the Keychain on macOS, and the Secret Service on Linux, which GNOME Keyring, KWallet and KeePassXC provide. A secret is kept under a service and an account, so a server's RCON password or a plugin's API key stays out of its config files.

```
import std.keyring

fn main() ! keyring.Error {
    try keyring.set("kern", "rcon:survival", "hunter2")
    println("{try keyring.get("kern", "rcon:survival")}")    // some(hunter2)
    println("{try keyring.delete("kern", "rcon:survival")}")    // true
    println("{try keyring.get("kern", "rcon:survival")}")    // none
}
```

| Python | Calx |
| --- | --- |
| `keyring.set_password(service, user, pw)` | `keyring.set(service, account, secret) ! Error` |
| `keyring.get_password(service, user)`, `None` if there's none | `keyring.get(service, account) -> String? ! Error`, `none` if there's none |
| `keyring.delete_password(service, user)`, which raises if there's none | `keyring.delete(service, account) -> bool ! Error`, `false` if there was none |
| A key as bytes, base64'd into a password first | `keyring.set_bytes(service, account, bytes)` and `keyring.get_bytes(service, account) -> List<u8>? ! Error` |
| `keyring.errors.NoKeyringError` | `Error.Unavailable(why)` |

A secret that isn't there isn't an error, so a failure is always the store's: `get` gives `none` and `delete` gives `false`, and an `Error` means the store couldn't answer.

## With no store

A headless Linux server often has no Secret Service, and may not have libsecret, and a Windows service's logon session has no Credential Manager. Every call then fails with `Unavailable`, at once, saying why, and a daemon keeps its secrets in an encrypted file of its own instead:

```calx
import std.env.{args}
import std.keyring
import std.keyring.{Error}

// The RCON password: from the system's store, or from the server's own file where there's no store.
fn rcon_password(server: String) -> String? ! Error {
    return match keyring.get("kern", "rcon:{server}") {
        ok(pw) => pw,
        err(e) => match e {
            Error.Unavailable(why) => {
                println("no credential store ({why}), so reading secrets.enc")
                none
            },
            _ => fail e,
        },
    }
}

fn main() {
    let a = args()
    if a.is_empty() {
        println("give a server's name, such as survival")    // out: give a server's name, such as survival
        return
    }
    match rcon_password(a[0].copy()) {
        ok(pw) => println(if pw == none { "no password kept" } else { "found it" }),
        err(e) => println("{e}"),
    }
}
```

| System | No store when | `Unavailable` says |
| --- | --- | --- |
| Linux | libsecret isn't installed | `libsecret isn't installed, which the Secret Service is reached through` |
| Linux | There's no D-Bus session bus, as under a system service or over SSH without a login session | `there's no D-Bus session bus to find the Secret Service on` |
| Linux | Nothing on the bus provides the Secret Service | `nothing on the session bus provides the Secret Service, such as GNOME Keyring or KeePassXC` |
| Linux | It's there, but needs someone to unlock it and there's no display to ask on | `the Secret Service didn't answer within 10 s` |
| Windows | The logon session has no Credential Manager, as a service's or a network logon's | `this logon session has no Credential Manager, ...` |
| macOS | There's no default keychain, as for a daemon run as root | the Keychain's own words |

## Errors

| `Error` | When |
| --- | --- |
| `Unavailable(String)` | There's no store, and why (above) |
| `Denied(String)` | The store is there and said no: the keychain or collection is locked and couldn't be unlocked without asking, or the person asked said no |
| `TooLong` | The secret is longer than the store takes. Windows takes 2560 bytes, 1280 UTF-16 units of text; the others take far more |
| `NotText` | `get` found a secret that isn't text, such as one `set_bytes` kept: read it with `get_bytes` |
| `BadName(String)` | The service is empty, or the service or account holds a NUL, or they're longer than the store takes |
| `Os(i32, String)` | Something else, with the system's code and its words: a Windows error, a macOS `OSStatus`, or libsecret's code |

Each shows as words: `no credential store: libsecret isn't installed, which the Secret Service is reached through`.

## Where each system keeps them

Each secret is where Python's keyring keeps it, so a Calx program and a Python script read each other's.

| System | Where | Text kept as |
| --- | --- | --- |
| Windows | A generic credential in Credential Manager, its target `account@service` and its user name the account, kept for this user on this computer. keyring keeps its newest under the service's own name, and `get` and `delete` look there too, if its user name is the account. They're under Windows Credentials in Control Panel, and `cmdkey /list` | UTF-16, as keyring and Credential Manager's own window keep it |
| macOS | A generic password in the login keychain, its service and account the service and account | UTF-8 |
| Linux | An item in the Secret Service's default collection, `login` in GNOME Keyring, with the attributes `service` and `username`, labelled `Password for 'account' on 'service'` as keyring labels it. It's found by those two attributes alone, so the items both of keyring's Linux backends wrote are found too | UTF-8 |

So text and bytes don't mix: read text with `get` and bytes with `get_bytes`, as they were kept.

Asking can take a person. macOS asks to unlock a locked keychain, and asks whether to let a program read a secret another program kept, which includes the same program rebuilt, since an unsigned program is known by its contents. GNOME Keyring asks to unlock a locked collection. Each call waits for the answer, up to 2 minutes on Linux, or 10 s when there's no display to ask on.

## Checked

The [tests](../../tests/std/keyring.calx) keep, read, replace and delete secrets under a service of their own, `calx-test-` and a random number, and delete everything they kept. They run the round trip in a copy of the test with a time limit, so a store that waits for someone can't hold them up. They check text past the BMP, an empty secret, bytes that aren't text, a secret too long for Credential Manager, and names that can't be one.

- Windows: the round trip against Credential Manager. By hand, a secret kept by Calx shows in `cmdkey /list` as `account@service`, and one `cmdkey` kept under the service's own name, as keyring keeps its newest, is read and deleted.
- Linux: without libsecret, every call gives `Unavailable` in about 10 ms. With libsecret and GNOME Keyring on a session bus of the test's own, the round trip works, unlocked, and with no keyring unlocked and no display, each call gives `Unavailable` after 10 s rather than waiting.
- macOS: the round trip, where the runner's login keychain is unlocked; where it's locked, or waits for someone, the test is skipped.

## How it's built

[runtime/calx_keyring.c](../../runtime/calx_keyring.c), linked only into programs that call it. Nothing is linked into the program: each system's library is opened when the first call comes.

| System | How |
| --- | --- |
| Windows | `CredWriteW`, `CredReadW` and `CredDeleteW` from advapi32, which every Windows has |
| macOS | `SecItemAdd`, `SecItemUpdate`, `SecItemCopyMatching` and `SecItemDelete` from the Security framework, opened with `dlopen`, as are the CoreFoundation dictionaries they take |
| Linux | libsecret, `libsecret-1.so.0`, opened with `dlopen`, which talks to the Secret Service over D-Bus. Each call is cancelled if it takes longer than 2 minutes, or 10 s with no display |

The runtime wipes each secret from its own memory before freeing it. The `String` or `List<u8>` that `get` gives is the program's, and isn't wiped.

`std.keyring` is written in Calx over those runtime calls: [std/keyring.calx](../../std/keyring.calx).
