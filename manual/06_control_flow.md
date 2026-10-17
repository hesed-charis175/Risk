# 6. Control Flow {#control-flow}

*[← Compound Types](@ref compound) | [Next: Functions & Closures →](@ref functions)*

## `if` / `else`

```risk
if (score >= 90) {
    ioPrintln("A");
} else if (score >= 80) {
    ioPrintln("B");
} else {
    ioPrintln("C");
}

```

The condition expression must be enclosed in parentheses. The branch can consist of a single statement or a block enclosed in `{ ... }`. The `else` clause is optional.

## `while`

```risk
i: i32 = 0;
while (i < 10) {
    ioPrintln(i);
    i += 1;
}

```

## `for`

The `for` loop uses a C-style three-clause form containing an initialization (either a variable declaration or an expression), a condition, and a post-expression:

```risk
for (i: i32 = 0; i < 10; i += 1) {
    ioPrintln(i);
}

```

The initialization clause can also use a bare assignment to an already-declared outer variable instead of a fresh variable declaration if you want the loop counter to remain accessible after the loop finishes:

```risk
i: i32 = 0;
for (i = 0; i < 10; i++) {
    ioPrintln(i);
}
// i is 10 here, still in scope

```

## `switch`

```risk
switch (code) {
    case 1:
        ioPrintln("one");
        break;
    case 2:
        ioPrintln("two");
        break;
    default:
        ioPrintln("other");
}

```

The `switch` statement compiles directly to a C `switch`. **Cases fall through** unless you include an explicit `break` statement, matching the behavior of C. There is no implicit break at the end of a case. This differs from languages with implicit-break switches such as Swift, Kotlin, or Rust, where omitting a `break` statement silently executes the body of the next case.

The `switch` statement also works directly with `opts` values (see [§9](@ref opts)), matching against `TypeName.Member` case labels:

```risk
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

## `break` / `continue`

The `break` statement exits the nearest enclosing `while`, `for`, or `switch` block. The `continue` statement skips to the next iteration of the nearest enclosing `while` or `for` loop. Using either statement outside of a loop, or using `break` outside of a loop or switch, results in a compile-time error.

## Scopes

Every `{ ... }` block introduces a separate lexical scope. Variables declared inside a block, including a `for` loop header, are not visible after the block ends. Shadowing an outer variable name within a nested scope is permitted.

## Examples

The following program demonstrates `if`, `while`, `for`, `switch`, `break`, `continue`, and scoping rules together:

```risk
func classify(n: i32) -> string {
    if (n < 0) {
        return "negative";
    } else if (n == 0) {
        return "zero";
    } else {
        return "positive";
    }
}

func main() -> i32 {
    ioPrintln(classify(-3), " ", classify(0), " ", classify(7));

    total: i32 = 0;
    for (i: i32 = 0; i < 10; i += 1) {
        if (i % 2 == 0) { continue; }   // skip evens
        if (i > 7) { break; }           // stop early
        total = total + i;
    }
    ioPrintln("total = ", total);       // 1 + 3 + 5 + 7 = 16

    code: i32 = 2;
    switch (code) {
        case 1:
            ioPrintln("one");
            break;
        case 2:
            ioPrintln("two");
            // no break statement, which causes it to fall through on purpose
        case 3:
            ioPrintln("two-or-three");
            break;
        default:
            ioPrintln("other");
    }
    // prints "two" then "two-or-three"

    return 0;
}

```

**A `for` loop whose counter survives past the loop**, using a bare assignment in the initialization clause instead of a fresh declaration:

```risk
func main() -> i32 {
    i: i32 = 0;
    for (i = 0; i < 10; i++) {
        // ...
    }
    ioPrintln("i is ", i, " here, still in scope");   // i is 10 here
    return 0;
}

```

**`switch` on an `opts` value**, matching against `TypeName.Member` case labels (refer to [§9](@ref opts) for details on `opts`):

```risk
opts Color { Red, Green, Blue }

func main() -> i32 {
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
    return 0;
}

```

---

*[← Compound Types](@ref compound) | [Next: Functions & Closures →](@ref functions)*
