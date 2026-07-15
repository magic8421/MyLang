# MyLang Grammar and Syntax Reference

This document describes the current syntax of **MyLang** as implemented in `src/`. Examples are taken from the test suite in `test/`.

## 1. Lexical Elements

### Keywords
```
u8 u16 u32 u64
i8 i16 i32 i64
f32 f64
ref weak
struct class interface
if else while return
new this as
```

`void` is also recognized as a type name, although it is not treated as a lexer keyword.

### Identifiers
- Begin with a letter or `_`, followed by letters, digits, or `_`.
- Case-sensitive.

### Comments
- Line comments only: `// ...`

### Literals
| Form | Example | Note |
|------|---------|------|
| Integer literal | `0`, `42`, `12345` | Decimal digits only |
| Character literal | `'A'`, `'\n'`, `'\t'`, `'\\'`, `'\''`, `'\0'` | Resolves to `i8` |

### Delimiters and Operators
```text
( ) { } [ ] ; , . :
+ - * / %
== != < <= > >=
&& || !
=
```

There is no `++`, `--`, `+=`, `-=`, `?:`, or bitwise/shift operators.

---

## 2. Types

### Primitive Types
```
i8 i16 i32 i64
u8 u16 u32 u64
f32 f64
void
```

`i32` is the default integer type. `void` is only used as a return type and is recognized as a special identifier by the parser.

### Reference Types
```my
ClassName       // heap-allocated, reference-counted class
InterfaceName   // fat-pointer value type
```

### Value Types
```my
StructName      // stack-allocated value type
```

### Weak Reference Types
```my
weak ClassName
weak InterfaceName
```

### Array Types
```my
T[]     // dynamic value-type vector
```

Examples:
```my
i32[] nums = new i32[10];
Vec[] vs = new Vec[3];
IShape[] shapes = new IShape[2];
weak IShape[] weak_shapes = new weak IShape[2];
```

Arrays are value-type vectors: they are not reference-counted, cannot be returned or passed by value, and cannot be assigned directly. Use `ref T[]` parameters and explicit `move_to(ref)` / `copy_to(ref)` to transfer or duplicate them.

`new InterfaceName[]` is allowed for dynamic arrays; `new InterfaceName` and `new StructName` are not.

---

## 3. Top-Level Declarations

A program is a sequence of declarations in any order:
- `class` declaration
- `struct` declaration
- `interface` declaration
- function declaration

### Function Declaration
```my
ReturnType name(Type param1, Type param2, ...) { body }
```

Example:
```my
i32 add(i32 a, i32 b) {
    return a + b;
}
```

### Class Declaration
```my
class Name [: Iface1, Iface2, ...] {
    FieldType fieldName;
    ReturnType methodName(Params) { body }
}
```

Example:
```my
class Square : IShape {
    i32 side;
    i32 area() { return this.side * this.side; }
}
```

Classes can implement multiple interfaces. There is **no class inheritance** (no base class / `extends`).

### Struct Declaration
```my
struct Name {
    PrimitiveType fieldName;
    ...
}
```

Example:
```my
struct Vec {
    i32 x;
    i32 y;
}
```

Struct fields are currently restricted to primitive types.

### Interface Declaration
```my
interface Name {
    ReturnType methodName(Params);
    ...
}
```

Example:
```my
interface IShape {
    i32 area();
}
```

Interfaces contain only method signatures, no bodies or fields.

---

## 4. Variables and Declarations

### Local Variable Declaration
```my
Type name;
Type name = initializer;
```

Examples:
```my
i32 x = 10;
Square sq = new Square;
IShape s = sq;
weak Node w = obj;
```

Multiple variables in one declaration are **not** supported (`i32 x, y;` is invalid).

### Assignment
```my
name = expression;
```

Assignment is a statement, not an expression, except when used as an initializer.

---

## 5. Statements

### Block
```my
{ stmt1; stmt2; ... }
```

### If Statement
```my
if (cond) stmt
if (cond) stmt else stmt
```

Example:
```my
if (x > 5) {
    return 1;
} else {
    return 42;
}
```

Declarations inside `if` conditions are **not** supported.

### While Statement
```my
while (cond) stmt
```

Example:
```my
while (i <= 10) {
    s = s + i;
    i = i + 1;
}
```

### Return Statement
```my
return;
return expr;
```

### Expression Statement
```my
expression;
```

Method calls and assignments are commonly used as statements.

### Vector Method Calls
```my
arr.push(value);
arr.pop();
arr.reserve(capacity);
arr.resize(length);
arr.clear();
arr.compact();
arr.move_to(ref dst);
arr.copy_to(ref dst);
```

---

## 6. Expressions

### Operator Precedence (lowest to highest)

1. `=` (assignment, right-associative)
2. `||`
3. `&&`
4. `==`, `!=`
5. `<`, `<=`, `>`, `>=`
6. `+`, `-`
7. `*`, `/`, `%`
8. Unary `-`, `!`
9. Postfix: `[]`, `.`, `()`, `as Type`

### Primary Expressions
```my
42          // integer literal
'A'         // character literal
name        // identifier
this        // current class instance
(expr)      // parenthesized expression
```

### New Expression
```my
new ClassName          // class instance
new Type[N]            // dynamic array
```

Examples:
```my
Square sq = new Square;
i32[] arr = new i32[n + 2];
Vec[] vs = new Vec[3];
IShape[] shapes = new IShape[2];
weak IShape[] wshapes = new weak IShape[4];
```

### Member and Method Access
```my
obj.field
obj.method(args)
```

Example:
```my
sq.side = 6;
i32 a = sq.area();
```

### Array Access
```my
arr[index]
```

Example:
```my
arr[0] = 10;
i32 x = arr[1];
```

### Function and Method Calls
```my
add(1, 2)
sq.area()
```

### Type Assertion (`as`)
```my
expr as ClassName
```

Example:
```my
Square s2 = s as Square;
```

The target of `as` must be a class type. It returns a class pointer or `NULL`.

### Null / Truthiness
There is no `null` keyword. A class or interface value is considered "false" when its data pointer is `NULL`:
```my
if (p) { ... }      // true if p is non-null
if (!p) { ... }     // true if p is null
```

---

## 7. Reference Parameters

Only `ref` parameters are supported.

```my
void inc(ref i32 x) {
    x = x + 1;
}
```

At the call site, the argument must be a simple local variable. The `ref` keyword is optional for scalar `ref` parameters, but array `ref` parameters are usually written explicitly:
```my
i32 a = 0;
inc(a);                 // OK

void fill(ref i32[] arr) { ... }
i32[] nums = new i32[10];
fill(ref nums);         // OK
```

Field access or array-element arguments are rejected.

---

## 8. Weak References

### Weak Class Reference
```my
weak ClassName w = obj;
ClassName s = w.lock();   // NULL if object is dead
```

### Weak Interface Reference
```my
weak InterfaceName w = iface_or_class;
InterfaceName s = w.lock();
```

Example:
```my
weak IShape w = sq;
IShape s = w.lock();
return s.area();
```

Weak references cannot be class fields. Dynamic arrays of weak interfaces are supported.

---

## 9. Memory Model (Brief)

- Class instances are heap-allocated and reference-counted.
- Arrays (`T[]`) are value-type vectors with a separate heap-allocated data buffer; they are not reference-counted and are freed automatically when the owning variable goes out of scope.
- The callee retains return values; the caller does not retain call or `new` results when stored.
- Local class, interface, weak, and array variables are released automatically at scope exit.
- Assignment retains the right-hand side before releasing the left-hand side to avoid use-after-free on self-assignment.

---

## 10. Comparison with C#

### Syntactic Similarities
- Classes, structs, and interfaces use similar declaration shapes.
- `:` introduces interface implementation on a class.
- `ref` parameters use a similar keyword.
- `as` performs a type assertion / safe cast.
- `new` allocates instances and arrays.
- `if`, `else`, `while`, `return` work as in C#.

### Major Differences

| Feature | MyLang | C# |
|---------|--------|-----|
| Class inheritance | Not supported | Single inheritance supported |
| Generics | Monomorphized generics implemented (`GENERICS_DESIGN.md`) | Full generic classes, methods, delegates |
| Constructors | None; `new Class` zero-initializes | Full constructor system |
| Visibility | No `public`/`private`/`protected` | Full access modifiers |
| Memory management | Reference counting | Garbage collection |
| Weak references | `weak T` + `lock()` | `WeakReference<T>` / `GCHandle` |
| Strings | No `string` type; use `i8[]` | Built-in `string` |
| Booleans | No `bool`; use `i32` or pointer truthiness | Built-in `bool` |
| Null literal | No `null` keyword | `null` exists |
| Operator overloading | Not supported | Supported |
| Indexers / properties | Not supported | Supported |
| Exceptions | Not supported | Supported |
| Delegates / events / lambdas | Not supported | Supported |
| `out` / `in` parameters | Removed; only `ref` | `ref`, `out`, `in` all exist |
| `++` / `--` / compound assignment | Not supported | Supported |
| Bitwise / shift operators | Not supported | Supported |
| Comments | `//` only | `//` and `/* */` |
| Type cast syntax | postfix `expr as Class` | `expr as Type` and `(Type)expr` |
| Multiple variable declarations | Not supported | `int x, y;` supported |

### Is MyLang a C# Subset?

**No.** MyLang is not a subset of C# for several reasons:

1. **MyLang has constructs C# does not have.** The `weak T` type, the `lock()` pseudo-method on weak references, and the `as ClassName` postfix assertion have no direct C# equivalents with the same syntax and semantics.

2. **Even common constructs differ semantically.** In C#, `new Class()` calls a constructor and returns a reference tracked by the GC. In MyLang, `new Class` allocates zeroed memory and the object is reference-counted. C#'s `ref` passes a managed reference with aliasing rules; MyLang's `ref` compiles to a C pointer and restricts arguments to local variables.

3. **C# has many features MyLang lacks.** Any MyLang program that uses no special keywords still cannot be parsed as C# because MyLang omits required C# constructs (for example, top-level statements and methods live outside a namespace/class, which is invalid C#), and MyLang's primitive type names (`i32`, `u8`, etc.) are not valid C# aliases.

MyLang is better described as a **C-like language influenced by C# syntax** rather than a subset of C#.

---

## 11. Example Program

```my
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
    IShape s = sq;
    return s.area();
}
```
