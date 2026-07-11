# MyLang Refcount / Codegen Issues

## Critical

### 1. Self-aliasing assignment is use-after-free  [FIXED]

- Generated old code: `((void)mylang_release(lhs), (lhs = rhs))`
- Repro: `b = b.set(5);` where `set` returns `this` -> segfault
- Fix: in `codegen_expr_stmt`, class-pointer assignments now use a temporary:
  ```c
  void* _my_assign_0 = Box_set(b, 5);
  mylang_release(b);
  b = _my_assign_0;
  ```
- Assignment is now restricted to standalone statements and direct variable
  initializers. The parser rejects assignments in `if`/`while` conditions,
  call arguments, return expressions, array indices, `new` array sizes, and
  other nested expression contexts, so the old unsafe codegen path is no
  longer reachable there.

### 2. Cleanup list is function-global, not scope-aware  [FIXED]

- Class locals inside `if` / `while` blocks are released at function end
- Repro: `if (1) { Box b = new Box; }` -> C compile error: `b` undeclared
- Repro: `while (...) { Box b = new Box; }` -> duplicate cleanup entries -> leak + double free
- Fix: added `cleanup_push_scope()` / `cleanup_pop_scope()`; every block (function,
  method, `if`/`while` body) now releases its own class locals on exit.
- `return` statements use `cleanup_reset()` so early returns still clean up all
  enclosing scopes correctly.

### 3. Call guards re-evaluate side-effecting non-local arguments  [FIXED]

- `use(a.get())` used to generate `Box_get(a)` three times
- Caused wrong behavior / leaks when calls had side effects or returned new objects
- Fix: in `codegen_expr_stmt`, guarded class subexpressions are now evaluated into
  typed temporaries once before the call. Retain/release guards operate on the
  temporaries instead of re-evaluating the original expression. Calls / `new`
  temporaries are released (they own a +1 reference); borrowed temporaries are
  retained then released.

### 4. Class field and array element assignment skip refcounting

- `list.head = new Node;` leaks old value and new Node
- `arr[0] = new Box;` leaks element
- Fix: handle `AST_MEMBER_ACCESS` / `AST_ARRAY_ACCESS` LHS in assignment

### 5. Class dynamic arrays use wrong header

- `mylang_new_array` stores `size_t count`; `mylang_release` expects `ObjHeader`
- `new Box[N]` leaks the array and corrupts refcount semantics
- Fix: separate array allocation or per-element headers

## Medium

6. `mylang_release` does not null the pointer
   - After release the variable still holds the old address; later access is UAF

7. Non-MSVC atomic macros are incorrect
   - `atomic_fetch_add` on `volatile long` is not valid C11 atomic
   - Use `_Atomic long` / `atomic_long`

8. `mylang_new_array` multiplication overflow not checked
   - `count * elem_size` can overflow

## Already Fixed (but still mentioned in note.txt)

9. `return r->x;` with local class `r`
   - Current codegen evaluates return value first, then releases `r`

## Runtime Primitives

- `mylang_retain` / `mylang_release` themselves are basically correct
- The bugs are mostly in how codegen emits calls to them
