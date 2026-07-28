# Agent Notes for MyLang

Detailed design and reference material lives in `docs/`; read the relevant
file when working on that area:
- `docs/language.md` — type system, bool/null, object, interfaces,
  weak/unowned refs, structs, strings/f-strings, arrays, control flow,
  operators, access modifiers, reference parameters.
- `docs/memory-model.md` — refcounting, weak/unowned, runtime functions,
  destructors.
- `docs/compiler-internals.md` — import/modules, codegen conventions, method
  dispatch, native methods, cleanup, lexer/string safety, leak debugging,
  string encryption, known limitations.

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
- Build artifacts go to `build/` (mylang.exe, .obj, .pdb, ASan DLL); test executables go to `build/test/`.
- Run the test suite with: `python test_runner.py` (runs both release and debug modes by default; use `--mode release` or `--mode debug` to run just one).
- Generated C files are compiled with `cl /std:c11`.
- Test expectations in test_runner.py: an `int` checks the process exit code,
  `"crash"` accepts any non-zero exit, and a tuple `(exit_code, stdout)` (or a
  plain string, meaning exit 0) additionally requires an exact stdout match.
  stderr is ignored for matching, so CRT/MyLang leak reports do not interfere.

## Source Layout
Source code lives under `src/`:
- `token.c/h`, `lexer.c`
- `ast.c/h`, `parser.c/h`
- `symtab.c/h`, `codegen.c/h`
- `main.c`, `util.h`

Library code written in MyLang lives under `lib/`:
- `lib/Map.my` — generic hash map (`class Map<K, V>`), separate chaining over
  parallel arrays; keys work with any type the builtin `hash(x)`/`equals(a, b)`
  support (primitives, string, classes implementing `IHashable`). Tests import
  it via `import("../lib/Map.my")`.

## Error Message Format
- All compiler diagnostics are emitted as `path(line,col): error: message`
  (MSVC / VS Code problem-matcher style). The printed path is the input path
  as given on the command line (relative if the user passed a relative path).
- Every `Token` carries a `filename` pointer (set from `Lexer.filename`), so
  diagnostics and panic frames follow the file the code was defined in, which
  matters once `import` merges multiple source files.
- Lexer/parser errors use `Lexer.filename` (set from `src_path` in `src/main.c`)
  and `parser_filename(p)` in `src/parser.c`.
- Codegen errors use `CodegenContext.current_file` via the `codegen_report_error()`
  helper in `src/codegen.c`. `codegen_set_current_file()` switches it per
  declaration from `node->ast_token.filename`, so `MY_PUSH` frames and errors
  from imported functions report the imported file. `MY_PUSH`/`MY_POP` also
  maintain the runtime `__my_file` used by `my_panic`'s "triggered at" line.
- Common codegen diagnostics include:
  - `unknown identifier 'x'`: an identifier is not in scope as a local, parameter,
    field, or builtin (function names are resolved separately at call sites).
  - `unknown function 'f'`: the callee of a call is not a known function, method,
    or in-scope local variable (e.g., the name was never declared).
  - `method 'ClassName.method' does not exist`: a method call targets a method
    not declared on the receiver's class or interface type.

## Compiler Flags
- `mylang --leak-check source.my out.c` — enables the `MYLANG_LEAK_CHECK` macro
  so the generated program tracks every class/interface allocation and prints
  unreleased objects at exit.
- `mylang --xor-strings source.my out.c` — encrypts every source string literal
  at compile time; see docs/compiler-internals.md.
