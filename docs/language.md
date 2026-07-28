# MyLang — Language

Detailed reference; the rules in AGENTS.md still apply.

## Type System
- Primitives: `i8/i16/i32/i64`, `u8/u16/u32/u64`, `f32/f64`, `bool`, `void`.
- User types: `class` (heap/reference), `struct` (value/stack), and `interface` (fat pointer).
- Type IDs: primitives use 0-15 (`bool` = 11, `object` = 12); classes, structs, and interfaces share a counter starting at 16.
- `TYPE_NULL` is a compile-time-only `TypeKind` for the `null` literal; it has no runtime type_id.
- `TYPE_OBJECT` is the top reference type (`void*` in C); see "The object Type" section.
- Flags: `TYPE_IS_ARRAY = 0x80000000`, `TYPE_IS_STRUCT = 0x40000000`, `TYPE_IS_WEAK = 0x20000000`, `TYPE_IS_INTERFACE = 0x10000000`, `TYPE_IS_UNOWNED = 0x08000000`.
- `Type` struct fields: `type_kind`, `class_name[64]`, `is_pointer`, `is_array`, `array_size`, `is_ref`, `is_weak`, `is_unowned`, `type_id`.
- `out` and `in` modifiers were removed (only `ref` remains).
- Interface types have `type_kind = TYPE_INTERFACE`, `is_pointer = 0`. The C type is a fat pointer struct (two pointers), not a raw pointer.
- Top-level `class`, `struct`, and `interface` names are pre-registered before
  their bodies are parsed, so types can refer to each other regardless of
  declaration order (e.g., `SdlWindow` can hold an `SdlApp` field while `SdlApp`
  is defined later). The parser reuses the pre-registered entry when the real
  definition is reached.

## Bool and Null Literals
- `bool` is a primitive type mapping to C `int`; `true`/`false` literals emit `1`/`0`.
- Comparison (`== != < <= > >=`), logical (`&& ||`), and logical-not (`!`) expressions have type `bool`.
- Strict bool rule: bool and numeric types do not implicitly convert. Checked at the assignment boundaries — variable initializers, assignments, call arguments, and `return` — via `bool_mismatch` in codegen.c. `bool b = 5;`, `i32 x = true;`, and `i32 x = a < b;` are compile errors.
- Reference/value type mismatch: reference-like types (`class`/`interface`/`object`,
  including weak/unowned) and value types (`primitive`/`struct`/`bool`) do not
  implicitly convert at the same assignment boundaries plus array element
  assignment and weak/unowned declarations (checked via `type_is_reference`
  in `codegen.c`). `u64 win = app.createWindow();` and `SdlWindow w = 5;` are
  compile errors.  The same separation applies to comparisons: reference-like
  types may only be compared with other reference-like types (or `null` for
  equality); `win == 0` is a compile error.
- Conditions are NOT required to be bool: `if (ptr)`, `while (w.lock())`, and `while (1)` keep C truthiness semantics.
- bool does not support compound assignment or `++`/`--`.
- f-string interpolation of bool prints `true`/`false` via the runtime method `String_append_bool`.
- `null` is a literal for reference types: class (including `string`), interface, weak class, and weak interface. It is allowed in variable initializers, assignments (locals, fields, array elements), `==`/`!=` comparisons, call arguments, and `return`.
- Generated shape of null: `NULL` for class/weak class pointers, `{ NULL, NULL }` for interface and weak interface fat pointers. Interface/null comparison emits `.data == NULL`; weak interface/null emits `.wr == NULL`.
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
- Not supported on structs, interfaces, or top-level functions/types — these positions produce a dedicated parse error.
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

## Interface System
- Syntax: `interface Name { method_sigs; }`, `class Foo : Iface1, Iface2 { ... }`.
- Multiple interfaces per class are supported via comma-separated `:` list.
- Interface values are fat pointers: `{void* data, const VTable* vtable}` struct in generated C.
- Each interface gets a VTable typedef containing `concrete_type_id` and one function-pointer field per method.
- Each class implementing an interface gets a static `const` vtable instance and a thunk function per method (casts `void*` to `ClassName*`, calls the real method).
- Dynamic dispatch: `s.area()` emits as `(s).vtable->area((s).data)`.
- Type assertion: `expr as ClassName` compares `vtable->concrete_type_id` against the class type_id, returns `(ClassName*)data` or `NULL`.
- Implicit class-to-interface conversion on variable init (`IShape s = obj`),
  assignment (`s = obj`), return (`return obj` where return type is interface),
  and call arguments (`use(obj)` where parameter type is interface).
- Interface refcounting: the `.data` pointer holds a reference count. Create = retain, destroy = release `.data`. Cleanup uses `CleanupEntry.is_interface` flag.
- Semantic validation (`symtab_validate_impls`): verifies at compile time that a class declares all methods required by declared interfaces with matching signatures. Aborts codegen on error.
- `override` keyword may be used on class methods that override an interface method. When present, the compiler verifies that the method actually matches an interface method from one of the implemented interfaces. It is optional but checked.
- Empty classes/structs emit `char _pad;` placeholder for MSVC compatibility (C requires at least one struct member).
- `ctx->return_type` tracks the enclosing function's return type so `codegen_return_stmt` can emit implicit class-to-interface conversion.
- Interface parameters pass by value (struct copy). Caller-side guard extraction handles complex expressions.
- `IHashable` is a builtin interface (`u64 hash()`, `bool equals(object other)`)
  registered in `symtab_init`. The builtin `hash(x)` / `equals(a, b)` dispatch
  to these methods (direct calls, no vtable) when the static class type
  declares `: IHashable`; classes that do not implement it get the default —
  identity (pointer) hashing and identity comparison, the C#/Java `Object`
  fallback. Interface-typed values always fall back to identity (there is no
  interface inheritance to route through a vtable).
- `as` keyword is parsed in `parse_postfix` as a postfix operator.

## Weak Interface Types
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

## Struct Value Types
- Structs are stack-allocated value types; assignment copies the whole struct.
- `new StructName` is illegal.
- `new StructName[N]` creates a dynamic array of structs.
- Struct fields may be primitives, bool, other structs, and reference types
  (class, interface, object, string, weak, unowned). Nested structs are embedded
  by value; recursive nesting (direct or indirect) is rejected by
  `symtab_validate_structs`. In the generated header, struct definitions are
  emitted in dependency order (inner before outer, and before any class that
  embeds them).
- Structs owning reference fields (transitively, `StructInfo.has_ref_fields`
  computed bottom-up in `struct_cycle_dfs`) get compiler-generated hooks
  `_mylang_retain_S(S*)` / `_mylang_release_S(S*)`: the retain hook retains /
  weak-copies every reference field (recursing into nested structs), the release
  hook drops them. The hooks are wired into every copy/destruction point:
  variable init (borrowed initializers retain; call results are already owned),
  plain assignment (retain new, release old, then copy — self-assign safe),
  by-value parameters (callee retains on entry, releases at scope exit via the
  cleanup list), `return` (borrowed values retain for the caller), discarded
  call results, class destructors (struct fields), guard/hoisted temporaries
  (owned struct call results used as call arguments or nested subexpressions
  are evaluated into `_gN`/`_iN` temps registered via `cleanup_add_struct_dtor`
  so their ref shares are released once at scope exit), and local cleanup
  (`CleanupEntry.is_struct_dtor`). Struct locals with reference fields are
  zero-initialized (`= {0}`) so the release hook is NULL-safe.
- Arrays of such structs are rejected (`type_is_ref_struct_array`): MyArray is
  type-erased and cannot run per-element hooks yet. Checked at local
  declarations, parameters, and class fields.
- Structs may declare methods: `RetType name(Params) { body }`. Emitted as
  `RetType StructName_method(StructName* thiz, ...)`, mirroring the class
  method path. Inside the body `this` is a ref-style alias of the receiver
  (no retain/release); the parser/codegen model it as a struct Type with
  `is_ref = 1`, and `codegen_expr` maps it to `(*thiz)` so ordinary `.` member
  access works.
- Calls require an lvalue receiver (local, field access, or array element):
  `v.m()` emits `StructName_m(&(v), ...)`, which also covers ref parameters and
  `this` (already `(*x)` in C). Calls on rvalues such as `make().m()` are a
  compile error.
- Struct methods are always public and cannot be `native` or `override`;
  structs do not implement interfaces (a struct-to-interface conversion would
  need boxing, which is not implemented).
- The runtime uses `MYLANG_ELEM_STRUCT` to copy/release struct array elements
  correctly.

## Strings and f-strings
- `string` is a builtin class-like type backed by `String` in `runtime.h`; string literals compile to owned `String*` objects via `mylang_string_new`.
- `String` is mutable and owns the native append API: `append_string`, `append_i32/i64/u32/u64/f32/f64`, `append_char`, `append_bool`, and `equals`. There is no separate `StringBuilder` type; appending through one alias is visible through all aliases (same as any other class), and there is no copy-on-write.
- `IToString` is a builtin interface (`string toString()`); classes implementing it can be interpolated in f-strings.
- `==`/`!=` on strong `string` operands is value comparison via `String_equals`
  (C# style), so two strings with equal contents compare equal even as distinct
  objects. Comparisons against `null` keep the pointer-vs-NULL shape, and
  weak/unowned strings keep identity comparison.
- f-strings `f"...{expr}..."` are lowered by the parser into an `AST_FSTRING` node containing ordered parts (string literals and expression nodes).
- Codegen emits a temporary `String` accumulator (`_fsN`), appends each part, and uses the accumulator itself as the expression value; literal segments go through `mylang_string_append_cstr` without allocating a temporary `String`. Each interpolated expression is evaluated exactly once and in source order.
- The accumulator is owned (+1) through the cleanup list, and caller-side
  retain/release guards (`guard_expr_is_owned`) treat `AST_FSTRING` as owned,
  so calls like `print(f"...")` do not emit redundant retain/release pairs.
- Interpolation dispatch: `string` and primitive types map to `String_append_*`; class types require a `string toString()` method (IToString) which is called directly; interface values dispatch through the vtable.
- Floating-point numeric literals are supported: `3.14` defaults to `f64`, and
  `1.5f` / `1.5F` are `f32`.  They can be used directly in f-strings (`{3.14}`)
  and in arithmetic.
- Escaped braces are supported: `{{` and `}}` collapse to literal braces, and `\{` / `\}` escape a single brace (in plain strings too). A single `{` starts an interpolation expression; a lone `}` stays a literal `}`. The lexer smuggles `\{` / `\}` through string tokens as sentinel bytes (`TOK_ESC_LBRACE` / `TOK_ESC_RBRACE` in token.h) so the f-string parser can tell them apart from interpolation braces; the parser converts them back before codegen.

## Array / Vector Value Types
- `T[]` compiles to the C value type `MyArray { size_t capacity; size_t length; void* data; }`.
- The vector is not reference-counted; its data buffer is allocated with `malloc`/`realloc` and freed with `free` via `mylang_array_free`.
- Arrays are created empty: `T[] a;` initializes `capacity = length = 0` and `data = NULL`; `a.push(x)` auto-grows from capacity 0. There is no `new T[N]` syntax.
- To preallocate capacity use `a.reserve(n)`; to set the initial length use `a.resize(n)`.
- Arrays cannot be returned by value, passed by value, or assigned with `=`. Use `ref T[]` parameters for mutation and `move_to(ref dst)` / `copy_to(ref dst)` for transfer or duplication.
- Builtin vector methods: `.push(v)`, `.pop()`, `.reserve(n)`, `.resize(n)`, `.clear()`, `.compact()`, `.move_to(ref dst)`, `.copy_to(ref dst)`.
- Element access uses `arr[i]` and is bounds-checked at runtime via `mylang_array_at()`; out-of-bounds triggers `my_panic` which calls `abort()`.
