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

### 5. Class dynamic arrays use wrong header  [FIXED]

Fix summary:

- `ObjHeader` now stores `refcount`, `type_id`, and `length`.
- `type_id` uses bit `0x80000000` for `TYPE_IS_ARRAY`; primitive IDs are 0-15
  and class IDs start at 16.
- `mylang_new_array(count, elem_size, type_id)` creates a single heap object
  with a proper `ObjHeader`.
- `mylang_release` is now generic: when the header's `type_id` has the array
  bit, it iterates non-NULL class-pointer elements and releases them before
  freeing the array block.
- Class dynamic arrays are arrays of pointers (`Box**` in C). Element reads use
  `array_get_class`, element writes use `array_replace_class`, which releases
  the old element and stores the new one.
- Primitive dynamic arrays use `array_get_T` / `array_set_T` inline helpers that
  perform bounds checks without re-evaluating side-effecting index expressions.
- Local array variables enter the cleanup list and are released with
  `mylang_release` at scope exit.

### 6. Add explicit fixed-width primitive types  [FIXED]

Implemented value types that do not participate in reference counting:

- Unsigned: `u8`, `u16`, `u32`, `u64`
- Signed:   `i8`, `i16`, `i32`, `i64`
- Float:    `f32`, `f64`
- Removed legacy `int` and `char` keywords; use `i32` and `i8` instead.

Rules:

- Value types are copied by value.
- Local value-type variables do not enter the refcount cleanup list.
- Arrays of value types allocate `N * sizeof(T)` directly.
- No retain/release on value-type fields or array elements.
- Map to C types: `uint8_t`, `int32_t`, `float`, `double`, etc.

### 7. C#-style ref/out parameters  [FIXED]

Implemented pass-by-reference for local variables only.

Syntax:

- `void f(ref i32 x)` receives a pointer to a local `i32`.
- `void f(out i32 x)` receives a pointer to a local `i32`; the caller does not
  need to initialize it.

Codegen rules:

- `ref` / `out` parameters are emitted as C pointers (`T* name`).
- Reading or writing a ref/out parameter dereferences the pointer.
- A call argument matching a ref/out parameter must be a local variable
  identifier.  Normal locals are passed as `&var`; an argument that is itself
  a ref/out parameter is passed as its raw pointer.
- `void` is accepted as a function return type.

## Runtime Primitives

- `mylang_retain` / `mylang_release` are basically correct for class objects.
- Arrays need a separate lifecycle (`mylang_new_array` / `mylang_free_array`).
