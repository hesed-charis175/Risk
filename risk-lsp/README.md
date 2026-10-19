# risk-lsp

Language server for Risk. It runs the compiler's lexer, parser and type
checker, so editor diagnostics are the same ones `riskc` prints.

## Build

`./build.sh` in the repository root builds and installs `riskc`, `risk-lsp` and
the stdlib together (`SKIP_LSP=1 ./build.sh` skips the server). To build only
the server:

```bash
cmake -S risk-lsp -B risk-lsp/build && cmake --build risk-lsp/build -j
# binary: risk-lsp/build/risk-lsp   (put it on your PATH)
```

It compiles `../src/{lexer,parser,typechecker}.cpp` directly.
If they are elsewhere: `-DRISK_SRC_DIR=/path/to/src`.

## What it does

Diagnostics (syntax and type errors, source `riskc`), hover, go to definition
(including into the stdlib and onto `import` lines), find references, document
highlight, document symbols, rename, signature help, and completion
(locals, globals, imported symbols, block fields, `opts` members, `Array` /
`Hashmap` / `Arena` methods, intrinsics, keywords, `import @` module names).

The stdlib is found from, in order: the `stdlibPath` initialization option,
`$RISK_STDLIB`, `<workspace>/stdlib`, `~/.risk/lib`.

Every analysis runs in a forked, time- and memory-capped child process, because
the compiler front-end was written for batch use and can hang or crash on
half-typed code. A bad parse never takes the server down.

## Editors

**VS Code**: `editors/vscode` is a small client plus a TextMate grammar.

```bash
cd risk-lsp/editors/vscode && npm install
# then: Extensions -> ... -> "Install from VSIX" after `npx @vscode/vsce package`,
# or run it with F5 from this folder. Settings: risk.serverPath, risk.stdlibPath.
```

**Neovim 0.11+**:

```lua
vim.filetype.add({ extension = { risk = "risk" } })
vim.lsp.config("risk", {
  cmd = { "risk-lsp" },
  filetypes = { "risk" },
  root_markers = { "stdlib", ".git" },
  init_options = { stdlibPath = vim.fn.expand("~/.risk/lib") },
})
vim.lsp.enable("risk")
```

Any other client: launch `risk-lsp` over stdio for the `risk` language.

## Tests

```bash
python3 risk-lsp/tests/test_features.py build/bin/risk-lsp .   # from the repo root, after ./build.sh
```
