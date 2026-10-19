#include "builtins.h"

namespace {

std::vector<Builtin> make_functions() {
  return {
      {"ioPrint", "func ioPrint(...values) -> void", "", {"...values"},
       "Prints any number of arguments of any type, converted to text and concatenated. No trailing newline."},
      {"ioPrintln", "func ioPrintln(...values) -> void", "", {"...values"},
       "Prints any number of arguments of any type, converted to text and concatenated, followed by a newline."},
      {"ioRawPrint", "func ioRawPrint(...values) -> void", "", {"...values"},
       "Lower-level variant of `ioPrint`."},
      {"ioRawPrintln", "func ioRawPrintln(...values) -> void", "", {"...values"},
       "Lower-level variant of `ioPrintln`."},
      {"ioInput", "func ioInput() -> string", "string", {},
       "Reads a line of input from the terminal."},
      {"type", "func type(value) -> string", "string", {"value"},
       "Reports the type of an expression as a string, e.g. `type(3.14)` is `f64`."},
      {"length", "func length(value) -> i32", "i32", {"value"},
       "Number of elements in an `Array`, `Hashmap` or string."},
      {"sizeof", "func sizeof(type_or_value) -> i32", "i32", {"type_or_value"},
       "Size in bytes of a type or value. For a fixed-size `Array<T, N>` this is the total byte size."},
      {"toString", "func toString(value) -> string", "string", {"value"},
       "Converts any value to its text form."},
      {"toInt", "func toInt(s: string) -> i32", "i32", {"s: string"},
       "Parses an optional sign and leading digits. Stops at the first non-digit; empty or non-numeric input gives `0`."},
      {"toFloat", "func toFloat(s: string) -> f64", "f64", {"s: string"},
       "Parses a floating-point number."},
      {"toBool", "func toBool(s: string) -> bool", "bool", {"s: string"},
       "Case-insensitive: prefixes matching `true`, `yes`, `on` or `1` give `true`; anything else gives `false`."},
      {"intToChar", "func intToChar(n: i32) -> string", "string", {"n: i32"},
       "Converts an ordinal into a single-character string."},
      {"strRepeat", "func strRepeat(s: string, n: i32) -> string", "string",
       {"s: string", "n: i32"},
       "Concatenates `n` copies of `s`. `n == 0` gives `\"\"`; a negative `n` yields a null string (undefined behaviour if used) -- check `n >= 0` first."},
      {"kbRead", "func kbRead() -> string", "string", {},
       "Reads one keypress without blocking. Returns `\"\"` if no key was pressed."},
      {"kbWait", "func kbWait() -> string", "string", {},
       "Like `kbRead`, but blocks until a key is available."},
      {"clrScr", "func clrScr() -> void", "", {},
       "Clears the terminal and resets the cursor using ANSI escapes."},
      {"PI", "func PI() -> f64", "f64", {}, "The constant pi. Called as a zero-argument function."},
      {"E", "func E() -> f64", "f64", {}, "Euler's number. Called as a zero-argument function."},
      {"dealloc", "func dealloc(p) -> void", "", {"p"},
       "Frees a `ptr<T>`. The compiler flags a double-free of the same variable."},
      {"rcNew", "func rcNew(value: T) -> refc<T>", "", {"value"},
       "Wraps a value in a reference-counted cell."},
      {"rcRetain", "func rcRetain(r: refc<T>) -> refc<T>", "", {"r: refc<T>"},
       "Shares ownership: bumps the reference count and returns the handle. Use this rather than plain `=`."},
      {"rcRelease", "func rcRelease(r: refc<T>) -> void", "", {"r: refc<T>"},
       "Releases your claim. Releasing the same handle twice is a compile error."},
      {"rcGet", "func rcGet(r: refc<T>) -> ptr<T>", "", {"r: refc<T>"},
       "Borrows the underlying pointer."},
      {"rcCount", "func rcCount(r: refc<T>) -> u64", "u64", {"r: refc<T>"},
       "Current number of owners."},
      {"arenaNew", "func arenaNew(bytes: u64) -> Arena", "Arena", {"bytes: u64"},
       "Creates a bump allocator with the given initial byte budget. It grows automatically if exceeded."},
  };
}

std::vector<Builtin> make_array_methods() {
  return {
      {"push", "push(v: $T)", "", {"v: $T"}, "Appends an element."},
      {"pop", "pop() -> $T", "$T", {}, "Removes and returns the last element."},
      {"pushFront", "pushFront(v: $T)", "", {"v: $T"}, "Prepends an element."},
      {"popFront", "popFront() -> $T", "$T", {}, "Removes and returns the first element."},
      {"insert", "insert(i: i32, v: $T)", "", {"i: i32", "v: $T"}, "Inserts a value at index `i`."},
      {"removeAt", "removeAt(i: i32)", "", {"i: i32"}, "Removes the element at index `i`."},
      {"clear", "clear()", "", {}, "Empties the array. Does not free what elements point to."},
      {"repr", "repr(raw: bool = false) -> string", "string", {"raw: bool = false"},
       "String form of the array. For arrays of blocks it uses each block's `repr` field; `raw = true` prints addresses instead."},
      {"freeAll", "freeAll()", "", {},
       "`dealloc`s every element then clears the array. Only valid on `Array<ptr<T>>` or `Array<refc<T>>`."},
      {"destroy", "destroy()", "", {},
       "Releases the array container itself (not what elements point to)."},
  };
}

std::vector<Builtin> make_hashmap_methods() {
  return {
      {"has", "has(k: $K) -> bool", "bool", {"k: $K"}, "True if the key exists."},
      {"contains", "contains(k: $K) -> bool", "bool", {"k: $K"}, "Alias of `has`."},
      {"size", "size() -> i32", "i32", {}, "Number of stored key/value pairs."},
      {"remove", "remove(k: $K)", "", {"k: $K"}, "Removes a key; a missing key is a no-op."},
      {"delete", "delete(k: $K)", "", {"k: $K"}, "Alias of `remove`."},
      {"clear", "clear()", "", {},
       "Removes all pairs. Values holding pointers are NOT freed (the compiler warns)."},
  };
}

std::vector<Builtin> make_arena_methods() {
  return {
      {"alloc", "alloc(n: u64) -> ptr<void>", "ptr<void>", {"n: u64"}, "Allocates `n` bytes out of the arena."},
      {"used", "used() -> u64", "u64", {}, "Bytes handed out so far, across every block."},
      {"KiB", "KiB() -> f64", "f64", {}, "`used()` in KiB."},
      {"MiB", "MiB() -> f64", "f64", {}, "`used()` in MiB."},
      {"GiB", "GiB() -> f64", "f64", {}, "`used()` in GiB."},
      {"capacityBytes", "capacityBytes() -> u64", "u64", {}, "Total bytes reserved (used + unused)."},
      {"capacityKiB", "capacityKiB() -> f64", "f64", {}, "`capacityBytes()` in KiB."},
      {"capacityMiB", "capacityMiB() -> f64", "f64", {}, "`capacityBytes()` in MiB."},
      {"capacityGiB", "capacityGiB() -> f64", "f64", {}, "`capacityBytes()` in GiB."},
      {"reset", "reset()", "", {}, "Marks all memory free for reuse (does not return it to the OS)."},
      {"destroy", "destroy()", "", {}, "Releases the arena's memory entirely."},
      {"scratchBegin", "scratchBegin() -> ptr<void>", "ptr<void>", {}, "Checkpoint for temporary workspace."},
      {"scratchEnd", "scratchEnd(mark: ptr<void>)", "", {"mark: ptr<void>"},
       "Rewinds only the scratch region taken since `mark`."},
  };
}

} // namespace

const std::vector<Builtin> &builtin_functions() {
  static const auto v = make_functions();
  return v;
}

const Builtin *find_builtin_function(const std::string &name) {
  for (auto &b : builtin_functions())
    if (b.name == name)
      return &b;
  return nullptr;
}

const std::vector<Builtin> &builtin_methods(const std::string &t) {
  static const auto arr = make_array_methods();
  static const auto map = make_hashmap_methods();
  static const auto arena = make_arena_methods();
  static const std::vector<Builtin> none;
  if (t == "Array")
    return arr;
  if (t == "Hashmap")
    return map;
  if (t == "Arena")
    return arena;
  return none;
}

const Builtin *find_builtin_method(const std::string &t, const std::string &n) {
  for (auto &b : builtin_methods(t))
    if (b.name == n)
      return &b;
  return nullptr;
}

const std::vector<std::string> &keywords() {
  static const std::vector<std::string> k = {
      "func",   "block",    "opts",    "generic", "typealias", "import",
      "extern", "lib",      "as",      "if",      "else",      "while",
      "for",    "switch",   "case",    "default", "break",     "continue",
      "return", "defer",    "const",   "register", "noalias",  "this",
      "true",   "false",    "null",    "cast",    "unsafe_cast", "sizeof",
      "INF",    "NAN"};
  return k;
}

const std::vector<std::string> &primitive_types() {
  static const std::vector<std::string> t = {
      "i8",  "i16", "i32", "i64", "u8",   "u16",  "u32",
      "u64", "f32", "f64", "bool", "char", "string", "void"};
  return t;
}

const std::vector<std::string> &container_types() {
  static const std::vector<std::string> t = {"Array", "Hashmap", "Arena", "ptr",
                                             "refc",  "opt",     "func"};
  return t;
}

std::string describe_builtin_word(const std::string &n) {
  static const std::vector<std::pair<std::string, std::string>> docs = {
      {"i8", "*primitive* — signed 8-bit integer (`int8_t`), −128 … 127"},
      {"i16", "*primitive* — signed 16-bit integer (`int16_t`)"},
      {"i32", "*primitive* — signed 32-bit integer (`int32_t`); the default type of an integer literal"},
      {"i64", "*primitive* — signed 64-bit integer (`int64_t`)"},
      {"u8", "*primitive* — unsigned 8-bit integer (`uint8_t`), 0 … 255"},
      {"u16", "*primitive* — unsigned 16-bit integer (`uint16_t`)"},
      {"u32", "*primitive* — unsigned 32-bit integer (`uint32_t`)"},
      {"u64", "*primitive* — unsigned 64-bit integer (`uint64_t`)"},
      {"f32", "*primitive* — 32-bit float (`float`)"},
      {"f64", "*primitive* — 64-bit float (`double`); the default type of a literal with a decimal point"},
      {"bool", "*primitive* — `true` or `false`"},
      {"char", "*primitive* — a single ASCII character; use `cast<i32>(c)` for its ordinal"},
      {"string", "*primitive* — heap-managed, null-terminated string; `==` / `!=` compare contents"},
      {"void", "*type* — no value"},
      {"Array", "*container* — `Array<T>` is a growable heap array; `Array<T, N>` is a fixed-size stack array"},
      {"Hashmap", "*container* — `Hashmap<K, V>`, created from an empty literal `{}`; use `m[k]`, `has`, `size`, `remove`, `clear`"},
      {"Arena", "*memory* — bump allocator created with `arenaNew(bytes)`"},
      {"ptr", "*type* — `ptr<T>`, a raw pointer; free with `dealloc`"},
      {"refc", "*type* — `refc<T>`, reference-counted handle: `rcNew`, `rcRetain`, `rcRelease`, `rcGet`, `rcCount`"},
      {"func", "*keyword* — declares a function, or (in type position) a function type `func(A, B) -> R`"},
      {"block", "*keyword* — declares a struct-like aggregate type"},
      {"opts", "*keyword* — declares an enum-like set of integer constants"},
      {"generic", "*keyword* — declares a named set of types usable as a generic constraint: `generic Num = [i32, f64];`"},
      {"typealias", "*keyword* — gives an existing type another name (not a distinct type)"},
      {"import", "*keyword* — `import \"file.risk\";` or `import @core;`"},
      {"extern", "*keyword* — `extern \"header.h\" lib \"m\" { ... }` declares C functions, structs and globals"},
      {"defer", "*keyword* — runs a statement when the enclosing scope exits"},
      {"const", "*keyword* — goes in the type position: `name: const T = value;`"},
      {"noalias", "*keyword* — parameter qualifier promising the pointer is not aliased"},
      {"register", "*keyword* — `name: register<T>(addr);` binds a name to a fixed memory-mapped address (local scope only)"},
      {"cast", "*keyword* — checked conversion: `cast<T>(expr)`"},
      {"unsafe_cast", "*keyword* — unchecked conversion: `unsafe_cast<T>(expr)`"},
      {"sizeof", "*keyword* — `sizeof(T)` or `sizeof(expr)`, in bytes"},
      {"this", "*keyword* — first parameter of a method field type, `func(this) -> R`; passed by reference"},
      {"INF", "*constant* — floating-point infinity"},
      {"NAN", "*constant* — floating-point not-a-number"},
  };
  for (auto &kv : docs)
    if (kv.first == n)
      return kv.second;
  return "";
}

