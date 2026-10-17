# 2. Getting Started: Compiling and Running Programs {#getting-started}

*[← Introduction](@ref intro) | [Next: Primitive Types →](@ref primitives)*

## Entry Point and Hello World

Every executable program requires a `main` function that takes either no parameters or a single `Array<string>` parameter containing command-line arguments. It must return an integer type:

```risk
import "@core";

func main() -> i32 {
    ioPrintln("Hello World");
    return 0;
}

```

The statement `import "@core";` pulls in the core standard library module. The `@` prefix instructs the compiler to look in the standard library path defined by the `RISK_STDLIB` environment variable, falling back to a local `./stdlib/` directory (see [§13](@ref imports)).

`ioPrintln` is a built-in print intrinsic that accepts any number of arguments of any type, converts each argument to text, concatenates them, and appends a trailing newline. `ioPrint` performs the same operation without the newline. Raw variants (`ioRawPrint` and `ioRawPrintln`) are also available for lower-level output.

The return value of `main` serves as the process exit code, identical to standard C.

### Zero-Import Intrinsics

Several built-in functions require **no import statements**, not even `@core`: `ioPrint`, `ioPrintln`, `ioRawPrint`, `ioRawPrintln`, `ioInput`, `type()`, `length()`, `sizeof()`, `toString()`, `toInt()`, `toFloat()`, `toBool()`, `intToChar()`, `strRepeat()`, `kbRead()`, `kbWait()`, and `clrScr()`. All other utilities require importing their respective modules.

| Intrinsic | Signature | Behavior |
| --- | --- | --- |
| `toInt(s)` | `(string) -> i32` | Parses an optional leading sign and digits. Non-numeric or empty input returns `0`. Parsing stops at the first non-digit character rather than generating an error. |
| `toFloat(s)` | `(string) -> f64` | Parses floating-point numbers using standard library conversion routines. |
| `toBool(s)` | `(string) -> bool` | Case-insensitive evaluation: prefixes matching `"true"`, `"yes"`, `"on"`, or `"1"` evaluate to `true`; all other inputs evaluate to `false`. |
| `intToChar(n)` | `(i32) -> string` | Converts an ordinal `i32` value into a single-character `string`. |
| `strRepeat(s, n)` | `(string, i32) -> string` | Concatenates `n` copies of string `s`. Passing `0` for `n` returns an empty string `""`. Passing a negative number returns a null string, which results in undefined behavior if used in further operations. Verify that `n >= 0` prior to invocation. |
| `kbRead()` | `() -> string` | Reads a single keypress **without blocking**. Returns `""` if no key was pressed since the last call, or a one-character string otherwise. Temporarily enables raw mode only for the duration of the call. |
| `kbWait()` | `() -> string` | Functions similarly to `kbRead()`, but blocks execution until a key is available. |
| `clrScr()` | `() -> void` | Clears the terminal screen and resets the cursor position using ANSI escape sequences. |
| `PI()` / `E()` | `() -> f64` | Mathematical constants (π and Euler's number). These are invoked as zero-argument functions rather than bare identifiers. |

```risk
// No import statement is required in this file.
func main() -> i32 {
    ioPrintln("no import needed");
    ioPrintln("type(5) reports: ", type(5));

    arr: Array<i32, 3> = [1, 2, 3];
    n: i32 = length(arr);         // 3

    sz: i32 = sizeof(i32);        // 4
    ioPrintln("toString(42) -> ", toString(42));

    return 0;
}

```

### Command-Line Arguments

The `main` function optionally accepts a single parameter of type `Array<string>`, which is populated with command-line arguments excluding the program executable name itself:

```risk
func main(args: Array<string>) -> i32 {
    ioPrintln("argc (excluding program name): ", length(args));
    i: i32 = 0;
    while (i < length(args)) {
        ioPrintln("arg[", i, "] = ", args[i]);
        i = i + 1;
    }
    return 0;
}

```

## Invoking the Compiler

The `riskc` driver translates `.risk` source files into C99 code and invokes `gcc` to produce a native binary:

```sh
riskc hello.risk -o hello
./hello

```

By default, intermediate `.c` files are automatically deleted after compilation, leaving only the final binary or object file. Refer to [§17](@ref compiler-flags) for complete documentation on driver flags (`-O0` through `-3`, `-Os`, `-Og`, `-flto`, `-march`, `-c`, `-g`, and `-i`), including optimization levels that are deliberately restricted.

## Header Modules (`HEADER_FILE`)

A `.risk` file starting with the literal marker `HEADER_FILE` compiles as a module rather than a standalone program. It produces an object file (`.o`) and a compiled interface file (`.riskh`) instead of an executable, and does not require a `main` function:

```risk
HEADER_FILE

func add(a: i32, b: i32) -> i32 {
    return a + b;
}

```

```sh
riskc header_mod.risk        # Generates header_mod.o and header_mod.riskh
riskc header_mod.risk -i     # Also retains header_mod_risk.c

```

Other files can consume this module using `import "header_mod.risk";` (see [§13](@ref imports)).

### Automatic Header Module Building

The `riskc` compiler automatically builds any `HEADER_FILE` module when it is imported directly or transitively:

```sh
riskc main.risk -o main    # header_mod.risk builds automatically if outputs are missing

```

Furthermore, `riskc` compares modification times between each imported header module source and its corresponding `.riskh` or `.o` file, triggering automatic rebuilds when source files are modified without requiring manual intervention.

## Examples

**Minimal Executable Program:**

```risk
import "@core";

func main() -> i32 {
    ioPrintln("Hello World");
    return 0;
}

```

```sh
riskc hello.risk -o hello
./hello
# Hello World
echo $?
# 0 -- main return value becomes process exit code

```

**Combining Command-Line Arguments, Intrinsics, and Modules:**

```risk
// greet_mod.risk -- a HEADER_FILE module without a main function
HEADER_FILE

func greet(name: string) -> string {
    return name;
}

```

```risk
// main.risk
import "@core";

func main(args: Array<string>) -> i32 {
    ioPrintln("argc (excluding program name): ", length(args));

    i: i32 = 0;
    while (i < length(args)) {
        ioPrintln("arg[", i, "] = ", args[i]);
        i = i + 1;
    }

    ioPrintln("type(3.14) reports: ", type(3.14));
    ioPrintln("sizeof(i64) = ", sizeof(i64));

    return 0;
}

```

```sh
riskc greet_mod.risk              # Generates greet_mod.o and greet_mod.riskh
riskc main.risk -o main
./main first second
# argc (excluding program name): 2
# arg[0] = first
# arg[1] = second
# type(3.14) reports: f64
# sizeof(i64) = 8

```

---

*[← Introduction](@ref intro) | [Next: Primitive Types →](@ref primitives)*
