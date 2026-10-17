# 17. Compiler Flags and Diagnostics {#compiler-flags}

*[← Standard Library Reference](@ref stdlib-reference) | [Back to Manual Index](@ref user-manual)*

The `riskc` compiler acts as a driver: it transpiles your `.risk` source code into C99 and then invokes `gcc` to handle the final build. While most standard flags pass straight through to `gcc`, a few are uniquely tailored for Risk.

---

## Optimization levels

Standard optimization flags like `-O0`, `-O1`, `-O2`, `-O3`, `-Os`, `-Og`, and `-flto` are fully supported and forwarded directly to `gcc`:

```sh
riskc hello.risk -O2 -o hello
riskc hello.risk -O2 -flto -o hello

```

However, **`-Ofast` and `-Oz` are deliberately rejected** rather than silently remapped or ignored:

* **`-Ofast`** is blocked because its unsafe floating-point semantics (`-ffast-math`) go against Risk's safety philosophy.


* **`-Oz`** is rejected because it is a Clang-specific flag that `gcc` does not understand.



---

## Target ISA: `-march=<value>`

The `-march` flag is forwarded verbatim to `gcc` without independent validation by `riskc`:

```sh
riskc hello.risk -march=native -o hello
riskc hello.risk -O3 -march=x86-64-v3 -o hello

```

Unlike optimization levels, `-march` is never enabled automatically—`gcc`'s default target ISA applies unless specified. This ensures you have explicit control over portability versus machine-specific tuning.

> ℹ️ `--profile` is recognized on the command line, but the profiling hook it enables is currently disabled in this build and has no effect.
> 
> 

---

## Compilation controls: `-c` and `-i`

* **`-c` (Compile only, no linking)**: Produces an object file instead of a complete executable.


```sh
riskc hello.risk -c              # -> hello.o (intermediate .c is cleaned up)
riskc hello.risk -c -o custom.o  # -> custom.o (respects explicit names)

```


* **`-i` (Keep intermediate C)**: Retains the generated C source file alongside the binary instead of deleting it after `gcc` finishes. For `HEADER_FILE` modules, the output uses a `_risk.c` suffix to avoid filename collisions.



---

## Debugging with `-g`

Passing `-g` embeds `#line` directives into the generated C code that map back to your original `.risk` source lines and file names. This allows source-level debuggers like `gdb` to step through and report errors directly against your Risk code:

```risk
// crash.risk -- deliberately crashes with a division by zero
func divide(a: i32, b: i32) -> i32 {
    result: i32 = a / b;
    return result;
}

func compute(n: i32) -> i32 {
    step1: i32 = n + 10;
    step2: i32 = divide(step1, n - n);   // n - n is always 0
    return step2;
}

func main() -> i32 {
    x: i32 = 5;
    y: i32 = compute(x);
    return y;
}

```

```sh
riskc crash.risk -g -o crash
gdb ./crash
(gdb) run

```

```text
Program received signal SIGFPE, Arithmetic exception.
0x... in mod__divide (a=15, b=0) at .../crash.risk:7
7           result: i32 = a / b;
(gdb) bt
#0  mod__divide (a=15, b=0) at .../crash.risk:7
#1  mod__compute (n=5) at .../crash.risk:13
#2  mod__main () at .../crash.risk:19

```

*Note on symbols*: Risk functions are compiled into C using a module prefix (e.g., `main` becomes `mod__main`). This naming convention is helpful if you ever need to set breakpoints in `gdb` using C symbol names directly (e.g., `gdb -ex "break mod__divide"`).

---

## Additional flags and defaults

* **`-o <name>` / `--out <name>**`: Specifies the output filename (defaults to `a.out` or `a.exe` on Windows).


* **`-h` / `--help` & `--version` / `-v**`: Prints usage info or version details and exits immediately before compilation begins.


* **Unrecognized flags**: Any flags `riskc` does not recognize are silently ignored rather than throwing an error. If a flag seems to do nothing, double-check its spelling.



---

## Examples in practice

### Common build workflows

```sh
# Fast local iteration: no optimization, debug symbols for gdb, and keep the generated C
riskc app.risk -O0 -g -i -o app

# Release build: full optimization, LTO, and hardware tuning
riskc app.risk -O3 -flto -march=native -o app

# CI check: verify compilation without linking or producing files
riskc app.risk -c -o /dev/null

```

### Handling flag rejections

```sh
riskc app.risk -Ofast -o app
# Rejected -- no binary is produced because -Ofast implies unsafe optimizations

```

### Working with object files and header modules

```sh
riskc app.risk -c              # -> app.o
riskc header_mod.risk -i       # -> header_mod.o, header_mod.riskh, header_mod_risk.c (retained)

```

---

*[← Standard Library Reference](@ref stdlib-reference) | [Back to Manual Index](@ref user-manual)*
