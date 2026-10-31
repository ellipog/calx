# Calx for VS Code

Support for the [Calx](https://calxlang.com) language:

- **Syntax highlighting** for `.calx` files, and for the ```` ```calx ```` blocks in Markdown, such as the examples in the Calx docs.
- **Errors as you type.** The extension runs `calx check` on the open file a moment after each change, and underlines what the compiler reports. Notes, such as where something was first declared, show as related locations.
- **Run File** (the play button in the editor's title bar, or **Calx: Run File** in the command palette) saves the file and runs `calx run` on it in a terminal panel.
- Comment toggling, bracket matching, auto-closing pairs, indentation, and `///` continuing on the next line of a doc comment.

## Installing

The extension isn't on the Marketplace yet. Package it from a checkout of the calx repo and install it:

```sh
py -I tools/vscode/package.py --install     # python3 on Linux
```

That writes `build/vscode/calx-<version>.vsix` and installs it with VS Code's `code` command. Reload VS Code's window afterwards. You can also install the `.vsix` from the Extensions view: **...** > **Install from VSIX...**.

## Finding the compiler

Errors need the `calx` compiler. The extension uses the first of these that exists:

1. The `calx.compilerPath` setting.
2. `build/compiler/<platform>/calx`, which `tests/run.py` builds, in a checkout of the calx repo that contains the file, or in a `calx` folder next to one of the folders the file is in. So files in the calx repo and in a calcite checkout beside it both find it.
3. The same path under `$CALX_HOME`.
4. `calx` on `PATH`.

If it can't run the compiler, it says so once and highlighting still works.

## Settings

| Setting | Default | Meaning |
| --- | --- | --- |
| `calx.compilerPath` | empty | The `calx` compiler to run |
| `calx.checkOnType` | `true` | Check as you type. When off, files are checked when opened and saved |

## Tasks

Tasks of type `calx` run the compiler on a file, for `tasks.json`:

```json
{
  "label": "run hello",
  "type": "calx",
  "command": "run",
  "file": "tests/basics/hello.calx"
}
```

`command` is `check`, `build` or `run`, and `"release": true` adds `--release`.

## How it works

The grammars are TextMate grammars in `syntaxes/`. For errors, the extension pipes the editor's text to `calx check --json --stdin <path>`, which prints each diagnostic as a line of JSON with editor positions ([the format](../../docs/reference/command.md#errors-for-editors)). The language server planned for Phase 9 (P9.2) will replace this with completion, go to definition and hover docs.

## Developing

```sh
cd tools/vscode
npm ci
npm test
```

The tests check the scopes the grammar gives a list of lines, tokenize every Calx example in the repo (the tests and the docs) to make sure none of it is marked invalid, and test the code that finds the compiler and reads its output. CI runs them.

To try a change without packaging, open `tools/vscode` in VS Code and press F5, which starts a window with the extension loaded from this folder. The file icons come from `assets/logo` when packaging, so that window shows the default icon.
