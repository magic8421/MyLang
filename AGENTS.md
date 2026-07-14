# Agent Notes for MyLang

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

## Type System
- Primitives: `i8/i16/i32/i64`, `u8/u16/u32/u64`, `f32/f64`, `void`.
- User types: `class` (heap/reference), `struct` (value/stack), and `interface` (fat pointer).
- Type IDs: primitives use 0-15; classes, structs, and interfaces share a counter starting at 16.
- Flags: `TYPE_IS_ARRAY = 0x80000000`, `TYPE_IS_STRUCT = 0x40000000`, `TYPE_IS_WEAK = 0x20000000`, `TYPE_IS_INTERFACE = 0x10000000`.
- `Type` struct fields: `type_kind`, `class_name[64]`, `is_pointer`, `is_array`, `array_size`, `is_ref`, `is_weak`, `type_id`.
- `out` and `in` modifiers were removed (only `ref` remains).
- Interface types have `type_kind = TYPE_INTERFACE`, `is_pointer = 0`. The C type is a fat pointer struct (two pointers), not a raw pointer.

## Interface System (Phase 1)
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
- Empty classes/structs emit `char _pad;` placeholder for MSVC compatibility (C requires at least one struct member).
- `g_return_type` (static) tracks the enclosing function's return type so `codegen_return_stmt` can emit implicit class-to-interface conversion.
- Interface parameters pass by value (struct copy). Caller-side guard extraction handles complex expressions.
- `as` keyword is parsed in `parse_postfix` as a postfix operator.

## Reference Parameters
- Only the `ref` keyword is supported.
- `ref T p` means the parameter aliases a caller variable.
- At the call site the argument must be a local variable.
- Codegen emits `&var` for normal locals and bare `var` when the argument itself is already a `ref` parameter.

## Struct Value Types (Phase 1)
- Structs are stack-allocated value types; assignment copies the whole struct.
- `new StructName` is illegal.
- `new StructName[N]` creates a dynamic array of structs.
- `StructName[N]` creates a fixed-size array.
- Struct fields are restricted to primitive types only.
- The runtime uses `TYPE_IS_STRUCT` to avoid treating struct arrays as class pointer arrays during release.

## Memory Model
- Heap objects use atomic reference counting via `ObjHeader`.
- `ObjHeader` holds `refcount`, `type_id`, `length`, and `WeakRef* weak`.
- Dynamic arrays and class instances are released through `mylang_release`.
- `mylang_obj_hdr(ptr)` macro subtracts `sizeof(ObjHeader)` to get the header from a user-data pointer.

## Refcounting Rules
- **Callee retains at return**: class-valued returns go through `mylang_retain(expr)`. Caller receives +1.
- **Caller does NOT retain call results**: `codegen_var_decl` and assignment skip retain when RHS is `AST_CALL` or `AST_NEW`.
- **Caller retain/release for guarded args**: non-local class-valued arguments to calls get caller-side `mylang_retain` before and `mylang_release` after the call. Local variables and parameters are unguarded (optimization).
- **Guarded temp extraction**: guarded expressions are evaluated into `_gN` temporaries before the call to prevent double-evaluation.
- **Class assignment**: RHS retained before LHS released to avoid UAF on self-assignment (`b = b.set(5)`).
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

## Cleanup System
- `CleanupEntry` array tracks class/weak variables requiring release at scope exit.
- Scope-based push/pop (`cleanup_push_scope`, `cleanup_pop_scope`) emits releases in reverse declaration order.
- `cleanup_emit` in return statements releases all variables on the return code path.
- `cleanup_reset` was removed: it zeroed `cleanup_scope_depth`, corrupting state for fallthrough code paths and causing memory leaks.
- Subsequent `cleanup_pop_scope` calls after returns generate dead code (harmless — after `return` in C, function exits immediately).

## Bounds Checking
- Dynamic arrays use runtime bounds checks via `mylang_check_bounds()` in getter/setter helpers.
- Fixed-size arrays use compile-time constant bounds with `MY_CHECK` at the point of access.
- Out-of-bounds triggers `my_panic` which calls `abort()`.

## Method Dispatch
- Methods name-mangled: `ClassName_method(ClassName* thiz, ...)`.
- `this` in MyLang source emits as `thiz` in C to avoid C++ keyword conflict.
- `p.foo(args)` emits as `ClassName_foo(p, args)`.
- Class name registered early in symtab for self-referential method return types.

## Runtime Functions
- `mylang_new_object(sz, type_id)` — allocates ObjHeader + data, refcount=1.
- `mylang_new_array(count, elem_size, type_id)` — allocates ObjHeader + array data.
- `mylang_retain(ptr)` / `mylang_release(ptr)` — atomic inc/dec on refcount.
- `array_get_*` / `array_set_*` / `array_get_class` / `array_replace_class` — bounds-checked dynamic array access.
- `array_get_struct_ptr` — returns pointer to struct element in dynamic array.
- Platform atomics: `Interlocked*` (MSVC) or `atomic_fetch_*` (GCC/Clang). CAS macro provided for weak ref lock.

## Memory Leak Debugging
- Enable at compile time with `mylang --leak-check source.my out.c`.
- Enable in the test runner with `python test_runner.py --leak-check`.
- Tracks only `ObjHeader` based allocations (class instances and dynamic arrays). WeakRef control blocks are not tracked.
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
- Weak references cannot be used as class fields (local variables and parameters only).
- `lock()` is a pseudo-method on weak refs; not a general keyword.
- Weak refs cannot be declared in if/while conditions (no `if (Node s = w.lock())`).
- Interface variables cannot be class fields (local variables and parameters only).
- `weak InterfaceName` variables not yet supported (Phase 2).
- Interface default method implementations not yet supported.
- No AST deallocation function (one-shot compiler).
