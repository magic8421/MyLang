# MyLang

**A statically compiled systems language with C#'s manners, C's honesty, and zero runtime baggage.**

MyLang compiles to clean, reviewable C — and from there to a native .exe with
nothing to install, nothing to bundle, and no garbage collector second-guessing
you. You get reference counting that Just Works, `weak`/`unowned` references
borrowed from Swift's playbook, lambdas that capture, monomorphized generics,
and an actual `string` type — all in a compiler small enough to read in a
weekend.

> It looks like C#. It behaves like C. It ships like a single `.exe`.

---

## Why MyLang?

- **Memory manages itself — but you stay the boss.** Class instances are
  reference-counted and released automatically when they go out of scope.
  Cycles? Break them with `weak` (lock-checked) or `unowned` (panic-checked).
  No GC pauses, no `free()`, no leaks you're not warned about.
- **Lambdas with real captures.** `(a, b) => a - b` desugars into an anonymous
  class implementing your interface — by-value capture, `weak` capture,
  zero new runtime machinery.
- **Generics without the bloat.** Monomorphized at compile time, C-style:
  `class Map<K, V>` in the standard library, instantiated per use, no type
  erasure games.
- **Interfaces with default methods.** Polymorphism without class inheritance.
  Simple, explicit, done.
- **Properties, enums, namespaces, `const`, static methods, default parameter
  values, `match`, `foreach`, f-strings** — the ergonomic checklist, checked.
- **C interop that doesn't fight you.** `native` methods bridge straight into
  your `.c` files; MyLang strings are secretly NUL-terminated, so native code
  gets a `const char*` with **zero copying**.
- **Strings that encrypt themselves.** One flag — `--xor-strings` — and every
  string literal in your binary is ciphered at compile time.
- **A paranoia-grade test suite.** 499 end-to-end programs, each compiled and
  executed twice: once under AddressSanitizer, once under the debug CRT with
  full leak reporting. If it leaks, we know.

## Show me the code

```mylang
interface Comparator {
    i32 compare(i32 a, i32 b);
}

i32 main() {
    Comparator c = (a, b) => a - b;   // a lambda. Yes, really.
    return c.compare(10, 3);          // dynamic dispatch, returns 7
}
```

```mylang
class Counter {
    private i32 _count;
    i32 Count {
        get { return this._count; }
        set { this._count = value; }
    }
    i32 Doubled {
        get { return this._count * 2; }
    }
}
```

```mylang
class Map<K, V> { ... }   // generic hash map, ships in lib/Map.my

class Widget {
    weak Owner owner;               // may die; lock() checks
    unowned Engine engine;          // must outlive; panics otherwise
}
```

## Quick start (Windows, MSVC)

```bat
build.bat                                   :: builds mylang.exe
build\mylang.exe hello.my hello.c           :: MyLang -> readable C
cl /std:c11 /I src hello.c src\runtime.c    :: C -> native .exe
hello.exe
```

Handy flags:

- `mylang --leak-check hello.my out.c` — reports every unreleased object at exit.
- `mylang --xor-strings hello.my out.c` — encrypts all string literals.

## It even does graphics

`examples/sdl3_events/` is a working SDL3 app: window creation, event
listeners via interfaces, IME text input, and PNG textures decoded with
stb_image — written in MyLang, bridged through `native` methods, compiled to
one `.exe` plus `SDL3.dll`.

## Docs

- `MANUAL_en.md` — the full user manual (syntax, semantics, pitfalls).
- `docs/language.md` — type system and language internals.
- `docs/memory-model.md` — refcounting, weak/unowned, destructors.
- `docs/compiler-internals.md` — how the compiler works.
- `AGENTS.md` — contributor/agent notes.

## Status

Actively developed. The compiler self-hosts nothing (yet), the standard
library is a proud work in progress, and the test suite is the specification:
**499/499 passing, in both ASan and leak-check modes.**

Not a C# subset — a C-like language that went to C# finishing school.
