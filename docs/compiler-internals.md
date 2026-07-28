# MyLang — Compiler Internals

Detailed reference; the rules in AGENTS.md still apply.

## Modules (import)
- `import("path/to/file.my")` at top level merges the referenced file's
  top-level declarations into the current compilation unit (whole-program
  compilation, no separate compilation and no namespaces). The trailing
  semicolon is optional.
- Implementation lives in `src/parser.c`: `parser_parse_import()` recursively
  parses the imported file with a fresh lexer/parser (the symtab is global),
  and the imported declarations are spliced into the importing file's
  declaration list at the import position.
- Paths are resolved relative to the importing file's directory, then
  canonicalized to an absolute path with forward slashes
  (`import_resolve_path()`). The canonical path is stored in the global dedup
  set `g_import_paths` and doubles as the sub-lexer's filename — tokens keep
  the pointer, so the path must outlive the parse (do not use a local buffer).
- Dedup happens before parsing, so diamond imports and import cycles are
  compiled once and never recurse forever. `MAX_IMPORT_FILES` (64) caps the
  file count and `MAX_IMPORT_DEPTH` (16) caps nesting.
- Only the root file may define `main`; an imported file defining `main` is a
  compile error.
- Panic file/line correctness across files: tokens carry `filename`, codegen
  switches `CodegenContext.current_file` per declaration, and the runtime
  `__my_file` is maintained by `MY_PUSH`/`MY_POP`.

## Codegen Conventions
- `CodegenContext` holds the output stream in `ctx->out`. Helper functions in `codegen.c` do not take a separate `FILE*` parameter.
- The current source line is tracked in the thread-local `__my_line` variable. The compiler emits `MY_LOC(line)` before expressions that may trigger runtime panics (e.g., array access) so `my_panic` can report the offending line.
- `break`/`continue` compile to `goto _my_breakN` / `goto _my_continueN` after emitting scope cleanup, so per-iteration releases are never skipped. Loops always emit the `_my_continueN` label even when the body has no `continue`, so the generated C preamble disables MSVC warning C4102 (unreferenced label) via `#pragma warning(disable: 4102)`.

## Method Dispatch
- Methods name-mangled: `ClassName_method(ClassName* thiz, ...)`.
- `this` in MyLang source emits as `thiz` in C to avoid C++ keyword conflict.
- `p.foo(args)` emits as `ClassName_foo(p, args)`.
- Static methods (`static` modifier) omit the `thiz` parameter: `ClassName.create(args)` emits as `ClassName_create(args)`. `static_call_method()` in codegen resolves the class-name callee; see "Static Methods" in docs/language.md.
- Class name registered early in symtab for self-referential method return types.

## Native Methods
- Syntax: `native RetType ClassName.method(Params);` inside a class. Method body is omitted and ends with `;`.
- The compiler generates a header (`<out>.h`) containing the method prototype using the MyLang mangled name: `RetType ClassName_method(ClassName* thiz, ...)`.
- The user provides a matching C implementation in a separate `.c` file (e.g. `#include "out.h"`) to call platform APIs.
- Native methods follow the same ABI as regular methods: class/interface return values must be retained by the callee (`mylang_retain`) before returning; parameters are borrowed for the duration of the call.
- `ref T` parameters become `T*`, `weak T` becomes `WeakRef*`, interfaces pass by value as their fat-pointer struct, and arrays `T[]` pass by value as `MyArray`.
- Generated `.c` files `#include` the generated header, so prototypes do not need to be repeated.
- Native methods are not automatically wrapped with `MY_PUSH`/`MY_POP` in phase 1; if a native implementation calls `my_panic`, the stack trace may stop at the MyLang caller.

## Cleanup System
- `CleanupEntry` array tracks class/weak/array variables requiring release at scope exit.
- Scope-based push/pop (`cleanup_push_scope`, `cleanup_pop_scope`) emits releases in reverse declaration order.
- `cleanup_emit` in return statements releases all variables on the return code path.
- `cleanup_reset` was removed: it zeroed `cleanup_scope_depth`, corrupting state for fallthrough code paths and causing memory leaks.
- Subsequent `cleanup_pop_scope` calls after returns generate dead code (harmless — after `return` in C, function exits immediately).

## Lexer Safety
- `read_char_literal` has EOF guards after opening quote, after backslash, and before closing quote peek.
- Truncated or unterminated char literals return a dummy token instead of reading out of bounds.

## String Handling
- Custom `strscpy` in `util.h` guarantees null termination.
- `CHECK_STRSCPY` and `CHECK_SNPRINTF` macros abort on truncation.
- All `snprintf` and `strscpy` call sites are checked.

## Memory Leak Debugging
- To verify there are no memory leaks, run the test suite in debug mode: `python test_runner.py --mode debug`.
- Tracks only `ObjHeader` based allocations (class instances and interface objects). Arrays are not tracked by the MyLang leak list.
- When enabled, the generated C code adds `next`/`prev`/`alloc_trace` to `ObjHeader` and records every allocation in a global circular doubly-linked list.
- `mylang_release` removes the block from the list when the destructor runs (the memory itself may be freed later, once the last weak share is gone).
- On the first allocation, `atexit(mylang_leak_check)` is registered; at exit, unreleased blocks are printed with address, type_id, refcount, length, and allocation stack trace.
- Stack traces are hashed into a 512-bucket table so identical call stacks share one `LeakTrace` record.
- The list and hash table are protected by a global lock: `SRWLOCK` on Windows (`SRWLOCK_INIT`), `pthread_mutex_t` with `PTHREAD_MUTEX_INITIALIZER` on POSIX.
- In `--mode debug` or when `--leak-check` is enabled, the test runner prints captured stdout/stderr so the CRT leak dump (`_CrtDumpMemoryLeaks`) or the MyLang leak report is visible.

## String Encryption (`--xor-strings`)
- Optional compiler flag: `mylang --xor-strings source.my out.c`.
- When enabled, every source string literal is XOR-encrypted at compile time with
  a per-literal 8-bit key. The generated C file contains `static const uint8_t _xsN[]`
  cipher arrays; each declaration carries a trailing `// "..."` comment with the
  original literal to keep the generated C reviewable (the cipher bytes are what
  the program actually reads).
- Codegen uses:
  - `mylang_string_new_encrypted(MYLANG_TID_String, _xsN, len, key)` for plain literals.
  - `mylang_string_append_cstr_encrypted(acc, _xsN, len, key)` for f-string literal segments.
  - `mylang_string_new_encrypted(..., NULL, 0, 1)` for empty strings.
- The runtime decrypts the bytes into a `String` object. This is a convenience
  obfuscation switch, not strong cryptography; each literal is trivially recovered
  from the key shipped in the generated C.
- The flag does not affect string semantics at runtime: interpolation, `print`,
  `String.equals`, and IToString all work identically.

## Known Limitations
- `lock()` is a pseudo-method on weak refs; not a general keyword.
- Fixed caps, each reported as a compile error when exceeded: 32 fields per class/struct
  (`MAX_FIELDS`), 32 methods per interface (`MAX_IFACE_METHODS`), 8 implemented
  interfaces per class (`MAX_IMPL`), 16 parameters per function/method, 8 generic
  parameters (`MAX_GENERIC_PARAMS`). Class method count is unlimited (linked list).
- Weak refs cannot be declared in if/while conditions (no `if (Node s = w.lock())`).
- Interface default method implementations are supported. Default bodies cannot use `this` and may only reference parameters, literals, and control flow.
- `match` does not support `true`/`false` literal arms; `match (null)` is a compile error.
- Mixed arithmetic containing bool operands (e.g. `1 + (a < b)`) is not checked at the operand level; C promotion rules apply. The strict bool rule is enforced only at assignment boundaries.
- `const` covers only primitive value types; const fields and const-qualified reference types are not supported yet.
- No AST deallocation function (one-shot compiler).
