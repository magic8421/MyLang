#!/usr/bin/env python3
"""MyLang test runner. Compiles .my sources, runs them, verifies results."""

import subprocess
import sys
import os
import tempfile
import shutil
import argparse

SCRIPT_DIR = os.path.dirname(os.path.abspath(__file__))
MYLANG_EXE = os.path.join(SCRIPT_DIR, "build", "mylang.exe")
VSPATH = r"C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat"

TEST_MODE = "release"  # "release" = ASan + release CRT, "debug" = no ASan + debug CRT

if not os.path.exists(VSPATH):
    VSPATH = r"C:\Program Files\Microsoft Visual Studio\2022\Professional\VC\Auxiliary\Build\vcvars64.bat"

def find_asan_dll():
    """Find the ASan runtime DLL and return its directory."""
    search_roots = [
        r"C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Tools\MSVC",
        r"C:\Program Files\Microsoft Visual Studio\2022\Professional\VC\Tools\MSVC",
    ]
    dll_name = "clang_rt.asan_dynamic-x86_64.dll"
    for root in search_roots:
        for dirpath, _dirs, files in os.walk(root):
            if dll_name in files and "bin\\Hostx64\\x64" in dirpath:
                return os.path.join(dirpath, dll_name)
    return None

def ensure_asan_dll():
    """Copy ASan DLL to build/ so executables can find it."""
    dll_path = find_asan_dll()
    if not dll_path:
        print("WARNING: ASan DLL not found, ASan may not activate")
        return ""
    build_dir = os.path.join(SCRIPT_DIR, "build")
    os.makedirs(build_dir, exist_ok=True)
    dest = os.path.join(build_dir, os.path.basename(dll_path))
    if not os.path.exists(dest):
        shutil.copy2(dll_path, dest)
    return build_dir

def shell(args, **kw):
    return subprocess.run(args, capture_output=True, text=True, shell=True, **kw)

def find_python():
    for name in ["python.exe", "python3.exe"]:
        p = shutil.which(name)
        if p:
            return p
    return sys.executable

def compile_mylang():
    """Build mylang.exe with MSVC + ASan (release) or debug CRT (debug)."""
    srcs = "src\\token.c src\\ast.c src\\lexer.c src\\symtab.c src\\parser.c src\\codegen.c src\\main.c"
    if TEST_MODE == "debug":
        flags = "/MDd /Zi"
    else:
        flags = "/fsanitize=address /Zi"
    cmd = f'call "{VSPATH}" >nul 2>&1 && cl /nologo /std:c11 /W3 {flags} /Fe:build\\mylang.exe /Fo:build\\ {srcs}'
    r = shell(cmd, cwd=SCRIPT_DIR)
    if r.returncode != 0:
        print("FAIL: mylang compilation failed")
        print(r.stdout)
        print(r.stderr)
        return False
    return True

def compile_c(src, exe):
    """Compile generated C code with MSVC + ASan (release) or debug CRT (debug)."""
    exedir = os.path.dirname(exe)
    os.makedirs(exedir, exist_ok=True)
    if TEST_MODE == "debug":
        flags = "/MDd /Zi"
    else:
        flags = "/fsanitize=address /Zi"
    cmd = f'call "{VSPATH}" >nul 2>&1 && cl /nologo /std:c11 {flags} /Fe:{exe} {src}'
    r = shell(cmd, cwd=exedir)
    if r.returncode != 0:
        print(f"  C compile error for {src}:")
        print(r.stdout[-500:] if len(r.stdout) > 500 else r.stdout)
        return False
    return True

def run_exe(exe, asan_dll_dir=""):
    """Run executable and return exit code + stdout+stderr."""
    env = os.environ.copy()
    if asan_dll_dir:
        env["PATH"] = asan_dll_dir + ";" + env.get("PATH", "")
    r = subprocess.run(exe, capture_output=True, text=True, shell=True,
                       cwd=SCRIPT_DIR, timeout=10, env=env)
    return r.returncode, r.stdout + r.stderr

# ============================================================
# TEST CASES: (name, source, expected_exit_code_or_pattern)
# ============================================================

def ST(token):
    """Convenience: exit code = STATUS_BREAKPOINT on Windows"""
    return token

STATUS_BREAKPOINT = -2147483645  # 0x80000003

TESTS = [
    ("basic_arith", """
i32 main() {
    i32 x = 10;
    i32 y = x + 5;
    if (y == 15) {
        return 100;
    }
    return 0;
}
""", 100),

    ("while_sum", """
i32 main() {
    i32 i = 1;
    i32 s = 0;
    while (i <= 10) {
        s = s + i;
        i = i + 1;
    }
    return s;
}
""", 55),

    ("if_else", """
i32 main() {
    i32 x = 3;
    if (x > 5) {
        return 1;
    } else {
        return 42;
    }
}
""", 42),

    ("new_class", """
class Point {
    i32 x;
    i32 y;
}
i32 main() {
    Point p = new Point;
    p.x = 10;
    p.y = 20;
    return p.x + p.y;
}
""", 30),

    ("class_pass_to_func", """
class Point {
    i32 x;
    i32 y;
}
i32 sum(Point a, Point b) {
    return a.x + b.x + a.y + b.y;
}
i32 main() {
    Point p = new Point;
    p.x = 1;
    p.y = 2;
    Point q = new Point;
    q.x = 3;
    q.y = 4;
    return sum(p, q);
}
""", 10),

    ("class_return", """
class Point {
    i32 x;
    i32 y;
}
Point make(i32 a, i32 b) {
    Point p = new Point;
    p.x = a;
    p.y = b;
    return p;
}
i32 main() {
    Point p = make(7, 3);
    return p.x + p.y;
}
""", 10),

    ("class_assign", """
class Point {
    i32 x;
    i32 y;
}
i32 main() {
    Point p = new Point;
    p.x = 10;
    Point q = p;
    q.x = 20;
    return p.x + q.x;
}
""", 40),

    ("bounds_dynamic_oob", """
i32 main() {
    i32[] arr = new i32[3];
    arr[0] = 1;
    arr[5] = 0;
    return 0;
}
""", "crash"),

    ("bounds_fixed_oob", """
i32 main() {
    i32[3] arr;
    arr[0] = 1;
    arr[10] = 0;
    return 0;
}
""", "crash"),

    ("bounds_in_bounds", """
i32 main() {
    i32[] arr = new i32[10];
    arr[5] = 42;
    return arr[5];
}
""", 42),

    ("neg_and_not", """
i32 main() {
    i32 x = 5;
    i32 y = -x;
    if (y != -5) return 0;
    if (!(x == 4)) {
        return 10;
    }
    return 0;
}
""", 10),

    ("compare_ops", """
i32 main() {
    i32 a = 3;
    i32 b = 5;
    i32 r = 0;
    if (a < b) r = r + 1;
    if (a <= b) r = r + 1;
    if (b > a) r = r + 1;
    if (b >= a) r = r + 1;
    if (a == a) r = r + 1;
    if (a != b) r = r + 1;
    return r;
}
""", 6),

    ("nested_calls", """
i32 mul(i32 a, i32 b) { return a * b; }
i32 add(i32 a, i32 b) { return a + b; }
i32 main() {
    return add(mul(2, 3), mul(4, 5));
}
""", 26),

    ("char_literal", """
i32 main() {
    i8 c = 'A';
    i32 x = 65;
    if (c == x) return 42;
    return 0;
}
""", 42),

    ("new_array_expr_size", """
i32 main() {
    i32 n = 5;
    i32[] arr = new i32[n + 2];
    arr[6] = 99;
    return arr[6];
}
""", 99),

    ("nested_if_while", """
i32 main() {
    i32 x = 0;
    i32 i = 0;
    while (i < 5) {
        if (i < 3) {
            x = x + i;
        } else {
            x = x + 10;
        }
        i = i + 1;
    }
    return x;
}
""", 23),

    ("method_get_set", """
i32 id(i32 x) { return x; }
class Counter {
    i32 value;
    i32 get() { return this.value; }
    void set(i32 v) { this.value = v; }
    i32 inc() { this.value = this.value + 1; return this.value; }
}
i32 main() {
    Counter c = new Counter;
    c.set(10);
    i32 a = c.get();
    i32 b = c.inc();
    return id(a + b);
}
""", 21),

    ("method_thiz_alias", """
class Box {
    i32 x;
    Box set(i32 v) {
        this.x = v;
        return this;
    }
    i32 get() { return this.x; }
}
i32 main() {
    Box b = new Box;
    b.set(7);
    Box c = b.set(3);
    return b.get() + c.get();
}
""", 6),

    ("method_multi_param", """
class Point {
    i32 x;
    i32 y;
    void move(i32 dx, i32 dy) {
        this.x = this.x + dx;
        this.y = this.y + dy;
    }
}
i32 main() {
    Point p = new Point;
    p.x = 1;
    p.y = 2;
    p.move(10, 20);
    return p.x + p.y;
}
""", 33),

    ("method_void_no_return", """
class Log {
    i32 total;
    void add(i32 v) { this.total = this.total + v; }
}
i32 main() {
    Log l = new Log;
    l.add(3);
    l.add(4);
    return l.total;
}
""", 7),

    ("method_return_captured", """
class Box {
    i32 v;
    Box set(i32 x) { this.v = x; return this; }
}
i32 main() {
    Box a = new Box;
    Box b = a.set(5);
    Box c = a.set(7);
    return b.v + c.v;
}
""", 14),

    ("method_return_discarded", """
class Box {
    i32 v;
    i32 total;
    Box set(i32 x) {
        this.v = x;
        return this;
    }
    Box incTotal() {
        this.total = this.total + 1;
        return this;
    }
}
i32 main() {
    Box b = new Box;
    b.set(10);
    b.set(20);
    b.incTotal();
    b.incTotal();
    return b.v + b.total;
}
""", 22),

    ("method_return_reassign", """
class Box {
    i32 v;
    Box set(i32 x) { this.v = x; return this; }
}
i32 main() {
    Box a = new Box;
    a.set(1);
    Box b = new Box;
    b.set(9);
    b = a.set(5);
    return a.v + b.v;
}
""", 10),

    ("class_array_basic", """
class Box {
    i32 v;
    i32 get() { return this.v; }
}
i32 main() {
    Box[] arr = new Box[3];
    arr[0] = new Box;
    arr[0].v = 1;
    arr[1] = new Box;
    arr[1].v = 10;
    arr[2] = arr[0];
    return arr[0].get() + arr[1].v + arr[2].v;
}
""", 12),

    ("class_array_pass_to_func", """
class Box {
    i32 v;
}
i32 sum(Box[] arr) {
    return arr[0].v + arr[1].v;
}
i32 main() {
    Box[] a = new Box[2];
    a[0] = new Box;
    a[0].v = 3;
    a[1] = new Box;
    a[1].v = 4;
    return sum(a);
}
""", 7),

    ("class_array_return_func", """
class Box {
    i32 v;
}
Box[] make() {
    Box[] a = new Box[2];
    a[0] = new Box;
    a[0].v = 5;
    a[1] = new Box;
    a[1].v = 7;
    return a;
}
i32 main() {
    Box[] x = make();
    return x[0].v + x[1].v;
}
""", 12),

    ("class_array_bounds", """
class Box {
    i32 v;
}
i32 main() {
    Box[] a = new Box[2];
    return a[5].v;
}
""", "crash"),

    ("class_array_replace", """
class Box {
    i32 v;
}
i32 main() {
    Box[] a = new Box[2];
    a[0] = new Box;
    a[0].v = 1;
    Box b = a[0];
    a[0] = new Box;
    a[0].v = 10;
    return b.v + a[0].v;
}
""", 11),

    ("class_array_loop", """
class Box {
    i32 v;
}
i32 main() {
    Box[] a = new Box[4];
    i32 i = 0;
    while (i < 4) {
        a[i] = new Box;
        a[i].v = i;
        i = i + 1;
    }
    i32 s = 0;
    i = 0;
    while (i < 4) {
        s = s + a[i].v;
        i = i + 1;
    }
    return s;
}
""", 6),

    ("class_array_method_call", """
class Box {
    i32 v;
    i32 get() { return this.v; }
}
i32 main() {
    Box[] a = new Box[2];
    a[0] = new Box;
    a[0].v = 5;
    return a[0].get();
}
""", 5),

    ("fixed_width_basic", """
i32 main() {
    u8  a = 200;
    i8  b = -5;
    u16 c = 60000;
    i16 d = -1000;
    u32 e = 123456;
    i64 f = -123456789;
    if (a == 200 && b == -5 && c == 60000 && d == -1000 && e == 123456 && f == -123456789) {
        return 42;
    }
    return 0;
}
""", 42),

    ("fixed_width_array", """
i32 main() {
    u32[] arr = new u32[4];
    arr[0] = 10;
    arr[1] = 20;
    arr[2] = arr[0] + arr[1];
    arr[3] = 0;
    return arr[2];
}
""", 30),

    ("fixed_width_func_arg", """
i32 sum(i16[] arr) {
    i32 s = 0;
    i32 i = 0;
    while (i < 3) {
        s = s + arr[i];
        i = i + 1;
    }
    return s;
}
i32 main() {
    i16[] a = new i16[3];
    a[0] = 10;
    a[1] = 20;
    a[2] = 30;
    return sum(a);
}
""", 60),

    ("float_array", """
i32 main() {
    f32[] a = new f32[3];
    a[0] = 1;
    a[1] = 2;
    a[2] = a[0] + a[1];
    if (a[2] == 3) {
        return 3;
    }
    return 0;
}
""", 3),

    ("ref_i32", """
void add_one(ref i32 x) {
    x = x + 1;
}
i32 main() {
    i32 a = 10;
    add_one(a);
    return a;
}
""", 11),

    ("ref_class", """
class Counter {
    i32 n;
}
void inc(ref Counter c) {
    c.n = c.n + 1;
}
i32 main() {
    Counter c = new Counter;
    c.n = 5;
    inc(c);
    return c.n;
}
""", 6),

    ("ref_chain", """
void bump(ref i32 x) {
    x = x + 1;
}
void proxy(ref i32 y) {
    bump(y);
}
i32 main() {
    i32 a = 10;
    proxy(a);
    return a;
}
""", 11),

    ("ref_multi_return", """
void divmod(i32 a, i32 b, ref i32 q, ref i32 r) {
    q = a / b;
    r = a % b;
}
i32 main() {
    i32 q = 0;
    i32 r = 0;
    divmod(17, 5, q, r);
    return q * 10 + r;
}
""", 32),

    ("method_ref", """
class Accumulator {
    i32 total;
    void add(ref i32 x) {
        this.total = this.total + x;
    }
    void get(ref i32 r) {
        r = this.total;
    }
}
i32 main() {
    Accumulator a = new Accumulator;
    a.total = 5;
    i32 v = 7;
    a.add(v);
    i32 result = 0;
    a.get(result);
    return result;
}
""", 12),

    ("ref_array", """
void fill(ref i32[] arr) {
    arr[0] = 1;
    arr[1] = 2;
    arr[2] = 3;
}
i32 main() {
    i32[] a = new i32[3];
    fill(a);
    return a[0] + a[1] + a[2];
}
""", 6),

    ("struct_basic", """
struct Point {
    i32 x;
    i32 y;
}
i32 main() {
    Point p;
    p.x = 3;
    p.y = 4;
    return p.x + p.y;
}
""", 7),

    ("struct_assign_copy", """
struct Vec {
    i32 v;
}
i32 main() {
    Vec a;
    a.v = 10;
    Vec b = a;
    b.v = 20;
    return a.v + b.v;
}
""", 30),

    ("struct_array_dynamic", """
struct Vec {
    i32 v;
}
i32 main() {
    Vec[] arr = new Vec[3];
    arr[0].v = 1;
    arr[1].v = 2;
    arr[2].v = 3;
    return arr[0].v + arr[1].v + arr[2].v;
}
""", 6),

    ("struct_array_fixed", """
struct Vec {
    i32 v;
}
i32 main() {
    Vec[3] arr;
    arr[0].v = 5;
    arr[1].v = 10;
    arr[2].v = 15;
    return arr[2].v;
}
""", 15),

    ("struct_func_arg_return", """
struct Pair {
    i32 a;
    i32 b;
}
Pair make(i32 x, i32 y) {
    Pair p;
    p.a = x;
    p.b = y;
    return p;
}
i32 main() {
    Pair q = make(2, 3);
    return q.a + q.b;
}
""", 5),

    ("struct_ref", """
struct Counter {
    i32 n;
}
void inc(ref Counter c) {
    c.n = c.n + 1;
}
i32 main() {
    Counter c;
    c.n = 7;
    inc(c);
    return c.n;
}
""", 8),

    ("struct_class_field", """
struct BoxVal {
    i32 v;
}
class Container {
    BoxVal box;
}
i32 main() {
    Container c = new Container;
    c.box.v = 42;
    return c.box.v;
}
""", 42),

    ("weak_basic", """
class Node {
    i32 v;
}
i32 main() {
    Node obj = new Node;
    obj.v = 42;
    weak Node w = obj;
    Node s = w.lock();
    return s.v;
}
""", 42),

    ("weak_after_release", """
class Node {
    i32 v;
}
Node make() {
    Node n = new Node;
    n.v = 7;
    return n;
}
i32 main() {
    weak Node w = make();
    Node s = w.lock();
    if (!s) return 1;
    return 0;
}
""", 1),

    ("weak_share", """
class Node {
    i32 v;
}
i32 main() {
    Node obj = new Node;
    obj.v = 10;
    weak Node w1 = obj;
    weak Node w2 = w1;
    Node s = w2.lock();
    return s.v;
}
""", 10),

    ("weak_param", """
class Node {
    i32 v;
}
i32 read(weak Node n) {
    Node s = n.lock();
    if (!s) return 0;
    return s.v;
}
i32 main() {
    Node obj = new Node;
    obj.v = 7;
    return read(obj);
}
""", 7),

    # ----------- interface tests -----------

    ("iface_basic", """
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
""", 36),

    ("iface_multi_method", """
interface ICalc {
    i32 add(i32 x, i32 y);
    i32 mul(i32 x, i32 y);
}
class Op : ICalc {
    i32 add(i32 x, i32 y) { return x + y; }
    i32 mul(i32 x, i32 y) { return x * y; }
}
i32 main() {
    Op op = new Op;
    ICalc c = op;
    i32 a = c.add(10, 2);
    i32 b = c.mul(10, 2);
    return a + b;
}
""", 32),

    ("iface_polymorphic", """
interface IMath {
    i32 compute(i32 x, i32 y);
}
class Adder : IMath {
    i32 compute(i32 x, i32 y) { return x + y; }
}
class Multiplier : IMath {
    i32 compute(i32 x, i32 y) { return x * y; }
}
i32 main() {
    Adder a = new Adder;
    Multiplier m = new Multiplier;
    IMath im = a;
    i32 r1 = im.compute(3, 4);
    IMath im2 = m;
    i32 r2 = im2.compute(3, 4);
    return r1 + r2;
}
""", 19),

    ("iface_as", """
interface IShape {
    i32 area();
}
class Square : IShape {
    i32 side;
    i32 area() { return this.side * this.side; }
}
i32 main() {
    Square sq = new Square;
    sq.side = 7;
    IShape s = sq;
    Square s2 = s as Square;
    return s2.side;
}
""", 7),

    ("iface_as_wrong_type", """
interface IShape {
    i32 area();
}
class Square : IShape {
    i32 side;
    i32 area() { return this.side * this.side; }
}
class Circle : IShape {
    i32 radius;
    i32 area() { return 3 * this.radius * this.radius; }
}
i32 main() {
    Square sq = new Square;
    sq.side = 5;
    IShape s = sq;
    Circle c = s as Circle;
    if (!c) {
        return 1;
    }
    return 0;
}
""", 1),

    ("iface_func_param", """
interface IShape {
    i32 area();
}
class Circle : IShape {
    i32 radius;
    i32 area() { return 3 * this.radius * this.radius; }
}
i32 getArea(IShape s) {
    return s.area();
}
i32 main() {
    Circle c = new Circle;
    c.radius = 3;
    IShape s = c;
    return getArea(s);
}
""", 27),

    ("iface_return", """
interface IShape {
    i32 area();
}
class Circle : IShape {
    i32 radius;
    i32 area() { return 3 * this.radius * this.radius; }
}
IShape makeShape(i32 r) {
    Circle c = new Circle;
    c.radius = r;
    return c;
}
i32 main() {
    IShape s = makeShape(4);
    return s.area();
}
""", 48),

    ("iface_multi_impl", """
interface IArea {
    i32 area();
}
interface IPerim {
    i32 perim();
}
class Rect : IArea, IPerim {
    i32 w;
    i32 h;
    i32 area() { return this.w * this.h; }
    i32 perim() { return 2 * (this.w + this.h); }
}
i32 main() {
    Rect r = new Rect;
    r.w = 3;
    r.h = 5;
    IArea a = r;
    IPerim p = r;
    return a.area() + p.perim();
}
""", 31),

    ("iface_assign", """
interface IVal {
    i32 get();
}
class Box : IVal {
    i32 v;
    i32 get() { return this.v; }
}
i32 main() {
    Box b1 = new Box;
    b1.v = 10;
    Box b2 = new Box;
    b2.v = 20;
    IVal iv = b1;
    i32 r1 = iv.get();
    iv = b2;
    i32 r2 = iv.get();
    return r1 + r2;
}
""", 30),

    ("iface_ref_param", """
interface IInc {
    void inc(ref i32 x);
}
class Counter : IInc {
    void inc(ref i32 x) {
        x = x + 1;
    }
}
i32 main() {
    Counter c = new Counter;
    IInc inc = c;
    i32 v = 10;
    inc.inc(v);
    return v;
}
""", 11),

    ("iface_ref_method", """
interface ISetter {
    void set(i32 v);
    i32 get();
}
class Holder : ISetter {
    i32 val;
    void set(i32 v) { this.val = v; }
    i32 get() { return this.val; }
}
i32 main() {
    Holder h = new Holder;
    ISetter is = h;
    is.set(42);
    return is.get();
}
""", 42),

    ("iface_void_method", """
interface IDisplay {
    void show();
}
i32 main() {
    return 99;
}
""", 99),

    ("iface_retain_class", """
interface IFactory {
    i32 val();
}
class Data : IFactory {
    i32 v;
    i32 val() { return this.v; }
}
Data makeData(i32 n) {
    Data d = new Data;
    d.v = n;
    return d;
}
i32 main() {
    Data d = makeData(13);
    IFactory f = d;
    return f.val();
}
""", 13),

    ("iface_nested", """
interface ITransform {
    i32 apply(i32 x);
}
class Doubler : ITransform {
    i32 apply(i32 x) { return x + x; }
}
i32 run(ITransform t, i32 x) {
    return t.apply(x);
}
i32 main() {
    Doubler d = new Doubler;
    ITransform t = d;
    return run(t, 21);
}
""", 42),

    ("circular_ref_leak", """
class Node {
    Node next;
    i32 value;
}
i32 main() {
    Node a = new Node;
    Node b = new Node;
    a.next = b;
    b.next = a.next;
    b.value = 42;
    return b.value;
}
""", 42),
]

# ============================================================
# RUNNER
# ============================================================

def run_test(idx, name, source, expected, asan_dll_dir):
    testdir = os.path.join(SCRIPT_DIR, "test")
    os.makedirs(testdir, exist_ok=True)
    exedir = os.path.join(SCRIPT_DIR, "build", "test")
    os.makedirs(exedir, exist_ok=True)

    my_file = os.path.join(testdir, f"_t{idx}.my")
    c_file  = os.path.join(testdir, f"_t{idx}.c")
    exe_file = os.path.join(exedir, f"_t{idx}.exe")

    with open(my_file, "w", encoding="utf-8") as f:
        f.write(source.strip() + "\n")

    # Compile .my -> .c
    r = shell(f'"{MYLANG_EXE}" {my_file} {c_file}', cwd=SCRIPT_DIR)
    if r.returncode != 0:
        # mylang might crash with ASan detection - capture exit code
        return False, f"mylang exit {r.returncode}: {r.stderr[-200:] if r.stderr else ''}"

    # Compile .c -> .exe
    if not compile_c(c_file, exe_file):
        return False, "C compile error"

    # Run
    try:
        exit_code, output = run_exe(exe_file, asan_dll_dir)
    except subprocess.TimeoutExpired:
        return False, "timeout"

    if expected == "crash":
        # Any non-zero exit means the program was terminated (breakpoint/ASan)
        if exit_code != 0:
            return True, f"exited {exit_code & 0xFFFFFFFF} (crash)"
        else:
            return False, f"exit 0, expected crash"

    if exit_code != expected:
        snippet = output[-300:] if len(output) > 300 else output
        return False, f"exit {exit_code}, expected {expected}\n  {snippet}"

    return True, f"exit {exit_code}"


def main():
    global TEST_MODE

    parser = argparse.ArgumentParser()
    parser.add_argument("--mode", choices=["release", "debug"], default="release",
                        help="release = ASan + release CRT, debug = no ASan + debug CRT")
    parser.add_argument("filters", nargs="*", default=None,
                        help="optional test name keywords to filter (case-insensitive)")
    args = parser.parse_args()
    TEST_MODE = args.mode
    filters = [f.lower() for f in args.filters] if args.filters else None

    if not os.path.exists(MYLANG_EXE):
        print("Building mylang.exe...")
        if not compile_mylang():
            sys.exit(1)

    asan_dll_dir = ""
    if TEST_MODE == "release":
        asan_dll_dir = ensure_asan_dll()
        if not asan_dll_dir:
            print("WARNING: running without ASan - tests may still pass")

    passed = 0
    failed = 0
    total = 0

    for i, (name, source, expected) in enumerate(TESTS):
        if filters:
            name_lower = name.lower()
            if not any(k in name_lower for k in filters):
                continue
        total += 1
        ok, msg = run_test(i, name, source, expected, asan_dll_dir)
        status = "PASS" if ok else "FAIL"
        print(f"[{status}] {name:30s} {msg}")
        if ok:
            passed += 1
        else:
            failed += 1

    if filters:
        print(f"\n{passed}/{total} passed, {failed} failed  (filtered from {len(TESTS)} tests)")
    else:
        print(f"\n{passed}/{total} passed, {failed} failed")
    sys.exit(0 if failed == 0 else 1)

if __name__ == "__main__":
    main()
