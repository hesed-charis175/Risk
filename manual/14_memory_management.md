# 14. Memory Management: Arenas and Reference Counting {#arena-rc}

*[← Imports](@ref imports) | [Next: C Interop →](@ref c-interop)*

Risk doesn't use a garbage collector. Instead, it provides three complementary memory-management tools so you don't have to fall back on the old routine of calling `malloc`/`free` and hoping for the best.

---

## Manual pointers: `dealloc`

To free a `ptr<T>`, use `dealloc(p)`. The compiler keeps track of freed variables and will instantly flag a **double-free** (if you try to free the same variable twice) or warn you if the data structure still contains un-freed nested pointers—giving you a compile-time safety net with zero runtime cost:

```risk
p: ptr<i32> = &someHeapValue;
dealloc(p);
dealloc(p);   // error: double-free - 'p' was already freed on line N

```

Raw allocation goes through `alloc(n)` (byte count in, `ptr<void>` out). This pairs with `copyMemory(dst, src, n)` and `setMemory(p, byteValue, n)` for bulk operations equivalent to `memcpy` and `memset`—check out [§16](@ref stdlib-reference) for the complete memory-module reference, including details on how `setMemory(p, 0, n)` zeros out a region.

---

## Reference counting: `refc<T>`

`refc<T>` handles reference-counted wrapping.

* Create a wrapper with `rcNew`.
* Share ownership with `rcRetain`.
* Release your claim with `rcRelease`.
* Read the underlying value using `rcGet`.
* Inspect the active reference count with `rcCount`.

```risk
shared: refc<i32> = rcNew(42);
alias: refc<i32> = rcRetain(shared);   // bump the refcount, share ownership

value: ptr<i32> = rcGet(shared);       // borrow the underlying pointer
n: u64 = rcCount(shared);              // how many owners right now

rcRelease(alias);
rcRelease(shared);
rcRelease(shared);  // error: double-release - 'shared' was already released

```

Much like `dealloc`, `rcRelease` protects against double-releases. If you try to assign a `refc<T>` to another variable using a plain `=` instead of `rcRetain`, the compiler triggers a warning. Doing a plain copy copies the handle without incrementing the reference count, which would trick both variables into thinking they are sole owners—meaning whoever releases first leaves the other holding a dangling reference.

---

## Bulk allocation: `Arena`

An `Arena` acts as a bump allocator. You initialize it with a fixed byte budget, allocate out of it freely, and release everything all at once. It's ideal for batches of short-lived allocations that share the same lifecycle:

```risk
scratch: Arena = arenaNew(1024 * 1024);   // 1 MiB arena

buf: ptr<void> = scratch.alloc(256);
// ... use buf ...

used: u64 = scratch.used();     // bytes handed out so far
mib: f64 = scratch.MiB();       // same figure, in MiB

scratch.reset();     // reuse the same memory for a new batch
scratch.destroy();   // release the arena's memory entirely

```

Should you allocate more than its initial budget, the arena automatically grows by adding internal blocks. Methods like `.used()`, `.KiB()`, `.MiB()`, and `.GiB()` aggregate totals **across every block**, ensuring consistency regardless of how many times the arena has expanded.

| Method | Signature | Reports |
| --- | --- | --- |
| `alloc(n)` | `(u64) -> ptr<void>` | Allocates `n` bytes out of the arena. |
| `used()` | `() -> u64` | Bytes handed out so far, in bytes. |
| `KiB()` / `MiB()` / `GiB()` | `() -> f64` | Same figure as `used()`, pre-converted. |
| `capacityBytes()` | `() -> u64` | Total bytes currently *reserved* by the arena (used + unused), in bytes. |
| `capacityKiB()` / `capacityMiB()` / `capacityGiB()` | `() -> f64` | Same figure as `capacityBytes()`, pre-converted. |
| `reset()` | `() -> void` | Marks all of the arena's memory as free again for reuse (does not return it to the OS). |
| `destroy()` | `() -> void` | Releases the arena's memory entirely. |

Don't confuse used memory with capacity:

* `used()`/`KiB()`/`MiB()`/`GiB()` tell you how much memory you've actively handed out.
* `capacityBytes()`/`capacityKiB()`/`capacityMiB()`/`capacityGiB()` tell you how much total reserve the arena holds right now.

On a fresh, untouched arena, these numbers match up (reflecting the initial budget passed to `arenaNew()`), but they diverge as soon as you use only a fraction of your reserve or trigger an automatic block expansion.

```risk
scratch: Arena = arenaNew(1024 * 1024);   // 1 MiB reserved up front
buf: ptr<void> = scratch.alloc(256);

scratch.used();           // 256
scratch.capacityBytes();  // 1048576 -- the full 1 MiB you asked for,
                           // regardless of how little you've used

```

The `Arena` type also supports a scratch checkpoint/rewind pair—`scratchBegin()` and `scratchEnd(mark)`—allowing you to borrow temporary workspace and return it instantly without resetting the entire arena:

```risk
a: Arena = arenaNew(64 * 1024 * 1024);
mark: ptr<void> = a.scratchBegin();

// ... allocate scratch space out of 'a' here ...

a.scratchEnd(mark);   // rewinds only the scratch region taken since mark
a.destroy();

```

---

## Memory-mapped registers: `register<T>(address)`

This fourth, specialized tool isn't meant for general heap management, but it handles direct memory access crucial for embedded systems. Declaring a local variable with `register<T>(addr)` doesn't allocate any heap space; instead, it binds a name directly to a fixed memory address to facilitate volatile hardware interactions:

```risk
func main() -> i32 {
    status: register<u32>(0x40021000);   // address as a decimal literal
    ctrl:   register<u32>(0x40021004);   // hex literals work too - the
                                          // lexer normalizes 0x... to
                                          // decimal before the parser
                                          // ever sees it

    ctrl = ctrl | 0x1;          // write through the register
    ready: bool = (status & 0x2) != 0;   // read through the register

    return 0;
}

```

This compiles straight into a C `#define`:

```c
#define status (*(volatile uint32_t*)0x40021000)
#define ctrl   (*(volatile uint32_t*)0x40021004)

```

Because the `volatile` qualifier is always included, neither the compiler nor `gcc` will optimize away or reorder reads and writes through a `register<T>` binding—preventing issues with hardware registers that can change asynchronously or trigger side-effects.

Keep a few ground rules in mind when using it:

* **Local statements only:** `register<T>` must be declared inside a function body, not at file scope next to `func` or `block`. If you need a register shared across multiple functions, wrap access inside a helper function.
* **Namespace sharing:** Since it expands to a `#define`, the name occupies the same namespace as regular local variables for the remainder of that scope, meaning normal variable shadowing rules apply.
* **No cleanup required:** There's no `dealloc` or teardown step because a `register<T>` binding never owns memory; it just names a pre-existing hardware address.

---

## Bulk-freeing an array of owned pointers: `.freeAll()`

Building on the array cleanup helpers discussed in [§5](@ref compound), the compiler includes specific validation for `.freeAll()`:

* It is exclusively permitted on `Array<ptr<T>>` or `Array<refc<T>>`.
* Calling it on any other array type (like standard `Array<T>` or `Array<block>`) triggers a compile-time error rather than failing silently.

```risk
ptrs: Array<ptr<i32>> = [];
ptrs.push(&someHeapI32);
ptrs.push(&anotherHeapI32);
// ... use ptrs ...
ptrs.freeAll();   // dealloc()s every element, then clears the array

nums: Array<i32> = [1, 2, 3];
// nums.freeAll();  // error: freeAll() can only be called on
                     // Array<ptr<T>> or Array<refc T>, but element type is i32

```

By contrast, `.destroy()` takes no arguments, releases the array container itself, and works with any element type. However, it *does not* free what the elements point to. If your array holds blocks containing `ptr<T>` or `refc<T>` fields, the compiler warns you that those fields will leak and instructs you to free the inner pointers first.

This same ownership logic drives a warning elsewhere: declaring a `block` with inner `ptr<T>` or `refc<T>` fields without providing a reachable cleanup path (like `freeAll()`) flags a potential memory leak right where the block type is registered.

---

## Choosing the right tool

| Tool | Best used when... |
| --- | --- |
| `ptr<T>` + `dealloc` | You have a single clear owner, standard lifespans, or are doing C-interop. |
| `refc<T>` | Multiple owners need to safely share a single value. |
| `Arena` | You are dealing with many short-lived allocations that will all be dropped together. |
| `register<T>(addr)` | You need direct, volatile access to a fixed hardware address (not heap memory). |

These tools compose naturally: you can easily pass an `Arena`-backed buffer through a `ptr<void>`, or nest a `refc<T>` inside a `block`'s fields.

---

## Examples

### Combining all three memory tools

```risk
import "@core";

func main() -> i32 {
    // Manual: ptr<T> + dealloc
    p: ptr<i32> = cast<ptr<i32>>(alloc(sizeof(i32)));
    *p = 7;
    ioPrintln("manual: ", *p);
    dealloc(p);

    // Reference-counted
    shared: refc<i32> = rcNew(42);
    alias: refc<i32> = rcRetain(shared);
    ioPrintln("refc: ", *rcGet(shared), " count=", rcCount(shared));
    rcRelease(alias);
    rcRelease(shared);

    // Arena
    scratch: Arena = arenaNew(1024 * 1024);
    buf: ptr<void> = scratch.alloc(256);
    ioPrintln("arena used: ", scratch.used(), " bytes, capacity: ",
              scratch.capacityBytes(), " bytes");
    scratch.destroy();

    return 0;
}
// manual: 7
// refc: 42 count=2
// arena used: 256 bytes, capacity: 1048576 bytes

```

### Hardware Register Access

*(Note: Because this is a local-statement-only form, it compiles successfully, though it produces no meaningful runtime output outside of physical hardware.)*

```risk
func main() -> i32 {
    ctrl: register<u32>(0x40021004);
    ctrl = ctrl | 0x1;                    // write through the register
    ready: bool = (ctrl & 0x2) != 0;      // read through the register
    ioPrintln(ready);
    return 0;
}

```

### Bulk-Freeing Pointer Arrays

```risk
func main() -> i32 {
    ptrs: Array<ptr<i32>> = [];
    ptrs.push(cast<ptr<i32>>(alloc(sizeof(i32))));
    ptrs.push(cast<ptr<i32>>(alloc(sizeof(i32))));
    ioPrintln("before: ", length(ptrs));
    ptrs.freeAll();
    ioPrintln("after: ", length(ptrs));
    return 0;
}
// before: 2
// after: 0

```

---

*[← Imports](@ref imports) | [Next: C Interop →](@ref c-interop)*
