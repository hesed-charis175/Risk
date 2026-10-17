# 13. Imports {#imports}

*[← Casting](@ref casting) | [Next: Memory Management →](@ref arena-rc)*

```risk
import "utils.risk";
import "utils.risk" as u;
import @core;
import @collections/sorting;
import @collections/sorting as sort;

```

* A plain quoted path imports a sibling `.risk` file (or its precompiled `.riskh` interface if the module was built separately as a `HEADER_FILE` module—see [§2](@ref getting-started)) relative to your program.
* An `@`-prefixed path resolves against the standard library search path (`RISK_STDLIB`, falling back to `./stdlib/`), optionally through a subdirectory like `@collections/sorting`.
* The `as alias` clause gives the imported module a local name. Without it, symbols are imported under their own names.
* Importing the same module twice (directly or transitively) is safe because the compiler processes each unique module only once.

## `@core` Versus Its Submodules

`import "@core";` pulls in the umbrella module, which re-exports individual submodules including string, math, memory, random, time, and file utilities (see [§16](@ref stdlib-reference)). Submodules can also be imported directly to maintain a smaller dependency surface, such as `import "@core/math.risk";`.

The Getting Started zero-import intrinsics (`ioPrintln`, `length`, `sizeof`, etc.) require no imports, whereas all other standard library features require their respective modules.

## The Two Import Spellings

```risk
import "utils.risk";        // Quoted path: no automatic alias
import @collections/sorting; // @-path: automatically aliased to "sorting"

```

* A **quoted** import receives no default alias; an alias is established only when explicitly written using `as alias`.
* An **`@`-prefixed** import automatically receives a default alias matching its **last path segment**, even without an explicit `as` clause. For example, `import @collections/sorting;` behaves as though `import @collections/sorting as sorting;` was written. (Specifying an alternate alias like `import @collections/sorting as sort;` overrides this default).

Explicit `.risk` suffixes on `@` imports (such as `import @tui/tui.risk;`) are recognized and stripped by the compiler since suffixes are appended internally. They are supported purely to prevent copy-pasted filenames from breaking syntax parsing.

### Qualified and Unqualified Names Both Remain Available

An alias does not hide the bare, unqualified name of imported symbols. Every imported function remains callable both qualified and unqualified:

```risk
import @collections/sorting;  // Auto-aliased as "sorting"

// Both calls reference the exact same function:
sorted1: Array<i32> = sorting.quicksort(nums);
sorted2: Array<i32> = quicksort(nums);          // Unqualified usage remains valid

```

The qualified form (`module.func()`) serves primarily to disambiguate functions when multiple imported modules define symbols with identical names.

### Duplicate Import Deduplication

```risk
import "utils.risk";
import "utils.risk";      // No-op: module already processed
import "utils.risk" as u; // No-op for processing -- adds the "u" alias without re-parsing

```

The compiler tracks loaded modules by path and skips re-processing, including when modules are reached transitively through multiple import chains (e.g., if both `B` and `C` import `D`, module `D` is compiled only once).

## Examples

The following example demonstrates both import spellings and dual call syntax:

```risk
// mathutils.risk
HEADER_FILE

func double(x: i32) -> i32 {
    return x * 2;
}

```

```risk
// main.risk
import "mathutils.risk";        // Quoted path without automatic alias
import "mathutils.risk" as m;   // Adds the "m" alias to the already-processed module

func main() -> i32 {
    a: i32 = double(21);        // Unqualified call works
    b: i32 = m.double(21);      // Qualified call via alias
    ioPrintln(a, " ", b);       // 42 42
    return 0;
}

```

```sh
riskc mathutils.risk           # Generates mathutils.o and mathutils.riskh
riskc main.risk -o main
./main

```

**Automatic `@`-prefixed aliasing using the last path segment:**

```risk
import @collections/sorting;   // Auto-aliased to "sorting"

func main() -> i32 {
    nums: Array<i32> = [3, 1, 2];
    sorted1: Array<i32> = sorting.quicksort(nums);   // Via auto-alias
    sorted2: Array<i32> = quicksort(nums);           // Unqualified call
    ioPrintln(sorted1.repr(), " ", sorted2.repr());
    return 0;
}

```

---

*[← Casting](@ref casting) | [Next: Memory Management →](@ref arena-rc)*
