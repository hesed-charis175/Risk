# extern "header.h" { ... } Test Files

These files test the `extern` block feature, which allows Risk code to call plain C functions declared in `.h` files.

## Files

* **add.h**: The C header file that declares and defines `int add(int a, int b)` inline for testing purposes.
* **extern_add_ok.risk**: A standard test case that declares `add` using an `extern` block and calls it from `main`. Expected output: `11`.
* **extern_add_body_error.risk**: A test case containing a function with a body inside an `extern` block. This produces a parse error with a diagnostic message stating that functions inside `extern` blocks cannot have bodies and must be signatures terminated with a semicolon.
* **extern_add_as_value_error.risk**: A test case that attempts to use `add` as a first-class function value (`f: func(i32,i32)->i32 = add;`) instead of making a direct call. This produces a type-checking error indicating that extern C functions can only be called directly.

## Running the Tests

Using the compiled `riskc` binary from this directory:

```sh
riskc extern_add_ok.risk -o test_add
./test_add
# Output: 11

```

```sh
riskc extern_add_body_error.risk -o /tmp/out      # Expect a parse error, no binary produced
riskc extern_add_as_value_error.risk -o /tmp/out   # Expect a type-check error, no binary produced

```

The compiler resolves `extern "add.h"` as a standard `#include "add.h"` relative to the directory containing the main `.risk` file. The driver passes `-I<source_dir>` to `gcc`, so `add.h` must remain located alongside the `.risk` test files.
