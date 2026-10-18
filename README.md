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

**Risk** is a statically typed programming language that lowers to C99 and delegates final machine code generation to GCC. `riskc`, its compiler, is single-pass and sequential.

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

The script installs any missing build dependencies (gcc, g++, cmake, make), builds `riskc`, copies it to `/usr/local/bin` (or `~/.local/bin` if you don't have sudo), installs and compiles the standard library into `~/.risk/lib`, and sets `RISK_STDLIB` and your `PATH` in your shell config. Open a new terminal afterwards and `riskc` is ready to use.

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

This produces the `riskc` executable in the `build` directory. Add it to your `PATH` and set `RISK_STDLIB` to the `stdlib` directory so the compiler can find the standard library.

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

## Documentation

- Language manual and syntax reference: [`manual/`](manual/)
- Compiler internals: [`ARCHITECTURE.md`](ARCHITECTURE.md)
- More programs: [`examples/`](examples/)

## License

Risk is free to use for anyone, including commercially, but it is source-available, not open source: you may not modify or redistribute it. See [LICENSE](LICENSE) for the full terms.

Programs you write in Risk, and the executables `riskc` produces from them, belong to you.
