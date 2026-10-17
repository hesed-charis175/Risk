# 12. Casting {#casting}

*[← Operators](@ref operators) | [Next: Imports →](@ref imports)*

Risk provides two spelling variations for casting, both implemented as prefix-style generic calls rather than an infix cast operator:

```risk
whole: i32 = cast<i32>(3.9);                        // Safe cast for structured conversions
raw: ptr<Header> = unsafe_cast<ptr<Header>>(buffer); // Programmer-acknowledged reinterpretation

```

* **`cast<T>(expr)`**: Performs ordinary, structured conversions such as numeric changes, safe widenings, `char` to `i32` conversions, and integer-to-`opts` conversions. Casting into an `opts` type automatically clamps the value to the declared range at runtime.
* **`unsafe_cast<T>(expr)`**: Provides an explicit, programmer-acknowledged bypass for pointer reinterpretation, C-interop conversions, or operations not permitted by default. The compiler does not verify that the reinterpretation is valid.

## Common Cast Targets

Beyond basic numeric widening and narrowing, casting is used for the following operations:

* Converting a `char` to its ordinal `i32` value and back using `cast<i32>(ch)` and `cast<char>(97 + 3)` (see [§3](@ref primitives)).
* Moving an `opts` value to or from its underlying integer representation using `cast<i32>(...)` or clamped `cast<Codes>(...)` (see [§9](@ref opts)).
* Reinterpreting a raw `ptr<void>` from an allocator or C-interop call as a concrete pointer type via `unsafe_cast<ptr<f32>>(rawBuffer)`, or casting a typed buffer to `ptr<void>` when passing it to a function accepting generic pointers.

## `cast<T>` and `unsafe_cast<T>` Generate Identical C Code

Unlike languages where safe and unsafe casts translate to distinct runtime operations, Risk compiles both forms identically. For standard conversions (excluding numeric-to-`opts` clamping and `char`-to-`string` operations), both spellings emit the exact same C expression: `((TargetCType)(expr))`.

The `unsafe_cast` keyword does not alter runtime behavior; it instructs the type checker to bypass restrictions while emitting a warning. The validity of the underlying data reinterpretation (such as reading an `i32` pointer as an `f32`) remains the programmer's responsibility.

```risk
p: ptr<i32> = &someI32;

a: ptr<f32> = unsafe_cast<ptr<f32>>(p);  // Compiles successfully
b: ptr<f32> = cast<ptr<f32>>(p);         // Also compiles successfully

```

### `cast<T>` Permissiveness for Pointers and Containers

The type checker special-cases any conversion where either type begins with `ptr<`, `opt<`, `Array<`, or `Hashmap<`, permitting them via plain `cast` without requiring `unsafe_cast`. Consequently, using `unsafe_cast` for pointer reinterpretation serves primarily as a documentation and review practice rather than an enforced compiler barrier:

```risk
i: ptr<i32> = &someI32;

// Both expressions type-check with plain `cast` without requiring `unsafe_cast`:
f: ptr<f32>        = cast<ptr<f32>>(i);
arr: Array<string>  = cast<Array<string>>(someArrayOfI32);

```

**Recommendation**: Use `unsafe_cast` for pointer and container reinterpretation to clearly document intent and trigger helpful review warnings, even when `cast` would accept the syntax.

### `char` and `string` Conversions

* `cast<char>(someString)` extracts **only the first byte** of the string and discards the remainder without validating whether the string contains a single character:
```risk
s: string = "hello";
c: char = cast<char>(s);   // c becomes 'h' while "ello" is dropped

```


* `cast<string>(someChar)` heap-allocates a new 2-byte buffer containing the character and a null terminator using the standard `alloc()` allocator. Because this heap allocation is managed via pointers, repeated casting in tight loops can leak memory if not properly released.

### Numeric and `opts` Clamping

As covered in [§9](@ref opts), casting an out-of-range integer into an `opts` type clamps it into the `[min, max]` span of the type's **declared field values**, rather than restricting it strictly to named members. If gaps exist between declared values, a clamped integer can land in a gap and match no named member:

```risk
opts Sparse {
    Low = 0,
    High = 100   // Range is [0, 100]; only 0 and 100 are explicitly named
}

mid: Sparse = cast<Sparse>(50);  // Clamps to 50 -- valid range, but matches neither Low nor High

```

Always include a `default:` arm in `switch` statements over sparse `opts` types to handle clamped values safely.

## Quick Reference: Spelling Requirements

| Conversion | `cast<T>` | `unsafe_cast<T>` |
| --- | --- | --- |
| Numeric to numeric (any width/direction) | Allowed | Allowed |
| `bool` to numeric | Allowed | Allowed |
| `char` to/from `i32`/`i64`/`u8`/`u32` | Allowed | Allowed |
| `char` to/from `string` | Allowed | Allowed |
| Numeric to/from `opts` (clamped) | Allowed | Allowed (clamping still applies) |
| `const<T>` to/from `T` | Allowed | Allowed |
| `ptr`, `opt`, `Array`, or `Hashmap` reinterpretation | Allowed | Allowed |
| Unrelated `block` types | Rejected (`unsupported cast`) | Allowed |
| Other unlisted type pairs | Rejected | Allowed |

## Examples

The following program demonstrates `cast` versus `unsafe_cast`, along with runtime `opts` clamping:

```risk
opts Codes { A, B, C }   // Range [0, 2]

func main() -> i32 {
    whole: i32 = cast<i32>(3.9);            // Structured numeric conversion -> 3
    ioPrintln(whole);

    too_high: i32 = 999;
    c: Codes = cast<Codes>(too_high);       // Clamps to C (2)
    ioPrintln(cast<i32>(c));

    s: string = "hello";
    ch: char = cast<char>(s);               // Extracts first byte -> 'h'
    ioPrintln(ch);

    i: ptr<i32> = &whole;
    f: ptr<f32> = unsafe_cast<ptr<f32>>(i); // Explicit reinterpretation
    ioPrintln(*f);                          // Reads raw bit pattern as f32

    return 0;
}

```

**`char` to `string` conversion and pointer reinterpretation:**

```risk
func main() -> i32 {
    c: char = 'Q';
    s: string = cast<string>(c);   // Heap-allocates a 2-byte buffer
    ioPrintln(s);                   // Q

    i: ptr<i32> = &(cast<i32>(0));  // Placeholder pointer
    f: ptr<f32> = cast<ptr<f32>>(i); // Allowed via permissive pointer rules
    return 0;
}

```

---

*[← Operators](@ref operators) | [Next: Imports →](@ref imports)*
