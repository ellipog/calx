---
title: Calx documentation
description: Index of the Calx docs, the page order, and the rules for writing them.
---

# Calx documentation

These pages are the source for the Calx docs website. Until the site exists, read them here.

## Pages

In reading order:

1. [Calx spec](CALX_SPEC.md): every feature that is designed but not built yet

The guide and the reference start in Phase 3, when the first features ship.

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
