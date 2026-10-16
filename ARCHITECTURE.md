# Risk Compiler Architecture Reference

## Table of Contents

1. [Overview](#overview)
2. [Pipeline Architecture](#pipeline)
3. [Lexical Analysis](#lexical)
4. [Abstract Syntax Tree](#ast)
5. [Parsing Strategy](#parsing)
6. [Type System and Semantic Analysis](#typecheck)
7. [Generics: Monomorphization](#generics)
8. [C Interop: `extern` Blocks](#extern)
9. [Memory-Safety Diagnostics](#memsafety)
10. [Lambdas and Closures](#lambdas)
11. [Numeric Promotion](#numeric)
12. [Compiler-Registered Builtins](#builtins)
13. [Code Generation](#codegen)
14. [The Embedded Runtime](#runtime)
15. [Module System (`.riskh`)](#modules)
16. [Diagnostics](#diagnostics)
17. [The Compilation Driver](#driver)
18. [Memory Model](#memmodel)
19. [Standard Library](#stdlib)
20. [File Reference](#files)

---

## 1. Overview

`riskc` is a single-pass, sequential compiler for Risk. Risk is a statically typed language that lowers to C99 and delegates final machine-code generation to GCC. The pipeline follows this sequence: **Lexer -> Parser -> TypeChecker -> CodeGen -> GCC**, orchestrated end-to-end by `driver.cpp::main()`.

The implementation includes generics (monomorphized at type-check time), `defer` statements, explicit-capture lambdas, C-library interop via `extern` blocks, `this`-receiver methods on `block` types, a `.riskh` module system with automatic header-dependency builds, and diagnostics for use-after-free and double-free scenarios. These features are active parts of `typechecker.cpp`, `codegen.cpp`, and `driver.cpp`. The source files themselves carry no comments; this document is the primary reference.

| Stage | Class | Input | Output |
| --- | --- | --- | --- |
| Lexing | `Lexer` (lexer.h/.cpp) | source `std::string` | `std::vector<Token>` |
| Parsing | `Parser` (parser.h/.cpp) | token stream | `TranslationUnit*` |
| Type checking | `TypeChecker` (typechecker.h/.cpp) | AST | diagnostics, `expr_type_map`, `compile_time_string_map`, `type_aliases`, and an AST mutated in place for generic instantiation |
| Code generation | `CodeGen` (codegen.h/.cpp) | checked AST | one `.c` file |
| Native build | driver → `gcc` | `.c` (plus linked `.o` or `.c` from `extern`) | executable or object file |

---

## 2. Pipeline Architecture

`driver.cpp::main()` performs the following steps in order:

1. Resolves the input path to an absolute path using `realpath` or `_fullpath`, and derives `source_dir` (the directory containing the input), used as the compile directory for all generated files.
2. Scans `argv` for `-h`/`--help` and `--version`/`-v` (printing and exiting immediately), then parses the remaining flags (see [The Compilation Driver](#driver)).
3. Reads the source file and tokenizes it using `Lexer`.
4. Scans forward past leading `Comment` tokens for a `HEADER_FILE` marker. If found, sets `is_header = true` and erases the marker token before parsing.
5. Parses the input into a `TranslationUnit*`, prints parser diagnostics, and aborts on error.
6. Calls `ensure_all_header_deps_built()`, which makes sure every imported header module has an up-to-date `.o` and `.riskh` (building missing ones by re-invoking the compiler executable on them) before anything depends on them.
7. **If `is_header` is true:** writes a temporary `<module>.h` C header, type-checks, generates `<module>_risk.c`, compiles it with `gcc -c` to `<module>.o`, and emits a `<module>.riskh` interface file. No executable is linked. The intermediate `_risk.c` and `.h` are deleted unless `-i` was passed.
8. **Otherwise:** type-checks the whole program (including the monomorphization of generic templates appended to the AST), prints diagnostics, and aborts on error.
9. Pre-reads each imported `.riskh` and registers `STRUCT` short names as `type_aliases` (short name -> `<module>__<name>`) so CodeGen resolves imported types before it runs.
10. Generates the module's `.c` file with `CodeGen`.
11. Re-reads the generated `.c` and splices the C declarations from every imported `.riskh` into it, right after the `typedef struct risk_hashmap_t risk_hashmap_t;` line (or at the top if that line is absent). Collects each imported module's `.o` (and its transitive header objects via `collect_transitive_header_objs()`) as link inputs.
12. Invokes `gcc` (see [The Compilation Driver](#driver) for the exact command shape), linking any `.o` files, `extern_link_sources` `.c` files and `-l<name>` flags from `extern` blocks (`TypeChecker::extern_link_sources` and `extern_link_libs`), plus `-I<dir>` for the source directory and every imported module directory (`TypeChecker::imported_dirs`).
13. Deletes the intermediate `.c` file after GCC returns, whether or not the build succeeded, unless `-i` (keep intermediate) was passed.

---

## 3. Lexical Analysis

The `Lexer` (defined in lexer.h and lexer.cpp across 58 and 315 lines) is a hand-written, character-at-a-time scanner that tracks position, line, and column numbers. Its primary public interface is `tokenize()`. It also exposes `lex_token()` so the parser can pull a single token directly during array-literal parsing without re-tokenizing.

* **Token Categories** (`TokenType`, token.h): `Identifier`, `Number`, `String`, `Char`, `Symbol`, `Comment`, `HeaderFileMarker`, `Boolean`, `Error`, and `EndOfFile`. `Boolean` and `HeaderFileMarker` are distinct token types recognized at scan time in `lex_identifier_or_keyword()`.
* **Numeric Literals**: `lex_number()` recognizes decimal, binary (`0b...`), and hexadecimal (`0x...`) formats. It normalizes binary and hex literals to decimal strings within the token lexeme, so the rest of the compiler only processes decimal representations. `INF` and `NAN` are lexed as `Number` tokens, and a leading-dot float such as `.4` is accepted (only when a digit follows the `.`, so member access is unaffected).
* **Malformed Numbers**: If a numeric literal runs directly into an identifier character (`33b`, `1.0a`, `0xFG`, or a non-binary digit after `0b`), `lex_number()` swallows the whole span into a single `TokenType::Error` token whose lexeme is the malformed source text. `Parser::parse_primary()` turns that token into an "invalid numeric literal" diagnostic. This prevents `a: i32 = 33b;` from silently parsing as two unrelated tokens.
* **Comment Handling**: `skip_whitespace_and_comments()` collects `//` and `/* */` comments into a caller-supplied vector, while `tokenize()` filters out comment tokens. `lex_token()` uses a separate throwaway vector so mid-array-literal parsing does not misread trailing slashes.
* **Two-Character Operators**: These are checked via a static null-terminated array (`==`, `!=`, `<=`, `>=`, `&&`, `||`, `<<`, `>>`, `+=`, `-=`, `*=`, `/=`, `%=`, `&=`, `|=`, `^=`, `->`) before falling back to single-character tokens.

---

## 4. Abstract Syntax Tree

`ast.h` (913 lines) defines all nodes under a base `Node` class, split into four functional families:

```
Node
├── TypeNode
├── Expr
│   ├── IdentifierExpr, NumberExpr, StringExpr, CharExpr, BoolExpr, NullExpr
│   ├── UnaryExpr         (prefix and postfix, distinguished by is_postfix)
│   ├── BinaryExpr        (binary operations and all assignment operations)
│   ├── CallExpr
│   ├── CastExpr          (safe and unsafe, distinguished by is_unsafe)
│   ├── LambdaExpr
│   ├── MemberExpr
│   ├── BlockLiteralExpr, ArrayLiteralExpr
│   ├── IndexExpr
│   └── SizeofExpr
├── Stmt
│   ├── ExprStmt, ReturnStmt, BlockStmt
│   ├── VarDeclStmt       (mutable or const, distinguished by is_const)
│   ├── IfStmt, WhileStmt, ForStmt, SwitchStmt
│   ├── BreakStmt, ContinueStmt
│   ├── DeferStmt
│   └── RegisterDecl      (hardware MMIO register, located in Stmt rather than Decl)
└── Decl
    ├── FuncDecl          (declaration or definition via is_definition; carries generic_params, is_extern, extern_header)
    ├── BlockDecl         (struct-like; carries generic_params and is_extern mirrors)
    ├── OptDecl           (enum-like; carries is_extern mirrors of C constants)
    ├── TypeAliasDecl
    ├── GenericSetDecl    (`generic Name = [Type, Type, ...];` constraint set for generic type parameters)
    ├── GlobalVarDecl
    └── ImportDecl

```

Memory ownership relies entirely on `std::unique_ptr` using four type aliases (`TypePtr`, `ExprPtr`, `StmtPtr`, `DeclPtr`). The tree contains no shared pointers and automatically frees itself when the root `TranslationUnit` goes out of scope. Downstream compiler stages use raw, non-owning pointers.

* `FuncParam` supports default values (`= expression`) and a `_` wildcard name for unused parameters.
* `GenericParam` records a function or block template parameter list, supporting type parameters constrained by a `GenericSetDecl` or `const N: i32` value parameters for array sizes.
* `BlockDecl`, `OptDecl`, and `FuncDecl` carry `is_extern` and `extern_header` fields to support both native Risk declarations and Risk-side mirrors of C declarations imported via `extern` blocks.
* Every expression, statement, and declaration tracks 1-based line and column numbers, with expressions also storing `end_col` for diagnostic underlines.

---

## 5. Parsing Strategy

`Parser` (parser.h at 117 lines and parser.cpp at 2,401 lines) uses hand-written recursive descent with precedence climbing for binary operators, up to `peek_n(n)` lookahead for disambiguation, and a diagnostic vector that accumulates errors without throwing exceptions to allow parsing to continue.

* **Variable Declarations**: There is no `let` keyword. Variable declarations are triggered by the pattern `IDENTIFIER ":"`. `parse_statement()` and `parse_decl()` route this pattern to `parse_var_decl_statement()`, or prepend `const` for constant declarations. `parse_multi_var_decl_statement()` supports declaring multiple names against a single type and initializer list (`a, b: i32 = 1, 2;`), desugaring them into a `BlockStmt` of individual variable declarations.
* **Defer Statements**: `parse_defer()` parses a single deferred statement. The type checker rejects `return` statements inside a `defer` body and prevents nesting one `defer` statement directly inside another using a `defer_depth` guard.
* **Lambdas**: `parse_primary()` recognizes a lambda when `(` is followed by `...` or by a capture-name list closed by `)` and then `[`. `parse_lambda()` reads the capture marker, an optional `[name: Type, ...]` parameter list, an optional `-> ReturnType`, and a body block. See [Lambdas and Closures](#lambdas).
* **Error Tokens**: `parse_primary()` reports any `TokenType::Error` token as an "invalid numeric literal" diagnostic and substitutes an `__err__` identifier so parsing can continue.
* **Generics**: `parse_generic_param_list()` parses optional `<T: SetName, U: SetName2, const N: i32>` clauses on functions or blocks. `parse_generic_type_arg()` parses use-site type arguments, compile-time constants (`Matrix<f32, 4>`), or enum value references (`Matrix<f32, Order.RowMajor>`). A `value_generic_scope` set tracks identifiers that refer to enclosing template value parameters rather than literal values.
* **Extern Blocks**: `parse_extern_block()` parses blocks structured as `extern "header.h" [lib "name", ...] { ... }`, treating signatures as body-less `FuncDecl` nodes tagged with `is_extern` and `extern_header`.
* **Generic Token Preprocessing**: The parser constructor runs `preprocess_generic_tokens()` (a file-static helper in parser.cpp) over the token stream, splitting a `>>` token into two `>` tokens when angle-bracket nesting depth is two or greater, resolving ambiguity for nested generics like `Array<Array<i32>>`.

---

## 6. Type System and Semantic Analysis

`TypeChecker` (typechecker.h: 199 lines; typechecker.cpp: 5,057 lines) handles semantic analysis. Types are represented by `TypeInfo`, which wraps a `std::string name`. Type equality is strictly name-based (nominal typing).

### Primitive Types and C Mappings

| Risk Type | C Type |  | Risk Type | C Type |
| --- | --- | --- | --- | --- |
| `i8`/`i16`/`i32`/`i64` | `int8_t`…`int64_t` |  | `u8`/`u16`/`u32`/`u64` | `uint8_t`…`uint64_t` |
| `f32`/`f64` | `float`/`double` |  | `bool` | `int` |
| `char` | `char` |  | `string` | `char*` |
| `void` | `void` |  | `null` | `NULL` |
| `Array<T>` | `risk_array_t*` |  | `ptr<T>` / `ref<T>` | `T*` |
| `Hashmap<K, V>` | `risk_hashmap_t*` |  | `const<T>` | `const T` |

### Processing Order

The `check()` method executes in a fixed order:

1. Opts types
2. Block types
3. Global variables
4. Compiler builtin registrations
5. User function signatures (non-generic functions go into `functions`; generic templates go into `generic_templates`)
6. Block-field default-value validation
7. `main` function validation
8. Function bodies (including processing the generic instantiation worklist to a fixed point)

### Symbol Tables and Type Inference

* `var_scopes` maintains a vector of `unordered_map<string, VarSymbol>` frames, pushed and popped at block and `for` loop boundaries.
* `functions` uses a flat `unordered_map<string, FuncSymbol>`. Risk does not support nested function declarations.
* `type_of_expr()` infers expressions and memoizes results in `expr_type_map`. Number literals with a decimal point default to `f64`, otherwise `i32`. Identifiers resolve through variable scopes, function signatures, and opts field names. Binary operations on numerics apply numeric promotion rules. Member expressions on arrays dispatch to methods like `.push` and `.pop`, while member expressions on blocks resolve fields or self-passing methods.
* The `"any"` pseudo-type skips argument-type checking for builtins that accept arbitrary types (`length`, `toString`, and the `io*` print intrinsics). It cannot be used in user type annotations.
* Additional validation rules reject assignments to `const` variables, require `ref<T>` initialization from lvalues with matching types, enforce loop and switch depth checks for `break` and `continue`, warn on uninitialized strings, blocks, and arrays, warn when returning addresses of local variables, and require `main` to return an integer type and to take either no parameters or a single `args: Array<string>` parameter.

---

## 7. Generics and Monomorphization

* **Generic Sets**: A declaration like `generic Name = [TypeA, TypeB, ...];` defines a constraint set checked structurally (`type_matches_pattern()` and `type_matches_generic_set()`).
* **Templates**: Generic function and block templates are stored in `generic_templates` and `generic_block_templates`, keeping them distinct from ordinary callable symbols.
* **Function Inference**: Function generics are inferred at call sites using `resolve_generic_call()`, which unifies declared parameter types against concrete argument types via `unify_generic_param()`.
* **Block Generics**: Block generics require explicit use-site arguments (e.g., `a: Matrix<f32, Order.RowMajor, 4, 4>`), matched positionally against template parameters.
* **Instantiation**: Instantiation clones the template using parameter substitution across types, expressions, statements, casts, and literals. It appends a concrete `FuncDecl` or `BlockDecl` to the live `TranslationUnit` under a mangled name generated by `mangle_generic_instance()`. Repeated instantiations reuse existing instances. Code generation only interacts with concrete, monomorphized declarations.

---

## 8. C Interop via `extern` Blocks

`extern "header.h" [lib "name", ...] { ... }` allows Risk code to interact with C APIs:

* Each function signature inside an extern block becomes a body-less `FuncDecl` tagged with `is_extern` and `extern_header`. CodeGen omits function bodies to prevent redeclaration conflicts.
* Nested `block` or `opts` declarations mirror existing C structs or constant sets for field access and value lookup. CodeGen emits no struct definitions or C enums for extern blocks; `opts` field values are inlined as integer constants.
* `TypeChecker::extern_link_libs` collects all program-wide library dependencies, passed to GCC via `-l<name>`.
* If an extern header directory contains a sibling `.c` file, its path is added to `extern_link_sources` and passed directly to the GCC invocation as an extra translation unit.

---

## 9. Memory-Safety Diagnostics

The type checker tracks memory-safety state alongside types:

* `freed_ptrs` records an `unordered_map<name, {line, col}>` tracking when variables are passed to free functions. Subsequent uses or double-frees are flagged at compile time.
* `block_has_owned_ptr_fields()` checks transitively whether a block type owns pointer fields subject to ownership tracking.
* Taking the address of a local variable in a `return` statement triggers a warning about returning dangling addresses.

This provides targeted safety checks rather than a full borrow checker.

---

## 10. Lambdas and Closures

Syntax follows the format `(captures) [params] -> RetType { body }`. The parenthesized marker lists the variables the lambda captures, or is the literal `(...)` for a lambda that captures nothing. Parameters are written `[x: i32, y: i32]` (the bracket list may be omitted) and the `-> RetType` is optional. For example, `(...) [x: i32] -> i32 { return x + 1; }` captures nothing, while `(offset) [x: i32] -> i32 { return x + offset; }` captures `offset`. Unannotated return types default to `void`.

* **Capture Analysis**: Captures are **explicit**. `analyze_lambda_captures()` validates the capture list the parser stored in `LambdaExpr::captures`: duplicate names and names that do not exist in an enclosing scope are errors. It then collects the free identifiers used in the body, subtracts the lambda's parameters and locally declared variables, and reports an error for any remaining name that resolves to an enclosing variable but was not listed (the message suggests adding it to the capture list). Captured variables are captured **by value**, and their types are recorded in `LambdaExpr::capture_type_names`.
* **Code Generation**: Capturing lambdas emit a C environment struct containing captured variables by value, a wrapper function taking a `void*` environment pointer and parameters, and a factory function that heap-allocates the environment and returns a `risk_closure_t*`. `risk_closure_t` (`{ void *env; void *fn; }`) is typedef'd by CodeGen in the generated C, not by `runtime.h`. Non-capturing lambdas compile to plain function pointers without environment allocations.

---

## 11. Numeric Promotion

`promote_numeric_name()` ranks numeric types to resolve binary operations between mixed operands:

| Type | Rank |  | Type | Rank |
| --- | --- | --- | --- | --- |
| `f64` | 60 |  | `i32` | 30 |
| `f32` | 50 |  | `u32` | 29 |
| `i64` | 40 |  | `i16` | 20 |
| `u64` | 39 |  | `u16` | 19 |
|  |  |  | `i8` / `u8` | 10 / 9 |

Floating-point operands promote the result to float (`f64` takes precedence over `f32`). Among integers, higher ranks win, and signed types take precedence over unsigned types of equal width. Integer literals inferred as `i32` are narrowed to smaller target types if their values fit the target range.

---

## 12. Compiler-Registered Builtins

`TypeChecker::check()` pre-registers the following built-in functions, available without imports:

| Function | Signature | Notes |
| --- | --- | --- |
| `ioInput` | `() -> string` | Backed by `risk_io_input()` |
| `toInt` | `(string) -> i32` |  |
| `toFloat` | `(string) -> f64` |  |
| `toBool` | `(string) -> bool` |  |
| `toString` | `(any) -> string` | Uses the `any` pseudo-type |
| `intToChar` | `(i32) -> string` |  |
| `length` | `(any) -> i32` | Works on arrays |
| `strRepeat` | `(string, i32) -> string` |  |
| `kbRead`, `kbWait` | `() -> string` | Implemented in `runtime.h` (separate POSIX and Windows variants) |
| `clrScr` | `() -> void` | Implemented in `runtime.h`; also special-cased in CodeGen |

A second group is handled directly as compiler intrinsics by the type checker and CodeGen rather than through the function table, because they take mixed or generic arguments: `ioPrint`, `ioPrintln`, `ioRawPrint`, `ioRawPrintln`, `sizeof`, `type`, the reference-counting helpers `rcNew`, `rcGet`, `rcRetain`, `rcRelease`, `rcCount`, the allocation helpers `arenaNew` and `dealloc`, and the zero-argument constants `PI()` and `E()`. Builtin naming follows **camelCase**. `dealloc` deliberately stays an intrinsic so the type checker's `freed_ptrs` analysis sees every call to it.

Functionality that used to be compiler magic now lives in the standard library and needs an import (see [Standard Library](#stdlib)): the `sys*` functions are in `@os/sys.risk`, and `kbHit`, `kbReadKey`, `rawModeEnter`, `rawModeLeave` and `flushStdout` are declared by `@tui` (POSIX only).

---

## 13. Code Generation

`CodeGen` (codegen.h: 182 lines; codegen.cpp: 4,820 lines) lowers a checked and monomorphized AST into a single C99 source file.

* **Defer Desugaring**: `collect_defers()` gathers `DeferStmt` instances within function bodies, emitting them in reverse declaration order before every `return` statement and at function exit.
* **Fixed-Size Array Returns**: Functions returning fixed-size arrays use output-parameter-style code generation since C cannot return raw arrays by value.
* **Self-Passing Methods**: Block fields typed as `func(this) -> ...` or `func(this, ...) -> ...` are invoked via member-call syntax (`obj.method(args)`) and compiled as C calls with the receiver passed as the first argument.
* **Name Mangling**: Generated C symbols use the format `<module>__<n>`, where module names replace directory separators with underscores.
* **GCC Invocation**: The driver invokes GCC using the format `gcc -w -std=c99 [opt flags] [-g] -I"<source_dir>" [-I"<imported_dir>" ...] -o"<out>" "<main.c>" ["<extern .c>" ...] [imported .o ...] [-l<lib> ...] -lm`. With `-c` the link inputs are omitted and the command is `... -c -o"<obj>" "<main.c>"`. Header modules are compiled with `gcc -c -w -std=c99 [opt flags] [-g] "<module>_risk.c" -o "<module>.o"`.

---

## 14. Embedded Runtime

`runtime.h` (988 lines) is emitted verbatim at the top of every generated `.c` file as `static inline` C, allowing dead-code elimination to strip unused routines. It includes:

* **Panic and Tracebacks**: A static manual call stack (`RISK_MAX_FRAMES`, 256 frames) with `risk_push_frame`/`risk_pop_frame`, plus `risk_print_traceback()` and `risk_panic(msg)` to print the call stack and abort.
* **Allocation**: `risk_alloc` and `risk_free` wrap standard memory allocation with null checks.
* **Data Structures**: Includes a string builder (`risk_strbuf_t`), reference-counting cells (`risk_rc_t` with `risk_rc_new`/`retain`/`release`/`count`/`get`), an arena allocator (`risk_arena_t`, with scratch begin/end and used/capacity queries in bytes, KiB, MiB, and GiB), dynamic arrays (`risk_array_t`, with push/pop at both ends, insert, remove-at, get/set, clear, and bulk-free helpers), and hashmaps (`risk_hashmap_t`, with create/set/get/has/remove/clear/size) using a hash-and-probe scheme.
* **I/O, Terminal and Conversions**: `risk_io_input()` for line input, `risk_to_str_int`/`risk_to_str_float`/`risk_to_str_bool`/`risk_to_float` conversion helpers, `is_nan`/`is_inf`, `char_to_str`, and the `kbRead`, `kbWait` and `clrScr` builtins, each with a Windows and a POSIX variant. `risk_closure_t` is not part of the runtime; CodeGen emits it.
* **Allocator Contract**: `risk_alloc` is always defined before any `extern` header is included in the generated C, so stdlib C headers call it directly without declaring it.

---

## 15. Module System (`.riskh`)

Compiling a module marked with `HEADER_FILE` produces both an object file and a `.riskh` interface file:

```
MODULE_NAME canvas
FUNC void canvas__draw(int32_t x, int32_t y);
STRUCT typedef struct canvas__Canvas canvas__Canvas;
STRUCTDEF struct canvas__Canvas {
STRUCTFIELD     int32_t width;
STRUCTFIELD     int32_t height;
STRUCTEND };
END

```

The driver reads imported `.riskh` files twice: first to inject type aliases into `tc.type_aliases` before CodeGen runs, and second to splice C declarations into the generated `.c` file. Import syntax supports local files (`import "file.risk";`) and standard library paths (`import @pkg/subdir;`, which the parser normalizes to `"@pkg/subdir.risk"` and which is resolved through `resolve_stdlib_file()` under the directory named by the `RISK_STDLIB` environment variable, defaulting to `./stdlib/`, trying both `<base><ext>` and `<base>/<base><ext>`). The driver also splices `ENUM` lines from `.riskh` files and, before each build, runs `ensure_all_header_deps_built()` so imported header modules are compiled first.

---

## 16. Diagnostics

`Diagnostic` (diagnostic.h, 20 lines) represents parser and type-checker messages:

```cpp
struct Diagnostic {
  enum class Severity { Error, Warning };
  Severity severity;
  std::string message;
  int line, col, end_col;
  std::string file;
};

```

Diagnostics are rendered with file locations, colored severity labels, source lines, and carets or underlines indicating error spans. Errors set a `has_error` flag to halt compilation after the current phase.

---

## 17. Compilation Driver

`driver.cpp` (1,288 lines) manages pipeline orchestration and CLI argument parsing. It resolves input paths to absolute paths early to ensure relative-path lookups remain consistent regardless of the working directory.

Usage: `riskc <file.risk> [-o out] [-O0|-O1|-O2|-O3|-Os|-Og] [-flto] [-march=...] [-c] [-g] [-i] [--profile] [--help] [--version]`

| Flag | Effect |
| --- | --- |
| `-o`, `--out <name>` | Output executable name (default `a.out`, or `a.exe` on Windows). With `-c`, an explicit `-o` names the object file. |
| `-O0`/`-O1`/`-O2`/`-O3`/`-Os`/`-Og` | Passed to GCC. |
| `-flto` | Appends `-flto` to the GCC flags. |
| `-march=<arch>` | Forwarded to GCC. |
| `-c` | Compile to an object file only (default `<module>.o`) without linking. |
| `-g` | Passes `-g` to GCC and enables debug info in CodeGen. |
| `-i` | Keep the intermediate `.c` (and, for header modules, the temporary `.h`). |
| `--profile` | Accepted but currently a no-op. |
| `-h`, `--help` | Print help and exit. |
| `--version`, `-v` | Print version information and exit. |

`-Ofast` and `-Oz` are rejected with an error: `-Ofast` enables `-ffast-math`, which can fold NaN/INF checks away, and `-Oz` is Clang-only. Unrecognized flags are ignored.

---

## 18. Memory Model

Risk uses manual memory management with optional safety features rather than a garbage collector:

* Ordinary heap objects such as arrays, hashmaps, strings, and closures are freed explicitly using dedicated destruction functions.
* `risk_rc_t` provides opt-in reference counting.
* `risk_arena_t` provides bulk allocation with single-call reset semantics.
* The type checker's `freed_ptrs` tracking catches common use-after-free and double-free errors at compile time without runtime overhead.
* Block instances created via block literals are stack-allocated in the generated C output, while arrays, hashmaps, strings, and closures are allocated on the heap.

---

## 19. Standard Library

The standard library is ordinary Risk source, split into three packages, plus small C headers for the few operations Risk cannot express itself (raw memory, `strlen`, clocks, syscalls). Packages are located through `resolve_stdlib_file()` under the directory named by the `RISK_STDLIB` environment variable (default `./stdlib/`), and are imported with `import @pkg;`, `import "@pkg";` or `import "@pkg/module.risk";`. Each package has an entry module that imports all of its parts, and every part can also be imported on its own.

C headers named in a module's `extern "..."` block are searched in the declaring module's own directory first (for example `stdlib/core/__core_headers__/__string__.h`, referenced from `stdlib/core/string.risk` as `__core_headers__/__string__.h`), then in the compile directory, then as written. Headers in the stdlib are `static inline` (apart from `@tui`'s input header) and rely on `risk_alloc` from the embedded runtime.

### `@core` (entry: `core.risk`)

| Module | Contents |
| --- | --- |
| `math.risk` | libm bindings (`pow`, `sqrt`, `cbrt`, `floor`, `ceil`, `round`, `exp`, `exp2`, `log`, `log10`, `log2`, `sin`, `cos`, `tan`, `asin`, `acos`, `atan`, `atan2`, `sinh`, `cosh`, `tanh`) plus pure-Risk `abs<T>`, `sq`, `mod<T>`, `max`, `min`, `degToRad`, `radToDeg`, `isNan`, `isInf`, and the constant `_MATH_PI`. The `_Num` generic set is `[f64, i32]`. |
| `string.risk` | Three C-backed primitives (`strLength`, `strCharAt`, `strConcat`, from `__string__.h`) and pure-Risk `charToInt`, `strIntToChar`, `charToString`, `strUpper`, `strLower`, `strCapitalize`, `strSubstr`, `strFind`, `strLastFind`, `strReplace`, `strSplit`, `strJoin`, `strTrim`, `strStartsWith`, `strEndsWith`, `strContains`, `strToChars`, `strChars`, `strCompare`. |
| `file.risk` | Built on libc `stdio.h`: `fioRead`, `fioWrite`, `fioAppend`, `fioExists`, `fioDelete`, `fioSize`, `fioCopy`, `fioBytesRead`, `fioBytesWrite`, `fioBufRead`, `fioBufWrite`. |
| `random.risk` | `randSeed`, `randInt`, `randRange`, `randFloat`, `randFloatRange` (imports `time.risk`). |
| `time.risk` | C-backed (`__time__.h`): `timeNow`, `timeMillis`, `timeMicros`, `timeSleep`, `timeMonotonic`. |
| `memory.risk` | `alloc`, `copyMemory`, and generic `setMemory<T>` (over `_Nums = [f32, f64, i32, i64, u64, u32]`), written in Risk over libc `malloc`, `abort` and `memcpy`. |

### `@os` (entry: `os.risk`)

| Module | Contents |
| --- | --- |
| `sys.risk` | `sysPlatform`, `sysArch`, `sysHostname`, `sysUsername`, `sysCwd`, `sysEnv`, `sysPid` (C-backed by `__sys__.h`, with Windows and POSIX branches). |
| `fs.risk` | `fsCwd`, `fsChdir`, `fsMkdir`, `fsMkdirAll`, `fsRemoveFile`, `fsRemoveDir`, `fsRemoveAll`, `fsRename`, `fsExists`, `fsIsFile`, `fsIsDir`, `fsFileSize`, `fsModTime`, `fsTempDir`, `fsListDirRaw`, and `fsListDir`, which splits the newline-joined raw listing into an `Array<string>`. |
| `process.risk` | `procExit`, `procSleepMs`, `procSystem`, `procSetEnv`, `procUnsetEnv`. |
| `path.risk` | C-backed `pathSeparator` and `pathAbsolute`, plus pure-Risk `pathIsAbsolute`, `pathJoin`, `pathBase`, `pathDir`, `pathExt`. |

### `@tui` (`tui.risk`, imports `@core`)

A terminal-UI toolkit, POSIX only. It owns the raw-mode and keyboard primitives (`kbHit`, `kbReadKey`, `rawModeEnter`, `rawModeLeave`, `flushStdout`) through `__tui_input__.h`, which is included into the module's own generated C so the symbols are only linked into programs that import `@tui`. It provides:

* ANSI control helpers (`tuiClear`, `tuiGoto`, cursor and alternate-screen control, `tuiFg`/`tuiBg`, 256-color variants, attribute flags).
* A cell buffer (`Cell`, `Buffer`, `tuiBufferNew`, `tuiBufferSet`, `tuiBufferFill`, `tuiBufferRender`) with box-drawing, block and symbol code points exposed as functions such as `BOX_TL()` and `BLOCK_FULL()`.
* Widgets: panels and boxes, rules, gauges, sparklines, bar and line charts, tables, a scrolling `LogView`, status and key-hint bars, legends, and a `BrailleCanvas` with a braille chart renderer.
* Key constants (`KEY_UP`, `KEY_F1`, `KEY_CTRL_C`, and so on; codes of 1000 and above for arrows and function keys, decoded from ANSI escape sequences), key classifiers (`keyIsPrintable`, `keyIsArrow`, ...), and `tuiSetup`/`tuiCleanup`.

---

## 20. File Reference

### Repository layout

```
src/                         compiler sources (riskc)
stdlib/
  core/                      @core: all .risk modules directly inside
    __core_headers__/        C headers used by @core
  os/                        @os: all .risk modules directly inside
    __os_headers__/          C headers used by @os
  tui/                       @tui
    __tui_header__/          C header used by @tui
```

### Compiler sources (`src/`)

| File | Role |
| --- | --- |
| `token.h` | `TokenType` and `Token` definitions |
| `lexer.h` / `lexer.cpp` | `Lexer` implementation |
| `ast.h` | AST node hierarchy and inline printing |
| `parser.h` / `parser.cpp` | `Parser` implementation |
| `typechecker.h` / `typechecker.cpp` | `TypeChecker`, `TypeInfo`, and generics monomorphization |
| `codegen.h` / `codegen.cpp` | `CodeGen` implementation |
| `runtime.h` | Embedded C99 runtime library |
| `diagnostic.h` | `Diagnostic` structure |
| `driver.cpp` | Pipeline orchestration and CLI handling |

### Standard library (`stdlib/`)

| Path | Role |
| --- | --- |
| `stdlib/core/*.risk` | `@core` modules: `core`, `math`, `string`, `file`, `random`, `time`, `memory` |
| `stdlib/core/__core_headers__/__string__.h`, `__time__.h` | C backing for `string.risk` and `time.risk` |
| `stdlib/os/*.risk` | `@os` modules: `os`, `sys`, `fs`, `process`, `path` |
| `stdlib/os/__os_headers__/__sys__.h`, `__fs__.h`, `__process__.h`, `__path__.h` | C backing for the matching `@os` modules |
| `stdlib/tui/tui.risk` | `@tui` library |
| `stdlib/tui/__tui_header__/__tui_input__.h` | POSIX raw-mode and keyboard input for `@tui` |
