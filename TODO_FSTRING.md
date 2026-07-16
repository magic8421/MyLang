# TODO: f-string support in MyLang

## Goal

Add Python-style f-strings to MyLang:

```mylang
string msg = f"hello {name}, count={n}";
print(msg);
```

The f-string is pure compiler syntax sugar: it is lowered at compile time into
a mutable `String` accumulator and a sequence of append calls.  No runtime
interpolation parsing, no C `printf`, and no variadic function support in the
language are required.

## Decisions

* `string` is a magic class name provided by the standard library.  It is not
  a keyword; the compiler simply knows how to treat it specially.
* `String` is mutable and owns the append API directly.  There is no separate
  `StringBuilder` type: unlike Java, our `String` is already a growable buffer,
  and mutation through aliases is consistent with how every other class
  behaves.
* There is no `print(...)` variadic function.  Output is `print(string)` plus
  f-strings.
* Format specifiers such as `{x:04d}` are not supported in the first version.
* Custom objects participate in f-strings through the `IToString` interface.

## Why not `printf`?

A C-style `printf` needs either a native variadic function (`...`) or unsafe
format-string interpretation.  Both add complexity that MyLang does not need:

* Type safety is weak.
* Format specifiers are runtime strings.
* It does not fit the reference-counted `string` class model.

For debugging before f-strings land, a small `Logger` class with native methods
such as `puts(string)` and `print_int(i32)` is enough.  Those map directly to
C functions and do not require any compiler changes.

## String model

`string` is a reference-counted class internally:

```c
typedef struct String {
    u8[] bytes;
} String;
```

* `string` variables hold a `String*` pointer.
* String literals (`"hello"`) compile to `String*` objects with refcount 1.
* `string` follows the same retain/release rules as other class instances.
* `string` can be passed by value (copying the class pointer), returned, stored
  in class fields, and pushed into arrays.

## String append API

`String` is mutable and has native append methods, implemented in `runtime.c`
and registered on the builtin `String` class:

```mylang
// methods of the builtin String class
native void append_string(string s);  // append another String object
native void append_i32(i32 v);
native void append_i64(i64 v);
native void append_u32(u32 v);
native void append_u64(u64 v);
native void append_f32(f32 v);
native void append_f64(f64 v);
native void append_char(i8 c);
```

Appending through one alias is visible through all aliases, exactly like
mutating any other class instance through a shared reference.  There is no
copy-on-write.

## IToString

```mylang
interface IToString {
    string toString();
}
```

A custom type that wants to appear inside an f-string implements this
interface.  Codegen emits `obj.toString()` and appends the resulting `string`.

## f-string lowering

`f"hello {name}, count={n}"` is parsed as an `AST_FSTRING` node containing an
ordered list of parts:

1. string literal `"hello "`
2. expression `name`  (resolved to `string` or `IToString`)
3. string literal `", count="`
4. expression `n`     (resolved to `i32`)

Codegen rewrites it as:

```c
String* _fs = mylang_string_new(MYLANG_TID_String, "");
mylang_string_append_cstr(_fs, "hello ");
String_append_string(_fs, name);   // or append_i32 / call toString() etc.
mylang_string_append_cstr(_fs, ", count=");
String_append_i32(_fs, n);
// _fs is the value of the f-string expression
```

Each interpolated expression is evaluated exactly once and in source order.
Side effects are preserved.

## Lexer / parser work

1. Add a string literal token.  Support `"..."` with escape sequences `\n`,
   `\t`, `\\`, `\"`, `\0`, etc.
2. Add an f-string literal token.  Lexer splits `f"..."` into segments:
   literal text, `{expr}`, literal text, ...
3. Parser builds `AST_FSTRING` from the segments.
4. Type checker resolves each expression and picks the right append method.
5. **Add `f32`/`f64` numeric literals.** ✅  Added `TOK_FLOAT_LIT` and
   `AST_FLOAT_LIT`.  Decimal literals such as `3.14159` default to `f64`;
   a trailing `f` or `F` suffix (e.g. `1.5f`) makes them `f32`.  These can be
   used directly in f-strings (`{3.14}`) and in arithmetic.

Escape braces inside f-strings: not supported in the first version.  `\{` and
`\}` are currently not recognized, and `{{` / `}}` are not treated specially;
`{` always starts an interpolation expression.

## Codegen changes

* Recognize `AST_STRING_LIT` and `AST_FSTRING`.
* For f-string, emit a unique `String` accumulator temporary, emit append
  calls for each segment, and use the accumulator itself as the value of the
  expression.
* The accumulator and any owned interpolation temporaries are released via the
  normal cleanup list.
* Update `c_type_str` so `string` maps to `String*`.

## Runtime additions

* `String` allocation helper: `String* mylang_string_new(uint32_t tid, const char* cstr)`.
* `String` native append methods implemented in C using the growable `bytes`
  buffer, plus the codegen-only `mylang_string_append_cstr` helper.
* `mylang_print_string(String* s)` is implemented by the runtime and exposed
  to MyLang as a builtin `print(string)` function; the compiler generates a
  direct call to `mylang_print_string`.  The temporary `Logger` class has been
  removed from the f-string examples.

## Phased plan

1. **String type and literals** ✅
   * Add `string` as a built-in class-like type.
   * Add `"..."` string literals.
   * Add native `puts(string)` and `puti(i32)` in a debug `Logger` class
     so development can continue without f-strings.

2. **String append runtime** ✅
   * Implement native append methods on the builtin `String` class.
   * Verify they can build a `string` and that refcounting is correct.

3. **f-string parsing** ✅
   * Add `f"..."` tokenization and `AST_FSTRING`.
   * Lower simple f-strings with only string variables and primitive values.

4. **IToString** ✅
   * Add the interface.
   * Support custom object interpolation (class and interface values).

5. **Cleanup** ✅
   * Added a builtin `print(string)` function that maps to `mylang_print_string`.
   * Removed the temporary `Logger` class from the `fstring_basics` and
     `fstring_append` examples; they now use `print(...)` and f-strings.
   * The `fstring_native.c` helper files for those examples are no longer
     needed and have been removed.
