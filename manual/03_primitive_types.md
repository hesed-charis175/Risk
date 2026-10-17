# 3. Primitive Types {#primitives}

*[← Getting Started](@ref getting-started) | [Next: Pointers & Constness →](@ref pointers)*

Risk primitive types map directly to fixed-width C types. The type system does not use platform-dependent `int` or `long` definitions.

## Integers

| Type | C Type | Range |
| --- | --- | --- |
| `i8` | `int8_t` | −128 to 127 |
| `i16` | `int16_t` | −32,768 to 32,767 |
| `i32` | `int32_t` | −2,147,483,648 to 2,147,483,647 |
| `i64` | `int64_t` | −9,223,372,036,854,775,808 to 9,223,372,036,854,775,807 |
| `u8` | `uint8_t` | 0 to 255 |
| `u16` | `uint16_t` | 0 to 65,535 |
| `u32` | `uint32_t` | 0 to 4,294,967,295 |
| `u64` | `uint64_t` | 0 to 18,446,744,073,709,551,615 |

An integer literal without a decimal point (`42`, `0xFF`, `0b1010`) defaults to `i32`. When assigned to a variable or passed where a narrower or wider type is expected, the compiler automatically converts the literal as long as the value fits within the target type range:

```risk
count: i32 = 42;
small: u8 = 200;         // Fits in u8
port: u16 = 0x1F90;      // Hex literal normalized to decimal internally
mask: i32 = 0b10101100;  // Binary literal fitting i32

```

This validity rule applies at function call boundaries. A parameter typed as `i8` accepts a literal argument as long as the value is within range, regardless of the literal default type:

```risk
func take_i8(x: i8) -> void {}
func take_u8(x: u8) -> void {}
func take_i16(x: i16) -> void {}
func take_u32(x: u32) -> void {}
func take_f64(x: f64) -> void {}

take_i8(100);      // 100 fits in i8 (maximum 127)
take_u8(200);      // 200 fits in u8 (maximum 255)
take_i16(30000);   // Fits in i16 (maximum 32767)
take_u32(99999);   // Fits in u32
take_f64(42);      // Integer literal widens to f64

```

Mixing integer types within a single expression promotes operands to the wider type according to the rules in [§11](@ref operators). Truncation does not occur automatically inside expressions, only during assignment or literal fitting.

## Floating Point

| Type | C Type |
| --- | --- |
| `f32` | `float` |
| `f64` | `double` |

A literal containing a decimal point (`3.14`, `0.5`) defaults to `f64`:

```risk
pi: f64 = 3.14159;
ratio: f32 = 0.5;
almost_one: f64 = 0.999999999;
huge: f64 = INF;
undefined: f64 = NAN;

```

`INF` and `NAN` are recognized directly as numeric literals without requiring function calls or imports. To test for them at runtime, use `isInf` and `isNan` from `@core/math.risk` (see [§16](@ref stdlib-reference)). For example, `isNan(0.0 / 0.0)` evaluates to `true`, while `isInf(0.0 / 0.0)` evaluates to `false` because an indeterminate `0/0` expression produces NaN rather than infinity.

### Malformed Numeric Literals

A floating-point literal may start with a decimal point, such as `.5` representing `0.5`.

Numeric literals cannot be immediately followed by letters. Suffixes and exponent notations are not supported. Literals like `33b`, `1.0a`, `1e5`, `0xFG`, and invalid binary digits like `0b012` trigger a compilation error rather than being parsed as a number followed by a variable identifier.

## Boolean

`bool` compiles to a C integer internally, but in Risk it only holds `true` or `false`:

```risk
done: bool = false;
ready: bool = (count > 0) && !done;

```

## Character

`char` represents a single ASCII character literal:

```risk
newline: char = '\n';
letter: char = 'A';
quote: char = '\'';

```

Standard C-style escape sequences (`\n`, `\t`, `\\`, `\'`, `\"`, `\0`) are supported in both `char` and `string` literals.

`char` functions as a distinct type rather than a raw integer, but it converts to and from `i32` using `cast` to retrieve its ordinal value for character arithmetic:

```risk
base: i32 = cast<i32>('a');        // 97
shifted: char = cast<char>(base + 3); // 'd'

```

## String

`string` is a heap-managed, null-terminated character pointer. String literals are enclosed in double quotes:

```risk
name: string = "Risk";
greeting: string = "Hello, ";
path: string = "C:\\Users\\dev";

```

A `string` represents a pointer to heap-owned bytes. Comparing two strings using `==` or `!=` compares their content rather than their underlying pointers, meaning `"ab" == someBuiltString` evaluates correctly regardless of independent heap allocations. For comprehensive string manipulation functions, see the [Standard Library Reference](@ref stdlib-reference).

## Examples

**Literal fitting, promotion, and character conversion:**

```risk
func main() -> i32 {
    small: u8 = 200;
    port: u16 = 0x1F90;

    a: i32 = 10;
    b: f64 = 2.5;
    mixed: f64 = a + b;          // i32 promotes to f64

    base: i32 = cast<i32>('a');
    shifted: char = cast<char>(base + 3);

    huge: f64 = INF;

    ioPrintln("small=", small, " port=", port, " mixed=", mixed,
              " shifted=", shifted, " huge=", huge);
    return 0;
}
// Output: small=200 port=8080 mixed=12.5 shifted=d huge=inf

```

**Literal fitting at function call boundaries:**

```risk
func take_i8(x: i8) -> void { ioPrintln("i8: ", x); }
func take_u32(x: u32) -> void { ioPrintln("u32: ", x); }

func main() -> i32 {
    take_i8(100);       // 100 fits in i8 (maximum 127)
    take_u32(99999);    // Fits in u32
    return 0;
}

```

**Boolean and string values:**

```risk
func main() -> i32 {
    count: i32 = 3;
    done: bool = false;
    ready: bool = (count > 0) && !done;
    ioPrintln("ready = ", ready);

    name: string = "Risk";
    greeting: string = "Hello, ";
    ioPrintln(greeting, name);
    return 0;
}
// Output:
// ready = true
// Hello, Risk

```

---

*[← Getting Started](@ref getting-started) | [Next: Pointers & Constness →](@ref pointers)*
