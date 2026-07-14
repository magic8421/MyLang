# MyLang Generic Class Design (C#-style)

> Status: design draft  
> Scope: generic **classes** first; generic functions/interfaces are future work.

## 1. Goals

- Support `class Box<T> { ... }` and `class Pair<T1, T2> { ... }`.
- Use angle-bracket syntax with colon constraints (`T : IFoo`).
- Fit the existing MyLang memory model:
  - reference-counted classes,
  - value structs,
  - fat-pointer interfaces,
  - weak references,
  - dynamic/fixed arrays.
- Keep the implementation simple: **compile-time monomorphization** (like C++ templates / C# instantiation), no runtime generic dictionaries.

## 2. Non-goals (for this phase)

- Generic functions (`func foo<T>(T x) { ... }`).
- Generic interfaces (`interface IList<T> { ... }`).
- Generic structs (`struct Vec3<T> { ... }`).
- Variance annotations (`in` / `out`).
- Reflection / runtime type information beyond the existing `type_id`.

## 3. Syntax

### 3.1 Declaration

```my
class Box<T> {
    T value;

    Box(T v) {
        this.value = v;
    }

    T get() {
        return this.value;
    }

    void set(T v) {
        this.value = v;
    }
}

class Pair<T1, T2> {
    T1 first;
    T2 second;

    Pair(T1 a, T2 b) {
        this.first = a;
        this.second = b;
    }
}
```

### 3.2 Instantiation

Generic types must always be fully instantiated (no "open" generic types).

```my
Box<i32> b = new Box<i32>(42);
i32 x = b.get();

Pair<string, Node> p = new Pair<string, Node>("hello", node);
```

### 3.3 Constraints

MyLang uses a colon directly on the type parameter, not C#'s `where` clause:

```my
class Logger<T : ILoggable> {
    T target;

    Logger(T t) { this.target = t; }

    void log() {
        this.target.write_log();   // allowed because T : ILoggable
    }
}

class Pool<T : IResettable, new()> {
    T alloc() {
        return new T();            // allowed because of new()
    }
}
```

The same colon syntax works for future generic functions:

```my
void sort<T : IComparable>(T[] arr) { ... }
```

Multiple interface constraints on the same parameter are comma-separated inside the angle brackets:

```my
class Pool<T : IResettable, new()> { ... }
class Multi<T : IFoo, IBar> { ... }
```

### Constraint strategy

C# supports arbitrary combinations (`where T : class, IFoo, IBar, new()`). For MyLang the MVP keeps constraints small but useful:

| Constraint | MVP? | Meaning |
|------------|------|---------|
| `T : IFoo` | Yes | `T` must implement interface `IFoo`. Enables interface calls on `T`. Multiple interfaces are allowed: `T : IFoo, IBar`. |
| `T : new()` | Yes | `T` has a public parameterless constructor. Enables `new T()`. |
| `T : class` / `T : struct` | No | Reserved for Phase 2. |
| `T : U` | No | Type-equality / base-type constraints, Phase 2. |

#### Why not C++-style duck typing?

A C++ template delays error checking until instantiation: if `T` lacks a method, the generated C++ code fails to compile. MyLang compiles to C, so a missing method would surface as an obscure C compiler error (e.g. `undefined symbol Foo_missing`). That hurts usability and breaks MyLang's goal of reporting errors in source terms. Therefore MyLang validates constraints explicitly at the generic definition / instantiation boundary.

#### Phase 2

- `class` / `struct` constraints.
- Multiple `new()`-like constructor shapes (parameterized constructors).
- Type equality `T : U`.

## 4. Type System Changes

### 4.1 Representing generic types

The current `Type` struct:

```c
typedef struct {
    TypeKind type_kind;
    char     class_name[64];
    int      is_pointer;
    int      is_array;
    int      array_size;
    int      is_ref;
    int      is_weak;
    int      type_id;
} Type;
```

Needs to be extended to carry type arguments. Two common approaches:

1. **Mangled name only**: `Box<i32>` becomes `Type{ class_name="Box_i32", type_id=... }`.
2. **Structured type arguments**: keep base name + list of argument `Type`s.

Recommended: **structured + mangled fallback**. Add fields to `Type`:

```c
#define MAX_TYPE_ARGS 4   /* start small; raise later */

typedef struct Type {
    TypeKind type_kind;
    char     class_name[64];          /* base name, e.g. "Box" */
    int      is_pointer;
    int      is_array;
    int      array_size;
    int      is_ref;
    int      is_weak;
    int      type_id;

    int      type_arg_count;
    struct Type* type_args[MAX_TYPE_ARGS];   /* owned heap pointers */
    char     mangled_name[128];       /* cached "Box_i32", "Pair_string_Node" */
} Type;
```

`type_equal` compares `mangled_name` (plus flags). `type_name` returns the mangled name.

Type arguments can be:
- primitives (`i32`, `f64`, ...),
- class / struct / interface types (including other instantiated generics),
- type parameters (`T`) inside the generic class body.

A type parameter itself needs a representation. Introduce a sentinel `TypeKind`:

```c
TYPE_TYPE_PARAM   /* type_kind for "T" */
```

For a type param, `class_name` holds the param name (`"T"`), `type_id` is unused or `-1`.

### 4.2 Generic class metadata

Extend `ClassInfo`:

```c
typedef struct ClassInfo {
    char name[64];              /* base name, e.g. "Box" */
    int  type_id;               /* only meaningful for closed instantiations */
    int  field_count;
    char field_names[MAX_FIELDS][64];
    Type field_types[MAX_FIELDS];
    MethodInfo* methods;
    int  impl_count;
    char impl_names[MAX_IMPL][64];
    InterfaceInfo* impl_infos[MAX_IMPL];

    int  generic_param_count;
    char generic_params[8][64]; /* names: T, T1, ... */
    struct ClassInfo* next;
} ClassInfo;
```

The symbol table stores the **generic definition** under the base name (`Box`). Concrete instantiations (`Box_i32`) are registered separately with their own `type_id` and generated code.

### 4.3 Type IDs

- Type IDs `0-15` remain primitives.
- IDs `16+` remain user-defined classes/structs/interfaces.
- Each **concrete** generic instantiation gets a fresh ID from the same counter.
- Type parameters have no ID.
- Arrays keep the `TYPE_IS_ARRAY` flag on the element type ID.

## 5. Name Mangling

Every concrete generic class needs a unique C identifier. Use a simple, deterministic mangling:

```
Box<i32>              -> Box_i32
Pair<string, Node>    -> Pair_string_Node
Map<string, Box<i32>> -> Map_string_Box_i32
```

Rules:
- base class name,
- `_` separator,
- recursively mangle each type argument.
- Primitive names use their keyword (`i32`, `u64`, `f64`, `void`).
- Interface names keep their source name.
- Arrays: `i32[]` -> `Arr_i32`, `Node[]` -> `Arr_Node`.
- Weak refs: `weak Node` -> `Weak_Node`, `weak IFoo` -> `Weak_IFoo`.
- Pointers are not user-visible in MyLang, so no mangling needed.

The mangled name becomes:
- the C struct tag,
- the prefix for method names,
- the `class_name` in `Type` and symbol table.

Example method names:

```c
/* Box<i32> */
Box_i32* Box_i32_new(Box_i32* thiz, i32 v);
i32      Box_i32_get(Box_i32* thiz);
void     Box_i32_set(Box_i32* thiz, i32 v);
```

## 6. Monomorphization Strategy

### 6.1 Overview

MyLang will be a **stencil / monomorphizing** generic system:

1. Parse the generic class definition once into a generic AST + `ClassInfo`.
2. During type checking / codegen, when a use site mentions a closed generic type (`Box<i32>`), create (or reuse) a concrete instantiation.
3. Clone the generic AST, substitute type parameters (`T`) with concrete types (`i32`), and generate code.

This matches the existing "no runtime generics" philosophy and keeps the C backend simple.

### 6.2 Instantiation points

Concrete instantiations are created at:

- Variable declarations: `Box<i32> b;`
- `new` expressions: `new Box<i32>(...)`
- Function signatures: parameters / return types that mention `Box<i32>`.
- Interface constraints and cast sites.
- Method bodies of already-instantiated classes (because methods are generated lazily with the class).

### 6.3 Lazy vs eager method generation

Recommended: **eager per class, lazy per instantiation**.

When `Box<i32>` is first needed:
- generate the C struct `struct Box_i32 { ... }`,
- generate all methods of `Box<T>` specialized to `Box<i32>`.

This is simple and avoids complicated per-call-site specialization. It can bloat code if many instantiations exist, but that is acceptable for the MVP.

### 6.4 Type substitution

Create a helper `type_substitute(Type* t, Type param_types[], int param_count)` that returns a new `Type`:

- If `t` is `TYPE_TYPE_PARAM`, look up its name in the param list and return the bound concrete type.
- If `t` is a generic class with its own args, recurse into each arg.
- Otherwise return a copy.

Apply this substitution to:
- field types,
- method parameter types,
- method return types,
- local variable types inside methods,
- expression result types during semantic analysis.

## 7. Interaction with Existing Features

### 7.1 Reference counting

Generic classes are classes, so they follow the existing rules:

- `new Box<i32>(...)` returns an owned +1 pointer.
- Assignments retain RHS before releasing LHS.
- Cleanup releases local variables.
- Method return values are retained by the callee.

If a type parameter is used as a class field, the field type must be copyable:

```my
class Box<T> {
    T value;    /* OK if T is primitive, struct, class pointer, interface, or weak ref */
}
```

MyLang already restricts struct fields to primitives; generic class fields should allow any **value-sized** type:
- primitives,
- structs (by value),
- class pointers (`ClassName*`),
- interface fat pointers,
- weak references (`weak ClassName` / `weak IFace`).

### 7.2 Interfaces

A generic class can make interface calls on a type parameter only if the parameter declares the interface:

```my
class Printer<T : IPrintable> {
    T item;
    void print() {
        this.item.print();   /* interface dispatch */
    }
}
```

The class itself does not need to be an interface implementer; the **type parameter** does. Interface dispatch on `T` works the same as on a concrete interface type.

If we later allow generic interfaces (`interface IList<T>`), a class would declare `class MyList<T> : IList<T>` and the vtable would need type-erased `T`. That is out of scope.

### 7.3 Arrays

Generic arrays work naturally because array flags are separate from the element type:

```my
class Bag<T> {
    T[] items;            /* dynamic array of T */
    T[4] fixed;           /* fixed-size array of T */
}
```

Code generation uses the concrete element type to pick the right `array_get_*` / `array_set_*` helper.

### 7.4 Weak references

Weak references to generic class instances are allowed:

```my
weak Box<i32> w = b;
Box<i32> s = w.lock();
```

The mangled name `Weak_Box_i32` becomes the C struct / helper prefix.

### 7.5 `ref` parameters

Generic `ref` parameters work as today:

```my
void swap<T>(ref T a, ref T b) { ... }   /* generic function; future work */
```

For this design, methods of a generic class can take `ref T`:

```my
class Box<T> {
    void update(ref T v) { this.value = v; }
}
```

## 8. Semantic Validation

New checks needed:

1. **Arity**: `Box<i32, string>` when `Box` declares one parameter is an error.
2. **Type argument well-formedness**: type args must be concrete types or other closed generics; type parameters are only legal inside the generic definition.
3. **Constraints**: at instantiation / use, verify each type argument satisfies the colon constraints.
   - Interface constraint: `T : IFoo` -> the concrete type must implement `IFoo`.
   - `new()` constraint: the concrete type must have a parameterless constructor.
4. **Field type restrictions**: a generic class field of type `T` cannot be `T` itself if `T` is later bound to a non-copyable type. Since MyLang only has copyable types, this is mostly free; arrays of `T` are fine.
5. **No recursion through type parameters**: `class Node<T> { Node<T> next; }` is fine; `class Bad<T> { Bad<Bad<T>> x; }` is also fine because it is just a pointer field. Circular value recursion (`class Bad<T> { Bad<T> value; }` where `T` is bound to `Bad<...>`) is already prevented by the "class fields are pointers" rule.

## 9. Code Generation Sketch

### 9.1 Struct layout

```my
class Box<T> {
    T value;
}
```

For `Box<i32>`:

```c
typedef struct Box_i32 {
    i32 value;
} Box_i32;
```

For `Box<Node>` (where `Node` is a class):

```c
typedef struct Box_Node {
    Node* value;
} Box_Node;
```

For `Box<IShape>`:

```c
typedef struct Box_IShape {
    IShape value;   /* fat pointer */
} Box_IShape;
```

### 9.2 Method generation

Generic definition in MyLang:

```my
class Box<T> {
    T get() { return this.value; }
}
```

Generated C for `Box<i32>`:

```c
i32 Box_i32_get(Box_i32* thiz) {
    return thiz->value;
}
```

For `Box<Node*>` (class pointer field):

```c
Node* Box_Node_get(Box_Node* thiz) {
    Node* _ret = mylang_retain(thiz->value);
    return _ret;
}
```

The existing `codegen` logic for return-statement retain can be reused once the field type is substituted.

### 9.3 Constructor

Constructors are just methods named `new`. For `Box<i32>`:

```c
Box_i32* Box_i32_new(Box_i32* thiz, i32 v) {
    thiz->value = v;
    return thiz;
}
```

The caller-side `new Box<i32>(42)` expands to:

```c
Box_i32* _tmp = (Box_i32*)mylang_new_object(sizeof(Box_i32), TYPEID_Box_i32);
Box_i32_new(_tmp, 42);
```

## 10. Parser / AST Additions

### 10.1 AST nodes

- `AST_GENERIC_CLASS_DECL` (or reuse `AST_CLASS_DECL` with a child list of type parameters).
- Add `AstNode* type_params` child to class declarations.
- Type identifiers can carry type arguments: `Box<i32>` becomes an `AST_TYPE_NAME` with `type_args`.

Since MyLang currently uses `class_name` strings directly in `Type`, the simplest path is:

- Parse `class Box < T > { ... }` as `AST_CLASS_DECL` with an extra child list.
- Store generic params in `ClassInfo`.
- Parse `Box<i32>` as a type token sequence that builds a `Type` with `type_args`.

### 10.2 New token

No new token needed; `<` and `>` are already operators. The parser needs a `parse_type_arguments()` helper that is only accepted after a type name in type contexts.

Ambiguity: `a < b` (less-than) vs `Box<i32>` (type args). This is only ambiguous in expression contexts; in type contexts (after `:`, `=`, `new`, param/return types) there is no ambiguity. For now, type arguments are only parsed in type contexts, so no expression-level ambiguity exists.

## 11. Step-by-Step Implementation Plan

1. **Extend `Type`** to carry `type_args` and `mangled_name`.
2. **Add `TYPE_TYPE_PARAM`** and helpers to create/substitute type params.
3. **Extend `ClassInfo`** with generic parameter list.
4. **Update parser** to parse `class Name<T, U>` and type instantiations `Name<T, U>`.
5. **Update symbol table** to store generic definitions separately from concrete instantiations.
6. **Add mangling helper** `type_mangle(Type* t)`.
7. **Implement monomorphization**: when a closed generic type is seen, clone the generic definition and substitute types.
8. **Update codegen** to emit one C struct + methods per instantiation.
9. **Add constraint checking** for `T : IFoo` and `T : new()`.
10. **Add tests**:
    - `Box<i32>`
    - `Pair<string, Node>`
    - generic class with interface constraint,
    - generic class with `new()` constraint,
    - arrays of `T` inside a generic class.

## 12. Open Questions

1. Do we want generic **methods** inside non-generic classes (`class Foo { T bar<T>(T x) { ... } }`)? This is more complex and should come after generic classes.
2. Do we want `T : struct` / `T : class` constraints? Useful for optimization and copy semantics.
3. Should the mangled name include a hash or be fully readable? Fully readable is easier for debugging; hashing can be added later if name length becomes an issue.
4. How do generic classes interact with interface vtables if the class itself is converted to an interface? For now, generic classes are not interface implementers (only their type parameters can be constrained to interfaces), so this is avoided.

## 13. Example Program

```my
interface IPrintable {
    void print();
}

class IntBox : IPrintable {
    i32 value;
    IntBox(i32 v) { this.value = v; }
    void print() { /* print value */ }
}

class Wrapper<T : IPrintable> {
    T item;

    Wrapper(T it) { this.item = it; }

    void show() {
        this.item.print();
    }
}

func main() -> i32 {
    IntBox b = new IntBox(10);
    Wrapper<IntBox> w = new Wrapper<IntBox>(b);
    w.show();
    return 0;
}
```

Generated sketch:

```c
typedef struct Wrapper_IntBox {
    IntBox* item;
} Wrapper_IntBox;

Wrapper_IntBox* Wrapper_IntBox_new(Wrapper_IntBox* thiz, IntBox* it) {
    thiz->item = mylang_retain(it);
    return thiz;
}

void Wrapper_IntBox_show(Wrapper_IntBox* thiz) {
    IPrintable _iface = mylang_make_IPrintable(thiz->item);
    _iface.vtable->print(_iface.data);
    /* release interface data */
    mylang_release_iface_data(&_iface);
}
```

