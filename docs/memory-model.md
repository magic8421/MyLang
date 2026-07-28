# MyLang — Memory Model

Detailed reference; the rules in AGENTS.md still apply.

## Memory Model
- Heap objects use atomic reference counting via `ObjHeader`.
- `ObjHeader` holds `refcount`, `weak_count`, `type_id`, and a per-class destructor function pointer.
- Class instances are released through `mylang_release`; the destructor is invoked before the object's memory is freed.
- Arrays (`T[]`) are standalone value-type vectors with their own data buffer allocated via `malloc`/`realloc`/`free`; they are not reference-counted and are freed with `mylang_array_free`.
- `mylang_obj_hdr(ptr)` macro subtracts `sizeof(ObjHeader)` to get the header from a user-data pointer.

## Refcounting Rules
- **Callee retains at return**: class-valued returns go through `mylang_retain(expr)`. Caller receives +1.
- **Caller does NOT retain call results**: `codegen_var_decl` and assignment skip retain when RHS is `AST_CALL` or `AST_NEW`.
- **Caller retain/release for guarded args**: non-local class-valued arguments to calls are evaluated once into `_gN` temporaries. Owned guarded expressions (calls, `new`, string literals) are tracked on the cleanup list and released once at scope exit — this also covers if/while/for conditions and return paths where no post-call release could be emitted. Non-owned guarded expressions (e.g. field accesses) get a caller-side `mylang_retain` before and `mylang_release` after the call. Local variables and parameters are unguarded (optimization).
- **Guarded temp extraction**: guarded expressions are evaluated into `_gN` temporaries before the call to prevent double-evaluation. Expressions whose value ownership is consumed by the surrounding statement (variable initializers, return expressions, expression-statement roots, assignment RHS) are never cleanup-tracked. Nested owned subexpressions inside arguments (e.g. the object of an `as` cast or an interface method receiver like `w.lock()` / `make().area()`) are first hoisted into `_iN` temporaries so side-effecting calls are evaluated exactly once. The same guard/hoist treatment applies to owned struct call results whose struct has reference fields (`call_needs_guard` / `subexpr_needs_temp` check `struct_has_ref_fields`); their temps are cleanup-tracked through `cleanup_add_struct_dtor` instead of `mylang_release`.
- **Class assignment**: RHS retained before LHS released to avoid UAF on self-assignment (`b = b.set(5)`).
- **Class/interface fields own their values**: assigning a local or parameter to a class or interface field retains the source; the per-class destructor releases fields when the object is freed.
- **Weak fields own their weak shares**: weak class and weak interface fields release their weak share (`mylang_weak_release`) in the class destructor.
- **Array fields are freed by the destructor**: `T[]` fields are released with `mylang_array_free` when the containing object is freed.
- **Discarded class return**: `(void)mylang_release(call(...))` in expression statements.
- **This in methods**: retained on entry, released via cleanup. Name mapped to `thiz` in generated C.

## Weak References
- Syntax: `weak ClassName v = obj;` to declare, `v.lock()` to acquire.
- There is no separate control block: the weak count lives inside the object header (`ObjHeader::weak_count`), make_shared style. `WeakRef` is just a typedef of `ObjHeader`, and a weak variable holds a pointer to the header. Codegen only manipulates `WeakRef*` opaquely through the runtime functions.
- `weak_count` counts live weak shares plus one implicit share held by the object itself: it starts at 1 in `mylang_new_object` and `mylang_release` drops the implicit share after the strong count reaches zero. `weak_count == 0` therefore implies the destructor has already run, and the block is freed exactly once, by whichever release drops the count to zero.
- While any weak share is held, the object block stays allocated. This is what makes `mylang_lock` safe against a concurrent free; the trade-off is that the shallow object block (header + fields) is freed only when the last weak share dies (the destructor still runs immediately at strong-count zero).
- `mylang_lock(wr)`: CAS loop on `ObjHeader::refcount`. `refcount == 0` is the liveness test; a successful CAS from a positive value returns a retained (+1) strong pointer, otherwise NULL.
- `mylang_weak_init(ptr)`: one atomic inc of `weak_count` and returns the header pointer. No allocation and no installation race; the caller always holds a strong reference, so the object is alive while its share is taken.
- `mylang_weak_init_owned(ptr)`: weakifies an owned strong reference — takes a WeakRef share, then releases the strong reference (used when the statement consumes RHS ownership, e.g. weak array element assignment from a call result).
- `mylang_weak_copy(wr)`: increments `weak_count` (for weak-to-weak copy).
- `mylang_weak_release(wr)`: decrements `weak_count`; on zero it frees the object block (the destructor has already run).
- On `mylang_release` refcount drop to zero: run the destructor, then drop the implicit weak share and free the block if that was the last one.
- Threads: with atomic counts and implicit-share pinning, the refcount/weak protocol itself is thread-safe. Actual multithreading still needs language-level support (thread APIs, shared-variable semantics).
- Cleanup uses `CleanupEntry.is_weak` to dispatch to `mylang_weak_release` vs `mylang_release`.
- Strong-to-weak parameter conversion is automatic: codegen wraps the argument in `mylang_weak_init()`.

## Unowned References
- Syntax: `unowned ClassName v = obj;` to declare. No `lock()`: the reference is used directly like a strong one (`u.field`, `u.method()`).
- Semantics (like Swift `unowned(safe)`): non-owning, but every read is checked — reading a dead reference calls `my_panic("unowned reference to dead object")`; reading a never-initialized one panics with `"unowned reference is null"`. Memory-safe: the share pins the object block, so the check can never touch freed memory.
- Representation: identical to weak — the variable holds a `WeakRef*` (header pointer) and takes a share of the same `ObjHeader::weak_count`. All share management (init, copy, release, cleanup, field destructors) reuses the weak machinery.
- Reads are wrapped by `codegen_expr` with `mylang_unowned_check(...)`, which validates and returns the user pointer. Weak/unowned share-management paths (copies, weakifying, assignment targets) suppress the wrapper via `codegen_expr_raw` (`CodegenContext.no_unowned_check`).
- Conversions: strong -> unowned via `mylang_weak_init` (implicit on init/assignment/argument); unowned -> unowned via `mylang_weak_copy`; unowned -> strong is allowed and emits check + retain (e.g. `Node s = u;`, strong parameters, `return u;` where the return type is a strong class). `unowned Node u = w.lock();` works through the owned-RHS path.
- Restrictions: class types only (no interfaces, no primitives); local declarations require an initializer; no `unowned` return types; no unowned arrays; `new unowned` is rejected; `.lock()` on unowned is a compile error; `match` on an unowned expression is rejected (convert to strong first); `as unowned T` is rejected.
- Choosing: use `weak` when the object dying first is a normal runtime event the code must handle (lock returns NULL); use `unowned` when it would be a structural bug and a panic is the right failure. Under future threads the check is exact in single-threaded code but only a backstop under data races — real concurrent lifetime pinning still requires `weak` + `lock()`.

## Runtime Functions
- `mylang_new_object(sz, type_id)` — allocates ObjHeader + data, refcount=1.
- `mylang_array_free(a, elem_size, elem_kind)` — releases elements according to kind and frees the data buffer.
- `mylang_array_at(a, idx, elem_size)` — bounds-checked pointer to element.
- `mylang_array_reserve(a, new_capacity, elem_size)` / `mylang_array_resize(a, new_length, elem_size, elem_kind)` / `mylang_array_move(src, dst, elem_size, elem_kind)` / `mylang_array_copy(src, dst, elem_size, elem_kind)` — vector manipulation.
- `mylang_array_push(a, elem_size, elem_kind, value)` / `mylang_array_pop(a, elem_size, elem_kind)` / `mylang_array_clear(a, elem_size, elem_kind)` / `mylang_array_compact(a, elem_size)` — vector methods.
- `mylang_retain(ptr)` / `mylang_release(ptr)` — atomic inc/dec on refcount for class/interface objects.
- `mylang_print_string(String* s)` — writes the string to stdout plus one trailing newline (println semantics; a bare LF, the CRT text mode turns it into CRLF).  Exposed to MyLang as the builtin `print(string)` function.
- `mylang_assert_failed(line, msg)` — backs the builtin `assert(cond)`: sets `__my_line` to the assert's source line and calls `my_panic`. `assert` is special-cased in `codegen_call` (not registered in symtab), accepts any truthy expression as its single argument, and emits `((cond) ? (void)0 : mylang_assert_failed(line, "assertion failed"))`. It is always on (no release-mode elision), and a user-defined function named `assert` shadows the builtin.
- `mylang_hash_u64/f64/string/ptr` — back the builtin `hash(x)`, which yields `u64` and is special-cased in `codegen_call` like `assert` (a user-defined `hash` shadows it). Dispatch is by the argument's type: a class implementing `IHashable` is hashed through its own `hash()` method; bool and integer types mix bits via a splitmix64 finalizer, `f32`/`f64` mix their bit pattern (`-0.0` hashes like `+0.0`), `string` uses FNV-1a over the raw bytes (content hash, so two equal literals hash equal), and other class/interface/object values use identity (pointer) hashing. Structs and arrays are compile errors; weak/unowned references must be `lock()`ed first.
- The builtin `equals(a, b)` (also special-cased, yields `bool`) mirrors the dispatch: primitives compare with `==`, strong strings use `String_equals`, a class implementing `IHashable` calls its `equals(object)`, and other class/interface/object values use identity comparison. Structs, arrays, and weak/unowned references are compile errors; a value/reference category mismatch between the two arguments is rejected ("cannot compare 'x' with 'y'").
- Platform atomics: `Interlocked*` (MSVC) or `atomic_fetch_*` (GCC/Clang). CAS macro provided for weak ref lock.

## Class Field Destructors
- Every concrete class has a compiler-generated finalizer that runs when the last reference to an object is released.
- The finalizer releases class, interface, weak, and array fields so they are no longer leaked.
- Circular references between objects still leak (there is no cycle collector); this is intentional and documented by the `circular_ref_leak` test.
