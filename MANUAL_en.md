# MyLang Language User Manual

This manual is written for users of MyLang. It covers the language syntax, usage
notes, and runtime-related practical advice. Compiler internals are out of scope.

## Contents

- [1. Language Overview](#1-language-overview)
- [2. Lexical Basics](#2-lexical-basics)
- [3. The Type System](#3-the-type-system)
- [4. Variables, Assignment, and Operators](#4-variables-assignment-and-operators)
- [5. Control Flow](#5-control-flow)
- [6. Functions](#6-functions)
- [7. Classes](#7-classes)
- [8. Interfaces](#8-interfaces)
- [9. Structs](#9-structs)
- [10. Strings and f-strings](#10-strings-and-f-strings)
- [11. Arrays (T[] Dynamic Vectors)](#11-arrays-t-dynamic-vectors)
- [12. Generic Classes](#12-generic-classes)
- [13. Memory Management: weak and unowned](#13-memory-management-weak-and-unowned)
- [14. Native Methods (C Interop)](#14-native-methods-c-interop)
- [15. Pitfalls Quick Reference](#15-pitfalls-quick-reference)
- [16. Runtime Tips](#16-runtime-tips)

---

## 1. Language Overview

MyLang is a statically compiled language whose syntax resembles a small subset of C#:

- Automatic memory management: class instances are reference-counted and released
  automatically when variables go out of scope. No manual free.
- No class inheritance; polymorphism is achieved through interfaces.
- No exceptions; runtime errors (e.g. out-of-bounds access) panic and terminate the program.
- The program entry point is `i32 main()`; its return value is the process exit code.

A source file is a sequence of top-level declarations in any order: `class`, `struct`,
`interface`, and functions. Types may refer to each other regardless of declaration
order (a class defined later can be used as a field type of one defined earlier).

Minimal example:

```mylang
i32 main() {
    string msg = "hello";
    print(f"{msg}, world!\n");
    return 0;
}
```

A fuller example (interfaces + polymorphism):

```mylang
interface IShape {
    i32 area();
}

class Square : IShape {
    i32 side;
    i32 area() { return this.side * this.side; }
}

i32 main() {
    Square sq = new Square;
    sq.side = 6;
    IShape s = sq;          // a class implicitly converts to an interface it implements
    return s.area();        // dynamic dispatch; returns 36
}
```

---

## 2. Lexical Basics

### Comments

Line comments only: `// ...`. No `/* */` block comments.

### Identifiers

Start with a letter or `_`, followed by letters, digits, or `_`. Case-sensitive.

### Keywords

```
u8 u16 u32 u64  i8 i16 i32 i64  f32 f64  bool  string
class struct interface object enum
ref weak unowned const
if else while for return break continue match
new this as native override
public private static
true false null
```

`void` is also available as a return type.

### Literals

| Form | Example | Type |
|------|---------|------|
| Integer literal | `0`, `42`, `0x2A` | `i32` by default; decimal, or hexadecimal with a `0x`/`0X` prefix |
| Float literal | `3.14` | `f64` |
| Float literal with suffix | `1.5f`, `1.5F` | `f32` |
| Boolean literal | `true`, `false` | `bool` |
| Null literal | `null` | reference types only |
| Character literal | `'A'`, `'\n'`, `'\t'`, `'\\'`, `'\''`, `'\0'` | `i8` |
| String literal | `"hello"` | `string` (each evaluation produces a new owned object) |
| f-string | `f"x={x}"` | `string` |

### What does not exist

- No exceptions (try/catch); errors are reported via return values or runtime panics.
- No ternary operator `?:`.
- No block comments `/* */`.
- No class inheritance (see Section 7).

---

## 3. The Type System

### 3.1 Primitive Types

```
i8 i16 i32 i64    signed integers
u8 u16 u32 u64    unsigned integers
f32 f64           floating point
bool              boolean (true / false)
void              return type only
```

`i32` is the default type of integer literals and the usual general-purpose integer type.
Comparison (`==`, `<`, ...) and logical (`&&`, `||`, `!`) expressions have type `bool`.

### 3.2 Three Kinds of User Types

| Kind | Semantics | Allocation | Assignment behavior |
|------|-----------|------------|---------------------|
| `class` | reference type, reference-counted | heap (`new`) | copies the reference; both variables point to the same object |
| `struct` | value type | stack | copies the whole value |
| `interface` | interface value (carries an object reference + dispatch info) | references the underlying object | copies the interface value; same underlying object |

How to choose:

- Shared, mutable objects, or polymorphism needed → `class`.
- Small pure-data aggregates (coordinates, colors, ...) → `struct`.
- Program to capabilities; the caller does not care about the concrete class → `interface`.

### 3.3 Other Type Forms

```
T[]             dynamic array (value-type vector, see Section 11)
weak T          weak reference (class or interface)
unowned T       unowned reference (class only)
object          universal reference type (see 3.5)
Box<i32>        generic class instantiation (see Section 12)
```

### 3.4 Built-in Types

- `string`: the built-in mutable string class (see Section 10).
- `IToString`: a built-in interface with the signature `string toString()`; classes
  implementing it can be interpolated into f-strings.
- `print(string)`: a built-in function that writes the string to standard output.

### 3.5 The object Type

`object` is the universal top reference type: any class (including `string`) or
interface value converts to it **implicitly**:

```mylang
object o = sq;          // Square implicitly converts to object
object o2 = "a string";
```

Rules:

- Converting back is **explicit only**: `Square s = o as Square;` (yields `null` on a
  type mismatch; applying `as` to a `null` value safely yields `null`). You can also
  use `match` type arms (see 5.5).
- `object` cannot be cast directly to an interface — cast to a concrete class first.
- `object` has no members: `o.field` and `o.method()` are compile errors; cast with `as` first.
- Assigning an `object` back to a concrete class variable without `as` is a compile error.
- There is no `weak object` / `unowned object`; `new object` is illegal.
- f-string interpolation and `print` do not accept `object` (cast to a concrete type
  implementing `IToString` first).

Typical uses: heterogeneous containers (`object[]` arrays holding different kinds of
objects), opaque handle passing.

### 3.6 The Reference/Value Boundary

MyLang strictly separates two families of types at compile time:

- **Reference-like**: class (including string), interface, object, plus their weak forms.
- **Value types**: primitive numeric types, bool, struct, arrays.

The two families never convert implicitly; this is checked at initialization, assignment,
call arguments, `return`, and array element assignment:
`u64 x = someObject;` and `SdlWindow w = 5;` are compile errors.

The same separation applies to comparisons: reference-like types may only be compared
with reference-like types (or with `null`, and only via `==`/`!=`); `someObject == 0`
is a compile error. Numeric comparisons within value types are unrestricted.

### 3.7 Enums (simple)

An enum declares a set of named integer constants under one type name
(C++ `enum class` style):

```mylang
enum Key { Up, Down, Left = 10, Right }
```

- Variants are accessed in scoped form only: `Key.Up`. Variant names do not
  enter the surrounding scope, and a local variable named `Key` shadows the
  enum name (same rule as class static calls).
- Values start at 0 and auto-increment; an explicit `= <integer>` (may be
  negative) restarts numbering from that value. In the example above,
  `Right` is 11.
- Enums are strongly typed, like `bool`: no implicit conversion between an
  enum and any integer type, and no implicit conversion between two different
  enum types. Use an explicit `as` cast in either direction:

```mylang
Key k = Key.Left;
i32 code = k as i32;    // 10
Key back = code as Key; // explicit cast back
```

- `==` and `!=` compare two values of the same enum type. Arithmetic,
  relational operators, compound assignment, and `++`/`--` are rejected.
- Enums work anywhere a value type works: local variables, function
  parameters and return values, `ref` parameters, struct/class fields, and
  array elements (`Key[]`).
- `match` supports enum variant constant arms (see 5.5). There is no
  exhaustiveness check.
- Current limitations: no enum methods, no `const Key`, no enum default
  parameter values, no direct f-string interpolation (use `as i32` first),
  and no payload (tagged-union) variants — `Variant(...)` or `Variant {...}`
  is a dedicated compile error.

---

## 4. Variables, Assignment, and Operators

### 4.1 Variable Declarations

```mylang
i32 x = 10;
i32 y;                  // zero-initialized
Square sq = new Square;
IShape s = sq;
weak Node w = obj;
Node nothing = null;    // reference types can be initialized with null
```

- Only one variable per declaration statement: `i32 x, y;` is invalid.
- Local variables of all types are cleaned up automatically at scope exit
  (references released, array buffers freed); nothing to do manually.

### 4.2 Assignment

```mylang
x = x + 1;
sq.side = 6;
arr[0] = 10;
```

Note: although assignment (`=`) is technically an expression, it is **not allowed** in:

- the conditions of `if` / `while` / `for` (this rules out the `if (a = b)` typo);
- variable initializers;
- `return` expressions.

### 4.3 Compound Assignment and Increment/Decrement

```mylang
x += 1;  x -= 2;  x *= 3;  x /= 4;               // arithmetic forms: numeric types
x &= 0xff;  x |= 1;  x ^= 2;  x <<= 3;  x >>= 1;   // bitwise forms: integers only
i++;  ++i;  i--;  --i;
obj.field++;
arr[i]--;
```

Restrictions:

- Arithmetic forms (`+= -= *= /=`) accept all primitive numeric types; bitwise forms
  (`&= |= ^= <<= >>=`) require integer types (no floats).
- Using compound assignment on class, interface, struct, array, object, or bool values
  is a compile error.
- `++` / `--` are only legal as **standalone expression statements**; they cannot be
  nested in expressions: `y = x++`, `return x++`, `foo(x++)`, and `if (x++)` are all invalid.
- Where they are legal, prefix and postfix forms have the same effect.
- Compound assignment is likewise banned in conditions, initializers, and `return`.

### 4.4 Bitwise Operators

```mylang
i32 flags = 0xff00 | (1 << 3);   // hex literals pair naturally with bit operations
i32 masked = flags & 0xff;       // 8
i32 inv = ~flags;
```

`&`, `|`, `^`, `~`, `<<`, `>>` are supported with C-compatible precedence:

- Operands must be integer types; floats, bool, and reference types are compile errors.
- The result type is the left operand's type (`~` yields the operand type).
- Shift counts are not checked at runtime (C semantics).
- Boolean logic keeps using `&&`, `||`, `!`; there are no bool `&` / `|` operators.

### 4.5 Operator Precedence (low to high)

| Level | Operators |
|-------|-----------|
| 1 | `=` (right-associative) |
| 2 | `||` |
| 3 | `&&` |
| 4 | `|` |
| 5 | `^` |
| 6 | `&` |
| 7 | `==` `!=` |
| 8 | `<` `<=` `>` `>=` |
| 9 | `<<` `>>` |
| 10 | `+` `-` |
| 11 | `*` `/` `%` |
| 12 | unary `-` `!` `~` |
| 13 | postfix: `[]` `.` `()` `as Type` |

### 4.6 Strict bool Rules

- `bool` is its own type and does **not** implicitly convert to or from numeric types.
  This is checked at the boundaries — initializers, assignments, call arguments, and
  `return`: `bool b = 5;`, `i32 x = true;`, and `i32 x = a < b;` are compile errors.
- Conditions are **not** required to be bool: `if (ptr)`, `while (1)`, and
  `while (w.lock())` keep their C truthiness semantics and remain legal.
- bool supports neither compound assignment nor `++`/`--`.
- Interpolating a bool in an f-string prints `true` / `false`.
- Known blind spot: mixing bool into arithmetic (e.g. `1 + (a < b)`) is not rejected at
  the operand level and follows C promotion rules — avoid writing such code.

### 4.7 null and Truthiness

`null` is the empty-value literal for reference types: class (including string),
interface, weak class, and weak interface:

```mylang
Node n = null;
n = obj;
if (n == null) { ... }     // == null / != null only work on reference types
```

- `null` is allowed in: initializers, assignments (locals, fields, array elements),
  `==`/`!=` comparisons, call arguments, and `return`.
- `null` is **rejected** for: primitive/bool/struct/array targets, `unowned` references,
  arithmetic and relational operators, member access, array indexing, `as` casts,
  `match` expressions, and f-string interpolation.

Conditions keep C truthiness semantics, so references can be tested directly:

```mylang
if (p) { ... }      // p is non-null
if (!p) { ... }     // p is null (same as p == null)
```

Check the results of `weak.lock()` and `as` this way:

```mylang
Node s = w.lock();
if (!s) { return; }        // the object is dead

Square sq = s as Square;   // null when the type does not match
if (sq) { ... }
```

### 4.8 const Values

`const` declares a read-only local variable or parameter, restricted to primitive
value types (integers, floats, bool):

```mylang
const i32 MAX = 100;            // an initializer is required
void f(const u32 x) { ... }     // parameters may be const too
```

Rules:

- An initializer is mandatory at declaration.
- Assigning to a const (including compound forms), applying `++`/`--` to it, or passing
  it to a `ref` parameter are all compile errors.
- Combining `ref` + `const` is illegal.
- `const` cannot be applied to class / interface / object / struct / array / weak /
  unowned types, nor to fields.

---

## 5. Control Flow

### 5.1 if / else

```mylang
if (x > 5) {
    return 1;
} else if (x > 0) {
    return 2;
} else {
    return 42;
}
```

- **Braces are mandatory** on branch bodies: `if (x > 5) return 1;` is a compile error.
  The only exception is that `else` may be followed directly by another `if`, enabling
  else-if chains.
- Conditions are not required to be bool; C truthiness applies (`if (ptr)`, `if (x)` are fine).
- Variable declarations are not allowed inside conditions
  (no `if (Node s = w.lock())` form).
- Assignment, compound assignment, and `++`/`--` are not allowed in conditions.

### 5.2 while

```mylang
while (i <= 10) {
    s = s + i;
    i = i + 1;
}
```

The loop body must be braced as well.

### 5.3 for

C-style three-part loop (the body must be braced):

```mylang
for (i32 i = 0; i < n; i = i + 1) {
    // ...
}
```

- `init` may be a variable declaration (`i32 i = 0`), an expression, or omitted.
- Variables declared in `init` are scoped to the loop and released on normal loop exit.
- `condition` may be omitted, which means an infinite loop; assignment and `++`/`--`
  are not allowed in the condition.
- `step` is an expression and may be omitted; `i++` is allowed in the step.

### 5.3.1 foreach

Arrays can be iterated directly (the body must be braced):

```mylang
i32[] a;
a.push(10);
a.push(20);
foreach (i32 x in a) {
    print(x);
}
```

- The loop variable is a per-iteration **copy** of the element and is scoped to
  the loop body. For class elements it is a normal strong reference.
- The collection must be an array; anything else is a compile error. `string`
  is not iterable (byte-wise vs UTF-8 semantics differ; use `char_at(i)` in a
  plain loop).
- The array length is re-read on every iteration: mutating the array inside
  the loop body affects iteration (bounds checks keep this memory-safe).
- `break` / `continue` work as in `for`.

### 5.4 break / continue

```mylang
for (i32 i = 0; i < 100; i = i + 1) {
    if (i == 3) { continue; }   // skip this iteration
    if (i > 10) { break; }      // exit the innermost loop
}
```

`break` / `continue` correctly run local-variable cleanup, so they are safe to use in
loops that hold objects.

### 5.5 match

`match` tries arms in order; the first matching arm runs and the rest are skipped:

```mylang
match (i) {
    1 => { s = s + 1; }        // integer literal arm
    2 => { s = s + 10; }
    else => { s = s + 100; }   // else must be the last arm
}
```

For class / interface / object expressions, type arms are available; the bound variable
is only visible inside the arm body:

```mylang
i32 classify(IShape s) {
    match (s) {
        Square sq => { return sq.side; }    // sq has type Square
        Circle ci => { return ci.radius; }
        else => { return 0; }
    }
}
```

For enum expressions, variant constant arms are available; the arm variant must
belong to the matched enum type:

```mylang
match (k) {
    Key.Up => { dy = -1; }
    Key.Down => { dy = 1; }
    else => { dy = 0; }       // no exhaustiveness check: unmatched variants fall through
}
```

Key points:

- The match expression is evaluated once (not once per arm).
- Type-arm requirement: when the expression is an interface, the pattern class must
  implement that interface.
- `else` must come last and matches everything remaining.
- Bound variables do not participate in reference counting (do not rely on them to keep
  the object alive — the object is protected by the reference the match expression itself holds).
- `true` / `false` literal arms are not supported; `match (null)` is a compile error.
- Matching on an `unowned` expression is rejected; convert to a strong reference first.

---

## 6. Functions

### 6.1 Declaration and Calls

```mylang
i32 add(i32 a, i32 b) {
    return a + b;
}

void noop() { }
```

- Top-level functions may be declared in any order; calling before declaring is fine.
- Use `return;` in `void` functions and `return expr;` elsewhere.

### 6.2 ref Parameters

`ref` makes a parameter alias a caller variable, so mutations are visible to the caller:

```mylang
void inc(ref i32 x) {
    x = x + 1;
}

i32 a = 0;
inc(ref a);      // ref is also required at the call site; a == 1 afterwards
```

Rules:

- The `ref` keyword at the call site is **mandatory**; omitting it is a compile error.
- The argument must be a **local variable** (or another `ref` parameter); field accesses
  and array elements (e.g. `inc(ref obj.x)`, `inc(ref arr[0])`) are rejected.
- A const variable cannot be passed to a `ref` parameter, and a `ref` parameter cannot
  itself be const.
- Arrays are passed via `ref T[]` (see Section 11).

### 6.3 Default Parameter Values

Trailing parameters may declare a default value, and callers may omit them:

```mylang
void greet(string name, string greeting = "hello", i32 times = 1) {
    for (i32 i = 0; i < times; i = i + 1) {
        print(f"{greeting} {name}");
    }
}

greet("bob");              // greeting = "hello", times = 1
greet("bob", "hi");        // times = 1
greet("bob", "hi", 3);     // all explicit
```

Rules:

- Defaults work on free functions, class methods, struct methods, and
  interface methods.
- Default values are **literals only**: integer, float, char, string,
  `true`/`false`, or `null`. The literal must match the parameter type
  (e.g. no bool literal for an `i32` parameter, no `null` for value types;
  `null` is allowed for class, interface, object, and weak parameters).
- Once a parameter has a default, every parameter after it must have one too.
- `ref` parameters cannot have defaults.
- Omitting a non-defaulted argument, or passing more arguments than there are
  parameters, is a compile error.
- Defaults bind to the **static type** at the call site: calling through an
  interface-typed variable uses the interface method's defaults, not the
  implementing class's.
- Named arguments (`greet(times: 3)`) are planned but not implemented yet;
  arguments are always positional.

### 6.4 Return-Value Conventions (from the user's side)

- For class / interface return values, the callee handles reference counting; the caller
  receives a ready-to-use reference that can be stored, reassigned, and returned safely.
- Arrays `T[]` **cannot** be returned by value or passed by value; use `ref T[]`
  parameters or `move_to` / `copy_to` instead.

### 6.5 Multi-file Modules: import

`import("path")` merges another source file's top-level declarations (classes,
structs, interfaces, functions) into the current compilation unit:

```mylang
import("lib/geometry.my");   // the semicolon is optional
import("lib/utils.my")

i32 main() {
    Point p = new Point;     // from geometry.my
    return helper(p);        // from utils.my
}
```

Rules:

- Only allowed at the **top level** (conventionally at the top of the file);
  the path is a string literal.
- Paths resolve relative to the **importing file's directory** (not the
  compiler's working directory).
- Each file is compiled once: diamond imports (a and b both import c) and
  import cycles (a ↔ b) are deduplicated automatically — no duplicate
  definitions, no infinite recursion.
- There are no namespaces: imported declarations enter the global namespace;
  name clashes are reported as duplicate definitions.
- Only the root file may define `main`; a `main` in an imported file is a
  compile error.
- Runtime panics report the file and line of the **definition site**, so
  cross-file debugging works.
- The output is still a single `.c`/`.h`; native method implementations keep
  `#include`-ing the final generated header as before.

---

## 7. Classes

```mylang
class Node {
    i32 value;
    Node next;

    void set(i32 v) {
        this.value = v;
    }
}

Node n = new Node;     // note: no parentheses
n.set(10);
```

Key points:

- `new ClassName` creates an instance with **no parentheses and no constructors**;
  all fields are zero-initialized.
- Field types may be primitives, bool, `string`, class, interface, object, weak,
  unowned, or arrays.
- Methods use `this` to refer to the current object.
- **No class inheritance** (no `extends`, no base classes); reuse via composition,
  polymorphism via interfaces.
- Classes have reference semantics: after `Node a = b;`, `a` and `b` point to the same
  object and see each other's mutations.
- An object is finalized and reclaimed as soon as its last reference disappears; any
  references held in its fields are released automatically.
- Class, struct, and interface names are registered before their bodies are parsed, so
  fields and method signatures can refer to types defined later in the file.

### 7.1 Implementing Interfaces

```mylang
class Square : IShape, IToString {
    i32 side;
    i32 area() { return this.side * this.side; }
    string toString() { return f"Square({this.side})"; }
}
```

- A class may implement multiple interfaces, comma-separated.
- The class must implement every method required by its interfaces (except methods with
  default implementations) with matching signatures, or compilation fails.
- The `override` keyword is **optional**: when written, the compiler verifies that the
  method actually overrides an interface method and reports signature mismatches.
  Writing it is recommended — it catches typos and signature drift early:

```mylang
override i32 area() { return this.side * this.side; }
```

### 7.2 The as Type Assertion

```mylang
IShape s = sq;
Square s2 = s as Square;   // succeeds when s is actually a Square; otherwise null
if (s2) { ... }
```

- `as` is a postfix operator: `expr as ClassName`.
- The target must be a class type; on failure the result is `null` — check it with
  truthiness or `== null`.
- Applying `as` to a `null` value is safe and yields `null`.
- `as unowned T` is illegal; an `object` cannot be cast directly to an interface
  (cast to a concrete class first).

### 7.3 Access Modifiers (public / private)

Class fields and methods can be marked `private` (the default is `public`, which may
also be written explicitly):

```mylang
class Counter {
    private i32 count;

    public void bump() {
        this.count = this.count + 1;   // private members are accessible inside the class
        this.log();                    // private methods can be called here too
    }

    private void log() { ... }
}
```

Rules (C++ style):

- A `private` member is visible only inside methods of the **same class** — any instance
  of that class (`this.x` and `other.x` both work). Access from other classes or free
  functions is a compile error.
- Modifiers are only allowed on class members; using them on structs, interfaces, or
  top-level functions/types produces a dedicated parse error.
- A method implementing an interface method must be `public`; `private` + `override`
  is a compile error.
- Enforcement is compile-time only and costs nothing at runtime.

### 7.4 Static Methods

A class method can be marked `static`. Static methods belong to the class itself
rather than to an instance, which makes them a good fit for factory methods:

```mylang
class Point {
    i32 x;
    i32 y;

    static Point create(i32 x, i32 y) {
        Point p = new Point;
        p.x = x;
        p.y = y;
        return p;
    }
}

Point p = Point.create(1, 2);   // called via the class name
```

Rules:

- A static method is called only via the class name: `ClassName.method(args)`.
  Calling it via an instance (`p.create(...)`) is a compile error, and calling an
  instance method via the class name is a compile error too.
- There is no `this` inside a static method; using `this` or an instance field
  is a compile error.
- `static` combines with access modifiers (`public static`, `private static`);
  a `private static` method is callable only inside the class.
- `static` cannot be combined with `native` or `override`, static fields are not
  supported, and interfaces cannot declare static methods.
- Default parameter values work the same as on instance methods. Static methods
  on generic classes are not supported.

---

## 8. Interfaces

### 8.1 Declaration

```mylang
interface IShape {
    i32 area();
    f64 perimeter();
}
```

Interfaces contain only method signatures — no fields. Once a class declares an
interface, it converts to that interface implicitly:

```mylang
IShape s = sq;        // variable initialization
s = sq2;              // assignment
return sq;            // when the return type is an interface
draw(sq);             // when the parameter type is an interface, class arguments convert too
```

Calls through an interface are dynamically dispatched: `s.area()` invokes the method of
the object `s` actually points to.

### 8.2 Default Interface Methods

An interface method may carry a default implementation, which implementing classes may omit:

```mylang
interface IGreeter {
    string name();
    string greet() {            // default implementation
        return "hello";
    }
}
```

Limitations: default method bodies **cannot use `this`** — only parameters, literals,
and control flow; default methods cannot be `native`.

### 8.3 Interface Value Semantics

- Interface values are copied by value (parameters are passed by value), but the
  underlying object is still a shared reference.
- Interface values participate in reference counting: holding an interface value holds a
  reference to the object, released automatically at scope exit.
- An interface value can be assigned `null`; test it with `if (s)` / `s == null`.

---

## 9. Structs

```mylang
struct Vec {
    i32 x;
    i32 y;
}

Vec v;
v.x = 1;
Vec w = v;        // copies the whole value; w and v are independent
```

- Value type, stack-allocated; assignment copies the whole struct.
- Fields may be primitives, bool, **other structs** (embedded by value), and
  **reference types** (class, interface, object, string, weak, unowned — see 9.2
  for the ownership semantics):

```mylang
struct Point { i32 x; i32 y; }
struct Rect {
    Point origin;     // embedded by value
    i32 w;
    i32 h;
}
```

  Direct or indirect recursive embedding (`A` contains `B`, `B` contains `A`) is a
  compile error.
- Array fields are not allowed (`T[]` cannot be a struct field).
- `new Vec` is illegal (structs need no `new`); `Vec[] arr; arr.resize(3);` creates an
  array of structs — but structs owning reference fields cannot be array elements
  (see 9.2).

### 9.1 Struct Methods

Structs may declare methods. Inside a method, `this` refers to the receiver
(mutating `this` mutates the original variable):

```mylang
struct Vec2 {
    i32 x;
    i32 y;

    void set(i32 nx, i32 ny) {
        this.x = nx;
        this.y = ny;
    }

    Vec2 add(Vec2 o) {          // params and returns are by value
        Vec2 r;
        r.x = this.x + o.x;
        r.y = this.y + o.y;
        return r;
    }
}

Vec2 a;
a.set(1, 2);
Vec2 b;
b.set(3, 4);
Vec2 c = a.add(b);              // c is an independent new value
```

Rules:

- The receiver of a method call must be an **lvalue**: a local variable, a field
  access, or an array element (`v.m()`, `obj.s.m()`, `arr[i].m()` all work); calling
  on a temporary (e.g. `make().m()`) is a compile error.
- Struct methods are always public; `native`, `override`, and access modifiers are
  not supported.
- Structs cannot implement interfaces (interface values reference heap objects;
  polymorphism for value types would require boxing, which is not implemented).
- Methods add behavior only — value semantics are unchanged: assignment, parameter
  passing, and returns still copy the whole struct, and there are no
  constructors/destructors.

### 9.2 Ownership of Reference Fields

Structs may hold reference-type fields (class, interface, object, string, weak,
unowned). The compiler generates retain/release hooks for such structs — you never
write any counting code yourself:

```mylang
class Node { i32 v; }
struct Box {
    Node n;              // Box holds one strong reference to the Node
}

Box a;
a.n = new Node;
Box b = a;               // copying retains: both Boxes hold a reference
// a and b each release automatically at scope exit
```

Rules:

- **Copying retains**: variable initialization (`Box b = a;`), assignment
  (`b = a;` — retains the new value before releasing the old, self-assignment
  safe), by-value parameters (the callee retains on entry and releases at scope
  exit), and by-value returns (retained for the caller) are all handled.
- **Destruction releases**: locals at scope exit, overwritten values, structs in
  a class being destructed, and nested structs torn down with their parent all
  release what they hold.
- A struct returned from a function is already owned; storing it does not
  double-retain.
- Uninitialized struct locals are zero-initialized (reference fields are null),
  so releasing is always safe.
- Nesting is recursive: copying/destroying the outer struct reaches the innermost
  fields.
- **Limitation**: structs owning reference fields cannot be array elements
  (`Box[] a;` is a compile error) — arrays cannot run per-element hooks yet;
  support is planned.
- weak / unowned fields are managed too: copying takes a weak share, destruction
  releases it.

---

## 10. Strings and f-strings

### 10.1 The string Type

`string` is the built-in mutable string class (reference semantics):

```mylang
string s = "ab";
s.append_string("cd");   // s is now "abcd"
s.append_i32(12);        // "abcd12"
s.append_i64(123456789012);
s.append_u32(7);
s.append_u64(7);
s.append_f32(1.5f);
s.append_f64(2.5);
s.append_char('X');
s.append_bool(true);     // appends "true"

bool same = s.equals("abcd12Xtrue");   // content comparison

u64 n = s.length();        // byte length: 10
i8 c = s.char_at(0);       // 'a' (bounds-checked)
```

The backing byte array is private: `s.bytes` is a compile error. Use
`length()` / `char_at(i)` for read access.

Note: string is a **mutable** reference type — appending through one alias is visible
through every alias to the same object (same as any other class):

```mylang
string a = "x";
string b = a;
b.append_string("y");    // a is now "xy" too
```

### 10.2 f-strings

```mylang
i32 n = 7;
string name = "bob";
bool ok = true;
print(f"hello {name}, n={n}, ok={ok}\n");
print(f"the answer is {42}\n");
print(f"f32={1.5f}, f64={3.14}\n");
```

- Interpolated expressions may be variables, arithmetic, method calls, etc. Each is
  evaluated exactly once, left to right.
- Types that can be interpolated: all primitive numeric types, `bool` (prints
  `true`/`false`), `string`, classes implementing `IToString`, and interface values
  (dispatched dynamically to `toString()`).
- `null` and `object` cannot be interpolated (cast object to a concrete type first).
- Brace escaping: `{{` and `}}` produce literal braces; `\{` and `\}` escape a single
  brace (also works in plain strings); a lone `{` always starts an interpolation, and a
  lone `}` is emitted literally.

```mylang
print(f"literal braces: {{ }} and \{x}\n");
```

### 10.3 IToString

Implement `IToString` to make a custom class interpolatable:

```mylang
class Point : IToString {
    i32 x;
    i32 y;
    string toString() {
        return f"({this.x}, {this.y})";
    }
}

Point p = new Point;
p.x = 3; p.y = 4;
print(f"p={p}\n");            // prints p=(3, 4)
```

### 10.4 print

```mylang
print(f"n={n}\n");
```

`print` takes a single `string` argument and writes it verbatim (no automatic newline —
add `\n` yourself).

---

## 11. Arrays (T[] Dynamic Vectors)

`T[]` is a value-type dynamic array (like C++ `std::vector`):

```mylang
i32[] a;              // the declaration itself is an empty array (length 0, capacity 0); no new needed
a.push(10);           // grows automatically
a.push(20);
a.resize(10);         // set the length (extra elements zero-initialized)
a.reserve(100);       // reserve capacity only, length unchanged

i32 x = a[0];         // indexed access (bounds-checked at runtime)
a[1] = 5;
i32 n = a.length();   // current length
```

Element types may be primitives, bool, class, interface, object, struct, or `weak T`.

### Built-in Methods

| Method | Effect |
|--------|--------|
| `a.length()` | current element count (u64) |
| `a.capacity()` | current reserved capacity (u64) |
| `a.push(v)` | append at the end, growing capacity as needed |
| `a.pop()` | remove the last element |
| `a.reserve(n)` | reserve capacity (length unchanged) |
| `a.resize(n)` | change the length (grown part zero-initialized) |
| `a.clear()` | empty the array (length 0) |
| `a.compact()` | release excess capacity |
| `a.move_to(ref dst)` | **move** the contents into dst; a becomes empty |
| `a.copy_to(ref dst)` | **deep-copy** the contents into dst; a is unchanged |

Arrays have no member fields: `a.length` / `a.capacity` (without parentheses)
are compile errors; use the methods `a.length()` / `a.capacity()` instead.

### Important Restrictions

- **Arrays cannot be assigned with `=`**: `b = a;` is illegal. Use `a.move_to(ref b)` to
  transfer and `a.copy_to(ref b)` to duplicate.
- **No pass-by-value or return-by-value**: functions that take or mutate arrays use
  `ref T[]` parameters:

```mylang
void fill(ref i32[] arr) {
    arr.push(1);
}

i32[] nums;
fill(ref nums);
```

- Arrays are allowed as class fields and are freed automatically when the object is destroyed.
- An array variable's buffer is freed at scope exit; if the elements are class/weak
  references, each element is handled correctly.

### Transfer Example

```mylang
i32[] a;
a.push(1);
a.push(2);

i32[] b;
a.copy_to(ref b);   // b = [1,2]; a is still [1,2]

i32[] c;
a.move_to(ref c);   // c = [1,2]; a becomes empty
```

---

## 12. Generic Classes

Generic classes are instantiated at compile time (monomorphization, like C++ templates /
C# generics):

```mylang
class Box<T> {
    T value;
    void set(T v) { this.value = v; }
    T get() { return this.value; }
}

Box<i32> b1 = new Box<i32>;
b1.set(10);

Box<Box<i32>> b2 = new Box<Box<i32>>;   // nesting works
b2.set(b1);
```

Multiple type parameters:

```mylang
class Pair<A, B> {
    A first;
    B second;
}

Pair<i32, string> p = new Pair<i32, string>;
```

### Constraints

Type parameters can carry interface constraints, after which interface methods can be
called directly:

```mylang
interface IPrintable {
    i32 get_value();
}

class Printer<T : IPrintable> {
    i32 print(T item) { return item.get_value(); }
}
```

Multiple constraints (`T : IFoo, IBar`) and the `new()` constraint — which allows
`new T` inside the generic class — are also supported:

```mylang
class Factory<T : new()> {
    T make() { return new T; }
}

Factory<Widget> f = new Factory<Widget>;
Widget w = f.make();
```

Key points:

- Generic types must always be fully instantiated (no "open" generic types).
- Only generic **classes** are supported for now; generic functions, interfaces, and
  structs are not.

---

## 13. Memory Management: weak and unowned

### 13.1 Automatic Reference Counting (what users need to know)

- Class instances, strings, interface values, and objects are all reference-counted;
  references held by locals, parameters, fields, and array elements are handled
  automatically — **you never need to free anything in normal code**.
- Reference semantics mean sharing: after `a = b`, mutating a field through `a` is
  visible through `b`.
- An object is finalized the moment its last strong reference disappears (deterministic
  destruction, not a GC's "whenever").

### 13.2 Reference Cycles Leak

When two objects hold strong references to each other, neither is ever released — this
is a **known, intentional limitation**:

```mylang
class Node { Node next; }

Node a = new Node;
Node b = new Node;
a.next = b;
b.next = a;      // a cycle: a and b can never be reclaimed
```

Break cycles by making one edge of the ring `weak` or `unowned` (e.g. the parent pointer):

```mylang
class Node {
    Node next;
    weak Node prev;     // a weak field does not keep the other side alive
}
```

### 13.3 weak: References That May Die

```mylang
weak Node w = obj;      // does not keep obj alive
Node s = w.lock();      // a strong reference if the object is alive, otherwise null
if (!s) { return; }     // the null check is mandatory
```

- `lock()` returns a strong reference (or `null`) and is the only way to reach the object.
- Interfaces work too: `weak IShape w = obj;`, `IShape s = w.lock();`.
- A weak variable can also be initialized with `null`: `weak Node w = null;`
  (`lock()` then yields `null`).
- Weak arrays are supported: `weak Node[] arr; arr.resize(n);`.
- Strong references convert automatically when passed to weak parameters/fields — no
  manual wrapping.
- **Restriction**: weak variables cannot be declared in `if` / `while` conditions.

### 13.4 unowned: Non-owning References Assumed to Stay Alive

```mylang
unowned Node u = obj;
u = obj2;
i32 v = u.v;            // used directly like a strong reference; no lock() needed
```

- Non-owning, but **every access is checked**: touching a dead object panics
  (`unowned reference to dead object`); touching a never-initialized one panics
  (`unowned reference is null`). It is memory-safe — it can never read freed memory.
- An unowned reference can be converted back to a strong one: `Node s = u;`
  (also checked).
- Restrictions: class types only (no interfaces, no primitives); local declarations
  require an initializer and **cannot be `null`**; no unowned return types; no unowned
  arrays; `new unowned` is illegal; calling `.lock()` on an unowned is an error;
  `match` and `as` on an unowned expression are rejected.

### 13.5 weak or unowned?

- The object dying first is a **normal business event** (caches, observers, closable
  resources) → use `weak`, and handle the null after each `lock()`.
- The object dying first would be a **structural bug** (e.g. a child pointing back to a
  parent that should outlive it) → use `unowned`, so a violated assumption panics
  immediately and surfaces the bug early.

---

## 14. Native Methods (C Interop)

Methods declared with `native` inside a class are implemented by you in C:

```mylang
class MySdl {
    native i32 init();
    native u64 createWindow(i32 w, i32 h);
    native void delay(i32 ms);
}

i32 main() {
    MySdl sdl = new MySdl;
    if (sdl.init() != 0) { return 1; }
    u64 win = sdl.createWindow(800, 600);
    if (win == 0) { sdl.quit(); return 2; }
    ...
}
```

Workflow:

1. Declare `native RetType ClassName.method(Params);` in MyLang source (no body; ends with `;`).
2. Compilation also produces a C header (`<output>.h`) containing the method prototypes.
3. In a separate `.c` file, `#include` that header and implement the functions (the name
   is the class name joined with the method name, e.g. `MySdl_init`); the first
   parameter is the object pointer.
4. Compile and link the generated `.c` together with your implementation `.c`.

Conventions the C side must follow:

- When returning class / string / interface types, **call `mylang_retain` before
  returning** (you return an owned reference).
- Parameters are borrowed: valid only for the duration of the call; retain them yourself
  if you store them.
- `ref T` parameters arrive as `T*`; `weak T` is an opaque `WeakRef*`; interfaces are
  passed by value; `T[]` is passed by value.
- The generated header already pulls in the runtime declarations, so after `#include`-ing
  it you can call runtime functions such as `mylang_retain` directly.

---

## 15. Pitfalls Quick Reference

Common traps, grouped by topic:

**Expressions and statements**

- Assignment `=` cannot appear in `if`/`while`/`for` conditions, variable initializers,
  or `return`.
- `++`/`--` are standalone statements only; `y = x++`, `foo(x++)`, `if (x++)` are all invalid.
- Arithmetic compound assignment (`+= -= *= /=`) is numeric-only; bitwise compound
  assignment (`&= |= ^= <<= >>=`) is integer-only; neither can appear in
  conditions/initializers/return.
- Bitwise `& | ^ ~ << >>` are integer-only; bool has no `&`/`|` (use `&&`/`||`).
- No `?:`, no block comments.
- `if` / `while` / `for` bodies **must be braced** (except the `else if` chain).

**Types and values**

- bool does not implicitly convert to/from numerics: `bool b = 5;` and
  `i32 x = a < b;` are errors (but conditions don't require bool; `if (x)` is fine).
- Reference-like and value types neither implicitly convert nor compare with each other;
  reference-like types only compare with reference-like types (or `null`).
- `null` is for reference types only; `unowned` cannot be null; `match (null)` is illegal.
- One variable per declaration statement.
- Classes are reference semantics, structs are value semantics, strings are mutable
  reference types — aliases share mutations.
- Struct fields can be primitives, bool, other structs (no recursive nesting), or
  reference types (class/interface/object/string/weak/unowned, with automatic
  retain/release); structs owning reference fields cannot be array elements.
- Structs can have methods, but the receiver must be an lvalue (no calls on
  temporaries like `make().m()`); no native/override/interface implementation.
- `new Class` has no parentheses and no constructors; fields are zero-initialized.
- No class inheritance; use interfaces for polymorphism.
- `object` has no members; cast with `as` before use; it cannot be assigned back to a
  concrete class directly.
- `const` only applies to primitive value types, needs an initializer, and cannot be
  used on fields.

**Arrays**

- `T[] a;` is already an empty array — there is no `new T[N]` syntax; start with
  `resize` / `reserve` / `push`.
- Arrays cannot be assigned with `=`, passed by value, or returned; use `ref T[]`,
  `move_to`, `copy_to`.
- Indexed access is bounds-checked at runtime; out-of-bounds panics.

**Interfaces and polymorphism**

- A class must implement all abstract methods of its interfaces (defaults excepted) with
  matching signatures; interface method implementations must be `public`.
- `override` is optional but recommended — the compiler checks it for you.
- Default interface methods cannot use `this`.
- `as` only targets class types and yields `null` on failure — remember to check.

**Lifetimes**

- Reference cycles leak; break rings with `weak`/`unowned`.
- `weak` must be used through `lock()`, and the `lock()` result must be null-checked.
- Accessing a dead object through `unowned` panics — it is an "assert never expires"
  tool, not a free pass.
- Weak variables cannot be declared in if/while conditions.

**match**

- Arms are tried in order; `else` must be last.
- Bound variables are only visible inside their arm body.
- No true/false literal arms; cannot match on null or unowned expressions.

**Fixed caps (compile error when exceeded)**

- 32 fields per class/struct; 32 methods per interface; 8 implemented interfaces per
  class; 16 parameters per function/method; 8 generic type parameters.
- Class method count is unlimited.

---

## 16. Runtime Tips

Practical advice for writing stabler, faster MyLang programs:

**Error handling**

- Out-of-bounds array access and unowned access to dead objects trigger a runtime panic:
  the offending **source line** is printed and the program aborts. The line number in a
  panic message is your first debugging clue — a panic means a bug to fix, not an
  exception to catch.
- Compile errors are printed as `path(line,col): error: message`, which editors like
  VS Code can jump to directly.
- Make it a habit to immediately null-check anything that can be empty (`lock()`,
  `as` results).

**Memory**

- During development, build with `--leak-check`
  (`mylang --leak-check source.my out.c`): at exit the program prints the address, type,
  and allocation call stack of every unreleased object. The most common cause of leaks
  is reference cycles — when the report appears, look for rings in your object graph first.
- Arrays are not reference-counted; they are freed at scope exit and have no sharing
  issues — which is also why `=` assignment is banned and transfer needs `move_to`.

**Performance**

- When the element count is known, call `arr.reserve(n)` first to avoid repeated growth
  during pushes.
- `a.compact()` reclaims excess memory after bulk pop/clear.
- `move_to` is an O(1) pointer transfer while `copy_to` is an element-wise deep copy —
  prefer `move_to` for large arrays.
- For string building, use the `append_*` family or a single f-string; both append into
  the same buffer and avoid piles of temporary strings.
- Each f-string interpolation is evaluated exactly once in order, so side-effecting
  expressions are safe (though keeping them simple is better for readability).
- `private`/`const` checks happen entirely at compile time with zero runtime cost.

**String protection**

- If you don't want string literals to appear as plain text in the generated C code when
  shipping a program, compile with `--xor-strings`
  (`mylang --xor-strings source.my out.c`): every literal is lightly encrypted and
  decrypted at runtime. This is a convenience obfuscation, not strong cryptography;
  runtime string behavior (interpolation, comparison, printing) is unchanged.

**Concurrency**

- There is currently no language-level threading API; write programs under a
  single-threaded model. Reference counting uses atomic operations internally, but that
  does not make data structures thread-safe.

**Interop**

- If a native method can trigger a runtime panic, the stack information may be
  incomplete — check critical preconditions on the MyLang side.
