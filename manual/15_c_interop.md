# 15. C Interop: `extern` Blocks {#c-interop}

*[← Memory Management](@ref arena-rc) | [Next: Standard Library Reference →](@ref stdlib-reference)*

Using `extern "header.h" { ... }`, Risk code can seamlessly declare and call existing C functions, mirror C struct layouts, reference opaque C types, read and write C global variables, or reuse C `enum` values as `opts` without writing wrapper code. Headers are located relative to the source `.risk` file containing the block, and the driver automatically supplies `-I<source_dir>` and module paths to `gcc`.

---

## Declaring and calling a C function

```risk
extern "add.h" {
    func add(a: i32, b: i32) -> i32;
}

func main() -> i32 {
    result: i32 = add(5, 6);
    ioPrintln(toString(result));   // 11
    return 0;
}

```

An `extern` function declaration must be signature-only and terminated with a semicolon rather than a body:

```risk
extern "add.h" { func add(a: i32, b: i32) -> i32 { return 0; } }
// parse error: function 'add' inside an 'extern' block cannot have a
// body -- it must be a signature only, terminated with ';'

```

Additionally, `extern` C functions must be **called directly** and cannot be used as first-class function values like regular Risk functions:

```risk
extern "add.h" { func add(a: i32, b: i32) -> i32; }

func main() -> i32 {
    f: func(i32,i32)->i32 = add;   // type-check error, not a parse error
    // error: 'add' is an extern C function and can only be directly
    // called (e.g. 'add(...)'), not used as a function value
    return 0;
}

```

---

## Mirroring a C struct

An `extern` block can declare a `block` to mirror an existing C `struct` layout, allowing values to be passed to or returned from C functions and manipulated directly in Risk:

```c
// matrix.h
struct Matrix {
  double *data;
  int rows, cols;
};

static inline struct Matrix make_matrix(double *data, int rows, int cols) { ... }
static inline double matrix_sum(struct Matrix m) { ... }

```

```risk
extern "matrix.h" {
  block Matrix {
    data: ptr<f64>;
    rows: i32;
    cols: i32;
  }
  func make_matrix(data: ptr<f64>, rows: i32, cols: i32) -> Matrix;
  func matrix_sum(m: Matrix) -> f64;
}

func main() -> i32 {
  buf: Array<f64, 4> = [1.0, 2.0, 3.0, 4.0];
  m: Matrix = make_matrix(cast<ptr<f64>>(&buf), 2, 2);

  m.rows;              // 2 -- field read from a struct returned by an extern function
  m.rows = 5;           // field mutation on the Risk side works too

  s: f64 = matrix_sum(m);   // struct passed BY VALUE into another extern function
  first: f64 = *(m.data);   // dereferencing a ptr<f64> field directly
  return 0;
}

```

This works directly with **bare tagged `struct` definitions with no `typedef**` on the C side (e.g., `struct Matrix`).

### Opaque handles

Declaring a `block Name;` with an empty body inside an `extern` block creates an **opaque** type with no Risk-side field mirror. It can be passed around as a type handle between extern calls, but its fields cannot be read or written directly:

```risk
extern "matrix.h" {
  block Handle;   // opaque -- struct Handle { int id; } on the C side
}

```

### Overriding default spelling: `block Name "spelling"`

By default, mirroring a `block` assumes the underlying C type is named `struct Name`. For structures that deviate from this convention—such as anonymous typedef structs (`XButtonEvent`) or tagged unions/structs where the tag differs from the typedef name (`union _XEvent`)—you can specify the exact C spelling using a string literal:

```c
// xevent.h (a simplified Xlib-style example)
typedef union _XEvent {
  int type;
  double data;
} XEvent;

typedef struct {
  int x, y;
} Point;   // anonymous struct -- no tag at all

```

```risk
extern "xevent.h" {
  block XEvent "union _XEvent" {
    type: i32;
    data: f64;
  }
  block Point "Point" {   // no "struct " prefix -- it's an anonymous struct
    x: i32;
    y: i32;
  }
}

```

Leaving the spelling off defaults to `block Name "struct Name"`, which also applies to opaque blocks (e.g., `block Handle "some_other_name";`).

---

## `opts` and bare variables inside `extern` blocks

An `extern` block can define an `opts` type to mirror C `enum` values, alongside bare `name: Type;` declarations that bind directly to C global variables:

```c
// orderlib.h
enum CBLAS_ORDER { CblasRowMajor = 101, CblasColMajor = 102 };
static int global_counter = 42;
static inline int order_as_int(int order) { return order; }

```

```risk
extern "orderlib.h" {
  opts Order {
    RowMajor = 101,
    ColMajor = 102
  }
  global_counter: i32;
  func order_as_int(order: i32) -> i32;
}

func main() -> i32 {
  // opts fields compile to plain compile-time integer literals -- no C
  // enum symbol is referenced at all
  cast<i32>(Order.RowMajor);   // 101

  // Order.RowMajor has type Order, not i32 -- same as any ordinary,
  // non-extern opts (see §9) -- so it needs an explicit cast to pass
  // into a function declared as taking i32
  order_as_int(cast<i32>(Order.RowMajor));   // 101

  // reading and writing a real C global directly
  global_counter;        // 42 (the C-side initial value)
  global_counter = 99;
  global_counter;        // 99
  return 0;
}

```

---

## Linking against a library: the `lib` clause

Using `extern "header.h" lib "name" { ... }` applies a `-lname` linking flag for system libraries or separately installed dependencies:

```risk
extern "mylib.h" lib "mylib" {
  func mylib_add(a: i32, b: i32) -> i32;
  func mylib_square(x: f64) -> f64;
}

mylib_add(3, 4);        // 7
mylib_add(-5, 2);       // -3
mylib_square(6.0);      // 36.0
mylib_square(-3.0);     // 9.0

```

The `lib` clause allows compilation to succeed for system-wide headers that are not stored locally relative to your source file. Multiple comma-separated libraries can be provided if a header requires several flags:

```risk
extern "cblas.h" lib "blas", "m" {   // -lblas -lm
  func cblas_sdot(n: i32, x: ptr<f32>, incx: i32,
                   y: ptr<f32>, incy: i32) -> f32;
}

```

```risk
extern "math.h" lib "m" {
  func sqrt(x: f64) -> f64;
}

```

---

## Header path resolution, and what happens when it fails

Relative subdirectory paths resolve normally (e.g., `extern "sub/add.h"`). A missing local header without a `lib` clause triggers a hard compile error:

```risk
extern "totally_missing_header.h" {
  func foo() -> i32;
}
// error contains: extern header

```

Adding a `lib` clause instructs the compiler to trust that `gcc` can locate the header via the linked library paths.

### Search order and sibling `.c` files

The compiler searches for an `extern "header.h"` file in the following order:

1. The directory of the current `.risk` file.


2. The directory of the main `.risk` file being compiled.


3. The exact path as written.



If a matching `name.c` file is found right next to the located `name.h` header, it is automatically passed to `gcc` for compilation. Unresolved headers are only permitted if a `lib` clause is specified.

---

## Examples

### Declaring, calling, and mirroring a C struct

```c
// point.h
struct Point { double x, y; };
static inline double point_dist(struct Point p) {
    return p.x * p.x + p.y * p.y;   // squared distance, kept simple
}

```

```risk
extern "point.h" {
    block Point {
        x: f64;
        y: f64;
    }
    func point_dist(p: Point) -> f64;
}

func main() -> i32 {
    p: Point = { x = 3.0, y = 4.0 };
    ioPrintln(point_dist(p));   // 25 -- passed by value into the extern call
    return 0;
}

```

```sh
riskc main.risk -o main   # resolves "point.h" next to main.risk automatically
./main

```

### Using `opts` and global variables

```c
// counter.h
enum Status { StatusOk = 0, StatusFail = 1 };
static int hit_count = 0;

```

```risk
extern "counter.h" {
    opts Status { Ok = 0, Fail = 1 }
    hit_count: i32;
}

func main() -> i32 {
    hit_count = hit_count + 1;
    ioPrintln("hits: ", hit_count);
    ioPrintln("status: ", cast<i32>(Status.Ok));
    return 0;
}

```

### Linking system libraries with explicit C spellings

```risk
extern "math.h" lib "m" {
    func sqrt(x: f64) -> f64;
}

extern "xevent.h" {
    block XEvent "union _XEvent" {   // real C spelling isn't "struct XEvent"
        type: i32;
    }
}

func main() -> i32 {
    ioPrintln(sqrt(9.0));   // 3
    return 0;
}

```

---

*[← Memory Management](@ref arena-rc) | [Next: Standard Library Reference →](@ref stdlib-reference)*
