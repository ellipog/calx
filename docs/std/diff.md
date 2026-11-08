---
title: std.diff
description: The fewest changes between two lists with Myers' algorithm, and unified diffs of text as diff -u writes them.
---

# `std.diff`

What changed between two lists, or two texts line by line: `edits` finds the fewest elements to delete and insert, and `unified` writes the changes between two texts as `diff -u` and Python's `difflib.unified_diff` do.

```calx
import std.diff

fn main() {
    let old = "apples\nbread\nmilk\neggs\n"
    let new = "apples\nbutter\nmilk\neggs\ntea\n"
    print(diff.unified(old, new, a_name: "list.txt", b_name: "list.txt"))
    // out: --- list.txt
    // out: +++ list.txt
    // out: @@ -1,4 +1,5 @@
    // out:  apples
    // out: -bread
    // out: +butter
    // out:  milk
    // out:  eggs
    // out: +tea
    let a: List<i32> = [1, 2, 3, 4]
    let b: List<i32> = [1, 3, 4, 5]
    for e in diff.edits(a, b) {
        println("{e.kind} {e.a_start}..{e.a_end} {e.b_start}..{e.b_end}")
    }
    // out: Keep 0..1 0..1
    // out: Delete 1..2 1..1
    // out: Keep 2..4 1..3
    // out: Insert 4..4 3..4
}
```

| Item | Gives |
| --- | --- |
| `edits(a, b)` | The runs that turn the list `a` into `b`, in order, with the fewest elements deleted and inserted. The elements can be of any type with `==` |
| `unified(a, b, a_name: "a", b_name: "b", context: 3)` | The changes between two texts as a unified diff, with `context` lines around each change. Equal texts give empty text |
| `Edit` | A run: `kind`, and the elements from `a_start` up to `a_end` of `a` and from `b_start` up to `b_end` of `b` |
| `EditKind` | `Keep`, `Delete`, `Insert` or `Replace`, as difflib's `equal`, `delete`, `insert` and `replace` |

- `edits` gives the runs as difflib's `get_opcodes` does: `Keep` runs between the changes, and a `Replace` where elements are deleted and others inserted in their place. Equal lists give one `Keep` run, and two empty lists none.
- `unified` writes the format `patch` and `git apply` read: `---` and `+++` lines naming the texts, then hunks that start `@@ -line,count +line,count @@`, with ` ` before a line in both, `-` before a deleted one and `+` before an inserted one. Changes closer than twice `context` lines share a hunk, as difflib groups them.
- A last line with no `\n` after it differs from the same line with one, and is marked `\ No newline at end of file`, as `diff` marks it. A `\r` before a `\n` is part of its line, so a file whose line endings changed shows every line changed, as `diff` shows it.

## How it works

`edits` is Myers' algorithm (1986), which finds a shortest edit script in time proportional to the lengths times the number of changes. It searches from both ends at once for the middle of the path, then works on each half, so it needs memory only in proportion to the lengths. What the two lists start and end with in common is set aside first, since most diffs are a few changes in long texts. `unified` numbers each different line once, with a map, so the search compares numbers rather than text.

On 150 random pairs of texts with lines inserted, deleted and changed, `unified` wrote the same text as GNU `diff -u --label a --label b` for 146. For the other 4, both changed the same number of lines, and the two chose different ones of several smallest diffs. On a file of 200,000 lines with 200 changes, it took 66 ms, and Python 3.14's `difflib.unified_diff` 757 ms, for the same output.

Python's difflib finds matching blocks its own way, the Ratcliff and Obershelp method, which doesn't always find the fewest changes. So where several diffs are about as small, its choice can differ from `diff`'s and from this one's.
