---
title: Editor support
description: The VS Code extension - highlighting, errors as you type and running files - and how other editors can use the same pieces.
---

# Editor support

## VS Code

The extension in [`tools/vscode`](../../tools/vscode/README.md) gives VS Code:

- Syntax highlighting for `.calx` files, and for ```` ```calx ```` blocks in Markdown, such as the examples in these docs.
- The compiler's errors as you type. A moment after each change, it runs `calx check` on the editor's text and underlines what it reports.
- **Calx: Run File**, the play button in the editor's title bar, which saves the file and runs `calx run` on it.
- Comment toggling, bracket matching and indentation, and `///` continuing on the next line of a doc comment.

Install it from a checkout of this repo, then reload VS Code's window:

```sh
py -I tools/vscode/package.py --install     # python3 on Linux
```

It finds the compiler that `tests/run.py` builds when the file is in this repo or in a checkout next to it, such as calcite's. Elsewhere, set `calx.compilerPath`, or put `calx` on `PATH`. The [extension's README](../../tools/vscode/README.md) has the details and its settings.

## Other editors

Two pieces aren't tied to VS Code:

- **The grammar**, [`calx.tmLanguage.json`](../../tools/vscode/syntaxes/calx.tmLanguage.json), is a TextMate grammar, which other editors that read TextMate grammars can load, such as JetBrains IDEs through their TextMate bundle support. Its scopes follow the usual names, such as `keyword.control`, `storage.type` and `entity.name.function`, so themes color it without changes.
- **Errors for editors.** `calx check --json --stdin path` reads the text from standard input and prints each error as a line of JSON, with positions counted the way editors count them ([the format](command.md#errors-for-editors)). An editor plugin can run it on each change.

The language server planned for Phase 9 will add completion, go to definition and hover docs, and replace the extension's own checking.
