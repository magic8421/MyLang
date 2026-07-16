# TODO: f-string support in MyLang

## Goal

Add Python-style f-strings to MyLang:

```mylang
string msg = f"hello {name}, count={n}";
print(msg);
```

The f-string is pure compiler syntax sugar: it is lowered at compile time into
a sequence of `StringBuilder` append calls.  No runtime interpolation parsing,
no C `printf`, and no variadic function support in the language are required.

## Decisions

* `string` is a magic class name provided by the standard library.  It is not
  a keyword; the compiler simply knows how to treat it specially.
* `StringBuilder` is a normal class visible to users, because it is genuinely
  useful on its own.
* There is no `print(...)` variadic function.  Output is `print(string)` plus
  f-strings.
* Format specifiers such as `{x:04d}` are not supported in the first version.
* Custom objects participate in f-strings through the `IStringable` interface.

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

## StringBuilder

A builtin standard-library class, also used by the compiler-generated f-string
lowering:

```mylang
class StringBuilder {
    native void append_string(string s);  // append another String object
    native void append_i32(i32 v);
    native void append_i64(i64 v);
    native void append_u32(u32 v);
    native void append_u64(u64 v);
    native void append_f32(f32 v);
    native void append_f64(f64 v);
    native void append_char(i8 c);
    native string toString();
}
```

`StringBuilder` is implemented in `runtime.c` and registered as a builtin class
so it can be used immediately without an import/module system.

## IStringable

```mylang
interface IStringable {
    string toString();
}
```

A custom type that wants to appear inside an f-string implements this
interface.  Codegen emits `obj.toString()` and appends the resulting `string`.

## f-string lowering

`f"hello {name}, count={n}"` is parsed as an `AST_FSTRING` node containing an
ordered list of parts:

1. string literal `"hello "`
2. expression `name`  (resolved to `string` or `IStringable`)
3. string literal `", count="`
4. expression `n`     (resolved to `i32`)

Codegen rewrites it as:

```c
StringBuilder _sb;
_sb.append_cstr("hello ");
_sb.append_string(name);   // or append_i32 / call toString() etc.
_sb.append_cstr(", count=");
_sb.append_i32(n);
String* _s = _sb.toString();
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
5. **Add `f32`/`f64` numeric literals.**  Currently floating-point values such
   as `3.14159` are not recognized by the lexer, so the `append_f32` and
   `append_f64` methods can only be exercised with expression results, not
   literals.  This also blocks `{3.14}` in f-strings.

Escape braces inside f-strings: not supported in the first version.  `\{` and
`\}` are currently not recognized, and `{{` / `}}` are not treated specially;
`{` always starts an interpolation expression.

## Codegen changes

* Recognize `AST_STRING_LIT` and `AST_FSTRING`.
* For f-string, emit a unique `StringBuilder` temporary variable, emit append
  calls for each segment, and emit `toString()` as the value of the expression.
* Make sure the temporary `StringBuilder` is released / cleaned up if it holds
  any owned references (it should not; it only borrows appended strings).
* Update `c_type_str` so `string` maps to `String*`.

## Runtime additions

* `String` allocation helper: `String* mylang_string_new(const char* cstr)`.
* `StringBuilder` native methods implemented in C using a growable byte buffer.
* Optional `mylang_print_string(String* s)` / `mylang_print_int(i32 v)` for
  the temporary debug `Logger` class.

## Phased plan

1. **String type and literals** ✅
   * Add `string` as a built-in class-like type.
   * Add `"..."` string literals.
   * Add native `puts(string)` and `puti(i32)` in a debug `Logger` class
     so development can continue without f-strings.

2. **StringBuilder runtime** ✅
   * Implement `StringBuilder` as a builtin class with native append methods.
   * Verify it can produce a `string` and that refcounting is correct.

3. **f-string parsing** ✅
   * Add `f"..."` tokenization and `AST_FSTRING`.
   * Lower simple f-strings with only string variables and primitive values.

4. **IStringable**
   * Add the interface.
   * Support custom object interpolation.

5. **Cleanup**
   * Remove or hide the temporary `Logger` class once `print(string)` or
     f-string based output is available.
