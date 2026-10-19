<h1 align="center">Risk Programming Language</h1>

<p align="center">
  <img width="200" alt="Risk programming language logo" src="https://github.com/user-attachments/assets/794d7da6-de33-4365-9c99-0b4742cec41e" />
</p>

<p align="center">
  A statically typed programming language that compiles to C99.
  <br />
  <a href="https://hesed-charis.vercel.app/projects/risk">Project page</a>
</p>

---

**Risk** is a statically typed programming language that lowers to C99 and delegates final machine code generation to GCC. `riskc`, its compiler, is single-pass and sequential. It also ships with `risk-lsp`, a language server that gives your editor the same diagnostics as the compiler.

> **Platform support:** Risk currently supports **Linux only** (x86_64 for prebuilt releases). macOS and Windows are not supported yet. On Windows, you can use it through [WSL](https://learn.microsoft.com/windows/wsl/).

## Hello, Risk

```risk
func main() -> i32 {
    println("Hello, Risk!");
    return 0;
}
```

## Repository Structure

- `src/`: all compiler source files, including the lexer, parser, type checker, code generator, and compilation driver.
- `risk-lsp/`: the language server (`risk-lsp`), plus editor integrations (`editors/`) and its test suite (`tests/`). It reuses the compiler's own lexer, parser, and type checker from `src/`.
- `CMakeLists.txt`: the primary CMake configuration file for building the project.
- `ARCHITECTURE.md`: developer guides and internal architecture references.
- `examples/`: sample Risk programs and feature demonstrations.
- `manual/`: the language manual and syntax reference.
- `stdlib/`: standard library modules written for Risk.

## Installation

Risk is Linux only for now. Clone the repository and run the install script:

```bash
git clone https://github.com/hesed-charis175/Risk.git
cd Risk
./build.sh
```

The script installs any missing build dependencies (gcc, g++, cmake, make), builds `riskc` and the language server `risk-lsp`, copies both to `/usr/local/bin` (or `~/.local/bin` if you don't have sudo), installs and compiles the standard library into `~/.risk/lib`, and sets `RISK_STDLIB` and your `PATH` in your shell config. Open a new terminal afterwards and `riskc` is ready to use.

If you only want the compiler, skip the language server:

```bash
SKIP_LSP=1 ./build.sh
```

To remove everything the script installed:

```bash
./build.sh uninstall
```

### Building manually

If you prefer to build it yourself, you need:

- A C++ compiler supporting C++17 or later
- CMake 3.10 or higher
- GCC (required for final code generation and linking)

```bash
mkdir build
cd build
cmake ..
make
```

This produces `riskc` and `risk-lsp` in `build/bin`. Add them to your `PATH` and set `RISK_STDLIB` to the `stdlib` directory so the compiler can find the standard library. To build the compiler without the language server, pass `-DRISK_BUILD_LSP=OFF` to `cmake`.

## Running Risk Programs

Compile a Risk source file into an executable by passing the source path and an output flag:

```bash
riskc path/to/source.risk -o output_binary
```

Then run the resulting executable:

```bash
./output_binary
```

### Example

Create a file named `main.risk` with the content from [Hello, Risk](#hello-risk), then compile and run it:

```bash
riskc main.risk -o app
./app
```

Expected output:

```text
Hello, Risk!
```

## Editor Support

`risk-lsp` is a [Language Server Protocol](https://microsoft.github.io/language-server-protocol/) server for Risk. Because it runs the compiler's own lexer, parser, and type checker, the errors you see in your editor are the same ones `riskc` reports.

Features:

- Diagnostics (reported with source `riskc`)
- Hover, including documentation for keywords and built-in types
- Go to definition, including into the standard library and from `import` lines
- Find references and document highlight
- Document symbols (outline)
- Rename (with prepare-rename)
- Signature help
- Completion (triggered on `.`, `@`, and `/`)

The server finds the standard library through, in order: the `stdlibPath` initialization option, `$RISK_STDLIB`, `<workspace>/stdlib`, and `~/.risk/lib`. If you installed with `./build.sh`, this works without any configuration.

It speaks standard LSP over stdio, so it works with any LSP-capable editor:

- **Neovim (0.11+):** register `risk-lsp` with `vim.lsp.config` and enable it for the `risk` filetype.
- **VS Code:** use the extension in [`risk-lsp/editors/vscode`](risk-lsp/editors/vscode).

Full setup instructions for each editor, plus how to run the test suite, are in [`risk-lsp/README.md`](risk-lsp/README.md).

## Documentation

- Language manual and syntax reference: [`manual/`](manual/)
- Compiler internals: [`ARCHITECTURE.md`](ARCHITECTURE.md)
- Language server and editor setup: [`risk-lsp/README.md`](risk-lsp/README.md)
- More programs: [`examples/`](examples/)

## License

Risk is free to use for anyone, including commercially, but it is source-available, not open source: you may not modify or redistribute it. See [LICENSE](LICENSE) for the full terms.

Programs you write in Risk, and the executables `riskc` produces from them, belong to you.
