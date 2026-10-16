# The Risk Programming Language - User Manual                              {#user-manual}

This manual is split into one page per topic so each can go deep without
turning into a single unreadable wall of text. Read it in order the first
time; use it as reference after that.

## Chapters

1. [Introduction](@ref intro) - what Risk is, and the design decisions
   that follow from "it compiles to C99."
2. [Getting Started: Compiling and Running Programs](@ref getting-started)
 - `main`, the `riskc` driver, command-line arguments, and the flags
   that control optimization, debug info, and intermediate output.
3. [Primitive Types](@ref primitives) - integers, floats, `bool`, `char`,
   `string`, and exactly how literal fitting and numeric promotion work.
4. [Pointers and the Four Constness Levels](@ref pointers) -
   `ptr<T>`, `const<T>`, and `noalias`, with a diagram.
5. [Compound Types: Array and Hashmap](@ref compound) - dynamic vs.
   fixed-size arrays, hash maps, and the sharp edges of each.
6. [Control Flow](@ref control-flow) - `if`, `while`, `for`, `switch`,
   `break`/`continue`, and scoping rules.
7. [Functions, Function Pointers, Closures, and Typealiases](@ref
   functions) - defaults, captures, and forward declarations.
8. [Blocks](@ref blocks) - Risk's struct type, literal construction, the
   `this`-by-reference receiver, and the `init`/`repr` conventions in
   depth.
9. [Opts (Enum-like Constants)](@ref opts) - declaration, comparison, and
   the runtime-clamped cast semantics.
10. [Generics](@ref lang-generics) - type and value parameters for both
    functions and blocks, inference rules, instance deduplication, and
    every documented compile-time error a misused generic can produce.
11. [Operators](@ref operators) - precedence, associativity, and numeric
    promotion ranking.
12. [Casting](@ref casting) - `cast` and `unsafe_cast`.
13. [Imports](@ref imports) - modules, the standard library search path,
    and aliasing.
14. [Memory Management: Arenas and Reference Counting](@ref arena-rc) -
    `ptr`+`dealloc`, `refc<T>`, and `Arena`, and how to choose between
    them.
15. [C Interop: `extern` Blocks](@ref c-interop) - calling C functions,
    mirroring C structs, opaque handles, extern globals, and linking
    against libraries.
16. [Standard Library Reference](@ref stdlib-reference) - the full,
    function-by-function behavior of `@core`'s string, math, memory,
    random, time, and file modules, including boundary conditions.
17. [Compiler Flags and Diagnostics](@ref compiler-flags) - optimization
    levels, `-g`/`-i`/`-c`, and using `gdb` directly against your `.risk`
    source.

