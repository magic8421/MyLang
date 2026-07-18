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

## Error Message Format
- All compiler diagnostics are emitted as `path(line,col): error: message`
  (MSVC / VS Code problem-matcher style).
- Lexer/parser errors use `Lexer.filename` (set from `src_path` in `src/main.c`)
  and `parser_filename(p)` in `src/parser.c`.
- Codegen errors use `CodegenContext.source_file` via the `codegen_report_error()`
  helper in `src/codegen.c`.
- The printed path is the input path as given on the command line (relative if
  the user passed a relative path).

## Codegen Conventions
- `CodegenContext` holds the output stream in `ctx->out`. Helper functions in `codegen.c` do not take a separate `FILE*` parameter.
- The current source line is tracked in the thread-local `__my_line` variable. The compiler emits `MY_LOC(line)` before expressions that may trigger runtime panics (e.g., array access) so `my_panic` can report the offending line.

## Type System
- Primitives: `i8/i16/i32/i64`, `u8/u16/u32/u64`, `f32/f64`, `bool`, `void`.
- User types: `class` (heap/reference), `struct` (value/stack), and `interface` (fat pointer).
- Type IDs: primitives use 0-15 (`bool` = 11, `object` = 12); classes, structs, and interfaces share a counter starting at 16.
- `TYPE_NULL` is a compile-time-only `TypeKind` for the `null` literal; it has no runtime type_id.
- `TYPE_OBJECT` is the top reference type (`void*` in C); see "The object Type" section.
- Flags: `TYPE_IS_ARRAY = 0x80000000`, `TYPE_IS_STRUCT = 0x40000000`, `TYPE_IS_WEAK = 0x20000000`, `TYPE_IS_INTERFACE = 0x10000000`, `TYPE_IS_UNOWNED = 0x08000000`.
- `Type` struct fields: `type_kind`, `class_name[64]`, `is_pointer`, `is_array`, `array_size`, `is_ref`, `is_weak`, `is_unowned`, `type_id`.
- `T[]` is a value-type vector (`MyArray`), not reference-counted and not copyable by assignment. Transfer is explicit via `move_to(ref)` and `copy_to(ref)`.
- `T[] a;` declares a zero-initialized empty vector; `a.push(x)` will auto-grow from capacity 0.
- `out` and `in` modifiers were removed (only `ref` remains).
- Interface types have `type_kind = TYPE_INTERFACE`, `is_pointer = 0`. The C type is a fat pointer struct (two pointers), not a raw pointer.
- Reference-like types (`class`, `interface`, `object`, including weak/unowned)
  and value types (`primitive`, `struct`, `bool`) do not implicitly convert.
  Codegen rejects these mismatches at variable init, assignment, array element
  assignment, and weak/unowned declarations.

## Bool and Null Literals
- `bool` is a primitive type mapping to C `int`; `true`/`false` literals emit `1`/`0`.
- Comparison (`== != < <= > >=`), logical (`&& ||`), and logical-not (`!`) expressions have type `bool` (changed in `resolve_type`; previously `i32`).
- Strict bool rule: bool and numeric types do not implicitly convert. Checked at the assignment boundaries — variable initializers, assignments, call arguments, and `return` — via `bool_mismatch` in codegen.c. `bool b = 5;`, `i32 x = true;`, and `i32 x = a < b;` are compile errors.
- Reference/value type mismatch: reference-like types (`class`/`interface`/`object`,
  including weak/unowned) and value types (`primitive`/`struct`/`bool`) do not
  implicitly convert at the same assignment boundaries plus array element
  assignment and weak/unowned declarations (checked via `type_is_reference`
  in `codegen.c`). `u64 win = app.createWindow();` and `SdlWindow w = 5;` are
  compile errors.
- Conditions are NOT required to be bool: `if (ptr)`, `while (w.lock())`, and `while (1)` keep C truthiness semantics.
- bool does not support compound assignment or `++`/`--` (it is not in the primitive-numeric lists, so the existing checks reject it automatically).
- f-string interpolation of bool prints `true`/`false` via the runtime method `String_append_bool`.
- `null` is a literal for reference types: class (including `string`), interface, weak class, and weak interface. It is allowed in variable initializers, assignments (locals, fields, array elements), `==`/`!=` comparisons, call arguments, and `return`.
- Generated shape of null: `NULL` for class/weak class pointers, `{ NULL, NULL }` for interface and weak interface fat pointers.
- Interface/null comparison emits `.data == NULL`; weak interface/null emits `.wr == NULL`.
- null is rejected at compile time for: primitive/bool/struct/array targets, `unowned` references (declaration, assignment, argument), arithmetic and relational operators, member access and method calls, array indexing, `as` casts, `match` expressions, and f-string interpolation.
- `mylang_weak_init(NULL)` returns NULL (guard in runtime.c), which makes the weak-class codegen paths (init, assignment, array elements, push) safe without special-casing.

## The object Type
- `object` is a top reference type (C `void*`): any class (including `string`) or interface value converts to it implicitly. Interface values contribute their `.data` pointer; the vtable is dropped.
- Refcounting is identical to class references (same `ObjHeader`): local variables are cleanup-tracked, class destructors release `object` fields, and `object[]` arrays use `MYLANG_ELEM_CLASS` (elements are retained/released by the runtime).
- Converting back is explicit only:
  - `o as ClassName` checks `mylang_obj_hdr(o)->type_id` and yields `(ClassName*)o` or `null` (null-safe). `as` to an interface is not supported — cast to a concrete class first.
  - `match (o)` works with class pattern arms via the same type_id cascade (a NULL value falls through to `else`).
- object has no members: `o.field` and `o.method()` are compile errors ("cast it with 'as' first"). There are no `weak object` / `unowned object`, and `new object` is rejected.
- Assigning object back to a class type without `as` is a compile error at var init, assignment, call arguments, and return (C would silently convert `void*` to any pointer).
- f-string interpolation and `print` do not accept object (cast and use IToString instead).

## Access Modifiers (public/private)
- Per-member modifiers on class fields and methods: `private i32 x;`, `private i32 helper() { ... }`. Parsed in the same modifier loop as `native`/`override`, in any order.
- Default is `public`; `public` may be written explicitly. `public` + `private` together is a compile error.
- Access rule (C++ style): a private member is visible only inside methods of the same class — any instance of that class (`this.x` and `other.x` both work), not from other classes or free functions.
- Not supported on structs (pure data value types), interfaces (methods are inherently public), or top-level functions/types — these positions produce a dedicated parse error.
- Interface constraints: a method implementing an interface method must be public (`symtab_validate_impls` rejects a private implementation); `private` + `override` is a compile error.
- Storage: `ClassInfo.field_private[MAX_FIELDS]` parallels `field_types`; `MethodInfo.is_private`. Generic instantiations clone both.
- Enforcement is compile-time only: `codegen_member_access` (fields) and `codegen_call` (methods) check against `CodegenContext.current_class`, which `codegen_method_decl` sets while emitting a method body (NULL in free functions and interface default methods). f-string interpolation rejects a private `toString`.
- Generated C is unchanged: visibility is not enforced at the C level, and compiler-generated code (vtables, thunks, destructors) is exempt.

## Compound Assignment Operators
- Supported: `+=`, `-=`, `*=`, `/=`, and the bitwise forms `&=`, `|=`, `^=`, `<<=`, `>>=`.
- Arithmetic forms accept primitive numeric types (`i8/i16/i32/i64`, `u8/u16/u32/u64`, `f32/f64`); bitwise forms require integer types (no floats, no bool).
- Class, interface, weak, struct, array, object, and bool types are rejected with a compile-time error.
- `x += y` is generated as `x = x + y`; the left-hand side is evaluated twice, which is safe for primitives but disallowed for non-primitives.
- Like simple assignment (`=`), compound assignment is an expression and is not allowed in `if`/`while` conditions, variable initializers, or `return` expressions.

## Bitwise Operators
- Supported: `&`, `|`, `^`, `~`, `<<`, `>>` with C-compatible precedence: `~` binds like the other unary operators; `<<`/`>>` sit between additive and relational; `&`, `^`, `|` sit between equality and `&&` (in that order).
- Operands must be integer types (`i8/i16/i32/i64`, `u8/u16/u32/u64`) — floats, bool, and reference types are compile-time errors (`operator '<op>' requires integer operands`).
- The result type is the left operand's type (same as arithmetic); `~` yields the operand type. Shift counts are unchecked (C semantics; `>>` on signed values is an arithmetic shift on MSVC).
- bool logic continues to use `&&`, `||`, `!`; there are no bool `&`/`|` operators.
- Bitwise compound assignments (`&=`, `|=`, `^=`, `<<=`, `>>=`) require integer types; see "Compound Assignment Operators".

## Const Value Types
- `const <primitive> name = expr;` declares a read-only local: `i8/i16/i32/i64`, `u8/u16/u32/u64`, `f32/f64`, `bool`. An initializer is required.
- Parameters may also be const (`f(const u32 x)`); `ref` + `const` is a compile error. Reading a const value is unrestricted.
- Compile-time enforcement: assignment (including compound forms) to a const variable, `++`/`--` on it, and passing it to a `ref` parameter are all errors; a missing initializer is an error.
- `const` is rejected on class/interface/object/struct/array/weak/unowned types and on class/struct fields (no constructors or field initializers exist, so a const field could only ever be zero).
- Representation: `Type.is_const`; `type_equal` ignores it (top-level const on values does not affect type compatibility or interface signature matching). Generated C is unchanged — enforcement lives entirely in the compiler front end.

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
- `body` must be a block (`{ ... }`); a brace-less single statement is a compile error. The same rule applies to `if`, `while`, and `else` bodies — except that an `else` body may be another `if` statement to allow else-if chains.
- Variables declared in `init` are scoped to the loop (not visible after it) and are released on normal loop exit.
- Assignment and increment/decrement are not allowed in the `condition` expression.
- `break` exits the innermost loop; `continue` skips to the next iteration. Both correctly run cleanup for local variables.

## Match Statement

- Syntax: `match (expr) { ClassName var => { body } ... else => { body } }`.
- Arms are evaluated in order; the first matching arm runs and the rest are skipped.
- Type-pattern arms match the concrete class type of an interface or class expression:
  - The pattern class must implement the interface when the expression is an interface.
  - The bound variable is a class pointer that is visible only inside the arm body.
- Integer literal arms match integer expressions.
- `else` must be the last arm and matches any remaining value.
- The match expression is evaluated once into a local temporary; the temporary is released
  if it is owned by the expression.
- Binding variables are scoped to the arm body and do not participate in reference counting.

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
- `ctx->return_type` tracks the enclosing function's return type so `codegen_return_stmt` can emit implicit class-to-interface conversion.
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
- `String` is mutable and owns the native append API: `append_string`, `append_i32/i64/u32/u64/f32/f64`, `append_char`, `append_bool`.  There is no separate `StringBuilder` type; appending through one alias is visible through all aliases (same as any other class), and there is no copy-on-write.
- `IToString` is a builtin interface (`string toString()`); classes implementing it can be interpolated in f-strings.
- f-strings `f"...{expr}..."` are lowered by the parser into an `AST_FSTRING` node containing ordered parts (string literals and expression nodes).
- Codegen emits a temporary `String` accumulator (`_fsN`, tracked by the cleanup list), appends each part, and uses the accumulator itself as the expression value; literal segments go through `mylang_string_append_cstr` without allocating a temporary `String`.  Each interpolated expression is evaluated exactly once and in source order.
- The f-string accumulator is owned (+1) through the cleanup list.  Caller-side
  retain/release guards (`guard_expr_is_owned`) treat `AST_FSTRING` as owned,
  so calls like `print(f"...")` do not emit redundant retain/release pairs.
- Interpolation dispatch: `string` and primitive types map to `String_append_*`; class types require a `string toString()` method (IToString) which is called directly; interface values dispatch through the vtable.
- Floating-point numeric literals are supported: `3.14` defaults to `f64`, and
  `1.5f` / `1.5F` are `f32`.  They can be used directly in f-strings (`{3.14}`)
  and in arithmetic.
- Escaped braces are supported: `{{` and `}}` collapse to literal braces, and `\{` / `\}` escape a single brace (in plain strings too). A single `{` starts an interpolation expression; a lone `}` stays a literal `}`. The lexer smuggles `\{` / `\}` through string tokens as sentinel bytes (`TOK_ESC_LBRACE` / `TOK_ESC_RBRACE` in token.h) so the f-string parser can tell them apart from interpolation braces; the parser converts them back before codegen.

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
- **Guarded temp extraction**: guarded expressions are evaluated into `_gN` temporaries before the call to prevent double-evaluation. Expressions whose value ownership is consumed by the surrounding statement (variable initializers, return expressions, expression-statement roots, assignment RHS) are never cleanup-tracked. Nested owned subexpressions inside arguments (e.g. the object of an `as` cast or an interface method receiver like `w.lock()` / `make().area()`) are first hoisted into `_iN` temporaries so side-effecting calls are evaluated exactly once.
- **Class assignment**: RHS retained before LHS released to avoid UAF on self-assignment (`b = b.set(5)`).
- **Class/interface fields own their values**: assigning a local or parameter to a class or interface field retains the source; the per-class destructor releases fields when the object is freed.
- **Weak fields own their weak shares**: weak class and weak interface fields release their weak share (`mylang_weak_release`) in the class destructor.
- **Array fields are freed by the destructor**: `T[]` fields are released with `mylang_array_free` when the containing object is freed.
- **Discarded class return**: `(void)mylang_release(call(...))` in expression statements.
- **This in methods**: retained on entry, released via cleanup. Name mapped to `thiz` in generated C.

## Weak References (Implemented)
- Syntax: `weak ClassName v = obj;` to declare, `v.lock()` to acquire.
- There is no separate control block: the weak count lives inside the object header (`ObjHeader::weak_count`), make_shared style. `WeakRef` is just a typedef of `ObjHeader`, and a weak variable holds a pointer to the header. Codegen only manipulates `WeakRef*` opaquely through the runtime functions.
- `weak_count` counts live weak shares plus one implicit share held by the object itself: it starts at 1 in `mylang_new_object` and `mylang_release` drops the implicit share after the strong count reaches zero. `weak_count == 0` therefore implies the destructor has already run, and the block is freed exactly once, by whichever release drops the count to zero.
- While any weak share is held, the object block stays allocated. This is what makes `mylang_lock` safe against a concurrent free; the trade-off is that the shallow object block (header + fields) is freed only when the last weak share dies (the destructor still runs immediately at strong-count zero, same as `make_shared`).
- `mylang_lock(wr)`: CAS loop on `ObjHeader::refcount`. `refcount == 0` is the liveness test; a successful CAS from a positive value returns a retained (+1) strong pointer, otherwise NULL.
- `mylang_weak_init(ptr)`: one atomic inc of `weak_count` and returns the header pointer. No allocation and no installation race; the caller always holds a strong reference, so the object is alive while its share is taken.
- `mylang_weak_init_owned(ptr)`: weakifies an owned strong reference — takes a WeakRef share, then releases the strong reference (used when the statement consumes RHS ownership, e.g. weak array element assignment from a call result).
- `mylang_weak_copy(wr)`: increments `weak_count` (for weak-to-weak copy).
- `mylang_weak_release(wr)`: decrements `weak_count`; on zero it frees the object block (the destructor has already run).
- On `mylang_release` refcount drop to zero: run the destructor, then drop the implicit weak share and free the block if that was the last one.
- Threads: with atomic counts and implicit-share pinning, the refcount/weak protocol itself is thread-safe. Actual multithreading still needs language-level support (thread APIs, shared-variable semantics).
- Cleanup uses `CleanupEntry.is_weak` to dispatch to `mylang_weak_release` vs `mylang_release`.
- Strong-to-weak parameter conversion is automatic: codegen wraps the argument in `mylang_weak_init()`.

## Unowned References (Implemented)
- Syntax: `unowned ClassName v = obj;` to declare. No `lock()`: the reference is used directly like a strong one (`u.field`, `u.method()`).
- Semantics (like Swift `unowned(safe)`): non-owning, but every read is checked — reading a dead reference calls `my_panic("unowned reference to dead object")`; reading a never-initialized one panics with `"unowned reference is null"`. Memory-safe: the share pins the object block, so the check can never touch freed memory.
- Representation: identical to weak — the variable holds a `WeakRef*` (header pointer) and takes a share of the same `ObjHeader::weak_count`. All share management (init, copy, release, cleanup, field destructors) reuses the weak machinery.
- Reads are wrapped by `codegen_expr` with `mylang_unowned_check(...)`, which validates and returns the user pointer. Weak/unowned share-management paths (copies, weakifying, assignment targets) suppress the wrapper via `codegen_expr_raw` (`CodegenContext.no_unowned_check`).
- Conversions: strong -> unowned via `mylang_weak_init` (implicit on init/assignment/argument); unowned -> unowned via `mylang_weak_copy`; unowned -> strong is allowed and emits check + retain (e.g. `Node s = u;`, strong parameters, `return u;` where the return type is a strong class). `unowned Node u = w.lock();` works through the owned-RHS path.
- Restrictions: class types only (no interfaces, no primitives); local declarations require an initializer; no `unowned` return types; no unowned arrays; `new unowned` is rejected; `.lock()` on unowned is a compile error; `match` on an unowned expression is rejected (convert to strong first); `as unowned T` is rejected.
- Choosing: use `weak` when the object dying first is a normal runtime event the code must handle (lock returns NULL); use `unowned` when it would be a structural bug and a panic is the right failure. Under future threads the check is exact in single-threaded code but only a backstop under data races — real concurrent lifetime pinning still requires `weak` + `lock()`.

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
- Tracks only `ObjHeader` based allocations (class instances and interface objects). Arrays are not tracked by the MyLang leak list.
- When enabled, the generated C code adds `next`/`prev`/`alloc_trace` to `ObjHeader` and records every allocation in a global circular doubly-linked list.
- `mylang_release` removes the block from the list when the destructor runs (the memory itself may be freed later, once the last weak share is gone).
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
- `match` does not support `true`/`false` literal arms; `match (null)` is a compile error.
- Mixed arithmetic containing bool operands (e.g. `1 + (a < b)`) is not checked at the operand level; C promotion rules apply. The strict bool rule is enforced only at assignment boundaries.
- Access modifiers are class-only: structs, interfaces, and top-level declarations do not accept `public`/`private`. Enforcement is compile-time only; generated C does not hide private members.
- `const` covers only primitive value types; const fields and const-qualified reference types are not supported yet.
- No AST deallocation function (one-shot compiler).

## Class Field Destructors
- Every concrete class has a compiler-generated finalizer that runs when the last reference to an object is released.
- The finalizer releases class, interface, weak, and array fields so they are no longer leaked.
- Circular references between objects still leak (there is no cycle collector); this is intentional and documented by the `circular_ref_leak` test.
