# 7. Functions, Function Pointers, Closures, Type Aliases {#functions}

*[← Control Flow](@ref control-flow) | [Next: Blocks →](@ref blocks)*

## Ordinary Functions

```risk
func add(a: i32, b: i32) -> i32 {
    return a + b;
}

func greet(name: string = "World") -> void {
    ioPrintln("Hello, ", name);
}

```

Parameters may specify default values using `= expression`. A parameter named `_` signifies an argument that is accepted but ignored. A function declared without a body, ending with a semicolon instead of `{ ... }`, serves as a forward declaration. This is useful when working with `extern` interop (see [§15](@ref c-interop)) or when you need a signature visible prior to its definition.

### Default Parameters in Depth

Any suffix of a parameter list can include default values, and each default parameter can be omitted independently at the call site as long as arguments are not skipped in the middle:

```risk
func add(a: i32 = 10, b: i32 = 20) -> i32 {
    return a + b;
}

r1: i32 = add();       // 30  (a=10, b=20)
r2: i32 = add(5);      // 25  (a=5,  b=20)
r3: i32 = add(5, 3);   // 8   (a=5,  b=3)

```

The compiler calculates a minimum argument floor by examining trailing defaulted parameters and accepts any call with an argument count falling within the range `[min_args, params.size()]`. This behavior applies identically to generic functions, meaning a generic function with a defaulted trailing parameter can also omit it at the call site:

```risk
generic _Num = [i32, f32];

func addWithDefault<T: _Num>(a: T, b: T = 10) -> T {
    return a + b;
}

x: i32 = 5;
ioPrintln(addWithDefault(x));       // Omitted b uses default value 10 -> 15
ioPrintln(addWithDefault(x, 100));  // Explicit b -> 105

```

## Function Pointers

A function type is expressed as `func(ParamTypes...) -> ReturnType`, and an ordinary function name acts as a value of that type:

```risk
op: func(i32, i32) -> i32 = add;
result: i32 = op(3, 4);

```

> This works as a local variable inside a function body. As a global (module-scope) variable, the identical declaration currently fails to compile with an `unknown identifier: add` error, even when `add` is declared earlier in the same file. This occurs because the type checker's handling of global initializers does not resolve top-level function names the same way local variable initializers do. If a function pointer value is required at module scope, initialize it from inside a function instead (such as assigning it in `main` before use or wrapping it behind a getter function).

## Lambdas and Closures

An anonymous function literal begins with a parenthesized capture list, followed by an optional bracketed parameter list and a function body enclosed in `{ ... }`:

```risk
square: func(i32) -> i32 = (...) [x: i32] { return x * x; };

```

### Captures are Explicit and Mandatory

The capture list is explicit; `(...)` does not mean capturing everything referenced in the body. It signifies zero captures, and the body of a lambda marked with `(...)` cannot reference any variable from the enclosing scope. To allow a lambda to access an outer variable, that variable must be explicitly named in the parenthesized capture list:

```risk
threshold: i32 = 10;

// Incorrect: The '(...)' marker forbids referencing outer variables.
// This triggers a compile-time error: "'threshold' is used in this lambda
// but not captured; add it to the capture list, e.g. '(threshold)['"
// isOver: func(i32) -> bool = (...) [x: i32] { return x > threshold; };

// Correct: List each outer name required by the body explicitly:
isOver: func(i32) -> bool = (threshold) [x: i32] {
    return x > threshold;
};

// Multiple captures are comma-separated:
lo: i32 = 0; hi: i32 = 100;
inRange: func(i32) -> bool = (lo, hi) [x: i32] {
    return x >= lo && x <= hi;
};

```

Several consequences follow from captures being an explicit list rather than an inferred set:

* Listing a name that does not exist in the enclosing scope or duplicating a name in the capture list results in a compile-time error (`cannot capture undefined variable '...'` or `'...' is captured more than once`), caught before code generation.
* `(...)` and `()` are equivalent and both denote zero captures. `(...)` is the conventional notation.
* Every captured variable is copied **by value** into the lambda storage when the lambda literal is evaluated. Primitive captures like `i32` or `bool` act as frozen snapshots; later modifications to the outer variable do not reach the lambda, and updates made by the lambda do not alter the outer variable.

### Capturing a `ptr<T>` Provides Shared Mutable State

Because captures copy by value, capturing a `ptr<T>` copies the pointer itself, which continues to reference the underlying memory location. This provides a mechanism for closures that require shared or mutable state:

```risk
counter: i32 = 0;
pc: ptr<i32> = &counter;

increment: func() -> void = (pc) [] {
    *pc = *pc + 1;
};

increment();
increment();
ioPrintln(counter);   // 2 -- both calls mutated the underlying i32

```

### Immediately-Invoked Lambdas Bypass the Heap

Calling a lambda literal directly without assigning it to a variable is recognized specially by the compiler and translated into a regular function call with captured values passed as standard arguments, avoiding any closure or environment allocation:

```risk
result: i32 = (n) [x: i32] { return x * n; }(21);

```

This represents an efficient approach for one-off scoped computations.

### Stored Closures and Memory Management

A lambda that is not immediately invoked compiles to a heap allocation via the C runtime allocator, managing the captured environment and closure handle. These allocations are outside the `alloc` and `dealloc` leak-tracking framework described in [§14](@ref arena-rc), and the generated code does not automatically free them:

```risk
func makeAdder(n: i32) -> func(i32) -> i32 {
    return (n) [x: i32] { return x + n; };   // Heap-allocates on every call
}

// Calling this in a loop leaks one closure environment per iteration.
i: i32 = 0;
while (i < 1000000) {
    adder: func(i32) -> i32 = makeAdder(i);
    i = i + 1;
}

```

This pattern is generally safe for closures instantiated once at startup, but should be managed carefully when minting closures inside tight loops.

### Lambdas as Ordinary Values

A lambda literal is an expression of type `func(...) -> T` and integrates with constructs accepting that type. It can be passed directly into function calls, stored in arrays, returned from functions, or assigned as a method implementation on a `block` field (refer to [§8](@ref blocks) for block method syntax).

```risk
func applyTwice(f: func(i32) -> i32, x: i32) -> i32 {
    return f(f(x));
}

nine: i32 = applyTwice((...) [x: i32] { return x + 3; }, 3);  // 3+3+3 -> 9

```

## Type Aliases

```risk
typealias Predicate = func(i32) -> bool;
typealias UserId = i32;

isEven: Predicate = (...) [x: i32] { return x % 2 == 0; };

```

A `typealias` serves as a naming convenience rather than creating a distinct nominal type. It assigns an alternate name to an existing type. For example, `typealias Meters = f64;` does not enforce type-safe units; `Meters` and `f64` remain fully interchangeable.

## Examples

The following program demonstrates default parameters, function pointers, capturing closures, and immediately-invoked lambdas:

```risk
func add(a: i32 = 10, b: i32 = 20) -> i32 {
    return a + b;
}

func makeAdder(n: i32) -> func(i32) -> i32 {
    return (n) [x: i32] { return x + n; };
}

typealias IntPredicate = func(i32) -> bool;

func main() -> i32 {
    ioPrintln(add());          // 30
    ioPrintln(add(5));         // 25
    ioPrintln(add(5, 3));      // 8

    op: func(i32, i32) -> i32 = add;
    ioPrintln(op(1, 1));       // 2

    addFive: func(i32) -> i32 = makeAdder(5);
    ioPrintln(addFive(10));    // 15

    isEven: IntPredicate = (...) [x: i32] { return x % 2 == 0; };
    ioPrintln(isEven(4), " ", isEven(7));   // true false

    // Immediately-invoked lambda -- no heap allocation
    threshold: i32 = 6;
    nine: i32 = (threshold) [x: i32] { return x + threshold; }(3);
    ioPrintln(nine);           // 9

    return 0;
}

```

**Capturing a `ptr<T>**` to maintain shared, mutable state across multiple calls:

```risk
func main() -> i32 {
    counter: i32 = 0;
    pc: ptr<i32> = &counter;

    increment: func() -> void = (pc) [] {
        *pc = *pc + 1;
    };

    increment();
    increment();
    ioPrintln(counter);   // 2 -- both calls mutated the underlying i32
    return 0;
}

```

**Using a parameter named `_**` to accept and ignore an argument:

```risk
func onEvent(name: string, _: i32) -> void {
    ioPrintln("event: ", name);   // the i32 payload is ignored
}

func main() -> i32 {
    onEvent("click", 42);
    return 0;
}

```

---

*[← Control Flow](@ref control-flow) | [Next: Blocks →](@ref blocks)*
