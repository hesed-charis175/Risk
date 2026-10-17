# 5. Compound Types: Array and Hashmap {#compound}

*[← Pointers & Constness](@ref pointers) | [Next: Control Flow →](@ref control-flow)*

---

## `Array<T>` - Dynamic Arrays

Risk features a heap-managed, growable array type known as `Array<T>`. It supports pushing, popping, and indexing without requiring any manual capacity management.

```risk
nums: Array<i32> = [1, 2, 3];
nums.push(4);
last: i32 = nums.pop();
nums.pushFront(0);
nums.insert(1, 99);
nums.removeAt(2);
n: i32 = length(nums);

```

### Available Methods

All array methods are invoked using dot syntax.

* **`push(v)`**: Appends an element to the end.


* **`pop()`**: Removes and returns the last element.


* **`pushFront(v)`**: Prepends an element to the beginning (note the **camelCase** spelling; legacy snake_case names no longer compile).


* **`popFront()`**: Removes and returns the first element.


* **`insert(i, v)`**: Inserts a value at index `i`.


* **`removeAt(i)`**: Removes the element at index `i`.


* **`clear()`**: Empties the array.


* **`repr(raw: bool = false)`**: Returns a string representation of the array.


* **`freeAll()` / `destroy()**`: Releases owned contents.



>  The older `.show()` method has been completely removed in favor of `.repr()`. Calling `.show()` now produces a clean compile-time error.
> 
> 

### Indexing and Safety

Elements can be read or written directly using standard bracket syntax:

```risk
nums[0] = 42;
first: i32 = nums[0];

```

 Indexing is **not** a safe operation in Risk. Accessing an out-of-range index dereferences a null internal pointer and crashes the program rather than throwing a catchable error or returning a default value. Always ensure indices stay within bounds (e.g., by checking against `length(nums)`).

### String Representation (`.repr()`) on Block Arrays

When called on an array of blocks, `Array<T>.repr()` formats each element using that block's custom `repr` field if one is defined. Otherwise, it falls back to displaying a raw memory address.

You can pass `raw = true` to override this behavior and force raw address printing, which is helpful when debugging object identity or aliasing:

```risk
ioPrintln(people.repr());          // Uses each Person's custom repr field[cite: 6]
ioPrintln(people.repr(true));      // Prints raw addresses instead[cite: 6]

```

---

## `Array<T, N>` - Fixed-Size Arrays

For scenarios requiring zero heap allocation and a fixed size, Risk provides `Array<T, N>`. It compiles directly into a stack-allocated C array (`T[N]`) and is the **only** array type that a function can return by value:

```risk
buffer: Array<u8, 16> = [0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0];

func zeros4() -> Array<i32, 4> {
    out: Array<i32, 4> = [0, 0, 0, 0];
    return out;
}

func main() -> i32 {
    z: Array<i32, 4> = zeros4();
    ioPrintln(z.repr());
    ioPrintln(z[0]);
    return 0;
}

```

* Fixed-size arrays do not support dynamic methods like `.push()` or `.pop()` because their dimensions are locked at compile time.


* They serve as core parameters for compile-time integer generics (such as `Array<T, + 1 N>`).


* The `sizeof()` operator returns a fixed array's **total byte size**, which can be used to calculate element counts:


```risk
r: Array<i32, 4> = [1, 2, 3, 4];
ioPrintln("i32==", sizeof(i32));                    // 4[cite: 6]
ioPrintln("array<i32>==", sizeof(r));               // 16 (4 elements * 4 bytes)[cite: 6]
ioPrintln("elementCount==", sizeof(r) / sizeof(i32)); // 4[cite: 6]

```



---

## `Hashmap<K, V>`

The `Hashmap<K, V>` type is a heap-managed hash map initialized from an empty literal block:

```risk
ages: Hashmap<string, i32> = {};
ages["Ada"] = 36;
ages["Grace"] = 85;

if (ages.has("Ada")) {
    v: i32 = ages["Ada"];
    ioPrintln(v);
}

n: i32 = ages.size();
ages.remove("Grace");
ages.clear();

```

### Hashmap Methods and Syntax

* **Access & Assignment**: Uses bracket syntax like `m[k]` and `m[k] = v`.


* **`has(k)` / `contains(k)**`: Returns `true` if key `k` exists in the map.


* **`size()`**: Returns the total number of stored key-value pairs.


* **`remove(k)` / `delete(k)**`: Removes a key and its value safely (missing keys result in a no-op rather than an error).


* **`clear()`**: Removes all key-value pairs.



> Argument types passed to map lookup and removal methods (`has`, `remove`, etc.) are strictly checked against the declared key type `K`.
> 
> 

### Memory Leaks Warning with `clear()`

Calling `.clear()` unconditionally discards all stored values without freeing them. If your map holds values containing pointers (`ptr<T>`), reference-counted handles (`refc<T>`), or blocks with inner pointers, the compiler triggers a warning:

```risk
handles: Hashmap<string, ptr<Widget>> = {};
handles["a"] = allocWidget();
handles.clear();
// warning: clear() on Hashmap<string, ptr<Widget>>: values will leak - free each value before clearing[cite: 6]

```

Unlike dynamic arrays, `Hashmap` lacks a built-in `freeAll()` method, meaning you must manually iterate and deallocate owned values before clearing.

---

## Examples

The following example combines dynamic arrays, fixed-size stack arrays, and hashmaps into a single program:

```risk
import "@core";

block Person {
    name: string;
    repr: func(this) -> string = personRepr;
}

func personRepr(self: Person) -> string {
    return self.name;
}

func main() -> i32 {
    // Dynamic array
    people: Array<Person> = [];
    people.push({ name = "Ada" });
    people.push({ name = "Grace" });
    ioPrintln(people.repr());          // [Ada, Grace] -- via Person's repr field[cite: 6]

    // Fixed-size array
    buf: Array<i32, 4> = [1, 2, 3, 4];
    ioPrintln("elementCount = ", sizeof(buf) / sizeof(i32));   // 4[cite: 6]

    // Hashmap
    ages: Hashmap<string, i32> = {};
    ages["Ada"] = 36;
    ages["Grace"] = 85;

    if (ages.has("Ada")) {
        ioPrintln("Ada is ", ages["Ada"]);
    }
    ioPrintln("size = ", ages.size());
    ages.remove("Grace");
    ioPrintln("size after remove = ", ages.size());

    return 0;
}

```

---

*[← Pointers & Constness](@ref pointers) | [Next: Control Flow →](@ref control-flow)*
