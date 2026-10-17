# 9. Opts (Enum-like Constants) {#opts}

*[← Blocks](@ref blocks) | [Next: Generics →](@ref lang-generics)*

`opts` is Risk's enum-like construct - a named set of integer constants:

```risk
opts Color {
    Red,        // 0
    Green,      // 1
    Blue = 10,  // explicit value
    Purple      // 11 -- auto-increments from the previous value
}

c: Color = Color.Green;

switch (c) {
    case Color.Red:
        ioPrintln("red");
        break;
    case Color.Green:
        ioPrintln("green");
        break;
    default:
        ioPrintln("other");
}
```

Field values must be compile-time constant expressions (integer literals
and the usual arithmetic/bitwise operators over them); an unspecified
value auto-increments from the field before it, C-enum style.

## `opts` as an ordinary type - and what that implies

An `opts` type can be used as an ordinary parameter, variable, or return
type, and a field reference like `Color.Blue` is itself a value of type
`Color` (not `i32`) - you can pass it anywhere a parameter is declared
with that `opts` type, dot access and all:

```risk
opts Codes { A, B, C }   // A=0, B=1, C=2

func classify(a: Codes) -> i32 {
    if (a == Codes.B) {   // comparing two Codes values yields bool, correctly
        return 1;
    }
    return 0;
}

matched: i32 = classify(Codes.B);    // 1
unmatched: i32 = classify(Codes.A);  // 0
```

Two behaviors are worth calling out explicitly because both were
historically easy to get wrong in a compiler that treats `opts` as
"basically an int":

1. **Comparing two `opts` values with `==` yields `bool`**, as it
   should - not the `opts` type itself (which would break using the
   comparison directly as an `if` condition, since `if` expects `bool`).
2. **`Color.Blue`'s type is `Color`**, not `i32` - so passing it to a
   parameter declared as the `opts` type directly type-checks with no
   cast required, but passing it somewhere an `i32` is expected (e.g.
   into an `extern` function) does require an explicit
   `cast<i32>(...)` - see [§15](@ref c-interop).

## Casting to/from an `opts` type is runtime-clamped

`cast<Codes>(someInt)` is allowed for any integer value - including one
outside the `opts` type's declared range - and the *result* is clamped
into the type's `[min, max]` range at runtime, so an out-of-range
integer can never produce an invalid enum value:

```risk
opts Codes { A, B, C }  // range is [0, 2]

too_low: i32 = -50;
too_high: i32 = 999;
in_range: i32 = 1;

c_low: Codes  = cast<Codes>(too_low);   // clamps to A (0)
c_high: Codes = cast<Codes>(too_high);  // clamps to C (2)
c_mid: Codes  = cast<Codes>(in_range);  // stays B (1)

back_low: i32  = cast<i32>(c_low);   // 0
back_high: i32 = cast<i32>(c_high);  // 2
back_mid: i32  = cast<i32>(c_mid);   // 1
```

This clamping is a genuine runtime operation, not merely a compile-time
range check - it protects any later `switch`/comparison logic over that
`opts` value from ever seeing a value outside its declared member set,
even if the integer that produced it came from somewhere untrusted (a
file, a C library, user input).

**One sharp edge**: the `[min, max]` range is the span between your
`opts` type's smallest and largest *declared* value, not the set of
declared values itself. If your members have gaps, a clamped value can
land in a gap and match no `case` at all:

```risk
opts Sparse { Low = 0, High = 100 }  // range is [0, 100]; only 0 and 100 named

mid: Sparse = cast<Sparse>(50);  // clamps to 50 -- in range, but not Low or High
```

Always give a `switch` over a sparse `opts` type a `default:` arm - see
[§12](@ref casting) for the full clamping mechanics and more examples.

## Examples

Declaration, `switch`, comparison, and the runtime-clamped cast:

```risk
opts Color {
    Red,
    Green,
    Blue = 10,
    Purple   // 11
}

func describe(c: Color) -> string {
    switch (c) {
        case Color.Red:
            return "red";
        case Color.Green:
            return "green";
        default:
            return "other";
    }
}

func main() -> i32 {
    c: Color = Color.Green;
    ioPrintln(describe(c));                 // green
    ioPrintln(c == Color.Green);            // true -- opts == opts yields bool

    too_high: i32 = 999;
    clamped: Color = cast<Color>(too_high); // clamps to Purple (11), the max
    ioPrintln(cast<i32>(clamped));          // 11

    return 0;
}
```

**The sparse-opts gap**, where a clamped value lands in-range but
matches no declared member:

```risk
opts Sparse { Low = 0, High = 100 }   // range is [0, 100]; only 0 and 100 named

func main() -> i32 {
    mid: Sparse = cast<Sparse>(50);   // clamps to 50 -- in range, but matches
                                       // neither Sparse.Low nor Sparse.High
    switch (mid) {
        case Sparse.Low:
            ioPrintln("low");
            break;
        case Sparse.High:
            ioPrintln("high");
            break;
        default:
            ioPrintln("neither -- always give a sparse opts switch a default");
    }
    return 0;
}
```

---

*[← Blocks](@ref blocks) | [Next: Generics →](@ref lang-generics)*
