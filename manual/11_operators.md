# 11. Operators {#operators}

*[← Generics](@ref lang-generics) | [Next: Casting →](@ref casting)*

## Precedence and Associativity

Risk follows standard operator precedence and associativity rules that match C99. When expressions become complex, parentheses can be used to explicitly control evaluation order.

| Precedence Level | Category | Operators | Associativity |
| --- | --- | --- | --- |
| 1 (highest) | Primary | `(...)`, `[...]`, `.`, `[]` | Left-to-right |
| 2 | Unary | `!`, `-`, `*`, `&`, `sizeof`, `type`, `cast`, `unsafe_cast` | Right-to-left |
| 3 | Multiplicative | `*`, `/`, `%` | Left-to-right |
| 4 | Additive | `+`, `-` | Left-to-right |
| 5 | Bitwise shift | `<<`, `>>` | Left-to-right |
| 6 | Relational | `<`, `<=`, `>`, `>=` | Left-to-right |
| 7 | Equality | `==`, `!=` | Left-to-right |
| 8 | Bitwise AND | `&` | Left-to-right |
| 9 | Bitwise XOR | `^` | Left-to-right |
| 10 | Bitwise OR | `|` | Left-to-right |
| 11 | Logical AND | `&&` | Left-to-right |
| 12 | Logical OR | `||` | Left-to-right |
| 13 (lowest) | Assignment | `=`, `+=`, `-=`, `*=`, `/=`, `%=`, `&=`, `|=`, `^=`, `<<=`, `>>=` | Right-to-left |

### Operator Precedence Example

```risk
func main() -> i32 {
    // Multiplication happens before addition based on precedence rules
    result: i32 = 2 + 3 * 4;     // Evaluates as 2 + (3 * 4) -> 14
    forced: i32 = (2 + 3) * 4;   // Parentheses override precedence -> 20
    ioPrintln("result = ", result, " forced = ", forced);
    return 0;
}

```

## Numeric Promotion Ranking

When binary operations involve mixed numeric types, the compiler promotes operands to a common type according to an internal rank hierarchy:

| Type | Rank |  | Type | Rank |
| --- | --- | --- | --- | --- |
| `f64` | 60 |  | `i32` | 30 |
| `f32` | 50 |  | `u32` | 29 |
| `i64` | 40 |  | `i16` | 20 |
| `u64` | 39 |  | `u16` | 19 |
| none | 0 |  | `i8` / `u8` | 10 / 9 |

### Promotion Rules

* **Floating-Point Priority**: If any operand in an expression is a floating-point type, the entire expression promotes to floating-point. `f64` takes precedence over `f32`.
* **Integer Width and Signedness**: Among integer types, higher ranks take precedence. When bit widths are equal, signed types take precedence over unsigned types. For example, `i32` (rank 30) outranks `u32` (rank 29).
* **Literal Narrowing**: Integer literals inferred as `i32` narrow implicitly to match smaller target types during assignments or parameter passing, provided the literal value fits within the target type range.

### Numeric Promotion Example

```risk
func main() -> i32 {
    a: i32 = 10;
    b: f64 = 2.5;
    // 'a' (i32) promotes to f64 automatically, producing an f64 result
    mixed: f64 = a + b;
    ioPrintln("mixed = ", mixed);
    return 0;
}

```

---

*[← Generics](@ref lang-generics) | [Next: Casting →](@ref casting)*
