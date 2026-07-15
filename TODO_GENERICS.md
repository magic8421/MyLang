# MyLang Generic Class Implementation TODO

> Goal: implement generic classes with C#-style angle brackets and colon constraints.
> Design: see `GENERICS_DESIGN.md`.

## Phase 1: Type System Foundation

### 1.1 Extend `Type` to carry type arguments
- [x] Modify `src/ast.h`:
  - Add `MAX_TYPE_ARGS` (start with 4).
  - Extend `Type` with:
    - `int type_arg_count;`
    - `struct Type* type_args[MAX_TYPE_ARGS];`
    - `char mangled_name[128];`
  - Add `TYPE_TYPE_PARAM` to `TypeKind`.
- [x] Update `src/ast.c`:
  - Implement `type_param_type(const char* name)` factory.
  - Update `type_equal()` to compare `type_arg_count` / `type_args` recursively (or compare `mangled_name` plus flags).
  - Update `type_name()` to return the mangled / display name.
  - Implement `type_is_generic(const Type* t)` helper.
  - Implement `type_substitute(Type* t, const char* params[], Type* args[], int count)`.

### 1.2 Add mangling utilities
- [x] Create `src/mangle.h` and `src/mangle.c` (or put helpers in `ast.c` / new `util.c`):
  - `mangle_type(const Type* t)` -> produces `Box_i32`, `Pair_string_Node`, etc.
  - Handle primitives, classes, structs, interfaces, arrays, weak refs.
  - Cache result in `Type.mangled_name`.

### 1.3 Extend symbol table metadata
- [x] Modify `src/symtab.h`:
  - Add to `ClassInfo`:
    - `int is_generic;`
    - `int generic_param_count;`
    - `char generic_params[8][64];`
    - `Type generic_constraints[8][4];` (store constraint types per param, max 4 constraints each)
    - `int generic_constraint_count[8];`
    - `AstNode* generic_ast;` (optional: keep the generic definition AST for cloning)
  - Add helper `ClassInfo* symtab_find_class_instantiation(const char* mangled_name)`.
- [x] Modify `src/symtab.c`:
  - Update `symtab_add_class()` to record whether the class is generic.
  - Add `symtab_add_class_instantiation(const char* base_name, ClassInfo* generic_def, Type* type_args, int arg_count)`.
  - Update `symtab_find_class()` so it can distinguish base generic definition from concrete instance.

## Phase 2: Parser Changes

### 2.1 Parse generic parameter list with constraints
- [x] Modify `src/parser.c` `parse_class_decl()`:
  - After parsing class name, check for `<`.
  - If present, parse comma-separated parameter declarations of the form `T` or `T : Constraint1, Constraint2`.
  - Constraints are currently:
    - interface name -> record as interface constraint,
    - `new()` -> record as constructor constraint.
  - Store params and constraints in `ClassInfo`.
  - Expect closing `>`.
  - Continue parsing optional interface implementation list (`:`) and class body.

### 2.2 Parse type arguments in type contexts
- [x] Modify `src/parser.c` `parse_type()`:
  - After parsing a user type name (`class` / `struct` / `interface`), check for `<` in type context.
  - Parse comma-separated type arguments recursively.
  - Build a `Type` with `type_arg_count > 0`.
  - Set `Type.type_kind` to `TYPE_CLASS` and compute `mangled_name`.
  - Continue parsing array suffixes (`[]`, `[N]`), `weak`, `ref` as before.

### 2.3 Parse type arguments in `new` expressions
- [x] Modify `src/parser.c` `parse_primary()`:
  - `new Box<i32>(...)` must parse the type with arguments.
  - Ensure constructor call arguments follow the type.

### 2.4 Disambiguation notes
- [x] Document that `<` as type arguments is only parsed in type contexts (variable decl, param, return type, `new`, `as Type`).
- [x] Expression-level generic method calls are out of scope for this phase.

## Phase 3: Semantic Analysis / Monomorphization

### 3.1 Build generic definition AST
- [x] Ensure `parse_class_decl()` produces a generic `ClassInfo` + AST for `class Box<T>` but does not immediately codegen it.
- [ ] Generic definitions do not get a `type_id` until instantiated. (Current implementation assigns a base type ID for the generic definition; it is not used for codegen.)

### 3.2 Instantiate on first use
- [x] Implement `symtab_instantiate_class_from_type(Type* t)` / `symtab_add_class_instantiation(...)`:
  - Verify arity matches `generic_param_count`.
  - Compute mangled name.
  - Check existing instantiation by mangled name; return existing if found.
  - Create new `ClassInfo` with substituted field types and method signatures.
  - Assign fresh `type_id`.
  - Register the concrete class in the symbol table.
  - Clone the generic AST body and substitute type parameters in field/method types.
- [x] Hook instantiation into `preinstantiate_generic_types()` and `codegen` paths so that seeing `Box<i32>` anywhere triggers it.

### 3.3 Constraint checking
- [x] Validate interface and `new()` constraints at instantiation time in `symtab_instantiate_class_from_type()`.
- [ ] Emit richer MyLang source-level error messages with line/column info.

### 3.4 Interface method availability on type parameters
- [ ] In `resolve_type()` / method lookup, when receiver type is `TYPE_TYPE_PARAM`, allow method calls only if the param has a matching interface constraint.
- [ ] Generate interface dispatch for such calls.

## Phase 4: Code Generation

### 4.1 Generate struct for each concrete instantiation
- [x] Modify `src/codegen.c` `codegen_class_decl()`:
  - For concrete generic classes, emit struct using `mangled_name` as the tag, e.g.:
    ```c
    typedef struct Box_i32 { int32_t value; } Box_i32;
    ```
  - Field types are the substituted concrete types.

### 4.2 Generate methods for each concrete instantiation
- [x] Modify `src/codegen.c` `codegen_method_decl()`:
  - Use mangled class name for method prefix: `Box_i32_get(Box_i32* thiz, ...)`.
  - Substitute type parameters inside method bodies via cloned AST.
  - [ ] Handle `new T()` inside methods when `T : new()` is present.

### 4.3 Generate constructors
- [x] Ensure `new Box<i32>()` emits:
  ```c
  Box_i32* _tmp = mylang_new_object(sizeof(Box_i32), TYPEID_Box_i32);
  ```
- [x] Assign type IDs for concrete generic classes before first use.

### 4.4 Method dispatch and member access
- [x] Update `codegen_call()` to use mangled names for generic class methods.
- [x] Update `codegen_member_access()` to use the concrete struct layout.

### 4.5 Interface dispatch on type parameters
- [ ] When calling a method on `T` where `T : IFoo`, emit fat-pointer construction + vtable dispatch.

### 4.6 Arrays, weak refs, cleanup
- [x] Dynamic arrays of generic class instances work (`Box<i32>[]`).
- [x] Fixed arrays of generic class instances work (`Box<i32>[4]`).
- [x] Weak references to generic class instances work (`weak Box<i32>`).
- [x] Cleanup retains/releases concrete class fields correctly.

## Phase 5: Runtime / Helpers

### 5.1 Type ID macros
- [ ] Ensure the generated C code defines a `TYPEID_<MangledName>` macro per concrete generic class, used by `mylang_new_object`.

### 5.2 No runtime changes expected
- [ ] Reference counting, bounds checking, weak ref locking should be unchanged because concrete types are just normal classes.

## Phase 6: Tests

### 6.1 Add positive tests to `test_runner.py`
- [x] `generics_basic` - `Box<i32>` and `Box<Box<i32>>` with get/set.
- [x] `generics_pair` - `Pair<i32, i8>`.
- [x] `generics_func` - generic class as function argument and return value.
- [x] `generics_nested_arg` - inner generic type used only as a type argument.
- [x] `gen_interface.my` - class with interface constraint (`T : IPrintable`) and method call.
- [x] `gen_new_constraint.my` - class with `T : new()` and `new T()`.
- [x] `gen_multi_constraint.my` - `T : IFoo, IBar`.
- [ ] `gen_array.my` - `T[]` and `T[N]` inside generic class.
- [ ] `gen_weak.my` - `weak Box<i32>`.

### 6.2 Add negative tests
- [ ] `gen_bad_arity.my` - wrong number of type args.
- [ ] `gen_bad_constraint.my` - type does not implement required interface.
- [ ] `gen_bad_new.my` - type lacks parameterless constructor when `new()` required.
- [ ] `gen_bad_method.my` - calling method on `T` without interface constraint.

### 6.3 Run test suite
- [x] Run `python test_runner.py`.
- [x] Run `python test_runner.py --leak-check`.
- [x] Fix regressions in existing tests.

## Phase 7: Cleanup and Documentation

- [ ] Update `AGENTS.md` if any language rules change (e.g. field type restrictions).
- [ ] Update `GENERICS_DESIGN.md` with any implementation divergences.
- [ ] Remove `TODO_GENERICS.md` or mark it complete when done.

## File Checklist

| File | Expected Changes |
|------|------------------|
| `src/ast.h` | Extend `Type`, add `TYPE_TYPE_PARAM`. |
| `src/ast.c` | Update `type_equal`, `type_name`, add substitution helpers. |
| `src/symtab.h` | Extend `ClassInfo` for generic params / constraints. |
| `src/symtab.c` | Add instantiation registry, constraint checks. |
| `src/parser.c` | Parse `<T : C1, C2>` and `Name<T1, T2>` in types. |
| `src/codegen.c` | Generate concrete structs/methods per instantiation. |
| `src/codegen.h` | Possibly add helper declarations. |
| `src/util.h` | String/copy helpers. |
| `src/mangle.h` / `src/mangle.c` | Type mangling utilities. |
| `test/gen_*.my` | New test cases. |
| `AGENTS.md` / `GENERICS_DESIGN.md` | Docs updates. |

## Known Issues / Follow-ups

- [x] Returning a `new` expression directly (e.g. `return new T;` inside a generic method, or `return new Widget;` in non-generic code) leaked one reference count. Fixed in `codegen_return_stmt` by skipping `mylang_retain` when the returned expression is `AST_NEW` or `AST_CALL`. Test coverage: `gen_new_return_direct`.

## Notes / Risks

- `Type.class_name[64]` may be too small for deeply nested mangled names; monitor and enlarge if needed.
- Monomorphization can increase compile time and code size; accept this for the MVP.
- Interface dispatch on type parameters needs careful handling of fat-pointer lifetime and retain/release.
- Existing method lookup by `symtab_find_method(class_name, method_name)` may need to use mangled names for concrete generic classes.
