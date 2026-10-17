# 1. Introduction {#intro}

*[← Manual Index](@ref user-manual) | [Next: Getting Started →](@ref getting-started)*

Risk is a statically typed, compiled systems language. The `riskc` compiler lowers your program to C99 and passes it to `gcc` to produce a native binary. Almost every design decision in this manual follows from this architecture.

## Manual Memory Management

You manage heap memory explicitly without a garbage collector. Risk provides three tools for memory management: manual allocation with `ptr<T>` and `dealloc`, reference-counted `refc<T>` cells, and bulk `Arena` allocation. These options are detailed in [Memory Management](@ref arena-rc). Ordinary local variables, including `block` instances created from literals, reside on the stack and require no explicit cleanup.

## Static and Explicit Types

Every variable declaration must include an explicit type annotation; there is no type-inferred shorthand for local variables such as `let x = 5;`. Once inside an expression, function bodies utilize expression-level type inference for numeric literals and arithmetic promotion, as covered in [Primitive Types](@ref primitives) and [Operators](@ref operators).

## Nominal Typing

Two types are identical only if they share the same name. Risk does not use structural typing, meaning a `block` with fields identical to another `block` remains a distinct type. This distinction is critical for generics ([§10](@ref lang-generics)), where a generic set member like `Array<i32>` matches structurally against a pattern, but independently declared `block` types are never interchangeable regardless of their fields.

## Predictable Control Flow

Control flow can be traced directly from source code without hidden exceptions or implicit destructors, with one opt-in feature: a block's `init` field ([§8](@ref blocks)), which the compiler executes automatically once per constructed instance. The `defer` keyword provides a mechanism to execute cleanup code near the end of a function. Deferred calls run in reverse (LIFO) order immediately before the function returns:

```risk
func main() -> i32 {
    defer ioPrintln(1);
    defer ioPrintln(2);
    defer ioPrintln(3);

    ioPrintln(0);
    return 0;
}
// prints: 0, 3, 2, 1

```

## Compile-Time Generics

Functions and `block` types support generics using type parameters constrained by named sets alongside integer value parameters, all resolved at compile time. By the code generation phase, only concrete functions and blocks exist without runtime type information or dynamic dispatch. See [§10](@ref lang-generics) for full details on error modes and usage.

## Direct C Interop

`extern` blocks allow you to declare and call existing C functions, structures, option constants, and global variables without wrapper code, and link against system libraries using a `lib` clause. See [§15](@ref c-interop).

## Transparent C Code Generation

You can inspect the generated C code by retaining the intermediate file with the `-i` flag. Building with `-g` enables `gdb` to step through your original `.risk` source code line-by-line rather than the generated C output. See [§17](@ref compiler-flags).

## Examples

**Memory ownership, `defer` ordering, and C99 translation:**

```risk
import "@core";

func loadConfig() -> ptr<i32> {
    p: ptr<i32> = cast<ptr<i32>>(alloc(sizeof(i32)));
    *p = 42;
    return p;
}

func main() -> i32 {
    defer ioPrintln("3: last-declared defer runs first");
    defer ioPrintln("2: middle defer");
    defer ioPrintln("1: first-declared defer runs last");

    ioPrintln("0: normal code runs top to bottom, same as any language");

    // Manual memory management -- 'cfg' is heap memory you own and must
    // release yourself, same as C's malloc/free (see §14).
    cfg: ptr<i32> = loadConfig();
    ioPrintln("config value: ", *cfg);
    dealloc(cfg);

    return 0;
}
// prints, in order:
// 0: normal code runs top to bottom, same as any language
// config value: 42
// 3: last-declared defer runs first
// 2: middle defer
// 1: first-declared defer runs last

```

Build the program with `riskc intro.risk -i -o intro` and open the generated `intro.c` file to inspect the C99 translation of the `defer` unwind and heap allocations.

**Explicit type annotations:**

```risk
func main() -> i32 {
    count: i32 = 5;         // type annotation is mandatory here
    total: f64 = 0.0;
    total = total + count;  // valid -- ordinary expression-level promotion
    ioPrintln(total);
    return 0;
}

```

**Nominal typing demonstration:**

```risk
block Point2D { x: i32; y: i32; }
block Velocity { x: i32; y: i32; }   // identical fields to Point2D, distinct type

func describe(p: Point2D) -> void {
    ioPrintln(p.x, ", ", p.y);
}

func main() -> i32 {
    p: Point2D = { 1, 2 };
    v: Velocity = { 3, 4 };
    describe(p);         // valid
    // describe(v);      // compile error: Velocity is not a Point2D,
                         // even though the fields match exactly
    return 0;
}

```

---

*[← Manual Index](@ref user-manual) | [Next: Getting Started →](@ref getting-started)*
