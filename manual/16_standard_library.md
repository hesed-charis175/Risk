# 16. Standard Library Reference {#stdlib-reference}

*[← C Interop](@ref c-interop) | [Next: Compiler Flags →](@ref compiler-flags)*

Welcome to the comprehensive reference guide for Risk's standard library. Rather than just showing you the happy path, this chapter highlights edge cases—like empty inputs, out-of-range indices, and missing files—so you can avoid common pitfalls. Every detail here comes directly from the compiler's conformance tests and standard library source code.

The standard library is organized into three independent packages:

* **`@core`**: Handles strings, math, memory, randomness, time, and file I/O—the foundation almost every program relies on.


* **`@os`**: Manages paths, the filesystem, and process control (distinct from `@core/file.risk`, which focuses on the contents of an already-known file).


* **`@tui`**: Provides raw terminal input and ANSI terminal outputs (colors, cursor control, and a character-buffer drawing API for building text user interfaces).



Top-level packages automatically re-export their submodules. For example, `import "@core";` brings in every file under `@core/*.risk`, though you can also import individual submodules directly (like `import "@core/math.risk";`) if you only need a specific piece.

> **A quick note on `string` equality**: Using `==` or `!=` on two strings compares their actual **contents**, not their underlying memory pointers (`"ab" == someBuiltString` works just like you'd expect in modern languages). If you browse older source files in `@core` or `@os`, you might notice workarounds like `strCompare` or character-by-character checks; these are holdovers from an older compiler version and are now redundant, though harmless. Plain `==` and `!=` are the current, idiomatic way to compare strings.
> 
> 

---

## `@core/string.risk`

| Function | Behavior |
| --- | --- |
| `strLength(s)` | Returns the byte length; `strLength("")` evaluates to `0`.

 |
| `strUpper(s)` / `strLower(s)` | Performs case conversions.

 |
| `strCapitalize(s)` | Uppercases only the first character; `strCapitalize("")` returns `""`.

 |
| `strSubstr(s, start, len)` | Extracts a substring starting at `start` for `len` characters. Out-of-range or **negative** start values return `""` rather than indexing from the end. Lengths exceeding the remainder of the string are silently clamped.

 |
| `strFind(s, needle)` | Finds the index of the first occurrence, or `-1` if missing. An **empty needle** matches at index `0`.

 |
| `strLastFind(s, needle)` | Finds the last occurrence index, or `-1`. An empty needle matches at `strLength(s)` (the very end).

 |
| `strConcat(a, b)` | Concatenates strings. This is strictly **two-argument**—passing one or three arguments causes a compile-time `argument count mismatch` error. Chain calls or use `strJoin` for multiple pieces.

 |
| `strReplace(s, old, new)` | Replaces **all** occurrences rather than just the first. Unmatched queries leave the string untouched.

 |
| `strSplit(s, delim)` | Splits into an `Array<string>`. Missing delimiters yield a single-element array with the original string, while an **empty delimiter** splits between every character.

 |
| `strJoin(arr, sep)` | The inverse of `strSplit`—joins an `Array<string>` using a specified separator.

 |
| `strTrim(s)` | Strips leading and trailing whitespace, returning `""` for empty or all-whitespace strings.

 |
| `strStartsWith(s, prefix)` / `strEndsWith(s, suffix)` | Returns a `bool`. Prefixes or suffixes **longer than `s` itself** safely return `false` without crashing.

 |
| `strContains(s, sub)` | Checks if a substring exists (`bool`).

 |
| `strCompare(s1, s2)` | Compares string contents character-by-character (`bool`). Maintained for backward compatibility; plain `s1 == s2` is preferred.

 |
| `strToChars(s)` | Splits into an `Array<char>` (one element per byte).

 |
| `strChars(s)` | Splits into an `Array<string>` (one single-character string per byte).

 |
| `charToString(c)` | Converts a `char` into a single-character `string`.

 |
| `strCharAt(s, i)` | Retrieves the character at index `i` as a `string`, returning `""` if out of range.

 |
| `charToInt(s)` | Returns the ordinal value of a string's first character.

 |
| `strIntToChar(n)` | Converts an ordinal `i32` into a single-character string (redundant with the zero-import `intToChar()` intrinsic).

 |

```risk
import "@core";

parts: Array<string> = strSplit("a,bb,ccc", ",");
// parts = ["a", "bb", "ccc"]
strJoin(parts, "-");                       // "a-bb-ccc"

strSubstr("hello world", 6, 5);            // "world"
strSubstr("hi", 10, 5);                    // "" (start out of range)
strSubstr("hello", -1, 3);                 // "" (negative start)

strFind("hello world", "world");           // 6
strFind("hi", "hello");                    // -1
strFind("hello", "");                      // 0 (empty needle)

strReplace("foo bar foo", "foo", "baz");   // "baz bar baz"

```

---

## `@core/math.risk`

Every function in this module (including `abs`, `max`, and `min`) is strictly **`f64`-only** with no separate integer overloads. While passing an integer literal like `abs(-5)` works because it widens to `f64` at the boundary, the resulting output is also an `f64`—assigning it back to an `i32` variable requires an explicit cast.

Trigonometric and logarithmic functions mirror their C `<math.h>` equivalents (`abs`, `pow`, `sqrt`, `sq`, `cbrt`, `mod`, `max`, `min`, `floor`, `ceil`, `round`, `exp`, `exp2`, `log`, `log10`, `log2`, `sin`, `cos`, `tan`, `asin`, `acos`, `atan`, `atan2`, `sinh`, `cosh`, `tanh`), along with a couple of standout conversion and predicate functions:

* **`mod(a, b)`** follows **truncated-division** semantics (matching C's `fmod`) rather than Euclidean or floored modulo, meaning the result's sign follows the *dividend*, not the divisor:


```risk
mod(10.0, 3.0);    // 1.0
mod(-7.0, 3.0);    // -1.0   (truncated, not floored)
mod(7.0, -3.0);    // 1.0
mod(-7.0, -3.0);   // -1.0

```


* **`degToRad(deg)` / `radToDeg(rad)**` provide simple angle conversions (e.g., `degToRad(180.0)` is roughly $\pi$). Use the zero-import `PI()` intrinsic if you need $\pi$ directly.


* **`isNan(x)` / `isInf(x)**` act as predicates distinct from the `NAN`/`INF` literals. An indeterminate operation like `0.0 / 0.0` yields `NaN` rather than infinity, making `isNan(0.0/0.0)` true and `isInf(0.0/0.0)` false. `isInf` is true for both `1.0/0.0` and `-1.0/0.0`.



---

## `@core/memory.risk`

These low-level, byte-oriented memory functions power the higher-level tools found in Chapter 14:

```risk
p: ptr<void> = alloc(16);          // raw allocation, byte count in

src: Array<u8, 4> = [10, 20, 30, 40];
copyMemory(p, cast<ptr<void>>(&src), 4);   // memcpy-equivalent

setMemory(p, 99, 4);                // fills the first 4 bytes with the byte value 99
setMemory(p, 0, 16);                // zeroes the whole 16-byte allocation

dealloc(p);

```

**`alloc(n)` aborts the process immediately if allocation fails** instead of returning `null`. Consequently, you don't need (and can't usefully write) a `null` check afterward—your program will either receive a valid pointer or terminate.

The function `setMemory(dest, value, count)` is generic: the type of `value` dictates how many bytes are written per element, where `count` represents the *element count* rather than total bytes. For example, `setMemory(p, cast<u8>(0), 16)` zeroes 16 individual bytes, while `setMemory(p, cast<i32>(0), 4)` zeroes four `i32` values (also 16 bytes).

---

## `@core/random.risk`

This module provides a **seeded, deterministic** pseudo-random generator, allowing you to reproduce identical sequences for reliable testing and simulations:

```risk
randSeed(1234);
a1: i32 = randInt(1000);
a2: i32 = randInt(1000);

randSeed(1234);
b1: i32 = randInt(1000);   // == a1
b2: i32 = randInt(1000);   // == a2

```

| Function | Behavior |
| --- | --- |
| `randInt(max)` | Returns a uniform integer within `[0, max)`. Non-positive arguments (like `randInt(-5)`) return `0` instead of throwing an error.

 |
| `randRange(min, max)` | Returns a uniform integer within `[min, max)`. If `max <= min`, it unconditionally returns `min` (e.g., `randRange(10, 5)` yields `10`).

 |
| `randFloat()` | Returns a uniform `f64` within `[0.0, 1.0)`.

 |
| `randFloatRange(min, max)` | Returns a uniform `f64` within `[min, max)`, following the same `max <= min` fallback behavior.

 |

If you skip calling `randSeed()`, the generator automatically seeds itself using `timeMicros()` upon your first draw, ensuring variability across separate runs unless reproducibility is explicitly requested.

---

## `@core/time.risk`

| Function | Behavior |
| --- | --- |
| `timeNow()` | Returns the current Unix timestamp in whole seconds as an `i64`.

 |
| `timeMillis()` / `timeMicros()` | Returns the current instant in milliseconds or microseconds.

 |
| `timeMonotonic()` | A monotonic clock measured in nanoseconds; guaranteed to never decrease across a process's lifetime, unaffected by system wall-clock adjustments.

 |
| `timeSleep(ms)` | Blocks the current thread for roughly `ms` milliseconds.

 |

---

## `@core/file.risk`

A synchronous file I/O module handling whole-file text operations, line-buffered I/O, raw bytes, and utility checks:

| Function | Behavior |
| --- | --- |
| `fioWrite(path, contents)` | Overwrites or creates a file with `contents`, returning success as a `bool`.

 |
| `fioAppend(path, contents)` | Appends text to an existing file.

 |
| `fioRead(path)` | Reads the entire file contents as a `string`.

 |
| `fioSize(path)` | Returns the file size in bytes as an `i64`, or **`-1`** if the file is missing.

 |
| `fioExists(path)` | Returns a `bool` indicating existence.

 |
| `fioCopy(src, dst)` | Performs a binary-safe copy.

 |
| `fioDelete(path)` | Removes a file, returning success as a `bool`.

 |
| `fioBufWrite(path, lines)` | Writes an `Array<string>` as newline-separated lines.

 |
| `fioBufRead(path)` | Reads a file back into an `Array<string>` line-by-line.

 |
| `fioBytesWrite(path, ptrVoid, n)` | Writes `n` raw bytes from a buffer, returning the written count (`i64`).

 |
| `fioBytesRead(path, ptrVoid, n)` | Reads `n` raw bytes into a buffer, returning the read count (`i64`).

 |

Line splitting via `fioBufRead` mirrors C's `fgets` behavior rather than splitting naively:

* An empty file yields `0` lines.


* A file without a trailing newline successfully captures its single line without duplicating or losing data.


* A file with just `\n` counts as a single blank line.



 Three functions—`fioRead`, `fioWrite`, and `fioAppend`—will **terminate the process immediately** (`exit(1)`) via an internal error handler if `fopen` fails. Always verify files with `fioExists(path)` or `fioSize(path) >= 0` beforehand if your program needs to handle missing files gracefully.

---

## `@os` - operating system, paths, filesystem, process

Importing `import "@os";` pulls in all four submodules: `@os/path.risk`, `@os/fs.risk`, `@os/process.risk`, and `@os/sys.risk`.

### `@os/sys.risk` - system information

* `sysPlatform()`: Returns `"windows"`, `"macos"`, `"linux"`, or `"unknown"`.


* `sysArch()`: Returns `"x86_64"`, `"arm64"`, `"arm"`, `"x86"`, or `"unknown"`.


* `sysHostname()`: Returns the machine name.


* `sysUsername()`: Returns the active user's name.


* `sysCwd()`: Returns the current working directory.


* `sysEnv(name)`: Retrieves environment variables, returning `""` if unset.


* `sysPid()`: Returns the current process ID (`i64`).



### `@os/path.risk` - path string manipulation

Performs pure string transformations (without disk access) supporting both `/` and `\` separators regardless of platform:

* `pathSeparator()`: Returns `"/"` or `"\\"` depending on the host.


* `pathIsAbsolute(path)`: Checks if a path starts with a root separator or Windows drive letter.


* `pathJoin(a, b)`: Combines paths, collapsing duplicate separators.


* `pathBase(path)` / `pathDir(path)`: Extracts the final file component or parent directory.


* `pathExt(path)`: Extracts the extension (including the leading `.`).


* `pathAbsolute(path)`: Resolves relative paths against the working directory.



### `@os/fs.risk` - filesystem management

Manages directories and filesystem states independently of file contents:

* `fsCwd()` / `fsChdir(path)`: Gets or sets the working directory.


* `fsMkdir(path)` / `fsMkdirAll(path)`: Creates directories (the latter mimics `mkdir -p`).


* `fsRemoveFile(path)` / `fsRemoveDir(path)` / `fsRemoveAll(path)`: Deletes files, empty directories, or recursive directory trees.


* `fsRename(from, to)`: Moves or renames filesystem entries.


* `fsExists(path)`, `fsIsFile(path)`, `fsIsDir(path)`: Inspects path types.


* `fsFileSize(path)` / `fsModTime(path)`: Retrieves size and last-modified timestamps.


* `fsTempDir()`: Returns the system temp path.


* `fsListDir(path)`: Returns an `Array<string>` of non-recursive entries, returning an empty array safely if the directory doesn't exist.



### `@os/process.risk` - process control

* `procExit(code)`: Terminates the process immediately.


* `procSleepMs(ms)`: Sleeps the thread for specified milliseconds.


* `procSystem(cmd)`: Executes a shell command, returning its exit code (a negative value indicates failure to spawn).


* `procSetEnv(name, value)` / `procUnsetEnv(name)`: Modifies process environment variables.



---

## `@tui` - terminal input and ANSI output

Importing `import "@tui";` gives you access to raw keyboard handling, ANSI escape sequence wrappers, and a retained-mode character-buffer API (`Buffer`/`Cell`).

### Raw input & ANSI helpers

* **`rawModeEnter()` / `rawModeLeave()**`: Toggles terminal raw mode (always pair these with `defer` to ensure cleanup on early returns).


* **`kbHit()` / `kbReadKey()**`: Polls and reads key codes non-blockingly.


* **ANSI Wrappers**: Functions like `tuiClear()`, `tuiGoto(row, col)`, `tuiHideCursor()`, `tuiFg(color)`, and `tuiEnterAltScreen()` make terminal styling clean and readable without manual escape code formatting.



### `Buffer`/`Cell` retained-mode drawing

For advanced terminal apps, `Buffer` offers an in-memory grid that renders updates in a single optimized pass, eliminating flicker:

* `tuiBufferNew(width, height)`: Allocates a new drawing buffer.


* `tuiBufferSet(...)` / `tuiBufferFill(...)`: Modifies cells or fills regions.


* `tuiBufferRender(buf)`: Draws changes to the screen efficiently.



The library also includes comprehensive layouts, widgets, gauges, sparklines, Braille canvas graphing tools (`BrailleCanvas`), tables, and scrolling log panes (`LogView`) designed for building robust terminal user interfaces.

---

*[← C Interop](@ref c-interop) | [Next: Compiler Flags →](@ref compiler-flags)*
