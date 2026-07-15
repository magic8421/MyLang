# Array / Header Refactor TODO

## Status

- [x] Phase 1: Array Independence
- [x] Phase 2: Return Restrictions + move_to/copy_to
- [x] Phase 3: Vector Methods
- [x] Phase 4: Remove Fixed Arrays
- [x] Phase 5: Documentation

测试结果：`python test_runner.py` 与 `python test_runner.py --mode debug` 均为 **108/108 passed**。

## Background

Currently `ObjHeader` is shared by class instances, dynamic arrays, and leak-check
bookkeeping.  Arrays are reference-counted through `ObjHeader`, which forces them
to share the same allocation model as classes.  This blocks:

- In-place `realloc` for vector growth (moving `ObjHeader` is unsafe under
  concurrent `retain`/`release`).
- Clear value semantics for arrays.
- First-class support for arrays of `weak` references without special-casing every
  operation.

## Final Design Decision

`T[]` becomes a **value-type dynamic vector** whose buffer is completely separate
from the `ObjHeader` reference-counting system.

### Rules

1. `T[]` is **not copyable by assignment**.
2. `T[]` is **not returnable by value** from functions.
3. Arrays are passed and mutated through `ref` parameters.
4. Explicit methods transfer or duplicate data:
   - `arr.move_to(ref dst)` — transfer ownership; `dst`’s old buffer is freed,
     `src` becomes empty.
   - `arr.copy_to(ref dst)` — deep copy; `dst`’s old buffer is freed.
5. Vector growth uses in-place `realloc` because no other array value can alias
   the same buffer.

### Why no return by value?

Returning a value-type array by value would copy `capacity`/`length`/`data`,
leaving two array values pointing at the same buffer.  If the caller then
`push`es, the buffer is `realloc`’d and the original `data` pointer becomes
dangling.  Forcing explicit `move_to(ref out)` avoids this entirely.

## Memory Layout

```c
typedef struct ObjHeaderTag {
    volatile long refcount;
    uint32_t type_id;
    WeakRef* weak;
    /* Leak-check fields are always present for class/interface allocations. */
    struct ObjHeaderTag* next;
    struct ObjHeaderTag* prev;
    struct LeakTraceTag* alloc_trace;
} ObjHeader;

typedef struct {
    size_t capacity;
    size_t length;
    void* data;
} MyArray;   /* name used in runtime; MyLang type is still T[] */
```

Runtime allocation:

```text
class instance:  [ObjHeader][class data]
array buffer:    [element0][element1]...[elementN]
```

The `MyArray` struct itself is a value type (lives on the stack or inside
another value).  It points to a standalone buffer allocated with `malloc` /
`realloc` / `free`.

## Element Categories

Array helpers are specialized by element category.  No function-pointer dispatch
is stored in the array header.

| Element category | C element type | Cleanup on free/resize-down |
|---|---|---|
| primitive (`i32`, `u8`, `f64`, ...) | value | none |
| struct value | value | none (structs are value-only) |
| class pointer | `void*` | `mylang_release` each element |
| interface fat pointer | `{void* data; VTable* vt;}` | `mylang_release(data)` |
| weak class | `WeakRef*` | `mylang_weak_release` |
| weak interface | `{WeakRef* wr; VTable* vt;}` | `mylang_weak_release(wr)` |

The compiler knows the element type at every array operation, so it emits the
right helper.  No C++-style template partial specialization is needed because
MyLang generics use monomorphization.

## Runtime API

```c
/* Allocation / lifecycle */
MyArray mylang_array_new(size_t count, size_t elem_size);
void    mylang_array_free(MyArray* a, size_t elem_size, int elem_kind);
void    mylang_array_reserve(MyArray* a, size_t new_capacity, size_t elem_size);
void    mylang_array_resize(MyArray* a, size_t new_length, size_t elem_size, int elem_kind);

/* Transfer / duplicate */
void mylang_array_move(MyArray* src, MyArray* dst, size_t elem_size, int elem_kind);
void mylang_array_copy(const MyArray* src, MyArray* dst, size_t elem_size, int elem_kind);

/* Access */
void* mylang_array_at(MyArray* a, size_t idx, size_t elem_size, const char* file, int line);

/* Vector methods */
void mylang_array_push(MyArray* a, size_t elem_size, int elem_kind, const void* value);
void mylang_array_pop(MyArray* a, size_t elem_size, int elem_kind);
void mylang_array_clear(MyArray* a, size_t elem_size, int elem_kind);
void mylang_array_compact(MyArray* a, size_t elem_size);

/* Element-kind constants for runtime dispatch */
#define MYLANG_ELEM_PRIMITIVE  0
#define MYLANG_ELEM_STRUCT     1
#define MYLANG_ELEM_CLASS      2
#define MYLANG_ELEM_INTERFACE  3
#define MYLANG_ELEM_WEAK_CLASS 4
#define MYLANG_ELEM_WEAK_IFACE 5
```

## Vector Methods

MyLang builtin methods on `T[]`:

```my
arr.push(v);       /* append */
arr.pop();         /* remove last element */
arr.reserve(n);    /* ensure capacity >= n */
arr.resize(n);     /* set length to n */
arr.clear();       /* length = 0 */
arr.compact();     /* capacity = length */
arr.move_to(ref dst);
arr.copy_to(ref dst);
```

`push`/`pop`/`resize` pick the element-category-specific path at compile time.

## Weak References in Arrays

```my
weak Box[] watchers = new weak Box[10];
watchers[0] = obj;            /* strong-to-weak conversion */
Box* s = watchers[0].lock();  /* lock() on weak array element */
```

Dynamic arrays of weak interfaces are also supported:

```my
weak IShape[] shapes = new weak IShape[4];
shapes[0] = sq;
IShape s = shapes[0].lock();
```

Array free/resize-down calls `mylang_weak_release` on every weak element.

## Function Return Restriction

The compiler rejects any function whose return type is an array:

```my
Box[] make() {        /* error: cannot return array by value */
    Box[] a = new Box[2];
    return a;
}

void make(ref Box[] out) {   /* OK */
    Box[] a = new Box[2];
    a.move_to(ref out);
}
```

## Fixed-Size Arrays

- [x] Fixed-size array syntax `T[N]` is removed entirely.
- [x] Fixed-array codegen branches removed.
- [x] Fixed-array tests deleted or rewritten.

All arrays are dynamic vectors.

## Leak Checking

Class/interface allocations continue to use `ObjHeader` leak-check tracking.
Array buffers are tracked separately.  During language implementation the debug CRT catches leaks;

## Implementation Phases

### Phase 1: Array Independence — [x] Done

- [x] Create standalone `MyArray` value type in `runtime.h`.
- [x] Implement `mylang_array_new/free/reserve/resize/move/copy` in `runtime.c`.
- [x] Change codegen so `T[]` is emitted as `MyArray`.
- [x] Update all array element access to use `mylang_array_at`.
- [x] Update cleanup code to call `mylang_array_free` instead of `mylang_release`.
- [x] Keep existing tests passing.

### Phase 2: Return Restrictions + move_to/copy_to — [x] Done

- [x] Add parser/codegen error for array return types.
- [x] Add parser error for non-`ref` array parameters.
- [x] Support explicit `ref` at call sites.
- [x] Implement `arr.move_to(ref dst)` and `arr.copy_to(ref dst)`.
- [x] Rewrite/delete tests that return arrays by value.

### Phase 3: Vector Methods — [x] Done

- [x] Implement `.push`, `.pop`, `.reserve`, `.resize`, `.clear`, `.compact`.
- [x] Add primitive/class/interface/weak specialized paths.
- [x] Add tests for each element category.

### Phase 4: Remove Fixed Arrays — [x] Done

- [x] Remove `T[N]` syntax from parser.
- [x] Remove fixed-array codegen branches.
- [x] Delete or rewrite fixed-array tests.

### Phase 5: Documentation — [x] Done

- [x] Update `AGENTS.md`:
  - `T[]` is a value-type dynamic vector.
  - Arrays cannot be returned by value.
  - Arrays use `move_to`/`copy_to` for transfer/duplication.
  - Remove fixed-array references.
- [x] Update `GRAMMA.md`.

## Testing Strategy

- [x] Run full `python test_runner.py` and `python test_runner.py --mode debug`.
- [x] Add negative test for returning array by value (covered by parser error).
- [x] Add tests for:
  - [x] `i32[]` push/pop/resize
  - [x] `Box[]` push/pop/resize with refcount checks
  - [x] `IShape[]` push/pop/resize
  - [x] `weak Box[]` / `weak IShape[]` push/pop/lock
- [x] Add negative test for `T[N]` syntax (covered by parser error).

## Risks

- Two separate memory-management paths (`ObjHeader` for classes, `MyArray` for
  arrays) increases the surface area for leaks and use-after-free.
- Changing the C representation of `T[]` touches almost every array code path in
  `codegen.c`.
- `ref T[]` parameter passing must be carefully updated because the C type is no
  longer a pointer to elements.
