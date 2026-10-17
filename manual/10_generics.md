# 10. Generics {#lang-generics}

*[← Opts](@ref opts) | [Next: Operators →](@ref operators)*

Generic type parameters are constrained by named **sets**, declared with
`generic`:

```risk
generic Numeric = [i32, i64, f32, f64];
```

Both functions and `block` types can be generic, but they differ in one
important way: **function generic arguments are always inferred; block
generic arguments are always written out explicitly.** The rest of this
chapter works through each mechanism in detail, plus every documented
failure mode, with the diagnostic text you'll actually see.

## Function generics - inferred at the call site

```risk
func biggest<T: Numeric>(a: T, b: T) -> T {
    if (a > b) { return a; }
    return b;
}

x: i32 = biggest(3, 7);        // T = i32, inferred from the arguments
y: f64 = biggest(2.5, 1.1);    // T = f64
```

You never write `biggest<i32>(3, 7)` yourself for a function call - the
concrete type(s) are inferred purely from the argument types and checked
against the constraint set. There is **no explicit type-argument call
syntax** for functions at all - inference is the *only* mechanism, which
has real consequences explored below (see "Unbound type parameters").

Each distinct combination of concrete types is compiled once, the first
time it's used, and every later call with the same combination reuses
that same compiled instance - see "Instance deduplication" below.

### Basic instantiation at two different concrete types

```risk
generic _Num = [i32, f32];

func add<T: _Num>(a: T, b: T) -> T {
    return a + b;
}

x: i32 = 3;
y: i32 = 4;
p: f32 = 1.5;
q: f32 = 2.5;

ioPrintln(add(x, y)); // instantiates add$i32, prints 7
ioPrintln(add(p, q)); // instantiates add$f32, prints 4
```

### Multiple independent generic parameters

A function can declare more than one generic parameter; each is bound
independently and each must satisfy its own constraint set - even if two
parameters happen to share the same constraint set, they can still be
bound to different concrete types in the same call:

```risk
generic _Any = [i32, f32, bool, string];

func pick<T: _Any, U: _Any>(useFirst: bool, a: T, b: U) -> T {
    return a;
}

a: i32 = 10;
b: string = "unused";
ioPrintln(pick(true, a, b));   // T=i32, U=string in the same call - 10
```

### Nested and recursive generic calls

A generic function can call another generic function from inside its
own body (the inner call is resolved, and its own concrete instance
queued for compilation, while the outer template's freshly-instantiated
body is being checked), and a generic function can call **itself**
recursively - the first instantiation is registered before its body is
checked, which is exactly what lets the recursive call inside its own
body resolve to that same instance instead of looping forever during
type-checking:

```risk
generic _Num = [i32, f32];

func square<T: _Num>(x: T) -> T {
    return x * x;
}

func sumSquares<T: _Num>(a: T, b: T) -> T {
    return square(a) + square(b);   // nested generic call
}

func fact<T: _Num>(n: T) -> T {
    if (n <= 1) { return 1; }
    return n * fact(n - 1);          // generic self-recursion
}

a: i32 = 3;
b: i32 = 4;
ioPrintln(sumSquares(a, b));   // 25

f: i32 = 5;
ioPrintln(fact(f));            // 120
```

### Instance deduplication

Calling `add<i32>` from five different call sites must produce exactly
**one** compiled C function (`add$i32`), reused every time - not five
colliding definitions. If instance caching were broken, this would fail
at the final `gcc` link/compile step with a symbol redefinition error,
so a clean build of code like this is itself the test:

```risk
generic _Num = [i32, f32];

func add<T: _Num>(a: T, b: T) -> T {
    return a + b;
}

i: i32 = 0;
while (i < 5) {
    a: i32 = 1 + i;
    b: i32 = 2 + i;
    ioPrintln(add(a, b));   // same add$i32 instance, called 5 times
    i = i + 1;
}
```

### Structural matching of compound types in a generic set

A generic set's members don't have to be bare type names - they can be
compound types like `Array<i32>`. The concrete argument type and the
pattern are walked structurally (same base name, same arity, each type
argument matched recursively), so `Array<i32>` satisfies a set member
written as `Array<i32>`, and the same structural walk lets `T` bind
correctly when the pattern is `Array<T>` and the argument is
`Array<i32>`:

```risk
generic _NumArr = [Array<i32>, Array<f32>];

func total<T: _NumArr>(a: T) -> i32 {
    return length(a);
}

xs: Array<i32> = [1, 2, 3];
ys: Array<f32> = [1.0, 2.0, 3.0, 4.0, 5.0, 6.0];
ioPrintln(total(xs));  // 3
ioPrintln(total(ys));  // 6
```

### Transitive (nested) generic set membership

A generic set's member list can name *another* generic set instead of a
concrete type, and membership is checked transitively:

```risk
generic _Int = [i32, i64];
generic _Num = [_Int, f32];   // "member of _Int, or is f32"

func inc<T: _Num>(x: T) -> T {
    return x + 1;
}

a: i32 = 5;
b: i64 = 10;
ioPrintln(inc(a));  // 6
ioPrintln(inc(b));  // 11
```

## Compile-time errors from misused function generics

Every case below is a genuine compile failure, with the actual
diagnostic substring the compiler is expected to produce.

**Constraint violation** - the argument type unifies fine, but isn't a
member of the declared set:

```risk
generic _Num = [i32, f32];
func add<T: _Num>(a: T, b: T) -> T { return a + b; }

s: string = "x";
t: string = "y";
add(s, t);
// error contains: is not a member of generic set '_Num'
```

**Conflicting types for the same parameter** - `T` gets bound from the
first argument, then the second argument's type disagrees:

```risk
generic _Num = [i32, f32];
func add<T: _Num>(a: T, b: T) -> T { return a + b; }

x: i32 = 1;
y: f32 = 2.0;
add(x, y);
// error contains: conflicting types inferred for generic parameter 'T'
```

**Argument count mismatch** - checked before any inference is even
attempted:

```risk
generic _Num = [i32, f32];
func add<T: _Num>(a: T, b: T) -> T { return a + b; }

x: i32 = 1;
add(x);
// error contains: argument count mismatch in call to generic function add
```

**Unbound type parameter** - if a generic parameter only appears in the
*return* type and never in any parameter type, nothing during argument
unification can ever bind it. Because there's no explicit
`f<T>(...)` call syntax to work around this (see above), this case can
never be resolved and always fails:

```risk
generic _Num = [i32, f32];
func makeZero<T: _Num>() -> T { return 0; }

makeZero();
// error contains: could not infer generic parameter 'T' in call to makeZero()
```

## Block generics - explicit at the use site

Unlike functions, a generic `block`'s type/value arguments are always
written out explicitly where you use the type - there's no inference:

```risk
generic Order = [Row, Column];   // e.g. two 'opts'-typed tags, or similar

block Matrix<T: Numeric, const Rows: i32, const Cols: i32> {
    data: Array<T, Rows>;
}

m: Matrix<f32, 4, 4> = { ... };
```

A generic `block` can mix all three kinds of generic parameter in one
declaration: a constrained **type** parameter, a **value parameter
typed as an `opts`**, and one or more **compile-time integer** value
parameters:

```risk
opts Order {
  RowMajor,
  ColMajor,
}

generic _Float = [f32, f64];

block Matrix<T: _Float, H: Order, C: i32, R: i32> {
  data: ptr<T>;
  cols: i32 = C;
  rows: i32 = R;
  order: Order = H;
}

a: Matrix<f32, Order.RowMajor, 4, 4>;
```

### Value generic parameters in computed compile-time expressions

A value generic parameter can be used bare (`Array<T, N>`) *and* inside
a computed compile-time expression (`Array<T, N + 1>`) within the same
block template - the compiler evaluates the symbolic size expression at
each instantiation:

```risk
generic _Float = [f32, f64];

block Buffer<T: _Float, const N: i32> {
  data: Array<T, N>;
  scratch: Array<T, N + 1>;
  size: i32 = N;
}

b: Buffer<f32, 4>;   // data: Array<f32,4>, scratch: Array<f32,5>
```

The same symbolic-size mechanism works for value-generic **functions**,
including a second array parameter whose size is a computed expression
built from a value parameter already bound by an earlier one:

```risk
func padded<const N: i32>(arr: Array<i32, N>) -> i32 {
  buf: Array<i32, N + 1> = [];   // local with a computed size, not just bare N
  i: i32 = 0;
  for (i = 0; i < N; i = i + 1) { buf[i] = arr[i]; }
  buf[N] = 999;
  return buf[N];
}

func checkSizes<const N: i32>(a: Array<i32, N>, b: Array<i32, N * 2>) -> i32 {
  return N;   // b's size (N*2) is derived from N, bound by a's plain Array<i32,N>
}
```

A function can also combine a constrained type parameter with a value
parameter, and use both to type an `Array` parameter and return type:

```risk
generic _Numeric = [i32, f32];

func sumArr<T: _Numeric, const N: i32>(arr: Array<T, N>) -> T {
  total: T = 0;
  i: i32 = 0;
  for (i = 0; i < N; i = i + 1) { total = total + arr[i]; }
  return total;
}

arr1: Array<i32, 4> = [1, 2, 3, 4];
s: i32 = sumArr(arr1);              // 10

arr2: Array<f32, 3> = [1.5, 2.5, 3.0];
s2: f32 = sumArr(arr2);             // 7.0
```

### Instance deduplication for generic blocks

Instantiating the same generic block template with different argument
combinations produces distinct concrete types that coexist; instantiating
it *again* with an already-seen combination reuses the existing instance
rather than minting a second, incompatible struct. You can observe this
directly: assigning one instance to another with plain `=` only
type-checks if they share the *exact* same underlying type -

```risk
generic _Float = [f32, f64];

block Vec<T: _Float, const N: i32> {
  data: Array<T, N>;
  len: i32 = N;
}

a: Vec<f32, 3>;
b: Vec<f32, 3>;   // same combination as 'a' -- reuses the same instance
c: Vec<f64, 3>;   // different T -- a distinct instance
d: Vec<f32, 4>;   // different N -- also a distinct instance

b = a;            // only type-checks because a and b share the exact same type
```

### Field usage after instantiation

Fields of a monomorphized generic block are ordinary fields once
instantiated - read, written, and used in arithmetic exactly like a
non-generic block's fields, with `N` baked in as a plain `i32`:

```risk
generic _Float = [f32, f64];

block Grid<T: _Float, const N: i32> {
  width: i32 = N;
  height: i32 = N;
  cell: T;
}

g: Grid<f32, 5>;
g.cell = 1.0;
total: i32 = g.width * g.height;   // 25
```

## Compile-time errors from misused block generics

**Wrong generic argument count:**

```risk
block Matrix<T: _Float, H: Order, C: i32, R: i32> { ... }

a: Matrix<f32, Order.RowMajor, 4>;   // missing R
// error like: block 'Matrix' expects 4 generic argument(s), got 3
```

**Type argument not a member of the constraint set:**

```risk
block Buffer<T: _Float, const N: i32> { data: Array<T, N>; }

b: Buffer<i32, 4>;   // i32 is not in _Float ([f32, f64])
// error like: type 'i32' is not a member of generic set '_Float' (parameter 'T' ...)
```

**Value argument out of range for its declared integer type:**

```risk
block Buffer<T: _Float, const N: u8> { data: Array<T, N>; }

b: Buffer<f32, 999>;   // 999 doesn't fit in u8 (max 255)
// error like: value 999 does not fit in 'u8' for generic parameter 'N' ...
```

## Examples

A function generic and a block generic, side by side:

```risk
generic _Num = [i32, f32];

func biggest<T: _Num>(a: T, b: T) -> T {
    if (a > b) { return a; }
    return b;
}

generic _Float = [f32, f64];

block Buffer<T: _Float, const N: i32> {
    data: Array<T, N>;
    size: i32 = N;
}

func main() -> i32 {
    ioPrintln(biggest(3, 7));        // T=i32, inferred -- 7

    p: f32 = 2.5;
    q: f32 = 1.1;
    ioPrintln(biggest(p, q));        // T=f32, inferred -- 2.5

    b: Buffer<f32, 4>;               // T and N always explicit for blocks
    b.data[0] = 1.5;
    ioPrintln("size = ", b.size, ", data[0] = ", b.data[0]);

    return 0;
}
```

**Transitive generic set membership**, and a **value generic parameter
in a computed expression**:

```risk
generic _Int = [i32, i64];
generic _Num = [_Int, f32];   // "member of _Int, or is f32"

func inc<T: _Num>(x: T) -> T {
    return x + 1;
}

generic _Float = [f32, f64];

block Ring<T: _Float, const N: i32> {
    data: Array<T, N>;
    scratch: Array<T, N + 1>;   // size computed from N, not just bare N
}

func main() -> i32 {
    a: i32 = 5;
    ioPrintln(inc(a));   // 6 -- i32 matches _Num transitively via _Int

    r: Ring<f32, 4>;     // data: Array<f32,4>, scratch: Array<f32,5>
    r.scratch[4] = 1.0;
    ioPrintln(r.scratch[4]);

    return 0;
}
```

---

*[← Opts](@ref opts) | [Next: Operators →](@ref operators)*
