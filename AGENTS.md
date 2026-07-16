# Agent Notes for MyLang

## Communication
- During long-running tasks, occasionally give a brief progress update on what you are currently doing and any issues encountered.

## Comment Style
All source comments must be written in plain English.
Do not use emojis or non-ASCII characters in comments or identifiers.
Keep everything ASCII.

## Naming Conventions
- Struct member names should be descriptive and include an abbreviation of the struct name as a prefix.
- Example: `Type.type_kind`, `AstNode.ast_kind`, `AstNode.ast_resolved_type`, `AstNode.ast_token`, `AstNode.ast_children`, `AstNode.ast_child_count`, `AstNode.ast_temp_name`.
- Avoid one-word member names like `kind`, `tok`, or `children` on public structs.

## Build & Test
- Windows MSVC build: run `build.bat` (it calls `vcvars64.bat`).
- The compiler itself is built with AddressSanitizer (`/fsanitize=address`).
- Build artifacts go to `build/` directory (mylang.exe, .obj, .pdb, ASan DLL).
- Run the test suite with: `python test_runner.py`.
- Test executables go to `build/test/`.
- Generated C files are compiled with `cl /std:c11`.

## Source Layout
Source code lives under `src/`:
- `token.c/h`, `lexer.c`
- `ast.c/h`, `parser.c/h`
- `symtab.c/h`, `codegen.c/h`
- `main.c`, `util.h`

## Codegen Conventions
- `CodegenContext` holds the output stream in `ctx->out`. Helper functions in `codegen.c` do not take a separate `FILE*` parameter.
- The current source line is tracked in the thread-local `__my_line` variable. The compiler emits `MY_LOC(line)` before expressions that may trigger runtime panics (e.g., array access) so `my_panic` can report the offending line.

## Type System
- Primitives: `i8/i16/i32/i64`, `u8/u16/u32/u64`, `f32/f64`, `void`.
- User types: `class` (heap/reference), `struct` (value/stack), and `interface` (fat pointer).
- Type IDs: primitives use 0-15; classes, structs, and interfaces share a counter starting at 16.
- Flags: `TYPE_IS_ARRAY = 0x80000000`, `TYPE_IS_STRUCT = 0x40000000`, `TYPE_IS_WEAK = 0x20000000`, `TYPE_IS_INTERFACE = 0x10000000`.
- `Type` struct fields: `type_kind`, `class_name[64]`, `is_pointer`, `is_array`, `array_size`, `is_ref`, `is_weak`, `type_id`.
- `T[]` is a value-type vector (`MyArray`), not reference-counted and not copyable by assignment. Transfer is explicit via `move_to(ref)` and `copy_to(ref)`.
- `T[] a;` declares a zero-initialized empty vector; `a.push(x)` will auto-grow from capacity 0.
- `out` and `in` modifiers were removed (only `ref` remains).
- Interface types have `type_kind = TYPE_INTERFACE`, `is_pointer = 0`. The C type is a fat pointer struct (two pointers), not a raw pointer.

## Compound Assignment Operators
- Supported: `+=`, `-=`, `*=`, `/=`.
- Only primitive numeric types (`i8/i16/i32/i64`, `u8/u16/u32/u64`, `f32/f64`) support compound assignment.
- Class, interface, weak, struct, and array types are rejected with a compile-time error.
- `x += y` is generated as `x = x + y`; the left-hand side is evaluated twice, which is safe for primitives but disallowed for non-primitives.
- Like simple assignment (`=`), compound assignment is an expression and is not allowed in `if`/`while` conditions, variable initializers, or `return` expressions.

## Increment / Decrement Operators
- Supported as standalone statements: `x++`, `++x`, `x--`, `--x`.
- Operand may be a local variable, a member access (`obj.field++`), or an array element (`arr[i]++`), including `ref` parameters.
- Only primitive numeric types support increment/decrement.
- `++`/`--` are parsed as expressions but are only legal as top-level expression statements; they are rejected in `if`/`while` conditions, variable initializers, `return` expressions, call arguments, and nested expressions (e.g. `y = x++`).
- In the allowed statement context, prefix and postfix forms have identical effect.

## For Loops
- C-style syntax: `for (init; condition; step) body`.
- `init` may be a variable declaration (`i32 i = 0`), an expression, or omitted.
- `condition` may be an expression or omitted; omitted condition means an infinite loop.
- `step` may be an expression or omitted.
- `body` is a single statement or a block.
- Variables declared in `init` are scoped to the loop (not visible after it) and are released on normal loop exit.
- Assignment and increment/decrement are not allowed in the `condition` expression.
- `break` and `continue` are not supported yet.

## Interface System (Phase 1 + Phase 2 weak interfaces)
- Syntax: `interface Name { method_sigs; }`, `class Foo : Iface1, Iface2 { ... }`.
- Multiple interfaces per class are supported via comma-separated `:` list.
- Interface values are fat pointers: `{void* data, const VTable* vtable}` struct in generated C.
- Each interface gets a VTable typedef containing `concrete_type_id` and one function-pointer field per method.
- Each class implementing an interface gets a static `const` vtable instance and a thunk function per method (casts `void*` to `ClassName*`, calls the real method).
- Dynamic dispatch: `s.area()` emits as `(s).vtable->area((s).data)`.
- Type assertion: `expr as ClassName` compares `vtable->concrete_type_id` against the class type_id, returns `(ClassName*)data` or `NULL`.
- Implicit class-to-interface conversion on variable init (`IShape s = obj`), assignment (`s = obj`), and return (`return obj` where return type is interface).
- Interface refcounting: the `.data` pointer holds a reference count. Create = retain, destroy = release `.data`. Cleanup uses `CleanupEntry.is_interface` flag.
- Semantic validation (`symtab_validate_impls`): verifies at compile time that a class declares all methods required by declared interfaces with matching signatures. Aborts codegen on error.
- `override` keyword may be used on class methods that override an interface method. When present, the compiler verifies that the method actually matches an interface method from one of the implemented interfaces. It is optional but checked.
- Empty classes/structs emit `char _pad;` placeholder for MSVC compatibility (C requires at least one struct member).
- `g_return_type` (static) tracks the enclosing function's return type so `codegen_return_stmt` can emit implicit class-to-interface conversion.
- Interface parameters pass by value (struct copy). Caller-side guard extraction handles complex expressions.
- `as` keyword is parsed in `parse_postfix` as a postfix operator.

## Weak Interface Types (Phase 2)
- Syntax: `weak InterfaceName v = obj;` to declare; `v.lock()` returns a strong interface fat pointer.
- Generated C type is `WeakIFoo { WeakRef* wr; IFooVTable* vt; }` per interface.
- Conversion helpers are emitted per interface: `mylang_lock_IFoo`, `mylang_weakify_IFoo`, `mylang_weakify_IFoo_owned`, `mylang_weakify_IFoo_from_ptr`, `mylang_weakify_IFoo_from_ptr_owned`.
- Initialization supports class instance, strong interface, and weak-to-weak copy.
- `lock()` returns `{ NULL, NULL }` if the object is dead; callers can check `result.data`.
- Dynamic arrays of weak interfaces (`weak IFoo[] arr = new weak IFoo[N];`) are supported; cleanup releases each element's `.wr`.
- Weak interface parameters pass by value and are released via cleanup on function exit.

## Reference Parameters
- Only the `ref` keyword is supported.
- `ref T p` means the parameter aliases a caller variable.
- At the call site the argument must be a local variable and the `ref` keyword is **required** (e.g. `inc(ref a)`, `fill(ref arr)`).
- Codegen emits `&var` for normal locals and bare `var` when the argument itself is already a `ref` parameter.

## Struct Value Types (Phase 1)
- Structs are stack-allocated value types; assignment copies the whole struct.
- `new StructName` is illegal.
- `new StructName[N]` creates a dynamic array of structs.
- Struct fields are restricted to primitive types only.
- The runtime uses `MYLANG_ELEM_STRUCT` to copy/release struct array elements correctly.

## Strings and f-strings
- `string` is a builtin class-like type backed by `String` in `runtime.h`; string literals compile to owned `String*` objects via `mylang_string_new`.
- `String` is mutable and owns the native append API: `append_string`, `append_i32/i64/u32/u64/f32/f64`, `append_char`.  There is no separate `StringBuilder` type; appending through one alias is visible through all aliases (same as any other class), and there is no copy-on-write.
- `IToString` is a builtin interface (`string toString()`); classes implementing it can be interpolated in f-strings.
- f-strings `f"...{expr}..."` are lowered by the parser into an `AST_FSTRING` node containing ordered parts (string literals and expression nodes).
- Codegen emits a temporary `String` accumulator (`_fsN`, tracked by the cleanup list), appends each part, and uses the accumulator itself as the expression value; literal segments go through `mylang_string_append_cstr` without allocating a temporary `String`.  Each interpolated expression is evaluated exactly once and in source order.
- Interpolation dispatch: `string` and primitive types map to `String_append_*`; class types require a `string toString()` method (IToString) which is called directly; interface values dispatch through the vtable.
- Floating-point numeric literals are supported: `3.14` defaults to `f64`, and
  `1.5f` / `1.5F` are `f32`.  They can be used directly in f-strings (`{3.14}`)
  and in arithmetic.
- Escaped braces (`\{`, `\}`, `{{`, `}}`) are not yet supported; `{` always starts an interpolation expression.

## Memory Model
- Heap objects use atomic reference counting via `ObjHeader`.
- `ObjHeader` holds `refcount`, `type_id`, `WeakRef* weak`, and a per-class destructor function pointer.
- Class instances are released through `mylang_release`; the destructor is invoked before the object's memory is freed.
- Arrays (`T[]`) are standalone value-type vectors with their own data buffer allocated via `malloc`/`realloc`/`free`; they are not reference-counted and are freed with `mylang_array_free`.
- `mylang_obj_hdr(ptr)` macro subtracts `sizeof(ObjHeader)` to get the header from a user-data pointer.

## Refcounting Rules
- **Callee retains at return**: class-valued returns go through `mylang_retain(expr)`. Caller receives +1.
- **Caller does NOT retain call results**: `codegen_var_decl` and assignment skip retain when RHS is `AST_CALL` or `AST_NEW`.
- **Caller retain/release for guarded args**: non-local class-valued arguments to calls are evaluated once into `_gN` temporaries. Owned guarded expressions (calls, `new`, string literals) are tracked on the cleanup list and released once at scope exit — this also covers if/while/for conditions and return paths where no post-call release could be emitted. Non-owned guarded expressions (e.g. field accesses) get a caller-side `mylang_retain` before and `mylang_release` after the call. Local variables and parameters are unguarded (optimization).
- **Guarded temp extraction**: guarded expressions are evaluated into `_gN` temporaries before the call to prevent double-evaluation. Expressions whose value ownership is consumed by the surrounding statement (variable initializers, return expressions, expression-statement roots, assignment RHS) are never cleanup-tracked. Nested owned subexpressions inside arguments (e.g. the object of an `as` cast or an interface method receiver like `w.lock()` / `make().area()`) are first hoisted into `_iN` temporaries so side-effecting calls are evaluated exactly once.
- **Class assignment**: RHS retained before LHS released to avoid UAF on self-assignment (`b = b.set(5)`).
- **Class/interface fields own their values**: assigning a local or parameter to a class or interface field retains the source; the per-class destructor releases fields when the object is freed.
- **Weak fields own their WeakRef control blocks**: weak class and weak interface fields release their `WeakRef` in the class destructor.
- **Array fields are freed by the destructor**: `T[]` fields are released with `mylang_array_free` when the containing object is freed.
- **Discarded class return**: `(void)mylang_release(call(...))` in expression statements.
- **This in methods**: retained on entry, released via cleanup. Name mapped to `thiz` in generated C.

## Weak References (Implemented)
- Syntax: `weak ClassName v = obj;` to declare, `v.lock()` to acquire.
- Each object has at most ONE `WeakRef` control block, stored in `ObjHeader::weak` (O(1) access).
- `WeakRef` holds `{volatile long refcount, ObjHeader* obj}`. refcount enables sharing across multiple weak variables.
- `mylang_lock(wr)`: CAS loop on `ObjHeader::refcount`. Returns retained (+1) strong pointer, or NULL if object is dead.
- `mylang_weak_init(ptr)`: creates or reuses the WeakRef for an object.
- `mylang_weak_copy(wr)`: increments WeakRef.refcount (for weak-to-weak copy).
- `mylang_weak_release(wr)`: decrements WeakRef.refcount, frees on zero, sets `obj->weak = NULL`.
- On `mylang_release` refcount drop to zero: `h->weak->obj = NULL` (O(1) single assignment) before `free(h)`.
- Cleanup uses `CleanupEntry.is_weak` to dispatch to `mylang_weak_release` vs `mylang_release`.
- Strong-to-weak parameter conversion is automatic: codegen wraps the argument in `mylang_weak_init()`.

## Array / Vector Value Types
- `T[]` compiles to the C value type `MyArray { size_t capacity; size_t length; void* data; }`.
- The vector is not reference-counted; its data buffer is allocated with `malloc`/`realloc` and freed with `free` via `mylang_array_free`.
- Arrays are created empty: `T[] a;` initializes `capacity = length = 0` and `data = NULL`. There is no `new T[N]` syntax.
- To preallocate capacity use `a.reserve(n)`; to set the initial length use `a.resize(n)`.
- Arrays cannot be returned by value, passed by value, or assigned with `=`. Use `ref T[]` parameters for mutation and `move_to(ref dst)` / `copy_to(ref dst)` for transfer or duplication.
- Builtin vector methods: `.push(v)`, `.pop()`, `.reserve(n)`, `.resize(n)`, `.clear()`, `.compact()`, `.move_to(ref dst)`, `.copy_to(ref dst)`.
- Element access uses `arr[i]` and is bounds-checked at runtime via `mylang_array_at()`.

## Cleanup System
- `CleanupEntry` array tracks class/weak/array variables requiring release at scope exit.
- Scope-based push/pop (`cleanup_push_scope`, `cleanup_pop_scope`) emits releases in reverse declaration order.
- `cleanup_emit` in return statements releases all variables on the return code path.
- `cleanup_reset` was removed: it zeroed `cleanup_scope_depth`, corrupting state for fallthrough code paths and causing memory leaks.
- Subsequent `cleanup_pop_scope` calls after returns generate dead code (harmless — after `return` in C, function exits immediately).

## Bounds Checking
- Arrays use runtime bounds checks via `mylang_array_at()`.
- Out-of-bounds triggers `my_panic` which calls `abort()`.

## Method Dispatch
- Methods name-mangled: `ClassName_method(ClassName* thiz, ...)`.
- `this` in MyLang source emits as `thiz` in C to avoid C++ keyword conflict.
- `p.foo(args)` emits as `ClassName_foo(p, args)`.
- Class name registered early in symtab for self-referential method return types.

## Native Methods
- Syntax: `native RetType ClassName.method(Params);` inside a class. Method body is omitted and ends with `;`.
- The compiler generates a header (`<out>.h`) containing the method prototype using the MyLang mangled name: `RetType ClassName_method(ClassName* thiz, ...)`.
- The user provides a matching C implementation in a separate `.c` file (e.g. `#include "out.h"`) to call platform APIs.
- Native methods follow the same ABI as regular methods: class/interface return values must be retained by the callee (`mylang_retain`) before returning; parameters are borrowed for the duration of the call.
- `ref T` parameters become `T*`, `weak T` becomes `WeakRef*`, interfaces pass by value as their fat-pointer struct, and arrays `T[]` pass by value as `MyArray`.
- Generated `.c` files `#include` the generated header, so prototypes do not need to be repeated.
- Native methods are not automatically wrapped with `MY_PUSH`/`MY_POP` in phase 1; if a native implementation calls `my_panic`, the stack trace may stop at the MyLang caller.

## Runtime Functions
- `mylang_new_object(sz, type_id)` — allocates ObjHeader + data, refcount=1.
- `mylang_array_free(a, elem_size, elem_kind)` — releases elements according to kind and frees the data buffer.
- `mylang_array_at(a, idx, elem_size)` — bounds-checked pointer to element. The current source line is tracked via `MY_LOC(line)` and reported by `my_panic` on out-of-bounds access.
- `mylang_array_reserve(a, new_capacity, elem_size)` / `mylang_array_resize(a, new_length, elem_size, elem_kind)` / `mylang_array_move(src, dst, elem_size, elem_kind)` / `mylang_array_copy(src, dst, elem_size, elem_kind)` — vector manipulation.
- `mylang_array_push(a, elem_size, elem_kind, value)` / `mylang_array_pop(a, elem_size, elem_kind)` / `mylang_array_clear(a, elem_size, elem_kind)` / `mylang_array_compact(a, elem_size)` — vector methods.
- `mylang_retain(ptr)` / `mylang_release(ptr)` — atomic inc/dec on refcount for class/interface objects.
- `mylang_print_string(String* s)` — writes the string to stdout.  Exposed to MyLang as the builtin `print(string)` function.
- Platform atomics: `Interlocked*` (MSVC) or `atomic_fetch_*` (GCC/Clang). CAS macro provided for weak ref lock.

## Memory Leak Debugging
- During compiler development, when need verify no memory leak, run the test suite in debug mode: `python test_runner.py --mode debug`.
- Tracks only `ObjHeader` based allocations (class instances and interface objects). Arrays and WeakRef control blocks are not tracked by the MyLang leak list.
- When enabled, the generated C code adds `next`/`prev`/`alloc_trace` to `ObjHeader` and records every allocation in a global circular doubly-linked list.
- `mylang_release` removes the block from the list before freeing it.
- On the first allocation, `atexit(mylang_leak_check)` is registered; at exit, unreleased blocks are printed with address, type_id, refcount, length, and allocation stack trace.
- Stack traces are hashed into a 512-bucket table so identical call stacks share one `LeakTrace` record.
- The list and hash table are protected by a global lock: `SRWLOCK` on Windows (`SRWLOCK_INIT`), `pthread_mutex_t` with `PTHREAD_MUTEX_INITIALIZER` on POSIX.
- In `--mode debug` or when `--leak-check` is enabled, the test runner prints captured stdout/stderr so the CRT leak dump (`_CrtDumpMemoryLeaks`) or the MyLang leak report is visible.

## Lexer Safety
- `read_char_literal` has EOF guards after opening quote, after backslash, and before closing quote peek.
- Truncated or unterminated char literals return a dummy token instead of reading out of bounds.

## String Handling
- Custom `strscpy` in `util.h` guarantees null termination.
- `CHECK_STRSCPY` and `CHECK_SNPRINTF` macros abort on truncation.
- All `snprintf` and `strscpy` call sites are checked.

## Known Limitations
- `ref` arguments must be local variables; field/array-element arguments are rejected.
- `out`/`in` keywords were removed for simplicity.
- Arrays cannot be returned by value, passed by value, or assigned directly; use `ref T[]` parameters and `move_to(ref)` / `copy_to(ref)`.
- `lock()` is a pseudo-method on weak refs; not a general keyword.
- Weak refs cannot be declared in if/while conditions (no `if (Node s = w.lock())`).
- Interface default method implementations are now supported. Default bodies cannot use `this` and may only reference parameters, literals, and control flow.
- No AST deallocation function (one-shot compiler).

## Class Field Destructors
- Every concrete class has a compiler-generated finalizer that runs when the last reference to an object is released.
- The finalizer releases class, interface, weak, and array fields so they are no longer leaked.
- Circular references between objects still leak (there is no cycle collector); this is intentional and documented by the `circular_ref_leak` test.
