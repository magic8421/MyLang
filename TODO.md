# MyLang Refcount / Codegen Issues

## Critical

### 1. Self-aliasing assignment is use-after-free  [FIXED]

- Generated old code: `((void)mylang_release(lhs), (lhs = rhs))`
- Repro: `b = b.set(5);` where `set` returns `this` -> segfault
- Fix: in `codegen_expr_stmt`, class-pointer assignments now use a temporary.
- Assignment is restricted to standalone statements and direct variable
  initializers. The parser rejects assignments in other contexts.

### 2. Cleanup list is function-global, not scope-aware  [FIXED]

- Added `cleanup_push_scope()` / `cleanup_pop_scope()`.
- Every block releases its own class locals on exit.
- `return` uses `cleanup_reset()` for early returns.

### 3. Call guards re-evaluate side-effecting non-local arguments  [FIXED]

- Guarded class subexpressions are extracted into typed temporaries once.
- Retain/release guards operate on temporaries instead of re-evaluating.
- Calls / `new` temporaries are released; borrowed temporaries are retained
  then released.

### 4. Class field and array element assignment skip refcounting  [FIXED]

- `codegen_expr` now handles any class-typed LHS.
- `emit_guarded_temp_decls` skips assignment LHS so temporaries do not replace
  the real lvalue.

## Open Design Questions

### 5. Class dynamic arrays use wrong header

Current problems:

- `mylang_new_array` stores `size_t count` before the payload.
- `mylang_release` expects `ObjHeader` (refcount) at the same offset.
- `new Box[N]` is treated as a class object, so cleanup calls `mylang_release`,
  which corrupts the header and never frees the array.
- `new int[N]` is never freed because `int[]` is not a class type and has no
  cleanup.

Proposed design:

- Arrays are **not** class objects. They have their own `size_t count` header
  and their own destructor `mylang_free_array(arr)`.
- A local array variable should be released with `mylang_free_array` at scope
  exit, not `mylang_release`.
- Class arrays should be arrays of **pointers**:
  - Allocation size: `sizeof(size_t) + N * sizeof(Class*)`
  - `arr[0] = new Box;` stores a `Box*` in the slot.
  - `arr[0].v` dereferences that pointer.
- When freeing a class-pointer array:
  - `mylang_release` every non-NULL element.
  - Then free the array block using the count header.
- Bounds checking should work for class arrays too.

### 6. Add explicit fixed-width primitive types

Plan to support value types that do not participate in reference counting:

- Unsigned: `u8`, `u16`, `u32`, `u64`
- Signed:   `i8`, `i16`, `i32`, `i64`
- Float:    `f32`, `f64`
- Existing: `int`, `char` (keep for compatibility)

Rules:

- Value types are copied by value.
- Local value-type variables do not enter the refcount cleanup list.
- Arrays of value types allocate `N * sizeof(T)` directly.
- No retain/release on value-type fields or array elements.
- Map to C types: `uint8_t`, `int32_t`, `float`, `double`, etc.

## Runtime Primitives

- `mylang_retain` / `mylang_release` are basically correct for class objects.
- Arrays need a separate lifecycle (`mylang_new_array` / `mylang_free_array`).
