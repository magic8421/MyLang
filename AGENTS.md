# Agent Notes for MyLang

## Comment Style
All source comments must be written in plain English.
Do not use emojis or non-ASCII characters in comments or identifiers.
Keep everything ASCII.

## Build & Test
- Windows MSVC build: run `build.bat` (it calls `vcvars64.bat`).
- The compiler itself is built with AddressSanitizer (`/fsanitize=address`).
- Run the test suite with: `python test_runner.py`.
- Generated C files are compiled with `cl /std:c11`.

## Source Layout
Source code lives under `src/`:
- `token.c/h`, `lexer.c`
- `ast.c/h`, `parser.c/h`
- `symtab.c/h`, `codegen.c/h`
- `main.c`, `util.h`

## Type System
- Primitives: `i8/i16/i32/i64`, `u8/u16/u32/u64`, `f32/f64`, `void`.
- User types: `class` (heap/reference) and `struct` (value/stack).
- Type IDs: primitives use 0-15; classes and structs share a counter starting at 16.
- Flags: `TYPE_IS_ARRAY = 0x80000000`, `TYPE_IS_STRUCT = 0x40000000`.

## Reference Parameters
- Only the `ref` keyword is supported (`out` and `in` were removed).
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
- Heap objects use reference counting via `ObjHeader`.
- `ObjHeader` currently holds `refcount`, `type_id`, and `length`.
- Dynamic arrays and class instances are released through `mylang_release`.

## Planned: Weak References
- Proposed syntax: `weak ClassName w = obj;` and `ClassName s = lock(w);`
- Proposed runtime: add a `WeakRef*` list inside `ObjHeader`; each weak-ref node stores `target/next/prev`.
  When an object is freed, walk the list and set every `target` to `NULL`.
- This feature is not implemented yet; see previous design discussion for trade-offs.
