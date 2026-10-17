# 17. Compiler Flags and Diagnostics {#compiler-flags}

*[← Standard Library Reference](@ref stdlib-reference) | [Back to Manual Index](@ref user-manual)*

`riskc` is a driver: it transpiles your `.risk` source to C99, then
invokes `gcc` on the result. Most of its flags simply pass through to
`gcc`, but a few are Risk-specific.

## Optimization levels

`-O0`, `-O1`, `-O2`, `-O3`, `-Os`, `-Og`, and `-flto` are all accepted
and passed straight through to the underlying `gcc` invocation:

```sh
riskc hello.risk -O2 -o hello
riskc hello.risk -O2 -flto -o hello
```

**`-Ofast` and `-Oz` are deliberately rejected**, not silently remapped
to something else:

```sh
riskc hello.risk -Ofast -o hello
# rejected, with an explanation mentioning -ffast-math (its unsafe
# floating-point semantics aren't something riskc will enable silently)
# -- no binary is produced

riskc hello.risk -Oz -o hello
# rejected as a clang-only flag gcc doesn't understand -- no binary produced
```

## `-march=<value>` - target ISA

Forwarded verbatim to `gcc`, the same way the `-O*` flags are - `riskc`
doesn't validate or interpret the value itself:

```sh
riskc hello.risk -march=native -o hello
riskc hello.risk -O3 -march=x86-64-v3 -o hello
```

Unlike the `-O*` levels, `-march` is never implied automatically -
`gcc`'s own default target ISA applies unless you pass this explicitly.
That's deliberate: whether `native` (fastest for the machine you're
building on, but the binary may not run on an older one) or a specific
baseline like `x86-64-v3` is correct depends entirely on where the
resulting binary needs to run, which `riskc` has no way to know on its
own.

> `--profile` is also accepted on the command line but currently does
> nothing - the profiling hook it's meant to enable is disabled in this
> build. Don't rely on it having any effect yet.


## `-c` - compile only, no link

Produces an object file instead of a linked executable:

```sh
riskc hello.risk -c              # -> hello.o, no executable, intermediate .c cleaned up
riskc hello.risk -c -o custom.o  # -> custom.o (explicit name respected)
```

## `-i` - keep the intermediate C

By default, the C file `riskc` generates on the way to calling `gcc` is
deleted as soon as `gcc` returns, whether or not the build succeeded. `-i` keeps it:

```sh
riskc hello.risk -o hello        # hello.c is generated, then deleted
riskc hello.risk -i -o hello     # hello.c is kept alongside the binary
```

For a `HEADER_FILE` module ([§2](@ref getting-started)), the kept
intermediate is named with a `_risk.c` suffix instead of replacing the
module's own name, so it doesn't collide with a hand-written C file of
the same base name:

```sh
riskc header_mod.risk -i          # -> header_mod.o, header_mod_risk.c
```

Without `-g`, the kept `.c` file has **no** `#line` directives - it's
just the plain generated source. Combine with `-g` (below) to also get
source-mapping annotations in the kept file.

## `-g` - debug info that points back at your `.risk` source

`-g` makes the generated C carry `#line` directives that reference your
*original* `.risk` file and line numbers, not the generated C file's own
lines. This means a binary built with `-g` lets `gdb` (or any other
source-level debugger) step through, list, and report crashes against
your actual Risk source:

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

```
Program received signal SIGFPE, Arithmetic exception.
0x... in mod__divide (a=15, b=0) at .../crash.risk:7
7           result: i32 = a / b;
(gdb) bt
#0  mod__divide (a=15, b=0) at .../crash.risk:7
#1  mod__compute (n=5) at .../crash.risk:13
#2  mod__main () at .../crash.risk:19
```

Note the function names in the backtrace: Risk functions are compiled
into C under a `mod__` prefix (`main` becomes `mod__main`, and so on) -
useful to know if you ever need to set a breakpoint by C symbol name
directly (`gdb -ex "break mod__divide"`).

`-g` works whether or not `-i` is also passed - without `-i`, the
annotated intermediate `.c` is still generated and used for the build,
just deleted afterward like normal; only the resulting binary's debug
info (and thus `gdb`'s ability to show your `.risk` source) is affected,
not whether the `.c` file survives on disk.

## Other flags and defaults

- `-o <name>` / `--out <name>` sets the output name. The default is
  `a.out` (`a.exe` on Windows). With `-c`, an explicit `-o` names the
  object file instead.
- `-h` / `--help` prints usage and exits; `--version` (or `-v`) prints
  version information and exits. Both are handled before any
  compilation starts.
- Flags `riskc` does not recognize are ignored rather than reported, so
  a misspelled flag has no effect. Check the spelling if a flag seems to
  do nothing.

## Combining flags

All of the above compose freely - e.g. an optimized, LTO'd, debuggable
build with the intermediate C kept for inspection:

```sh
riskc hello.risk -O2 -flto -g -i -o hello
```

## Examples

A realistic "everyday" build vs. a "ship it" build, showing several
flags composing:

```sh
# Fast local iteration: no optimization, debug info for gdb, keep the
# generated C around so you can glance at it if something looks off.
riskc app.risk -O0 -g -i -o app

# Release build: full optimization, LTO across the whole program,
# tuned for the machine that will actually run it.
riskc app.risk -O3 -flto -march=native -o app

# CI: just verify it compiles, without linking or leaving anything behind.
riskc app.risk -c -o /dev/null
```

And the specific case this chapter calls out - confirming a flag is
correctly rejected rather than silently accepted:

```sh
riskc app.risk -Ofast -o app
# rejected -- no binary is produced, since -Ofast implies -ffast-math
```

**`-c`** for an object file instead of an executable, and **`-i`** on a
`HEADER_FILE` module (note the `_risk.c` suffix, not a plain `.c`):

```sh
riskc app.risk -c              # -> app.o, no executable
riskc app.risk -c -o custom.o  # -> custom.o (explicit name respected)

riskc header_mod.risk -i       # -> header_mod.o, header_mod.riskh,
                                #    header_mod_risk.c (kept, not deleted)
```

**`-g`** for a binary `gdb` can step through against the original
`.risk` source, not the generated C:

```sh
riskc crash.risk -g -o crash
gdb ./crash
# (gdb) run
# ... crashes with a backtrace listing crash.risk:7, not crash.c:142 ...
```

---

*[← Standard Library Reference](@ref stdlib-reference) | [Back to Manual Index](@ref user-manual)*
