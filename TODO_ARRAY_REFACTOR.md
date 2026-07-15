# Array / Header Refactor TODO

## Background

Currently `ObjHeader` is shared by both class instances and dynamic arrays:

```c
typedef struct ObjHeaderTag {
    volatile long refcount;
    uint32_t type_id;
    WeakRef* weak;
    size_t length;   /* only meaningful for arrays; wasted for classes */
} ObjHeader;
```

Every class instance pays for an unused `length` field.  Arrays also lack
capacity and element-size information, which blocks a proper `push`/`pop`/
`reserve` vector API.

## Goals

1. Split `ObjHeader` and `ArrayHeader` so classes do not carry array metadata.
2. Add `capacity` and `length` to `ArrayHeader`.  Do **not** store `elem_size`; the
   compiler passes it as an argument for primitive/struct arrays, and class/
   interface helpers are specialized.
3. Extract runtime helpers into a dedicated `runtime.c` file.
4. Remove fixed-size array syntax `T[N]` from the language.
5. Lay the groundwork for vector methods on `T[]`:
   - `.push(v)`
   - `.pop()`
   - `.reserve(n)`
   - `.resize(n)`
   - `.clear()`
   - `.compact()`

## Non-Goals

- Do not implement generic `Vector<T>` as a MyLang class (would add an extra
  pointer indirection).
- Do not keep fixed-size arrays; they are buggy for class/struct elements and
  limit expressiveness.
- Do not change weak-reference object layout; `WeakRef.obj` still points to an
  `ObjHeader*`.

## New Header Layout

```c
typedef struct ObjHeaderTag {
    volatile long refcount;
    uint32_t type_id;
    WeakRef* weak;
} ObjHeader;

typedef struct ArrayHeaderTag {
    ObjHeader base;      /* refcount, type_id, weak */
    size_t capacity;     /* allocated slots */
    size_t length;       /* used slots */
} ArrayHeader;
```

Memory layout:

```text
class instance:  [ObjHeader][class data]
array:           [ObjHeader][capacity][length][array data]
```

Leak-check fields (`next`, `prev`, `alloc_trace`) stay in `ObjHeader` because
both classes and arrays are tracked allocations.

`elem_size` is intentionally **not** stored in `ArrayHeader`.  The compiler
already knows the size of every element type:
- primitive: `sizeof(int32_t)`, etc.
- struct: `sizeof(Point)`
- class array: element is a pointer, `sizeof(void*)`
- interface array: element is a fat-pointer struct, `sizeof(IFoo)`

For primitive/struct helpers the compiler passes `elem_size` as a call argument;
class/interface helpers are specialized and do not need it.

## Design Decision: Runtime Dispatch vs. Compiler Specialization

Two ways to handle class/interface/weak/struct element lifetimes in array
operations:

### Option A: Function pointer in `ArrayHeader` (`cleanup_fn`)

Store a function pointer inside `ArrayHeader` that releases one element:

```c
typedef void (*ArrayReleaseFn)(void* elem);

typedef struct ArrayHeaderTag {
    ObjHeader base;
    size_t capacity;
    size_t length;
    size_t elem_size;
    ArrayReleaseFn release_elem;   /* how to release one slot */
} ArrayHeader;
```

Pros:
- Runtime is fully generic; one `mylang_array_push` covers all types.
- Easy to extend to new categories later.

Cons:
- One extra pointer per array (8 bytes).
- Indirect call for every element release.

### Option B: Compiler Specialization (Recommended)

The compiler emits calls to category-specific runtime helpers based on the
element type:

| Element category | Runtime helpers |
|---|---|
| primitive / struct value | `mylang_array_push_val`, `mylang_array_pop_val`, ... |
| class pointer | `mylang_array_push_class`, `mylang_array_pop_class`, ... |
| interface fat pointer | `mylang_array_push_iface`, `mylang_array_pop_iface`, ... |
| weak class | `mylang_array_push_weak`, `mylang_array_pop_weak`, ... |

Pros:
- No per-array function pointer overhead.
- Direct calls, easier for compiler to optimize.
- Primitive/struct paths can use plain `memcpy`.

Cons:
- More runtime functions to maintain.
- Codegen must pick the right helper.

**Decision: use Option B.**  The compiler already knows the element type, so it
should generate the specialized call.  Runtime keeps a small family of helpers
for each lifetime category.

## Phase 1: Header Split + Build Infrastructure

### 1.1 Create `src/runtime.c` / `src/runtime.h`

Move the following runtime helpers from `codegen.c` (where they are currently
emitted as strings) into real C source files:

- `mylang_new_object`
- `mylang_new_array`
- `mylang_retain`
- `mylang_release`
- `mylang_check_bounds`
- `array_get_*` / `array_set_*`
- `array_replace_class`
- `array_get_struct_ptr`
- `mylang_weak_init`, `mylang_weak_copy`, `mylang_weak_release`, `mylang_lock`
- Platform atomic wrappers
- `my_panic`
- Leak-check helpers (when enabled)

`codegen.c` will no longer print these definitions; it will only emit `#include
"runtime.h"` (or a generated equivalent).

Open question: should `runtime.h` be a real header in `src/`, or should the
test runner copy a pre-built `runtime.c` next to generated test files?  For now,
keep `runtime.c`/`runtime.h` in `src/` and `#include` them.

### 1.2 Update `test_runner.py` / `build.bat` / `Makefile`

- Compile `src/runtime.c` into the final executable alongside generated `out.c`.
- Ensure `--mode debug` / `--leak-check` flags are passed consistently.

### 1.3 Split `ObjHeader` / `ArrayHeader`

- Remove `length` from `ObjHeader`.
- Add `ArrayHeader` with `base`, `capacity`, `length`, `elem_size`.
- Update `mylang_new_object` to allocate `sizeof(ObjHeader) + sz`.
- Update `mylang_new_array` to allocate `sizeof(ArrayHeader) + count * elem_size`
  and set `capacity = length = count`.
- Update `mylang_obj_hdr` macro to still work for both.
- Update `mylang_release` to use `ArrayHeader*` inside the `TYPE_IS_ARRAY`
  branch.
- Update bounds checks to read `ArrayHeader->length`.
- Update struct array helper to receive `elem_size` as a compile-time argument.

### 1.4 Fix `type_id` flag usage

`TYPE_IS_ARRAY` (0x80000000) remains the discriminator.  `mylang_release`
checks this flag to decide whether to cast to `ArrayHeader*`.

## Phase 2: Remove Fixed-Size Arrays

### 2.1 Parser

- Remove `T[N]` syntax from `parse_type()`.
- Remove fixed-array branches from `stmt_looks_like_var_decl` if any.

### 2.2 Codegen

- Delete fixed-array handling in:
  - `codegen_class_decl` (field emission)
  - `codegen_var_decl`
  - `codegen_array_access`
  - assignment / cleanup code for fixed arrays
- Keep `MY_CHECK` macro for future bounds checks.

### 2.3 Tests

Identify and update tests that rely on `T[N]`:

- `bounds_fixed_oob` -> delete or rewrite with `T[]` + expected OOB crash
- `ref_array` -> uses `ref i32[]`, probably unaffected
- Any other fixed-array usage in `test_runner.py`

## Phase 3: Specialized Array Runtime Helpers

Add category-specific helpers in `runtime.c`.

### 3.1 Primitive / struct value arrays

```c
void mylang_array_push_val(void** arr_ptr, const void* elem, size_t elem_size);
int  mylang_array_pop_val(void* arr, void* out_elem, size_t elem_size);
void mylang_array_resize_val(void** arr_ptr, size_t new_length, size_t elem_size);
```

- Use `memcpy` for all element movement.
- For resize-down, just discard trailing bytes.
- Structs are value-only (no pointers), so no retain/release.

### 3.2 Class pointer arrays

```c
void mylang_array_push_class(void** arr_ptr, void* elem);
void* mylang_array_pop_class(void* arr);
void mylang_array_resize_class(void** arr_ptr, size_t new_length);
```

- `push`: retain new element, release overwritten element if any.
- `pop`: return element (caller owns it), or release if caller discards.
- `resize` down: release dropped elements.

### 3.3 Interface fat-pointer arrays

```c
void mylang_array_push_iface(void** arr_ptr, void* data, const void* vtable);
mylang_array_pop_iface_result mylang_array_pop_iface(void* arr);
void mylang_array_resize_iface(void** arr_ptr, size_t new_length);
```

- Similar to class but operate on `.data` of the fat pointer.

### 3.4 Weak reference arrays

```c
void mylang_array_push_weak_class(void** arr_ptr, WeakRef* wr);
WeakRef* mylang_array_pop_weak_class(void* arr);
```

- `weak class` and `weak interface` have their own helpers.
- Current AGENTS.md already says dynamic arrays of `weak InterfaceName` are not
  supported; this restriction can stay until needed.

### 3.5 Shared internals

All value-type helpers use a common
`mylang_array_ensure_capacity(void** arr_ptr, size_t needed, size_t elem_size)`
that reallocates when `capacity < needed`.  Class/interface helpers call a
similar helper with `elem_size = sizeof(void*)` or `sizeof(IFoo)`.

## Phase 4: Vector Methods on `T[]`

### 4.1 Parser / codegen

Treat `.push`, `.pop`, `.reserve`, `.resize`, `.clear`, `.compact` as
array-type builtin methods:

```my
i32[] arr = new i32[10];
arr.push(42);
i32 x = arr.pop();
arr.reserve(100);
arr.resize(200);
arr.clear();
arr.compact();
```

Code generation picks the category-specific runtime helper based on the
element type.

### 4.2 Return semantics

- `push(v)` -> `void`
- `pop()` -> element type (owned reference for class/interface)
- `reserve(n)` -> `void`
- `resize(n)` -> `void`
- `clear()` -> `void`
- `compact()` -> `void`

### 4.3 Reference counting at call sites

- `arr.push(new Widget)` -> new widget is owned, push retains it.
- `x = arr.pop()` -> caller receives owned ref; if discarded, release it.
- `arr.resize(0)` -> release all class/interface elements.

## Phase 5: Cleanup and Documentation

- Update `AGENTS.md`:
  - Remove fixed-array references.
  - Document that `T[]` is a dynamic vector.
  - Document `runtime.c`.
- Update `GENERICS_DESIGN.md` / `TODO_GENERICS.md`:
  - Mark `gen_array.my` as covering `T[]` vector methods.
  - Remove fixed-array references.
- Update `GRAMMA.md`:
  - Remove `T[N]`.
  - Add vector method syntax.

## Testing Strategy

1. After Phase 1: run full `python test_runner.py` and `python test_runner.py --mode debug`.
2. After Phase 2: verify no fixed-array syntax remains; add negative test for
   `T[N]` parse error.
3. After Phase 3: add tests for each element category:
   - `i32[]` push/pop/resize
   - `Widget[]` push/pop/resize with refcount checks
   - `IShape[]` push/pop/resize
   - `Point[]` (struct) push/pop
4. After Phase 4: add `gen_array.my` covering the full vector API.

## Risks

- The header split touches `mylang_release`, which is central to memory safety.
  Any mistake causes leaks or use-after-free.
- Moving runtime code from generated strings to `runtime.c` may break `--leak-check`
  conditional compilation; need careful handling of `#ifdef` blocks.
- Removing fixed arrays will delete some existing tests and may reduce coverage
  unless replaced.

## Open Questions

1. Should `runtime.c` be compiled into every test binary, or linked as a static
   library?
2. Should `.pop()` return an owned reference for class/interface, or should it
   return a borrowed reference and require the caller to retain if needed?
