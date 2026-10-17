# 8. Blocks {#blocks}

*[← Functions & Closures](@ref functions) | [Next: Opts →](@ref opts)*

`block` is Risk's struct-like aggregate type:

```risk
block Point {
    x: i32;
    y: i32 = 0;   // fields can have default values
}

```

## Three Ways to Build a Block Literal

```risk
a: Point = { 3, 4 };          // Positional - one value per field, in order
b: Point = { x = 3, y = 4 };  // Named - order does not matter
c: Point = { x = 3 };         // Named + relies on y's default value (0)
d: Point = { 3, y = 9 };      // Mixed positional + named in one literal

```

Positional literals must supply exactly one value per field. Named literals may omit fields that have a default value; omitting a field without a default results in a compile-time error. A third construction path involves declaring a variable with no initializer at all and assigning fields individually:

```risk
p3: Person;
p3.age = 30;
p3.name = "Charlie";

```

This uninitialized declaration still executes the automatic `init` field (detailed below) exactly once at the point of declaration.

## Field Access

```risk
p: Point = { 3, 4 };
p.x = 10;
sum: i32 = p.x + p.y;

```

## Self-Passing Methods

A field whose type is `func(this) -> ReturnType` (or `func(this, ExtraArgs...) -> ReturnType`) defines a **method**. The identifier `this` is a reserved marker that the compiler resolves to a reference to the block instance itself, passed implicitly:

```risk
block Person {
    name: string;
    age: i32;
    repr: func(this) -> string = personRepr;
}

func personRepr(self: Person) -> string {
    return self.name;
}

guy: Person = { name = "Sam", age = 30 };
label: string = guy.repr();     // guy is passed implicitly

```

Two special field names carry specific conventions:

* **`repr`**: When present, it is called explicitly via `.repr()`. It is also utilized by `Array<T>.repr()` to format elements when `T` is a block type defining a `repr` field, rather than printing a raw memory address (see [§5](@ref compound)).
* **`init`**: When present, it is invoked **automatically, exactly once**, immediately after a variable of that block type finishes construction, functioning similarly to a constructor. Explicit manual calls to `.init()` for this automatic invocation are unnecessary (though explicit calls can still be made on top of it).

```risk
block Counter {
    value: i32 = 0;
    init: func(this) -> void = resetCounter;
}

func resetCounter(self: Counter) -> void {
    self.value = 0;      // 'this' is passed by reference, so this write persists
}

```

## `this` is Passed **by Reference**

A critical semantic detail of self-passing methods is that the reserved `this` receiver of any method field—whether `init`, `repr`, or a custom field—is passed **by reference**. Field modifications made through it are visible to the caller:

```risk
func setup(p: Counter) -> void {
    p.n = 42;
}

block Counter {
    n: i32;
    init: func(this) -> void = setup;
}

func main() -> i32 {
    a: Counter;
    return a.n;   // 42, not 0 - setup's write through 'p' (== 'this') persisted
}

```

This behavior applies to any self-passing method field across repeated explicit calls:

```risk
func bump(p: Counter) -> void {
    p.n = p.n + 1;
}

block Counter {
    n: i32;
    bumper: func(this) -> void = bump;
}

func main() -> i32 {
    a: Counter;
    a.n = 10;
    a.bumper();   // n: 10 -> 11
    a.bumper();   // n: 11 -> 12
    return a.n;   // 12
}

```

It also applies after block-literal initialization:

```risk
func setup(p: Widget) -> void {
    p.size = p.size * 10;
}

block Widget {
    label: string;
    size: i32;
    init: func(this) -> void = setup;
}

func main() -> i32 {
    a: Widget = { label = "a", size = 3 };
    return a.size;   // 30, not 3
}

```

Each constructed variable receives a reference to itself without cross-instance aliasing between separately constructed variables of the same block type:

```risk
seed: i32 = 0;

func setup(p: Counter) -> void {
    seed = seed + 1;
    p.n = seed * 100;
}

block Counter {
    n: i32;
    init: func(this) -> void = setup;
}

func main() -> i32 {
    a: Counter;   // a.n = 100
    b: Counter;   // b.n = 200
    c: Counter;   // c.n = 300
    return (a.n / 100) + (b.n / 100) + (c.n / 100); // 1 + 2 + 3 = 6
}

```

**Crucially, this by-reference behavior applies only to the reserved `this` receiver**, not to ordinary functions taking a block parameter. An ordinary function with a plain block-typed parameter—one not bound to a `func(this) -> ...` field—retains standard by-value semantics:

```risk
func mutate(p: Counter) -> void {
    p.n = 99;
}

block Counter {
    n: i32;
}

func main() -> i32 {
    a: Counter;
    a.n = 1;
    mutate(a);
    return a.n; // Remains 1: mutate() received its own copy, not a reference
}

```

## `init` Signature Requirements

A field functions as an auto-invoked constructor only if its signature matches `func(this) -> void` exactly, mirroring how `repr` requires `func(this) -> string`. A field named `init` with any other signature is treated as a regular field and is not auto-invoked:

```risk
count: i32 = 0;

func bump_by(p: Counter, amount: i32) -> void {
    count = count + amount;
}

block Counter {
    n: i32;
    init: func(this, i32) -> void = bump_by;   // Incorrect signature: takes 2 parameters
}

func main() -> i32 {
    a: Counter;
    return count;   // 0 -- bump_by was never automatically invoked
}

```

## Explicitly Calling `init`

Because `init` is a standard self-passing method field, it can be called explicitly on demand in addition to the automatic initialization call:

```risk
count: i32 = 0;

func mark_init(p: Person) -> void {
    count = count + 1;
}

block Person {
    name: string;
    init: func(this) -> void = mark_init;
}

func main() -> i32 {
    a: Person;   // Automatic call #1
    b: Person;   // Automatic call #2
    a.init();    // Explicit call #3
    return count; // 3
}

```

## Blocks Without an `init` Field

A plain block lacking `init` or `repr` fields behaves with standard by-value field access and no hidden invocation steps:

```risk
block Plain {
    x: i32;
    y: i32;
}

func main() -> i32 {
    a: Plain;
    a.x = 5;
    a.y = 7;
    return a.x + a.y;   // 12
}

```

## Examples

The following example combines `init`, `repr`, the by-reference `this` receiver, and all three block-literal construction forms:

```risk
block Counter {
    label: string;
    value: i32 = 0;
    init: func(this) -> void = resetCounter;
    repr: func(this) -> string = counterRepr;
}

func resetCounter(self: Counter) -> void {
    self.value = 0;   // 'this' is by-reference, so this write persists
}

func counterRepr(self: Counter) -> string {
    return self.label;
}

func main() -> i32 {
    a: Counter = { label = "a" };   // Named literal, value defaults to 0
    b: Counter = { "b", 5 };        // Positional literal

    c: Counter;                     // Uninitialized path
    c.label = "c";
    c.value = 30;

    b.value = b.value + 1;
    ioPrintln(a.repr(), " ", b.repr(), " ", c.repr());   // a b c
    ioPrintln(b.value);                                  // 6

    return 0;
}

```

**Mixed positional and named literal usage**, alongside an incorrect `init` signature:

```risk
count: i32 = 0;

func bumpBy(p: Widget, amount: i32) -> void {
    count = count + amount;
}

block Widget {
    label: string;
    size: i32 = 1;
    init: func(this, i32) -> void = bumpBy;   // Incorrect signature
}

func main() -> i32 {
    w: Widget = { "gadget", size = 9 };   // Mixed positional + named literal
    ioPrintln(w.label, " ", w.size);      // gadget 9
    ioPrintln("count = ", count);         -- 0 -- bumpBy was not auto-invoked
    return 0;
}

```

---

*[← Functions & Closures](@ref functions) | [Next: Opts →](@ref opts)*
