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

TEST_MODE = "release"  # "release" = ASan + release CRT, "debug" = no ASan + debug CRT

_VSPATH_CANDIDATES = [
    r"C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat",
    r"C:\Program Files\Microsoft Visual Studio\2022\Professional\VC\Auxiliary\Build\vcvars64.bat",
    r"C:\Program Files\Microsoft Visual Studio\2022\Enterprise\VC\Auxiliary\Build\vcvars64.bat",
    r"C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat",
    r"C:\Program Files (x86)\Microsoft Visual Studio\18\BuildTools\VC\Auxiliary\Build\vcvars64.bat",
]
VSPATH = None
for _vc_candidate in _VSPATH_CANDIDATES:
    if os.path.exists(_vc_candidate):
        VSPATH = _vc_candidate
        break
if not VSPATH:
    raise RuntimeError("Cannot find vcvars64.bat")

def _capture_msvc_env():
    """Call vcvars64.bat once and capture the resulting environment variables."""
    cmd = f'call "{VSPATH}" >nul 2>&1 && set'
    r = subprocess.run(cmd, capture_output=True, text=True, shell=True)
    if r.returncode != 0:
        raise RuntimeError("Failed to setup MSVC environment")
    env = os.environ.copy()
    for line in r.stdout.splitlines():
        if '=' in line:
            k, v = line.split('=', 1)
            for existing in list(env.keys()):
                if existing.lower() == k.lower() and existing != k:
                    del env[existing]
            env[k] = v
    return env

MSVC_ENV = _capture_msvc_env()

def msvc_cl(args, cwd=None):
    """Run a cl.exe command using the pre-captured MSVC environment."""
    return subprocess.run(args, capture_output=True, text=True, shell=True,
                          env=MSVC_ENV, cwd=cwd)

def find_asan_dll():
    """Find the ASan runtime DLL and return its directory."""
    search_roots = [
        r"C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Tools\MSVC",
        r"C:\Program Files\Microsoft Visual Studio\2022\Professional\VC\Tools\MSVC",
        r"C:\Program Files\Microsoft Visual Studio\2022\Enterprise\VC\Tools\MSVC",
        r"C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Tools\MSVC",
        r"C:\Program Files (x86)\Microsoft Visual Studio\18\BuildTools\VC\Tools\MSVC",
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
    srcs = "src\\token.c src\\ast.c src\\mangle.c src\\lexer.c src\\symtab.c src\\parser.c src\\codegen.c src\\main.c"
    if TEST_MODE == "debug":
        flags = "/MDd /Zi"
    else:
        flags = "/fsanitize=address /Zi"
    cmd = f'cl /nologo /std:c11 /W3 {flags} /Fe:build\\mylang.exe /Fo:build\\ {srcs}'
    r = msvc_cl(cmd, cwd=SCRIPT_DIR)
    if r.returncode != 0:
        print("FAIL: mylang compilation failed")
        print(r.stdout)
        print(r.stderr)
        return False
    return True

def compile_pch_and_runtime():
    """Pre-compile runtime.h PCH and runtime.obj once per mode."""
    if TEST_MODE == "debug":
        pch_name = "runtime_debug"
        flags = "/MDd /Zi"
    else:
        pch_name = "runtime"
        flags = "/fsanitize=address /Zi"

    pch_path = os.path.join(SCRIPT_DIR, "build", pch_name + ".pch")
    runtime_obj = os.path.join(SCRIPT_DIR, "build", pch_name + ".obj")
    pch_obj = os.path.join(SCRIPT_DIR, "build", pch_name + "_pch.obj")

    build_dir = os.path.join(SCRIPT_DIR, "build")
    os.makedirs(build_dir, exist_ok=True)

    runtime_pch_c = os.path.join(SCRIPT_DIR, "src", "runtime_pch.c")
    cmd1 = f'cl /nologo /std:c11 {flags} /Isrc /Ycruntime.h /Fp{pch_path} /Fo{pch_obj} /c {runtime_pch_c}'
    r = msvc_cl(cmd1, cwd=SCRIPT_DIR)
    if r.returncode != 0:
        print("FAIL: PCH creation failed")
        print(r.stdout[-500:])
        return False

    runtime_c = os.path.join(SCRIPT_DIR, "src", "runtime.c")
    cmd2 = f'cl /nologo /std:c11 {flags} /Isrc /Yuruntime.h /Fp{pch_path} /Fo{runtime_obj} /c {runtime_c}'
    r = msvc_cl(cmd2, cwd=SCRIPT_DIR)
    if r.returncode != 0:
        print("FAIL: runtime.c compilation with PCH failed")
        print(r.stdout[-500:])
        return False

    return True

def compile_c(src, exe, extra_sources=None):
    """Compile generated C code + runtime.c with MSVC + ASan (release) or debug CRT (debug).
    extra_sources: list of additional .c files to link (e.g. native implementations)."""
    exedir = os.path.dirname(exe)
    os.makedirs(exedir, exist_ok=True)
    if TEST_MODE == "debug":
        pch_name = "runtime_debug"
        flags = "/MDd /Zi"
    else:
        pch_name = "runtime"
        flags = "/fsanitize=address /Zi"
    pch_path = os.path.join(SCRIPT_DIR, "build", pch_name + ".pch")
    runtime_obj = os.path.join(SCRIPT_DIR, "build", pch_name + ".obj")
    pch_obj = os.path.join(SCRIPT_DIR, "build", pch_name + "_pch.obj")
    testdir = os.path.join(SCRIPT_DIR, "test")
    extras = ""
    if extra_sources:
        extras = " " + " ".join(extra_sources)
    cmd = f'cl /nologo /std:c11 {flags} /FS /Isrc /I"{testdir}" /Yuruntime.h /Fp{pch_path} /Fo:"{exedir.replace(os.sep, "/")}/" /Fe:{exe} {src} {pch_obj} {runtime_obj}{extras}'
    r = msvc_cl(cmd, cwd=SCRIPT_DIR)
    if r.returncode != 0:
        print(f"  C compile error for {src}:")
        print(r.stdout[-500:] if len(r.stdout) > 500 else r.stdout)
        return False
    return True

def run_exe(exe, asan_dll_dir=""):
    """Run executable and return (exit code, stdout, stderr)."""
    env = os.environ.copy()
    if asan_dll_dir:
        env["PATH"] = asan_dll_dir + ";" + env.get("PATH", "")
    r = subprocess.run(exe, capture_output=True, text=True, shell=True,
                       cwd=SCRIPT_DIR, timeout=10, env=env)
    return r.returncode, r.stdout, r.stderr

# ============================================================
# TEST CASES: (name, source, expected)
#   expected is one of:
#     int                  - expected process exit code
#     "crash"              - any non-zero exit (abort/breakpoint/ASan)
#     (exit_code, stdout)  - exit code plus exact stdout match
#     "some output"        - shorthand for (0, "some output")
# A 4th element may carry native C source for native-method tests.
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

    ("hex_literals", """
i32 main() {
    i32 a = 0x10;        // 16
    i32 b = 0xff;        // 255
    i32 c = 0X2A;        // 42 (uppercase prefix and digits)
    i32 d = 0xAb1;       // 2737 (mixed case digits)
    if (a != 16) { return 1; }
    if (b != 255) { return 2; }
    if (c != 42) { return 3; }
    if (d != 2737) { return 4; }
    i32 flags = (0x1 << 4) | 0x3;
    if (flags != 19) { return 5; }
    i32 neg = -0x10;
    if (neg != -16) { return 6; }
    i32 m = 0;
    match (0x2) {
        0x1 => { m = 1; }
        0x2 => { m = 20; }
        else => { m = 30; }
    }
    if (m != 20) { return 7; }
    return a + c;
}
""", 58),

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

    ("compound_add", """
i32 main() {
    i32 x = 5;
    x += 3;
    return x;
}
""", 8),

    ("compound_sub", """
i32 main() {
    i32 x = 10;
    x -= 4;
    return x;
}
""", 6),

    ("compound_mul", """
i32 main() {
    i32 x = 7;
    x *= 3;
    return x;
}
""", 21),

    ("compound_div", """
i32 main() {
    i32 x = 20;
    x /= 4;
    return x;
}
""", 5),

    ("compound_array", """
i32 main() {
    i32[] arr;
    arr.push(10);
    arr[0] += 5;
    arr[0] -= 2;
    arr[0] *= 2;
    arr[0] /= 3;
    return arr[0];
}
""", 8),

    ("compound_member", """
class Counter {
    i32 val;
}
i32 main() {
    Counter c = new Counter;
    c.val = 10;
    c.val += 5;
    c.val -= 2;
    return c.val;
}
""", 13),

    ("compound_float", """
f32 main() {
    f32 x = 5;
    x += 2;
    x *= 2;
    return x;
}
""", 14),

    ("compound_struct_array", """
struct Pos {
    f32 x;
    f32 y;
}
i32 main() {
    Pos[] arr;
    arr.resize(1);
    arr[0].x = 1;
    arr[0].x += 10;
    i32 r = arr[0].x;
    return r;
}
""", 11),

    ("inc_basic", """
i32 main() {
    i32 x = 5;
    x++;
    ++x;
    return x;
}
""", 7),

    ("dec_basic", """
i32 main() {
    i32 x = 5;
    x--;
    --x;
    return x;
}
""", 3),

    ("inc_loop", """
i32 main() {
    i32 i = 0;
    i32 s = 0;
    while (i < 5) {
        s += i;
        i++;
    }
    return s;
}
""", 10),

    ("inc_member", """
class Counter {
    i32 n;
}
i32 main() {
    Counter c = new Counter;
    c.n = 10;
    c.n++;
    ++c.n;
    return c.n;
}
""", 12),

    ("inc_array", """
i32 main() {
    i32[] arr;
    arr.push(1);
    arr.push(2);
    arr[0]++;
    ++arr[1];
    return arr[0] + arr[1];
}
""", 5),

    ("for_sum", """
i32 main() {
    i32 s = 0;
    for (i32 i = 1; i <= 10; i = i + 1) {
        s = s + i;
    }
    return s;
}
""", 55),

    ("for_no_init", """
i32 main() {
    i32 s = 0;
    i32 i = 0;
    for (; i < 5; i = i + 1) {
        s = s + i;
    }
    return s;
}
""", 10),

    ("for_no_step", """
i32 main() {
    i32 s = 0;
    for (i32 i = 0; i < 5;) {
        s = s + i;
        i = i + 1;
    }
    return s;
}
""", 10),

    ("for_no_cond", """
i32 main() {
    i32 s = 0;
    for (i32 i = 0;; i = i + 1) {
        if (i >= 5) { return s; }
        s = s + i;
    }
}
""", 10),

    ("for_float", """
i32 main() {
    i32 x = 0;
    for (i32 i = 0; i < 4; i++) {
        x += 2;
    }
    return x;
}
""", 8),

    ("break_while", """
i32 main() {
    i32 i = 0;
    i32 s = 0;
    while (i < 10) {
        i = i + 1;
        if (i > 5) { break; }
        s = s + i;
    }
    return s;
}
""", 15),

    ("break_for", """
i32 main() {
    i32 s = 0;
    for (i32 i = 0; i < 10; i = i + 1) {
        if (i > 5) { break; }
        s = s + i;
    }
    return s;
}
""", 15),

    ("continue_while", """
i32 main() {
    i32 i = 0;
    i32 s = 0;
    while (i < 10) {
        i = i + 1;
        if (i % 2 == 0) { continue; }
        s = s + i;
    }
    return s;
}
""", 25),

    ("continue_for", """
i32 main() {
    i32 s = 0;
    for (i32 i = 0; i < 10; i = i + 1) {
        if (i % 2 == 0) { continue; }
        s = s + i;
    }
    return s;
}
""", 25),

    ("break_with_cleanup", """
class Counter {
    i32 val;
}
i32 main() {
    i32 s = 0;
    i32 i = 0;
    while (i < 10) {
        Counter c = new Counter;
        c.val = i;
        if (i > 5) { break; }
        s = s + c.val;
        i = i + 1;
    }
    return s;
}
""", 15),

    ("continue_with_cleanup", """
class Counter {
    i32 val;
}
i32 main() {
    i32 s = 0;
    i32 i = 0;
    while (i < 10) {
        i = i + 1;
        Counter c = new Counter;
        c.val = i;
        if (i % 2 == 0) { continue; }
        s = s + c.val;
    }
    return s;
}
""", 25),

    ("break_nested", """
i32 main() {
    i32 s = 0;
    i32 i = 0;
    while (i < 3) {
        i32 j = 0;
        while (j < 3) {
            s = s + 1;
            if (j == 1) { break; }
            j = j + 1;
        }
        i = i + 1;
    }
    return s;
}
""", 6),

    ("while_cond_call", """
class Node {
    i32 v;
}
i32 main() {
    Node a = new Node;
    Node b = new Node;
    weak Node w = a;
    i32 iters = 0;
    while (w.lock()) {
        iters = iters + 1;
        if (iters == 3) { a = b; }
        if (iters > 10) { return 100; }
    }
    return iters;
}
""", 3),

    ("for_cond_call", """
class Node {
    i32 v;
}
i32 main() {
    Node a = new Node;
    Node b = new Node;
    weak Node w = a;
    i32 last = -1;
    for (i32 i = 0; w.lock(); i = i + 1) {
        last = i;
        if (i == 2) { a = b; }
        if (i > 10) { return 100; }
    }
    return last;
}
""", 2),

    ("if_cond_call", """
class Node {
    i32 v;
}
i32 main() {
    Node a = new Node;
    weak Node w = a;
    i32 x = 0;
    if (w.lock()) { x = 1; } else { x = 2; }
    return x;
}
""", 1),

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
    i32[] arr;
    arr.resize(3);
    arr[0] = 1;
    arr[5] = 0;
    return 0;
}
""", "crash"),

    ("bounds_in_bounds", """
i32 main() {
    i32[] arr;
    arr.resize(10);
    arr[5] = 42;
    return arr[5];
}
""", 42),

    ("neg_and_not", """
i32 main() {
    i32 x = 5;
    i32 y = -x;
    if (y != -5) { return 0; }
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
    if (a < b) { r = r + 1; }
    if (a <= b) { r = r + 1; }
    if (b > a) { r = r + 1; }
    if (b >= a) { r = r + 1; }
    if (a == a) { r = r + 1; }
    if (a != b) { r = r + 1; }
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
    if (c == x) { return 42; }
    return 0;
}
""", 42),

    ("new_array_expr_size", """
i32 main() {
    i32 n = 5;
    i32[] arr;
    arr.resize(n + 2);
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
    Box[] arr;
    arr.resize(3);
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
i32 sum(ref Box[] arr) {
    return arr[0].v + arr[1].v;
}
i32 main() {
    Box[] a;
    a.resize(2);
    a[0] = new Box;
    a[0].v = 3;
    a[1] = new Box;
    a[1].v = 4;
    return sum(ref a);
}
""", 7),

    ("class_array_return_func", """
class Box {
    i32 v;
}
void make(ref Box[] out) {
    Box[] a;
    a.resize(2);
    a[0] = new Box;
    a[0].v = 5;
    a[1] = new Box;
    a[1].v = 7;
    a.move_to(ref out);
}
i32 main() {
    Box[] x;
    make(ref x);
    return x[0].v + x[1].v;
}
""", 12),

    ("class_array_bounds", """
class Box {
    i32 v;
}
i32 main() {
    Box[] a;
    a.resize(2);
    return a[5].v;
}
""", "crash"),

    ("class_array_replace", """
class Box {
    i32 v;
}
i32 main() {
    Box[] a;
    a.resize(2);
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
    Box[] a;
    a.resize(4);
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
    Box[] a;
    a.resize(2);
    a[0] = new Box;
    a[0].v = 5;
    return a[0].get();
}
""", 5),

    ("array_vector_methods", """
i32 main() {
    i32[] a;
    a.resize(2);
    a[0] = 1;
    a[1] = 2;
    a.push(3);
    a.push(4);
    a.pop();
    a.reserve(10);
    a.resize(5);
    a.clear();
    a.push(5);
    return a[0] + a.length;
}
""", 6),

    ("class_array_vector_methods", """
class Box {
    i32 v;
}
i32 main() {
    Box[] a;
    a.resize(1);
    a[0] = new Box;
    a[0].v = 2;
    Box b = new Box;
    b.v = 3;
    a.push(b);
    a.pop();
    return a[0].v;
}
""", 2),

    ("array_move_copy", """
i32 main() {
    i32[] a;
    a.resize(2);
    a[0] = 10;
    a[1] = 20;
    i32[] b;
    a.copy_to(ref b);
    i32[] c;
    a.move_to(ref c);
    return b[0] + b[1] + c.length;
}
""", 32),

    ("weak_iface_dyn_array", """
interface IBox {
    i32 get();
}
class Box : IBox {
    i32 v;
    i32 get() { return this.v; }
}
i32 main() {
    weak IBox[] w;
    w.resize(2);
    Box a = new Box;
    a.v = 7;
    Box b = new Box;
    b.v = 9;
    w[0] = a;
    w[1] = b;
    IBox s = w[0].lock();
    return s.get();
}
""", 7),

    ("iface_array_push", """
interface IShape {
    i32 area();
}
class Square : IShape {
    i32 side;
    i32 area() { return this.side * this.side; }
}
i32 main() {
    IShape[] arr;
    Square sq = new Square;
    sq.side = 3;
    IShape s = sq;
    arr.push(s);
    arr.push(s);
    arr.push(s);
    arr.push(s);
    arr.push(s);
    return arr[0].area() + arr[4].area();
}
""", 18),

    ("weak_class_array_push", """
class Node {
    i32 v;
}
i32 main() {
    weak Node[] arr;
    Node a = new Node;
    a.v = 5;
    Node b = new Node;
    b.v = 7;
    weak Node wa = a;
    weak Node wb = b;
    arr.push(wa);
    arr.push(wb);
    arr.push(wa);
    arr.push(wb);
    arr.push(wa);
    Node s1 = arr[4].lock();
    Node s2 = arr[1].lock();
    if (!s1) { return 0; }
    if (!s2) { return 0; }
    return s1.v + s2.v;
}
""", 12),

    ("weak_iface_array_push", """
interface IBox {
    i32 get();
}
class Box : IBox {
    i32 v;
    i32 get() { return this.v; }
}
i32 main() {
    weak IBox[] arr;
    Box a = new Box;
    a.v = 4;
    Box b = new Box;
    b.v = 6;
    weak IBox wa = a;
    weak IBox wb = b;
    arr.push(wa);
    arr.push(wb);
    arr.push(wa);
    arr.push(wb);
    arr.push(wb);
    IBox s = arr[4].lock();
    return s.get() + arr.length;
}
""", 11),

    ("weak_class_array_elem_assign", """
class Node {
    i32 v;
}
i32 main() {
    weak Node[] arr;
    arr.resize(2);
    Node a = new Node;
    a.v = 5;
    Node b = new Node;
    b.v = 7;
    weak Node wa = a;
    weak Node wb = b;
    arr[0] = wa;
    arr[1] = wb;
    Node s1 = arr[0].lock();
    Node s2 = arr[1].lock();
    if (!s1) { return 0; }
    if (!s2) { return 0; }
    return s1.v + s2.v;
}
""", 12),

    ("weak_class_array_elem_assign_strong", """
class Node {
    i32 v;
}
i32 main() {
    weak Node[] arr;
    arr.resize(1);
    Node a = new Node;
    a.v = 9;
    arr[0] = a;
    Node s = arr[0].lock();
    if (!s) { return 0; }
    return s.v;
}
""", 9),

    ("weak_class_array_elem_assign_owned", """
class Node {
    i32 v;
}
Node make() {
    Node n = new Node;
    n.v = 4;
    return n;
}
i32 main() {
    weak Node[] arr;
    arr.resize(1);
    arr[0] = make();
    Node s = arr[0].lock();
    if (!s) { return 1; }
    return 0;
}
""", 1),

    ("weak_iface_array_elem_assign", """
interface IBox {
    i32 get();
}
class Box : IBox {
    i32 v;
    i32 get() { return this.v; }
}
i32 main() {
    weak IBox[] arr;
    arr.resize(2);
    Box a = new Box;
    a.v = 3;
    weak IBox wa = a;
    arr[0] = wa;
    arr[1] = wa;
    IBox s1 = arr[0].lock();
    IBox s2 = arr[1].lock();
    return s1.get() + s2.get();
}
""", 6),

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
    u32[] arr;
    arr.resize(4);
    arr[0] = 10;
    arr[1] = 20;
    arr[2] = arr[0] + arr[1];
    arr[3] = 0;
    return arr[2];
}
""", 30),

    ("fixed_width_func_arg", """
i32 sum(ref i16[] arr) {
    i32 s = 0;
    i32 i = 0;
    while (i < 3) {
        s = s + arr[i];
        i = i + 1;
    }
    return s;
}
i32 main() {
    i16[] a;
    a.resize(3);
    a[0] = 10;
    a[1] = 20;
    a[2] = 30;
    return sum(ref a);
}
""", 60),

    ("float_array", """
i32 main() {
    f32[] a;
    a.resize(3);
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
    add_one(ref a);
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
    inc(ref c);
    return c.n;
}
""", 6),

    ("ref_chain", """
void bump(ref i32 x) {
    x = x + 1;
}
void proxy(ref i32 y) {
    bump(ref y);
}
i32 main() {
    i32 a = 10;
    proxy(ref a);
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
    divmod(17, 5, ref q, ref r);
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
    a.add(ref v);
    i32 result = 0;
    a.get(ref result);
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
    i32[] a;
    a.resize(3);
    fill(ref a);
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
    Vec[] arr;
    arr.resize(3);
    arr[0].v = 1;
    arr[1].v = 2;
    arr[2].v = 3;
    return arr[0].v + arr[1].v + arr[2].v;
}
""", 6),

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
    inc(ref c);
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

    ("struct_nested_basic", """
struct Rect {
    Point tl;
    Point br;
}
struct Point {
    i32 x;
    i32 y;
}
i32 main() {
    Rect r;
    r.tl.x = 1;
    r.tl.y = 2;
    r.br.x = 3;
    r.br.y = 4;
    return r.tl.x + r.tl.y + r.br.x + r.br.y;
}
""", 10),

    ("struct_nested_copy", """
struct Inner {
    i32 v;
}
struct Outer {
    Inner a;
    Inner b;
}
i32 main() {
    Outer o1;
    o1.a.v = 10;
    o1.b.v = 20;
    Outer o2 = o1;
    o2.a.v = 99;
    return o1.a.v + o2.a.v + o2.b.v;
}
""", 129),

    ("struct_nested_in_class", """
struct Point {
    i32 x;
    i32 y;
}
struct Line {
    Point a;
    Point b;
}
class Shape {
    Line line;
}
i32 main() {
    Shape s = new Shape;
    s.line.a.x = 5;
    s.line.b.y = 7;
    return s.line.a.x + s.line.b.y;
}
""", 12),

    ("struct_nested_array", """
struct Point {
    i32 x;
}
struct Pair {
    Point p;
    Point q;
}
i32 main() {
    Pair[] arr;
    arr.resize(2);
    arr[0].p.x = 1;
    arr[0].q.x = 2;
    arr[1].p.x = 3;
    arr[1].q.x = 4;
    return arr[0].p.x + arr[0].q.x + arr[1].p.x + arr[1].q.x;
}
""", 10),

    ("struct_class_field_forward", """
class Holder {
    Point p;
}
struct Point {
    i32 x;
}
i32 main() {
    Holder h = new Holder;
    h.p.x = 21;
    return h.p.x * 2;
}
""", 42),

    ("struct_method_basic", """
struct Vec2 {
    i32 x;
    i32 y;
    void set(i32 nx, i32 ny) {
        this.x = nx;
        this.y = ny;
    }
    i32 sum() {
        return this.x + this.y;
    }
    Vec2 add(Vec2 o) {
        Vec2 r;
        r.x = this.x + o.x;
        r.y = this.y + o.y;
        return r;
    }
    void scale(i32 k) {
        this.x = this.x * k;
        this.y = this.y * k;
    }
}
i32 main() {
    Vec2 a;
    a.set(1, 2);
    Vec2 b;
    b.set(3, 4);
    Vec2 c = a.add(b);
    if (c.sum() != 10) { return 1; }
    c.scale(2);
    if (c.sum() != 20) { return 2; }
    a.scale(10);
    if (a.sum() != 30) { return 3; }
    if (b.sum() != 7) { return 4; }
    return c.sum();
}
""", 20),

    ("struct_method_ref_and_array", """
struct Counter {
    i32 n;
    void bump() { this.n = this.n + 1; }
    i32 get() { return this.n; }
}
void touch(ref Counter c) {
    c.bump();
}
class Holder {
    Counter c;
}
i32 main() {
    Counter c;
    c.n = 0;
    touch(ref c);
    if (c.get() != 1) { return 1; }
    Counter[] arr;
    arr.resize(2);
    arr[0].n = 10;
    arr[0].bump();
    arr[1].n = 20;
    arr[1].bump();
    if (arr[0].get() != 11) { return 2; }
    Holder h = new Holder;
    h.c.n = 5;
    h.c.bump();
    if (h.c.get() != 6) { return 3; }
    return arr[0].get() + arr[1].get() + c.get() + h.c.get();
}
""", 39),

    ("struct_method_this_value", """
struct Acc {
    i32 v;
    Acc plus(Acc o) {
        Acc r;
        r.v = this.v + o.v;
        return r;
    }
    bool eq(Acc o) {
        return this.v == o.v;
    }
}
i32 main() {
    Acc a;
    a.v = 40;
    Acc b;
    b.v = 2;
    Acc c = a.plus(b);
    if (!c.eq(c)) { return 1; }
    return c.v;
}
""", 42),

    ("import_basic", """
import("import_basic_lib.my");
import("import_basic_inner.my")
import("import_basic_lib.my");

i32 main() {
    Point p = new Point;
    p.x = 1;
    p.y = 2;
    Vec v;
    v.v = 5;
    v.double_it();
    return helper(p.sum()) + v.v + inner_val();
}
""", 47, None, [
    ("import_basic_lib.my", """
class Point {
    i32 x;
    i32 y;
    i32 sum() { return this.x + this.y; }
}
struct Vec {
    i32 v;
    void double_it() { this.v = this.v * 2; }
}
i32 helper(i32 n) {
    return n * 10;
}
"""),
    ("import_basic_inner.my", """
i32 inner_val() {
    return 7;
}
"""),
]),

    ("import_diamond", """
import("import_b.my");
import("import_c.my");

i32 main() {
    Shared s = new Shared;
    s.v = 5;
    return dval() + bval() + cval() + s.v;
}
""", 23, None, [
    ("import_d.my", """
class Shared {
    i32 v;
}
i32 dval() {
    return 5;
}
"""),
    ("import_b.my", """
import("import_d.my");
i32 bval() {
    return dval() + 1;
}
"""),
    ("import_c.my", """
import("import_d.my");
i32 cval() {
    return dval() + 2;
}
"""),
]),

    ("import_cycle", """
import("import_x.my");

i32 main() {
    return xval() + yval();
}
""", 7, None, [
    ("import_x.my", """
import("import_y.my");
i32 xval() {
    return 3;
}
"""),
    ("import_y.my", """
import("import_x.my");
i32 yval() {
    return 4;
}
"""),
]),

    ("import_panic_location", """
import("import_pl_lib.my");

i32 main() {
    boom();
    return 0;
}
""", ("crash_contains", "import_pl_lib.my:4"), None, [
    ("import_pl_lib.my", """
void boom() {
    i32[] a;
    a.resize(2);
    a[9] = 1;
}
"""),
]),

    ("struct_ref_retain_release", """
class Node {
    i32 v;
}
struct Box {
    Node n;
}
i32 main() {
    weak Node w;
    i32 r = 0;
    {
        Box b;
        {
            Node n = new Node;
            n.v = 7;
            w = n;
            b.n = n;
        }
        Node s = w.lock();
        if (!s) { return 1; }
        r = s.v;
    }
    Node s2 = w.lock();
    if (s2) { return 2; }
    return r;
}
""", 7),

    ("struct_ref_param", """
class Node {
    i32 v;
}
struct Box {
    Node n;
}
i32 read_v(Box b, weak Node w) {
    Node s = w.lock();
    if (!s) { return 0; }
    return s.v;
}
i32 main() {
    weak Node w;
    i32 r;
    {
        Box b;
        {
            Node n = new Node;
            n.v = 11;
            w = n;
            b.n = n;
        }
        r = read_v(b, w);
    }
    Node s2 = w.lock();
    if (s2) { return 99; }
    return r;
}
""", 11),

    ("struct_ref_return", """
class Node {
    i32 v;
}
struct Box {
    Node n;
}
Box make(i32 v) {
    Box b;
    Node n = new Node;
    n.v = v;
    b.n = n;
    return b;
}
i32 main() {
    weak Node w;
    i32 r;
    {
        Box x = make(13);
        w = x.n;
        Node s = w.lock();
        if (!s) { return 98; }
        r = s.v;
    }
    Node s2 = w.lock();
    if (s2) { return 99; }
    return r;
}
""", 13),

    ("struct_ref_assign", """
class Node {
    i32 v;
}
struct Box {
    Node n;
}
i32 main() {
    weak Node w1;
    weak Node w2;
    {
        Box b;
        {
            Node n1 = new Node;
            n1.v = 1;
            w1 = n1;
            b.n = n1;
        }
        {
            Box b2;
            {
                Node n2 = new Node;
                n2.v = 2;
                w2 = n2;
                b2.n = n2;
            }
            b = b2;
            if (!w2.lock()) { return 96; }
        }
        if (w1.lock()) { return 97; }
        if (!w2.lock()) { return 98; }
    }
    if (w2.lock()) { return 99; }
    return 5;
}
""", 5),

    ("struct_ref_iface", """
interface IVal {
    i32 get();
}
class Num : IVal {
    i32 v;
    override i32 get() { return this.v; }
}
struct Box {
    IVal s;
}
i32 main() {
    weak Num w;
    i32 r;
    {
        Box b;
        {
            Num n = new Num;
            n.v = 21;
            w = n;
            b.s = n;
        }
        r = b.s.get();
    }
    if (w.lock()) { return 99; }
    return r;
}
""", 21),

    ("struct_ref_nested", """
class Node {
    i32 v;
}
struct Inner {
    Node n;
}
struct Outer {
    Inner in;
}
i32 main() {
    weak Node w;
    i32 r = 0;
    {
        Outer o1;
        {
            Node n = new Node;
            n.v = 17;
            w = n;
            o1.in.n = n;
        }
        Outer o2 = o1;
        Node s = w.lock();
        if (!s) { return 1; }
        r = s.v;
    }
    Node s2 = w.lock();
    if (s2) { return 2; }
    return r;
}
""", 17),

    ("struct_ref_class_field", """
class Node {
    i32 v;
}
struct Box {
    Node n;
}
class Holder {
    Box b;
}
i32 main() {
    weak Node w;
    {
        Holder h = new Holder;
        {
            Node n = new Node;
            n.v = 3;
            w = n;
            h.b.n = n;
        }
    }
    if (w.lock()) { return 99; }
    return 6;
}
""", 6),

    ("struct_ref_weak_field", """
class Node {
    i32 v;
}
struct WBox {
    weak Node w;
}
i32 main() {
    weak Node probe;
    i32 r = 0;
    {
        WBox b;
        {
            Node n = new Node;
            n.v = 9;
            probe = n;
            b.w = n;
        }
        WBox b2 = b;
        if (probe.lock()) { return 95; }
        r = 8;
    }
    if (probe.lock()) { return 96; }
    return r;
}
""", 8),

    ("struct_ref_discard", """
class Node {
    i32 v;
}
struct Box {
    Node n;
}
Box make_with(Node n) {
    Box b;
    b.n = n;
    return b;
}
i32 main() {
    weak Node w;
    {
        Node n = new Node;
        n.v = 1;
        w = n;
        make_with(n);
    }
    if (w.lock()) { return 99; }
    return 4;
}
""", 4),

    ("struct_ref_object_field", """
class Node {
    i32 v;
}
struct Box {
    object o;
}
i32 main() {
    weak Node w;
    i32 r = 0;
    {
        Box b;
        {
            Node n = new Node;
            n.v = 33;
            w = n;
            b.o = n;
        }
        Node s = w.lock();
        if (!s) { return 1; }
        r = s.v;
    }
    Node s2 = w.lock();
    if (s2) { return 2; }
    return r;
}
""", 33),

    ("struct_temp_arg_release", """
class Node {
    i32 v;
}
struct Box {
    Node n;
}
Box make_with(Node n) {
    Box b;
    b.n = n;
    return b;
}
i32 take(Box b) {
    return b.n.v;
}
i32 main() {
    weak Node w;
    i32 r;
    {
        Node n = new Node;
        n.v = 5;
        w = n;
        r = take(make_with(n));
    }
    if (w.lock()) { return 96; }
    return r;
}
""", 5),

    ("struct_temp_member_release", """
class Node {
    i32 v;
}
struct Box {
    Node n;
}
Box make_with(Node n) {
    Box b;
    b.n = n;
    return b;
}
i32 main() {
    weak Node w;
    i32 r;
    {
        Node n = new Node;
        n.v = 5;
        w = n;
        r = make_with(n).n.v;
    }
    if (w.lock()) { return 96; }
    return r;
}
""", 5),

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
    if (!s) { return 1; }
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
    if (!s) { return 0; }
    return s.v;
}
i32 main() {
    Node obj = new Node;
    obj.v = 7;
    return read(obj);
}
""", 7),

    # ----------- unowned tests -----------

    ("unowned_basic", """
class Node {
    i32 v;
    i32 get() { return this.v; }
}
i32 main() {
    Node obj = new Node;
    obj.v = 42;
    unowned Node u = obj;
    return u.v + u.get();
}
""", 84),

    ("unowned_share_copy", """
class Node {
    i32 v;
}
i32 main() {
    Node obj = new Node;
    obj.v = 3;
    unowned Node u1 = obj;
    unowned Node u2 = u1;
    return u1.v + u2.v;
}
""", 6),

    ("unowned_to_strong", """
class Node {
    i32 v;
}
i32 main() {
    Node obj = new Node;
    obj.v = 7;
    unowned Node u = obj;
    Node s = u;
    return s.v;
}
""", 7),

    ("unowned_param", """
class Node {
    i32 v;
}
i32 read(unowned Node n) {
    return n.v;
}
i32 read_strong(Node n) {
    return n.v;
}
i32 main() {
    Node obj = new Node;
    obj.v = 9;
    unowned Node u = obj;
    return read(u) + read_strong(u) + read(obj);
}
""", 27),

    ("unowned_assign", """
class Node {
    i32 v;
}
i32 main() {
    Node a = new Node;
    a.v = 1;
    Node b = new Node;
    b.v = 2;
    unowned Node u = a;
    u = b;
    return u.v;
}
""", 2),

    ("unowned_field", """
class Node {
    i32 v;
}
class Holder {
    unowned Node n;
}
i32 main() {
    Node obj = new Node;
    obj.v = 5;
    Holder h = new Holder;
    h.n = obj;
    i32 r = h.n.v;
    Holder h2 = new Holder;
    h2.n = h.n;
    return r + h2.n.v;
}
""", 10),

    ("unowned_from_lock", """
class Node {
    i32 v;
}
Node make() {
    Node n = new Node;
    n.v = 8;
    return n;
}
i32 main() {
    Node obj = make();
    weak Node w = obj;
    unowned Node u = w.lock();
    return u.v;
}
""", 8),

    ("unowned_dead_access", """
class Node {
    i32 v;
}
Node make() {
    Node n = new Node;
    n.v = 7;
    return n;
}
i32 main() {
    unowned Node u = make();
    return u.v;
}
""", "crash"),

    ("unowned_dead_method", """
class Node {
    i32 v;
    i32 get() { return this.v; }
}
Node make() {
    Node n = new Node;
    n.v = 7;
    return n;
}
i32 main() {
    unowned Node u = make();
    return u.get();
}
""", "crash"),

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

    ("iface_multi_param", """
interface IOp {
    i32 calc(i32 a, i32 b, i32 c);
}
class Worker : IOp {
    i32 calc(i32 a, i32 b, i32 c) { return a * b + c; }
}
i32 main() {
    Worker w = new Worker;
    IOp op = w;
    return op.calc(5, 7, 3);
}
""", 38),

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
    inc.inc(ref v);
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

    ("iface_default_method_basic", """
interface IShape {
    i32 area();
    i32 isSquare() { return 0; }
}
class Circle : IShape {
    i32 radius;
    i32 area() { return 3 * this.radius * this.radius; }
}
i32 main() {
    Circle c = new Circle;
    c.radius = 2;
    IShape s = c;
    i32 a = s.area();
    i32 sq = s.isSquare();
    return a + sq;
}
""", 12),

    ("iface_default_method_override", """
interface IShape {
    i32 area();
    i32 isSquare() { return 0; }
}
class Square : IShape {
    i32 side;
    i32 area() { return this.side * this.side; }
    i32 isSquare() { return 1; }
}
i32 main() {
    Square sq = new Square;
    sq.side = 4;
    IShape s = sq;
    return s.isSquare();
}
""", 1),

    ("iface_default_method_param", """
interface IShape {
    i32 area();
    i32 scaled(i32 factor) { return factor * 2; }
}
class Circle : IShape {
    i32 radius;
    i32 area() { return 3 * this.radius * this.radius; }
}
i32 main() {
    Circle c = new Circle;
    c.radius = 2;
    IShape s = c;
    return s.scaled(5);
}
""", 10),

    ("iface_default_method_void", """
interface IShape {
    i32 area();
    void bump() { }
}
class Circle : IShape {
    i32 radius;
    i32 area() { return 3 * this.radius * this.radius; }
}
i32 main() {
    Circle c = new Circle;
    c.radius = 2;
    IShape s = c;
    s.bump();
    return s.area();
}
""", 12),

    ("iface_default_method_local", """
interface IShape {
    i32 area();
    i32 doubled() {
        i32 x = 2;
        return x * 2;
    }
}
class Circle : IShape {
    i32 radius;
    i32 area() { return 3 * this.radius * this.radius; }
}
i32 main() {
    Circle c = new Circle;
    c.radius = 2;
    IShape s = c;
    return s.doubled();
}
""", 4),

    ("iface_override_basic", """
interface IShape {
    i32 area();
}
class Square : IShape {
    i32 side;
    override i32 area() { return this.side * this.side; }
}
i32 main() {
    Square sq = new Square;
    sq.side = 5;
    IShape s = sq;
    return s.area();
}
""", 25),

    ("iface_override_default", """
interface IShape {
    i32 area();
    i32 isSquare() { return 0; }
}
class Square : IShape {
    i32 side;
    i32 area() { return this.side * this.side; }
    override i32 isSquare() { return 1; }
}
i32 main() {
    Square sq = new Square;
    sq.side = 5;
    IShape s = sq;
    return s.isSquare();
}
""", 1),

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

    ("iface_as_if", """
interface IShape {
    i32 area();
}
class Square : IShape {
    i32 side;
    i32 area() { return this.side * this.side; }
}
i32 main() {
    Square s = new Square;
    s.side = 4;
    IShape is = s;
    Square p = is as Square;
    if (p) {
        return p.area();
    }
    return 0;
}
""", 16),

    ("iface_return_class", """
class Box {
    i32 v;
}
interface IFactory {
    Box make(i32 v);
}
class Factory : IFactory {
    Box make(i32 v) {
        Box b = new Box;
        b.v = v;
        return b;
    }
}
i32 main() {
    Factory f = new Factory;
    IFactory ifact = f;
    Box b = ifact.make(7);
    return b.v;
}
""", 7),

    ("iface_return_iface", """
interface IInner {
    i32 val();
}
interface IOuter {
    IInner getInner();
}
class Inner : IInner {
    i32 val() { return 99; }
}
class Outer : IOuter {
    IInner getInner() {
        Inner i = new Inner;
        return i;
    }
}
i32 main() {
    Outer o = new Outer;
    IOuter io = o;
    IInner ii = io.getInner();
    return ii.val();
}
""", 99),

    ("iface_self_assign", """
interface IVal {
    i32 get();
}
class Box : IVal {
    i32 v;
    i32 get() { return this.v; }
}
i32 main() {
    Box b = new Box;
    b.v = 3;
    IVal iv = b;
    iv = iv;
    return iv.get();
}
""", 3),

    ("iface_assign_from_iface", """
interface IVal {
    i32 get();
}
class Box : IVal {
    i32 v;
    i32 get() { return this.v; }
}
i32 main() {
    Box b = new Box;
    b.v = 7;
    IVal a = b;
    IVal c = a;
    return c.get();
}
""", 7),

    ("iface_leak", """
interface IVal {
    i32 get();
}
class Box : IVal {
    i32 v;
    i32 get() { return this.v; }
}
i32 use() {
    Box b = new Box;
    b.v = 123;
    IVal iv = b;
    return iv.get();
}
i32 main() {
    return use();
}
""", 123),

    ("iface_compare", """
interface IVal {
    i32 get();
}
class A : IVal {
    i32 get() { return 1; }
}
class B : IVal {
    i32 get() { return 2; }
}
i32 main() {
    A a = new A;
    B b = new B;
    IVal ia = a;
    IVal ib = b;
    IVal ia2 = a;
    if (ia == ib) { return 0; }
    if (ia != ia2) { return 0; }
    return 99;
}
""", 99),

    ("weak_iface_basic", """
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
    weak IShape w = sq;
    IShape s = w.lock();
    return s.area();
}
""", 36),

    ("weak_iface_from_strong_iface", """
interface IShape {
    i32 area();
}
class Square : IShape {
    i32 side;
    i32 area() { return this.side * this.side; }
}
i32 main() {
    Square sq = new Square;
    sq.side = 5;
    IShape strong = sq;
    weak IShape w = strong;
    IShape s = w.lock();
    return s.area();
}
""", 25),

    ("weak_iface_dead_after_release", """
interface IShape {
    i32 area();
}
class Square : IShape {
    i32 side;
    i32 area() { return this.side * this.side; }
}
IShape make() {
    Square sq = new Square;
    sq.side = 9;
    return sq;
}
i32 main() {
    weak IShape w = make();
    IShape s = w.lock();
    if (s.data) { return 1; }
    return 0;
}
""", 0),

    ("weak_iface_param_from_class", """
interface IShape {
    i32 area();
}
class Square : IShape {
    i32 side;
    i32 area() { return this.side * this.side; }
}
i32 read(weak IShape w) {
    IShape s = w.lock();
    if (!s.data) { return 0; }
    return s.area();
}
i32 main() {
    Square sq = new Square;
    sq.side = 7;
    return read(sq);
}
""", 49),

    ("weak_iface_param_from_iface", """
interface IShape {
    i32 area();
}
class Square : IShape {
    i32 side;
    i32 area() { return this.side * this.side; }
}
i32 read(weak IShape w) {
    IShape s = w.lock();
    if (!s.data) { return 0; }
    return s.area();
}
i32 main() {
    Square sq = new Square;
    sq.side = 4;
    IShape strong = sq;
    return read(strong);
}
""", 16),

    ("weak_iface_param_owned_dead", """
interface IShape {
    i32 area();
}
class Square : IShape {
    i32 side;
    i32 area() { return this.side * this.side; }
}
IShape make() {
    Square sq = new Square;
    sq.side = 3;
    return sq;
}
i32 read(weak IShape w) {
    IShape s = w.lock();
    if (!s.data) { return 0; }
    return s.area();
}
i32 main() {
    return read(make());
}
""", 0),

    ("weak_iface_assign", """
interface IShape {
    i32 area();
}
class Square : IShape {
    i32 side;
    i32 area() { return this.side * this.side; }
}
i32 main() {
    Square sq = new Square;
    sq.side = 8;
    IShape strong = sq;
    weak IShape w;
    w = strong;
    weak IShape w2 = w;
    IShape s = w2.lock();
    return s.area();
}
""", 64),

    ("weak_iface_as_locked", """
interface IShape {
    i32 area();
}
class Square : IShape {
    i32 side;
    i32 area() { return this.side * this.side; }
}
i32 main() {
    Square sq = new Square;
    sq.side = 5;
    weak IShape w = sq;
    Square p = w.lock() as Square;
    if (p) { return p.area(); }
    return 0;
}
""", 25),

    ("iface_chain_as", """
interface IShape {
    i32 area();
}
class Square : IShape {
    i32 side;
    i32 area() { return this.side * this.side; }
}
IShape make() {
    Square sq = new Square;
    sq.side = 6;
    return sq;
}
i32 main() {
    Square p = make() as Square;
    if (p) { return p.area(); }
    return 0;
}
""", 36),

    ("iface_chain_method", """
interface IShape {
    i32 area();
}
class Square : IShape {
    i32 side;
    i32 area() { return this.side * this.side; }
}
IShape make() {
    Square sq = new Square;
    sq.side = 4;
    return sq;
}
i32 main() {
    return make().area();
}
""", 16),

    ("iface_chain_compare", """
interface IShape {
    i32 area();
}
class Square : IShape {
    i32 side;
    i32 area() { return this.side * this.side; }
}
IShape make(i32 side) {
    Square sq = new Square;
    sq.side = side;
    return sq;
}
i32 main() {
    if (make(3) == make(3)) { return 1; }
    return 0;
}
""", 0),

    ("iface_chain_as_arg", """
interface IShape {
    i32 area();
}
class Square : IShape {
    i32 side;
    i32 area() { return this.side * this.side; }
}
IShape make() {
    Square sq = new Square;
    sq.side = 7;
    return sq;
}
i32 use(Square p) {
    if (p) { return p.area(); }
    return 0;
}
i32 main() {
    return use(make() as Square);
}
""", 49),

    ("weak_iface_chain_as_arg", """
interface IShape {
    i32 area();
}
class Square : IShape {
    i32 side;
    i32 area() { return this.side * this.side; }
}
i32 use(Square p) {
    if (p) { return p.area(); }
    return 0;
}
i32 main() {
    Square sq = new Square;
    sq.side = 7;
    weak IShape w = sq;
    return use(w.lock() as Square);
}
""", 49),

    ("iface_dyn_array_basic", """
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
    IShape[] arr;
    arr.resize(2);
    Square sq = new Square;
    sq.side = 3;
    Circle ci = new Circle;
    ci.radius = 2;
    arr[0] = sq;
    arr[1] = ci;
    return arr[0].area() + arr[1].area();
}
""", 21),

    ("iface_array_replace", """
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
    IShape[] arr;
    arr.resize(1);
    Square sq = new Square;
    sq.side = 2;
    arr[0] = sq;
    Circle ci = new Circle;
    ci.radius = 3;
    arr[0] = ci;
    return arr[0].area();
}
""", 27),

    ("iface_array_bounds", """
interface IShape {
    i32 area();
}
class Square : IShape {
    i32 side;
    i32 area() { return this.side * this.side; }
}
i32 main() {
    IShape[] arr;
    arr.resize(2);
    return arr[5].area();
}
""", "crash"),

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

    ("generics_basic", """
class Box<T> {
    T value;
    void set(T v) { this.value = v; }
    T get() { return this.value; }
}
i32 main() {
    Box<i32> b1 = new Box<i32>;
    b1.set(10);
    Box<Box<i32>> b2 = new Box<Box<i32>>;
    b2.set(b1);
    return b2.get().get();
}
""", 10),

    ("generics_pair", """
class Pair<T, U> {
    T first;
    U second;
    void setFirst(T v) { this.first = v; }
    void setSecond(U v) { this.second = v; }
    T getFirst() { return this.first; }
    U getSecond() { return this.second; }
}
i32 main() {
    Pair<i32, i8> p = new Pair<i32, i8>;
    p.setFirst(7);
    p.setSecond('A');
    return p.getFirst() + p.getSecond();
}
""", 72),

    ("generics_func", """
class Box<T> {
    T value;
    void set(T v) { this.value = v; }
    T get() { return this.value; }
}
i32 doubleBox(Box<i32> b) {
    return b.get() * 2;
}
Box<i32> make(i32 v) {
    Box<i32> b = new Box<i32>;
    b.set(v);
    return b;
}
i32 main() {
    Box<i32> b = make(21);
    return doubleBox(b);
}
""", 42),

    ("generics_nested_arg", """
class Box<T> {
    T value;
    void set(T v) { this.value = v; }
    T get() { return this.value; }
}
class Pair<A, B> {
    A a;
    B b;
}
i32 main() {
    Box<Pair<i32, i8>> bp = new Box<Pair<i32, i8>>;
    Pair<i32, i8> p = new Pair<i32, i8>;
    p.a = 5;
    bp.set(p);
    return bp.get().a;
}
""", 5),

    ("gen_array_field", """
class Bag<T> {
    T[] items;
    void add(T v) { this.items.push(v); }
    T at(i32 i) { return this.items[i]; }
    i32 size() { return this.items.length; }
}
i32 main() {
    Bag<i32> b = new Bag<i32>;
    b.add(10);
    b.add(20);
    b.add(30);
    return b.at(0) + b.at(2) + b.size();
}
""", 43),

    ("gen_map_class", """
import("../lib/Map.my");

i32 main() {
    Map<i32, i32> m = new Map<i32, i32>;
    m.set(1, 100);
    m.set(2, 200);
    m.set(3, 300);
    m.set(2, 222);
    i32 v = 0;
    if (m.get(2, ref v)) {
        print(f"v={v}");
    }
    if (!m.get(9, ref v)) {
        print("miss ok");
    }
    print(f"count={m.count()}");
    if (m.remove(1)) {
        print(f"after remove count={m.count()}");
    }
    if (!m.contains(1) && m.contains(3)) {
        print("contains ok");
    }
    Map<i32, string> sm = new Map<i32, string>;
    sm.set(1, "one");
    sm.set(2, "two");
    sm.set(2, "TWO");
    string sv = "";
    if (sm.get(2, ref sv)) {
        print(sv);
    }
    m.clear();
    print(f"cleared={m.count()}");
    return 0;
}
""", (0, "v=222\nmiss ok\ncount=3\nafter remove count=2\ncontains ok\nTWO\ncleared=0\n")),

    ("map_string_keys", """
import("../lib/Map.my");

i32 main() {
    Map<string, i32> m = new Map<string, i32>;
    m.set("one", 1);
    m.set("two", 2);
    string k = "tw";
    k.append_string("o");
    m.set(k, 22);
    i32 v = 0;
    if (m.get("two", ref v)) {
        print(f"two={v}");
    }
    if (m.contains(k) && m.contains("one")) {
        print("contains ok");
    }
    if (m.remove(k)) {
        print("remove ok");
    }
    if (!m.contains("two") && m.contains("one")) {
        print("post-remove ok");
    }
    return 0;
}
""", (0, "two=22\ncontains ok\nremove ok\npost-remove ok\n")),

    ("map_ihashable_keys", """
class Point : IHashable {
    i32 x;
    i32 y;
    void set(i32 a, i32 b) {
        this.x = a;
        this.y = b;
    }
    u64 hash() {
        return hash(this.x) ^ hash(this.y);
    }
    bool equals(object other) {
        Point p = other as Point;
        if (p == null) {
            return false;
        }
        return this.x == p.x && this.y == p.y;
    }
}
import("../lib/Map.my");
i32 main() {
    Map<Point, i32> pm = new Map<Point, i32>;
    Point p1 = new Point;
    p1.set(1, 2);
    pm.set(p1, 100);
    Point p2 = new Point;
    p2.set(1, 2);
    pm.set(p2, 200);
    if (pm.count() != 1) {
        return 1;
    }
    Point q = new Point;
    q.set(1, 2);
    i32 v = 0;
    if (!pm.get(q, ref v) || v != 200) {
        return 2;
    }
    Point r = new Point;
    r.set(9, 9);
    if (pm.contains(r)) {
        return 3;
    }
    return 7;
}
""", 7),

    ("map_rehash", """
import("../lib/Map.my");
i32 main() {
    Map<i32, i32> m = new Map<i32, i32>;
    i32 i = 0;
    while (i < 200) {
        m.set(i, i * 10);
        i++;
    }
    if (m.count() != 200) {
        return 1;
    }
    i32 v = 0;
    i = 0;
    while (i < 200) {
        if (!m.get(i, ref v) || v != i * 10) {
            return 2;
        }
        i++;
    }
    m.set(50, 999);
    if (!m.get(50, ref v) || v != 999 || m.count() != 200) {
        return 3;
    }
    i = 0;
    while (i < 100) {
        if (!m.remove(i * 2)) {
            return 4;
        }
        i++;
    }
    if (m.count() != 100) {
        return 5;
    }
    i = 0;
    while (i < 100) {
        i32 k = i * 2 + 1;
        if (!m.get(k, ref v) || v != k * 10) {
            return 6;
        }
        if (m.contains(i * 2)) {
            return 7;
        }
        i++;
    }
    if (m.remove(12345)) {
        return 8;
    }
    return 9;
}
""", 9),

    ("gen_new_infer", """
class Box<T> {
    T value;
    void set(T v) { this.value = v; }
    T get() { return this.value; }
}
i32 main() {
    Box<i32> b = new Box;
    b.set(42);
    return b.get();
}
""", 42),

    ("gen_interface", """
interface IPrintable {
    i32 get_value();
}
class Printer<T : IPrintable> {
    i32 print(T item) { return item.get_value(); }
}
class Num : IPrintable {
    i32 n;
    i32 get_value() { return this.n; }
}
i32 main() {
    Printer<Num> p = new Printer<Num>;
    Num n = new Num;
    n.n = 42;
    return p.print(n);
}
""", 42),

    ("gen_new_constraint", """
class Factory<T : new()> {
    T make() {
        T t = new T;
        return t;
    }
}
class Widget {
    i32 value;
}
i32 main() {
    Factory<Widget> f = new Factory<Widget>;
    Widget w = f.make();
    w.value = 42;
    return w.value;
}
""", 42),

    ("gen_new_return_direct", """
class Factory<T : new()> {
    T make() { return new T; }
}
class Widget {
    i32 value;
}
i32 main() {
    Factory<Widget> f = new Factory<Widget>;
    Widget w = f.make();
    w.value = 42;
    return w.value;
}
""", 42),

    ("gen_multi_constraint", """
interface IFoo {
    i32 foo();
}
interface IBar {
    i32 bar();
}
class Multi<T : IFoo, IBar> {
    i32 doBoth(T item) { return item.foo() + item.bar(); }
}
class Thing : IFoo, IBar {
    i32 foo() { return 10; }
    i32 bar() { return 32; }
}
i32 main() {
    Multi<Thing> m = new Multi<Thing>;
    Thing t = new Thing;
    return m.doBoth(t);
}
""", 42),

    ("gen_weak", """
class Box<T> {
    T value;
    void set(T v) { this.value = v; }
    T get() { return this.value; }
}
i32 main() {
    Box<i32> strong = new Box<i32>;
    strong.set(42);
    weak Box<i32> w = strong;
    Box<i32> locked = w.lock();
    if (locked != null) {
        return locked.get();
    }
    return 0;
}
""", 42),

    ("class_field_release", """
class Inner {
    i32 x;
}
class Outer {
    Inner inner;
}
i32 main() {
    Outer o = new Outer;
    o.inner = new Inner;
    o.inner.x = 42;
    return o.inner.x;
}
""", 42),

    ("array_field_release", """
class Holder {
    i32[] nums;
}
i32 main() {
    Holder h = new Holder;
    h.nums.resize(3);
    h.nums[0] = 10;
    h.nums[1] = 20;
    h.nums[2] = 30;
    return h.nums[0] + h.nums[1] + h.nums[2];
}
""", 60),

    ("interface_field", """
interface IVal {
    i32 get();
}
class Box : IVal {
    i32 v;
    i32 get() { return this.v; }
}
class Wrapper {
    IVal val;
}
i32 main() {
    Wrapper w = new Wrapper;
    Box b = new Box;
    b.v = 7;
    w.val = b;
    return w.val.get();
}
""", 7),

    ("weak_interface_field", """
interface IVal {
    i32 get();
}
class Box : IVal {
    i32 v;
    i32 get() { return this.v; }
}
class Wrapper {
    weak IVal val;
}
i32 main() {
    Wrapper w = new Wrapper;
    Box b = new Box;
    b.v = 9;
    w.val = b;
    IVal locked = w.val.lock();
    return locked.get();
}
""", 9),

    ("weak_class_field_release", """
class Node {
    i32 v;
}
class Holder {
    weak Node n;
}
i32 main() {
    Holder h = new Holder;
    Node x = new Node;
    x.v = 5;
    h.n = x;
    Node locked = h.n.lock();
    return locked.v;
}
""", 5),

    ("native_add", """
class Math {
    native i32 add(i32 a, i32 b);
}
i32 main() {
    Math m = new Math;
    return m.add(10, 20);
}
""", 30, """
int32_t Math_add(Math* thiz, int32_t a, int32_t b) {
    return a + b;
}
"""),

    ("fstring_basic", """
i32 main() {
    i32 n = 42;
    string name = "world";
    print(f"hello {name}, n={n}");
    return 0;
}
""", (0, "hello world, n=42\n")),

    ("fstring_types", """
i32 main() {
    i8 c = 'X';
    i32 n = -123;
    print(f"{c}:{n}");
    return 0;
}
""", (0, "X:-123\n")),

    ("fstring_arg", """
i32 main() {
    i32 n = 7;
    print(f"n={n}");
    return 0;
}
""", (0, "n=7\n")),

    ("fstring_escape_dbl_braces", """
i32 main() {
    i32 n = 42;
    print(f"{{{n}}}");
    print(f"a{{b}}c");
    print(f"}}");
    print(f"lone }} brace");
    return 0;
}
""", (0, "{42}\na{b}c\n}\nlone } brace\n")),

    ("fstring_escape_bs_braces", """
i32 main() {
    i32 n = 42;
    print(f"\\{n\\}={n}");
    print(f"a\\{b");
    print(f"b\\}c");
    print(f"\\\\{{");
    return 0;
}
""", (0, "{n}=42\na{b\nb}c\n\\{\n")),

    ("string_escape_braces", """
i32 main() {
    print("x\\{y\\}z");
    print("{{not collapsed}}");
    return 0;
}
""", (0, "x{y}z\n{{not collapsed}}\n")),

    ("string_append", """
i32 main() {
    string s = "ab";
    s.append_string("cd");
    s.append_i32(12);
    s.append_char('X');
    print(s);
    return 0;
}
""", (0, "abcd12X\n")),

    ("string_equals_method", """
i32 main() {
    string a = "hello";
    string b = "hello";
    string c = "world";
    if (!a.equals(b)) { return 1; }
    if (a.equals(c)) { return 2; }
    if (!a.equals(a)) { return 3; }
    string empty1;
    string empty2;
    if (!empty1.equals(empty2)) { return 4; }
    return 0;
}
""", 0),

    ("fstring_tostring", """
class Point : IToString {
    i32 x;
    i32 y;
    string toString() {
        string s = "(";
        s.append_i32(this.x);
        s.append_string(", ");
        s.append_i32(this.y);
        s.append_string(")");
        return s;
    }
}
Point make_point() {
    Point p = new Point;
    p.x = 9;
    p.y = 8;
    return p;
}
i32 main() {
    Point p = new Point;
    p.x = 3;
    p.y = 4;
    print(f"p={p}");
    IToString it = p;
    print(f"it={it}");
    print(f"m={make_point()}");
    print(f"s={p.toString()}");
    return 0;
}
""", (0, "p=(3, 4)\nit=(3, 4)\nm=(9, 8)\ns=(3, 4)\n")),

    ("float_literal_arith", """
f32 main() {
    f32 x = 1.5f;
    f32 y = 2.5f;
    return x + y;
}
""", 4),

    ("float_literal_fstring", """
i32 main() {
    f32 x = 1.5f;
    f64 y = 2.5;
    print(f"{x},{y}");
    return 0;
}
""", (0, "1.5,2.5\n")),

    ("print_builtin", """
i32 main() {
    print("hello");
    print(f"world {42}");
    return 0;
}
""", (0, "hello\nworld 42\n")),

    ("assert_pass", """
i32 main() {
    i32 x = 5;
    assert(x == 5);
    assert(x);
    assert(true);
    return 0;
}
""", 0),

    ("assert_fail", """
i32 main() {
    assert(1 == 2);
    return 0;
}
""", "crash"),

    ("assert_in_func", """
i32 check(i32 x) {
    assert(x > 0);
    return x;
}
i32 main() {
    check(5);
    check(0);
    return 0;
}
""", "crash"),

    ("hash_basic", """
class Node {
    i32 v;
}
i32 main() {
    u64 h1 = hash(42);
    u64 h2 = hash(42);
    if (h1 != h2) {
        return 1;
    }
    if (h1 == hash(43)) {
        return 2;
    }
    string a = "hello";
    string b = "hel";
    b.append_string("lo");
    if (hash(a) != hash(b)) {
        return 3;
    }
    if (hash("x") == hash("y")) {
        return 4;
    }
    f64 f = 3.14;
    if (hash(f) != hash(3.14)) {
        return 5;
    }
    Node n = new Node;
    if (hash(n) != hash(n)) {
        return 6;
    }
    bool bo = true;
    u64 hb = hash(bo);
    i64 neg = -1;
    u64 hn = hash(neg);
    if (hb == hn) {
        return 8;
    }
    return 7;
}
""", 7),

    ("str_eq_value", """
i32 main() {
    string a = "hel";
    a.append_string("lo");
    string b = "hello";
    if (a != b) {
        return 1;
    }
    string e1 = "";
    string e2 = "";
    if (!(e1 == e2)) {
        return 2;
    }
    string nn = null;
    if (nn == b || b == nn) {
        return 3;
    }
    if (!(nn == null) || !(b != null)) {
        return 4;
    }
    return 5;
}
""", 5),

    ("equals_builtin", """
class Plain {
    i32 v;
}
i32 main() {
    if (!equals(42, 42) || equals(42, 43)) {
        return 1;
    }
    if (!equals(1.5, 1.5) || equals(1.5, 2.5)) {
        return 2;
    }
    string a = "hel";
    a.append_string("lo");
    if (!equals(a, "hello") || equals(a, "hellp")) {
        return 3;
    }
    if (equals(a, null) || equals(null, a)) {
        return 4;
    }
    if (!equals(null, null)) {
        return 5;
    }
    Plain p1 = new Plain;
    Plain p2 = new Plain;
    if (!equals(p1, p1) || equals(p1, p2)) {
        return 6;
    }
    return 7;
}
""", 7),

    ("ihashable_dispatch", """
class Point : IHashable {
    i32 x;
    i32 y;
    void set(i32 a, i32 b) {
        this.x = a;
        this.y = b;
    }
    u64 hash() {
        return hash(this.x) ^ hash(this.y);
    }
    bool equals(object other) {
        Point p = other as Point;
        if (p == null) {
            return false;
        }
        return this.x == p.x && this.y == p.y;
    }
}
i32 main() {
    Point q1 = new Point;
    q1.set(1, 2);
    Point q2 = new Point;
    q2.set(1, 2);
    Point q3 = new Point;
    q3.set(3, 4);
    if (hash(q1) != hash(q2)) {
        return 1;
    }
    if (hash(q1) == hash(q3)) {
        return 2;
    }
    if (!equals(q1, q2) || equals(q1, q3)) {
        return 3;
    }
    if (equals(q1, null)) {
        return 4;
    }
    if (q1 == q2) {
        return 5;
    }
    return 7;
}
""", 7),


    ("match_iface_type", """
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
i32 classify(IShape s) {
    match (s) {
        Square sq => { return sq.side; }
        Circle ci => { return ci.radius; }
        else => { return 0; }
    }
    return -1;
}
i32 main() {
    Square sq = new Square;
    sq.side = 5;
    IShape s = sq;
    return classify(s);
}
""", 5),

    ("match_int", """
i32 main() {
    i32 x = 2;
    match (x) {
        1 => { return 10; }
        2 => { return 20; }
        else => { return 0; }
    }
    return -1;
}
""", 20),

    ("match_iface_else", """
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
    sq.side = 7;
    IShape s = sq;
    match (s) {
        Circle ci => { return 99; }
        else => { return 42; }
    }
    return -1;
}
""", 42),

    ("match_iface_call", """
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
i32 classify(IShape s) {
    match (s) {
        Square sq => { return sq.side; }
        Circle ci => { return ci.radius; }
        else => { return 0; }
    }
    return -1;
}
IShape makeSquare(i32 side) {
    Square sq = new Square;
    sq.side = side;
    return sq;
}
i32 main() {
    return classify(makeSquare(4));
}
""", 4),

    ("match_int_in_loop", """
i32 main() {
    i32 s = 0;
    i32 i = 0;
    while (i < 5) {
        match (i) {
            1 => { s = s + 1; }
            2 => { s = s + 10; }
            else => { s = s + 100; }
        }
        i = i + 1;
    }
    return s;
}
""", 311),

    ("bool_basic", """
i32 main() {
    bool b = true;
    if (b) {
        bool c = false;
        if (c) { return 0; }
        return 1;
    }
    return 2;
}
""", 1),

    ("bool_ops", """
i32 main() {
    bool a = 3 > 2;
    bool b = !false;
    bool c = a && b;
    bool d = false || c;
    bool e = 1 == 1 && 2 != 3;
    if (c && d && e) { return 7; }
    return 0;
}
""", 7),

    ("bool_relational", """
i32 main() {
    i32 x = 5;
    bool a = x <= 5;
    bool b = x >= 6;
    bool c = x < 10;
    if (a && !b && c) { return 3; }
    return 0;
}
""", 3),

    ("bool_param_return", """
bool flip(bool x) { return !x; }
bool id(bool x) { return x; }
i32 main() {
    if (flip(true)) { return 0; }
    if (!id(true)) { return 0; }
    if (flip(false) && id(false) == false) { return 5; }
    return 1;
}
""", 5),

    ("bool_field", """
class Flag {
    bool on;
}
struct Pair {
    bool a;
    bool b;
}
i32 main() {
    Flag f = new Flag;
    f.on = true;
    Pair p;
    p.a = true;
    p.b = false;
    if (f.on && p.a && !p.b) { return 4; }
    return 0;
}
""", 4),

    ("bool_array", """
i32 main() {
    bool[] a;
    a.push(true);
    a.push(false);
    a.push(1 < 2);
    if (a[0] && !a[1] && a[2]) { return a.length; }
    return 0;
}
""", 3),

    ("bool_fstring", """
i32 main() {
    bool t = true;
    string s = f"{t}-{false}-{(1 < 2)}";
    return s.bytes.length;
}
""", 15),

    ("null_class", """
class Node {
    i32 v;
}
i32 main() {
    Node n = null;
    if (n != null) { return 0; }
    n = new Node;
    n.v = 5;
    if (n == null) { return 0; }
    n = null;
    if (n == null) { return 7; }
    return 1;
}
""", 7),

    ("null_string", """
i32 main() {
    string s = null;
    if (s == null) { s = "abc"; }
    if (s != null) { return s.bytes.length; }
    return 0;
}
""", 3),

    ("null_iface", """
interface IShape {
    i32 area();
}
class Square : IShape {
    i32 side;
    i32 area() { return this.side * this.side; }
}
IShape make(bool want) {
    if (want) {
        Square sq = new Square;
        sq.side = 3;
        return sq;
    }
    return null;
}
i32 main() {
    IShape a = make(false);
    if (a != null) { return 0; }
    IShape b = make(true);
    if (b == null) { return 0; }
    b = null;
    if (b == null) { return 9; }
    return 2;
}
""", 9),

    ("null_weak", """
class Node {
    i32 v;
}
i32 main() {
    weak Node w = null;
    if (w == null) {
        Node n = new Node;
        w = n;
        if (w.lock() == null) { return 0; }
        w = null;
        if (w == null) { return 5; }
        return 1;
    }
    return 0;
}
""", 5),

    ("null_weak_iface", """
interface IBox {
    i32 get();
}
class Box : IBox {
    i32 get() { return 42; }
}
i32 main() {
    weak IBox w = null;
    Box b = new Box;
    w = b;
    IBox s = w.lock();
    if (s == null) { return 0; }
    w = null;
    return s.get();
}
""", 42),

    ("null_arg", """
class Node {
    i32 v;
}
i32 check(Node n) {
    if (n == null) { return 1; }
    return n.v;
}
i32 main() {
    i32 a = check(null);
    Node n = new Node;
    n.v = 2;
    return a + check(n);
}
""", 3),

    ("null_field", """
class Node {
    i32 v;
}
class Holder {
    Node child;
}
i32 main() {
    Holder h = new Holder;
    h.child = new Node;
    h.child.v = 3;
    h.child = null;
    if (h.child == null) { return 6; }
    return 0;
}
""", 6),

    ("null_return_class", """
class Node {
    i32 v;
}
Node make(bool want) {
    if (want) { return new Node; }
    return null;
}
i32 main() {
    Node a = make(false);
    if (a != null) { return 0; }
    Node b = make(true);
    if (b == null) { return 0; }
    return 8;
}
""", 8),

    ("private_field", """
class Counter {
    private i32 value;
    void set(i32 v) { this.value = v; }
    i32 get() { return this.value; }
    void copy_from(Counter other) { this.value = other.value; }
}
i32 main() {
    Counter a = new Counter;
    a.set(5);
    Counter b = new Counter;
    b.copy_from(a);
    return b.get();
}
""", 5),

    ("private_method", """
class Engine {
    private i32 mix(i32 a, i32 b) { return a * 10 + b; }
    i32 run() { return this.mix(3, 7); }
}
i32 main() {
    Engine e = new Engine;
    return e.run();
}
""", 37),

    ("private_public_default", """
class P {
    public i32 x;
    i32 y;
    i32 sum() { return this.x + this.y; }
}
i32 main() {
    P p = new P;
    p.x = 3;
    p.y = 4;
    return p.sum();
}
""", 7),

    ("private_generic", """
class Box<T> {
    private T v;
    void set(T x) { this.v = x; }
    T get() { return this.v; }
}
i32 main() {
    Box<i32> b = new Box<i32>;
    b.set(9);
    return b.get();
}
""", 9),

    ("object_basic", """
class Node {
    i32 v;
}
class Other {
    i32 w;
}
i32 main() {
    Node n = new Node;
    n.v = 5;
    object o = n;
    if (o == null) { return 0; }
    Node m = o as Node;
    if (m == null) { return 0; }
    if (m.v != 5) { return 0; }
    Other x = o as Other;
    if (x != null) { return 0; }
    return 7;
}
""", 7),

    ("object_iface", """
interface IShape {
    i32 area();
}
class Square : IShape {
    i32 side;
    i32 area() { return this.side * this.side; }
}
i32 main() {
    Square sq = new Square;
    sq.side = 4;
    IShape s = sq;
    object o = s;
    Square back = o as Square;
    if (back == null) { return 0; }
    return back.area();
}
""", 16),

    ("object_null", """
class Node {
    i32 v;
}
i32 main() {
    object o = null;
    if (o != null) { return 0; }
    Node n = new Node;
    o = n;
    if (o == null) { return 0; }
    o = null;
    if (o == null) { return 3; }
    return 1;
}
""", 3),

    ("object_string", """
i32 main() {
    string s = "abc";
    object o = s;
    String back = o as String;
    if (back == null) { return 0; }
    return back.bytes.length;
}
""", 3),

    ("object_arg_return", """
class Node {
    i32 v;
}
object wrap(Node n) {
    return n;
}
Node unwrap(object o) {
    return o as Node;
}
i32 main() {
    Node n = new Node;
    n.v = 11;
    object o = wrap(n);
    Node m = unwrap(o);
    if (m == null) { return 0; }
    return m.v;
}
""", 11),

    ("object_field", """
class Node {
    i32 v;
}
class Holder {
    object item;
}
i32 main() {
    Holder h = new Holder;
    Node n = new Node;
    n.v = 6;
    h.item = n;
    Node m = h.item as Node;
    if (m == null) { return 0; }
    h.item = null;
    if (h.item != null) { return 0; }
    return m.v;
}
""", 6),

    ("object_array", """
class Node {
    i32 v;
}
interface IBox {
    i32 get();
}
class Box : IBox {
    i32 get() { return 9; }
}
i32 main() {
    object[] arr;
    Node n = new Node;
    n.v = 1;
    arr.push(n);
    Box b = new Box;
    IBox ib = b;
    arr.push(ib);
    arr.push(null);
    Node r = arr[0] as Node;
    if (r == null) { return 0; }
    Box rb = arr[1] as Box;
    if (rb == null) { return 0; }
    if (arr[2] != null) { return 0; }
    arr[0] = null;
    if (arr[0] != null) { return 0; }
    return r.v + rb.get() + arr.length;
}
""", 13),

    ("object_match", """
class A {
    i32 x;
}
class B {
    i32 y;
}
i32 classify(object o) {
    match (o) {
        A a => { return 1; }
        B b => { return 2; }
        else => { return 3; }
    }
    return 0;
}
i32 main() {
    A a = new A;
    B b = new B;
    return classify(a) * 100 + classify(b) * 10 + classify(null);
}
""", 123),

    ("bit_ops", """
i32 main() {
    i32 a = 12;
    i32 b = 10;
    i32 r = (a & b) * 100 + (a | b) * 10 + (a ^ b);
    return r;
}
""", 946),

    ("bit_not", """
i32 main() {
    i32 x = 0;
    u8 y = 0;
    i32 a = ~x;
    i32 b = ~5;
    u8 c = ~y;
    if (a != -1) { return 0; }
    if (b != -6) { return 0; }
    if (c != 255) { return 0; }
    return 9;
}
""", 9),

    ("bit_shift", """
i32 main() {
    i32 a = 1 << 4;
    i32 b = 256 >> 3;
    u32 c = 1 << 31;
    c = c >> 31;
    i32 d = -16;
    d = d >> 2;
    if (a != 16 || b != 32) { return 0; }
    if (c != 1) { return 0; }
    if (d != -4) { return 0; }
    return a + b + c + d;
}
""", 45),

    ("bit_precedence", """
i32 main() {
    i32 x = 1 | 2 & 3 ^ 4;
    if (x != 7) { return 1; }
    i32 y = 1 << 2 + 1;
    if (y != 8) { return 2; }
    i32 z = 4 + 2 << 1;
    if (z != 12) { return 3; }
    i32 w = 3 & 1 | 4 ^ 6;
    if (w != 3) { return 4; }
    return 7;
}
""", 7),

    ("bit_compound", """
i32 main() {
    i32 a = 12;
    a &= 10;
    a |= 5;
    a ^= 6;
    a <<= 2;
    a >>= 3;
    return a;
}
""", 5),

    ("bit_types", """
i32 main() {
    u8 a = 255;
    a = a & 15;
    u16 c = 65535;
    c = c ^ 255;
    i64 b = 1 << 20;
    b = b << 20;
    b = b >> 40;
    i32 r = 0;
    if (a == 15) { r = r + 1; }
    if (c == 65280) { r = r + 2; }
    if (b == 1) { r = r + 4; }
    return r;
}
""", 7),

    ("const_local", """
i32 main() {
    const u32 x = 7;
    const i32 y = x * 2 + 1;
    u32 z = x + y;
    if (z != 22) { return 0; }
    return z;
}
""", 22),

    ("const_param", """
i32 twice(const i32 v) {
    return v + v;
}
class Calc {
    i32 base;
    i32 add(const i32 d) { return this.base + d; }
}
i32 main() {
    Calc c = new Calc;
    c.base = 10;
    const u32 k = 5;
    return twice(k) + c.add(3);
}
""", 23),

    ("const_bool_float", """
i32 main() {
    const f64 pi = 3.5;
    const bool flag = true;
    f64 area = pi * 4;
    if (flag && area == 14.0) { return 8; }
    return 0;
}
""", 8),

    ("elseif_chain", """
i32 classify(i32 x) {
    if (x < 0) {
        return 1;
    } else if (x == 0) {
        return 2;
    } else if (x < 10) {
        return 3;
    } else {
        return 4;
    }
    return 0;
}
i32 main() {
    return classify(-5) + classify(0) * 10 + classify(7) * 100 + classify(99) * 1000;
}
""", 4321),

    ("default_params_func", """
i32 add3(i32 a, i32 b = 10, i32 c = 100) {
    return a + b + c;
}
i32 main() {
    i32 full = add3(1, 2, 3);      // 6
    i32 partial = add3(1, 2);      // 103
    i32 minimal = add3(1);         // 111
    if (full != 6) { return 1; }
    if (partial != 103) { return 2; }
    if (minimal != 111) { return 3; }
    return full + partial + minimal;
}
""", 220),

    ("default_params_class_method", """
class Acc {
    i32 total;
    void add(i32 v, i32 mul = 2) { this.total = this.total + v * mul; }
    i32 get() { return this.total; }
}
i32 main() {
    Acc a = new Acc;
    a.add(5);        // +10
    a.add(5, 3);     // +15
    return a.get();
}
""", 25),

    ("default_params_struct_method", """
struct Counter {
    i32 n;
    void bump(i32 by = 1) { this.n = this.n + by; }
}
i32 main() {
    Counter c;
    c.n = 0;
    c.bump();
    c.bump();
    c.bump(10);
    return c.n;
}
""", 12),

    ("default_params_string", """
void greet(string name, string greeting = "hello") {
    print(f"{greeting} {name}");
}
i32 main() {
    greet("bob");
    greet("bob", "hi");
    greet("ann");
    return 0;
}
""", (0, "hello bob\nhi bob\nhello ann\n")),

    ("default_params_null", """
class Node {
    i32 v;
}
i32 unwrap(Node n = null, i32 fallback = 42) {
    if (n == null) { return fallback; }
    return n.v;
}
i32 main() {
    Node x = new Node;
    x.v = 7;
    i32 a = unwrap();          // 42 (null default)
    i32 b = unwrap(x);         // 7
    i32 c = unwrap(null, 5);   // 5
    if (a != 42) { return 1; }
    if (b != 7) { return 2; }
    if (c != 5) { return 3; }
    return a + b + c;
}
""", 54),

    ("default_params_all_optional", """
i32 defaults(i32 a = 1, bool b = true, i32 c = 3) {
    if (!b) { return 0; }
    return a + c;
}
i32 main() {
    return defaults();
}
""", 4),

    ("default_params_generic", """
class Box<T> {
    T value;
    void set(T v, i32 tag = 7) {
        this.value = v;
    }
    T get() { return this.value; }
    i32 label(i32 suffix = 3) { return suffix; }
}
i32 main() {
    Box<i32> b = new Box<i32>;
    b.set(10);
    Box<string> s = new Box<string>;
    s.set("x", 1);
    return b.get() + b.label() + b.label(1);
}
""", 14),

    ("default_params_interface", """
interface IGreeter {
    string name(i32 id = 5);
}
class Greeter : IGreeter {
    string name(i32 id = 9) {
        return f"g{id}";
    }
}
i32 main() {
    Greeter g = new Greeter;
    IGreeter ig = g;
    print(ig.name());
    print(g.name());
    print(ig.name(1));
    return 0;
}
""", (0, "g5\ng9\ng1\n")),

    ("default_params_arity_still_works", """
i32 f(i32 a, i32 b = 2) {
    return a + b;
}
i32 main() {
    return f(40);
}
""", 42),

    ("static_method_factory", """
class Point {
    i32 x;
    i32 y;
    static Point create(i32 x, i32 y) {
        Point p = new Point;
        p.x = x;
        p.y = y;
        return p;
    }
    i32 sum() { return this.x + this.y; }
}
i32 main() {
    Point p = Point.create(1, 2);
    if (p.x != 1) { return 1; }
    if (p.y != 2) { return 2; }
    if (p.sum() != 3) { return 3; }
    return p.sum() + 39;
}
""", 42),

    ("static_method_default_params", """
class Math {
    static i32 add(i32 a, i32 b = 10, i32 c = 100) {
        return a + b + c;
    }
}
i32 main() {
    i32 full = Math.add(1, 2, 3);   // 6
    i32 partial = Math.add(1, 2);   // 103
    i32 minimal = Math.add(1);      // 111
    if (full != 6) { return 1; }
    if (partial != 103) { return 2; }
    if (minimal != 111) { return 3; }
    return full + partial + minimal;
}
""", 220),

    ("static_method_access_modifiers", """
class Counter {
    static i32 make(i32 v) { return v * 2; }
    public static i32 pub(i32 v) { return v + 1; }
    private static i32 priv(i32 v) { return v - 1; }
    static i32 use_priv(i32 v) { return Counter.priv(v); }
}
i32 main() {
    i32 a = Counter.make(10);     // 20
    i32 b = Counter.pub(10);      // 11
    i32 c = Counter.use_priv(11); // 10
    if (a != 20) { return 1; }
    if (b != 11) { return 2; }
    if (c != 10) { return 3; }
    return a + b + c;
}
""", 41),

    ("static_method_calls_static", """
class A {
    static i32 one() { return 1; }
    static i32 two() { return A.one() + 1; }
}
i32 main() {
    return A.two() + 40;
}
""", 42),


]

# ============================================================
# NEGATIVE TEST CASES: (name, source, expected_error_substring)
# mylang.exe compilation is expected to fail.
# ============================================================

NEGATIVE_TESTS = [
    ("assert_arg_count", """
i32 main() {
    assert();
    return 0;
}
""", "assert expects 1 argument"),

    ("bad_hex_no_digits", """
i32 main() {
    i32 x = 0x;
    return x;
}
""", "expected hexadecimal digits after '0x'"),

    ("bad_class_too_many_fields", "class Big {\n" + "".join(
        f"    i32 field{i};\n" for i in range(33)) + "}\ni32 main() {\n    return 0;\n}\n",
     "too many fields in class 'Big'"),

    ("bad_struct_too_many_fields", "struct BigS {\n" + "".join(
        f"    i32 field{i};\n" for i in range(33)) + "}\ni32 main() {\n    return 0;\n}\n",
     "too many fields in struct 'BigS'"),

    ("bad_iface_too_many_methods", "interface IBig {\n" + "".join(
        f"    i32 m{i}();\n" for i in range(33)) + "}\ni32 main() {\n    return 0;\n}\n",
     "too many methods in interface 'IBig'"),

    ("bad_struct_method_rvalue", """
struct Vec2 {
    i32 x;
    void bump() { this.x = this.x + 1; }
}
Vec2 make() {
    Vec2 v;
    v.x = 1;
    return v;
}
i32 main() {
    make().bump();
    return 0;
}
""", "struct method receiver must be a variable, field, or array element"),

    ("bad_struct_native_method", """
struct S {
    i32 x;
    native void foo();
}
i32 main() {
    return 0;
}
""", "native methods are not supported in structs"),

    ("bad_struct_method_missing", """
struct S {
    i32 x;
}
i32 main() {
    S s;
    s.nope();
    return 0;
}
""", "method 'S.nope' does not exist"),

    ("bad_struct_ref_array_local", """
class Node {
    i32 v;
}
struct Box {
    Node n;
}
i32 main() {
    Box[] a;
    a.resize(1);
    return 0;
}
""", "arrays of struct 'Box' with reference fields are not supported yet"),

    ("bad_struct_ref_array_field", """
class Node {
    i32 v;
}
struct Box {
    Node n;
}
class Holder {
    Box[] a;
}
i32 main() {
    return 0;
}
""", "arrays of struct 'Box' with reference fields are not supported yet"),

    ("bad_struct_ref_array_param", """
class Node {
    i32 v;
}
struct Box {
    Node n;
}
void fill(ref Box[] a) {
    a.resize(1);
}
i32 main() {
    return 0;
}
""", "arrays of struct 'Box' with reference fields are not supported yet"),

    ("bad_import_missing", """
import("no_such_import_file_xyz.my");

i32 main() {
    return 0;
}
""", "cannot open imported file"),

    ("bad_import_main_in_imported", """
import("import_dup_main.my");

i32 main() {
    return 0;
}
""", "must not define 'main'", [
    ("import_dup_main.my", """
i32 helper() {
    return 1;
}
i32 main() {
    return helper();
}
"""),
]),

    ("bad_new_interface", """
interface IShape {
    i32 area();
}
class Square : IShape {
    i32 area() { return 1; }
}
i32 main() {
    IShape s = new IShape;
    return 0;
}
""", "cannot create instance of interface"),

    ("bad_assign_to_iface", """
interface IShape {
    i32 area();
}
i32 main() {
    IShape s = 123;
    return 0;
}
""", "cannot initialize interface"),

    ("bad_as_to_iface", """
interface IShape {
    i32 area();
}
class Square : IShape {
    i32 area() { return 1; }
}
i32 main() {
    Square sq = new Square;
    IShape s = sq;
    IShape s2 = s as IShape;
    return 0;
}
""", "'as' target must be a class type"),

    ("bad_missing_interface_method", """
interface IShape {
    i32 area();
}
class Square : IShape {
    i32 side;
}
i32 main() {
    return 0;
}
""", "does not implement"),

    ("bad_default_method_this", """
interface IShape {
    i32 area();
    i32 bad() { return this.side; }
}
class Circle : IShape {
    i32 radius;
    i32 area() { return 3 * this.radius * this.radius; }
}
i32 main() {
    return 0;
}
""", "'this' is not allowed in interface default method"),

    ("bad_override_no_iface", """
interface IShape {
    i32 area();
}
class Square : IShape {
    i32 side;
    i32 area() { return this.side * this.side; }
    override i32 volume() { return 0; }
}
i32 main() {
    return 0;
}
""", "is marked 'override' but does not override any interface method"),

    ("bad_iface_method_conflict", """
interface IFoo {
    i32 compute(i32 x);
}
interface IBar {
    i32 compute(i32 x, i32 y);
}
class Worker : IFoo, IBar {
    i32 compute(i32 x) { return x; }
}
i32 main() {
    return 0;
}
""", "conflicting signatures"),

    ("gen_bad_method", """
class Bad<T> {
    i32 doBad(T item) { return item.getValue(); }
}
class Thing {
    i32 value;
}
i32 main() {
    return 0;
}
""", "no interface constraint providing"),

    ("gen_new_missing_args", """
class Box<T> {
    T value;
}
i32 main() {
    Box<i32> b = new Box<i32>;
    b = new Box;
    return 0;
}
""", "requires 1 type argument(s)"),

    ("hash_bad_struct", """
struct P {
    i32 x;
}
i32 main() {
    P p;
    u64 h = hash(p);
    return 0;
}
""", "cannot hash values of this type"),

    ("hash_bad_weak", """
class Node {
    i32 v;
}
i32 main() {
    Node n = new Node;
    weak Node w = n;
    u64 h = hash(w);
    return 0;
}
""", "cannot hash a weak/unowned reference"),

    ("equals_bad_struct", """
struct P {
    i32 x;
}
i32 main() {
    P a;
    P b;
    bool r = equals(a, b);
    return 0;
}
""", "cannot compare values of this type with equals"),

    ("equals_bad_weak", """
class Node {
    i32 v;
}
i32 main() {
    Node n = new Node;
    weak Node w = n;
    bool r = equals(w, w);
    return 0;
}
""", "cannot compare weak/unowned references with equals"),

    ("equals_bad_mixed", """
i32 main() {
    bool r = equals(42, "42");
    return 0;
}
""", "cannot compare 'i32' with 'String'"),

    ("ihashable_bad_impl", """
class Lazy : IHashable {
    i32 v;
}
i32 main() {
    return 0;
}
""", "does not implement 'IHashable"),

    ("ref_missing_keyword", """
void inc(ref i32 x) {
    x = x + 1;
}
i32 main() {
    i32 a = 0;
    inc(a);
    return a;
}
""", "missing 'ref' keyword"),

    ("bad_new_array", """
i32 main() {
    i32[] arr = new i32[3];
    return 0;
}
""", "arrays are created empty"),

    ("bad_compound_class", """
class Counter {
    i32 n;
}
i32 main() {
    Counter c = new Counter;
    c += c;
    return 0;
}
""", "compound assignment not supported"),

    ("bad_inc_class", """
class Counter {
    i32 n;
}
i32 main() {
    Counter c = new Counter;
    c++;
    return 0;
}
""", "increment/decrement not supported"),

    ("bad_inc_expr", """
i32 main() {
    i32 x = 0;
    i32 y = x++;
    return y;
}
""", "increment/decrement not allowed"),

    ("bad_inc_condition", """
i32 main() {
    i32 x = 0;
    if (x++) {
        return 1;
    }
    return 0;
}
""", "increment/decrement not allowed"),

    ("bad_for_condition", """
i32 main() {
    for (i32 i = 0; i++; i < 5) {
        return 0;
    }
    return 0;
}
""", "increment/decrement not allowed"),

    ("bad_native_body", """
class C {
    native i32 f() { return 1; }
}
i32 main() {
    return 0;
}
""", "expected ';'"),

    ("bad_native_semi", """
class C {
    native i32 f()
}
i32 main() {
    return 0;
}
""", "expected ';'"),

    ("bad_fstring_type", """
class Foo {
    i32 v;
}
i32 main() {
    Foo f = new Foo;
    string s = f"{f}";
    return 0;
}
""", "cannot interpolate type 'Foo'"),

    ("bad_break_outside_loop", """
i32 main() {
    break;
    return 0;
}
""", "'break' outside of loop"),

    ("bad_continue_outside_loop", """
i32 main() {
    continue;
    return 0;
}
""", "'continue' outside of loop"),

    ("bad_match_not_impl", """
interface IShape {
    i32 area();
}
class Square : IShape {
    i32 side;
    i32 area() { return this.side * this.side; }
}
class Circle {
    i32 radius;
}
i32 main() {
    Square sq = new Square;
    IShape s = sq;
    match (s) {
        Circle ci => { return ci.radius; }
        else => { return 0; }
    }
    return -1;
}
""", "does not implement interface"),

    ("bad_match_int_on_iface", """
interface IShape {
    i32 area();
}
class Square : IShape {
    i32 side;
    i32 area() { return this.side * this.side; }
}
i32 main() {
    Square sq = new Square;
    IShape s = sq;
    match (s) {
        1 => { return 1; }
        else => { return 0; }
    }
    return -1;
}
""", "integer match pattern cannot match"),

    ("too_many_generic_params", """
class Nine<A, B, C, D, E, F, G, H, I> {
}
i32 main() {
    return 0;
}
""", "too many generic parameters"),

    ("too_many_generic_params_constraint", """
interface IFoo {
    void foo();
}
class Nine<A, B, C, D, E, F, G, H, I : IFoo> {
}
i32 main() {
    return 0;
}
""", "too many generic parameters"),

    ("bad_unowned_lock", """
class Node {
    i32 v;
}
i32 main() {
    Node obj = new Node;
    unowned Node u = obj;
    Node s = u.lock();
    return 0;
}
""", "unowned references do not have lock()"),

    ("bad_unowned_interface", """
interface IShape {
    i32 area();
}
i32 main() {
    unowned IShape s;
    return 0;
}
""", "unowned requires a class type"),

    ("bad_unowned_primitive", """
i32 main() {
    unowned i32 x;
    return 0;
}
""", "unowned requires a class type"),

    ("bad_unowned_noinit", """
class Node {
    i32 v;
}
i32 main() {
    unowned Node u;
    return 0;
}
""", "unowned variable 'u' requires an initializer"),

    ("bad_unowned_new", """
class Node {
    i32 v;
}
i32 main() {
    unowned Node u = new unowned Node;
    return 0;
}
""", "'new' cannot be used with unowned"),

    ("bad_unowned_array", """
class Node {
    i32 v;
}
i32 main() {
    unowned Node[] a;
    return 0;
}
""", "unowned arrays are not supported"),

    ("bad_unowned_return", """
class Node {
    i32 v;
}
unowned Node f(Node n) {
    return n;
}
i32 main() {
    return 0;
}
""", "unowned return type is not supported"),

    ("bad_unowned_as", """
interface IShape {
    i32 area();
}
class Square : IShape {
    i32 area() { return 1; }
}
i32 main() {
    Square sq = new Square;
    IShape s = sq;
    Square x = s as unowned Square;
    return 0;
}
""", "unowned is not a valid cast target"),

    ("bad_unowned_match", """
class Node {
    i32 v;
}
i32 main() {
    Node obj = new Node;
    unowned Node u = obj;
    match (u) {
        else => { return 0; }
    }
    return 1;
}
""", "cannot match on an unowned reference"),

    ("bad_null_int", """
i32 main() {
    i32 x = null;
    return x;
}
""", "cannot initialize 'i32' with 'null'"),

    ("bad_null_bool", """
i32 main() {
    bool b = null;
    return 0;
}
""", "cannot initialize 'bool' with 'null'"),

    ("bad_null_struct", """
struct Point {
    i32 x;
}
i32 main() {
    Point p = null;
    return 0;
}
""", "cannot initialize 'Point' with 'null'"),

    ("bad_bool_from_int", """
i32 main() {
    bool b = 5;
    return 0;
}
""", "cannot initialize 'bool' with 'i32'"),

    ("bad_int_from_bool", """
i32 main() {
    i32 x = true;
    return x;
}
""", "cannot initialize 'i32' with 'bool'"),

    ("bad_int_from_compare", """
i32 main() {
    i32 x = 1 < 2;
    return x;
}
""", "cannot initialize 'i32' with 'bool'"),

    ("bad_bool_assign", """
i32 main() {
    bool b = true;
    b = 0;
    return 0;
}
""", "cannot assign 'i32' to 'bool'"),

    ("bad_bool_compound", """
i32 main() {
    bool b = true;
    b += 1;
    return 0;
}
""", "compound assignment not supported for this type"),

    ("bad_bool_inc", """
i32 main() {
    bool b = true;
    b++;
    return 0;
}
""", "increment/decrement not supported for this type"),

    ("bad_null_unowned", """
class Node {
    i32 v;
}
i32 main() {
    Node n = new Node;
    unowned Node u = null;
    return 0;
}
""", "cannot initialize unowned 'Node' with 'null'"),

    ("bad_null_relational", """
class Node {
    i32 v;
}
i32 main() {
    Node n = new Node;
    if (n < null) { return 1; }
    return 0;
}
""", "operator '<' not allowed with null"),

    ("bad_null_arg", """
void foo(i32 x) { }
i32 main() {
    foo(null);
    return 0;
}
""", "cannot pass null to 'i32' parameter"),

    ("bad_null_assign_int", """
i32 main() {
    i32 x = 0;
    x = null;
    return x;
}
""", "cannot assign 'null' to 'i32'"),

    ("bad_bool_arg", """
void foo(bool b) { }
i32 main() {
    foo(5);
    return 0;
}
""", "cannot pass 'i32' to 'bool' parameter"),

    ("bad_bool_return", """
i32 f() {
    return true;
}
i32 main() {
    return f();
}
""", "cannot return 'bool' from function returning 'i32'"),

    ("bad_null_member", """
i32 main() {
    return null.x;
}
""", "cannot access member 'x' on null"),

    ("bad_null_assign_unowned", """
class Node {
    i32 v;
}
i32 main() {
    Node n = new Node;
    unowned Node u = n;
    u = null;
    return 0;
}
""", "cannot assign null to unowned reference"),

    ("bad_private_field_read", """
class C {
    private i32 x;
}
i32 main() {
    C c = new C;
    return c.x;
}
""", "cannot access private field 'C.x'"),

    ("bad_private_field_write", """
class C {
    private i32 x;
}
i32 main() {
    C c = new C;
    c.x = 1;
    return 0;
}
""", "cannot access private field 'C.x'"),

    ("bad_private_method", """
class C {
    private i32 m() { return 1; }
}
i32 main() {
    C c = new C;
    return c.m();
}
""", "cannot call private method 'C.m'"),

    ("bad_private_other_class", """
class A {
    private i32 x;
}
class B {
    i32 peek(A a) { return a.x; }
}
i32 main() {
    return 0;
}
""", "cannot access private field 'A.x'"),

    ("bad_private_iface_impl", """
interface I {
    i32 m();
}
class C : I {
    private i32 m() { return 1; }
}
i32 main() {
    return 0;
}
""", "implements interface 'I' but is private"),

    ("bad_private_override", """
interface I {
    i32 m();
}
class C : I {
    private override i32 m() { return 1; }
}
i32 main() {
    return 0;
}
""", "cannot be both private and override"),

    ("bad_private_both", """
class C {
    public private i32 x;
}
i32 main() {
    return 0;
}
""", "duplicate or conflicting access modifier"),

    ("bad_private_in_struct", """
struct S {
    private i32 x;
}
i32 main() {
    return 0;
}
""", "access modifiers are not allowed in structs"),

    ("bad_private_in_iface", """
interface I {
    private i32 m();
}
i32 main() {
    return 0;
}
""", "access modifiers are not allowed in interfaces"),

    ("bad_object_primitive", """
i32 main() {
    object o = 5;
    return 0;
}
""", "cannot initialize 'object' with 'i32'"),

    ("bad_object_member", """
class Node {
    i32 v;
}
i32 main() {
    Node n = new Node;
    object o = n;
    return o.v;
}
""", "cannot access member 'v' on object"),

    ("bad_object_method", """
class Node {
    i32 foo() { return 1; }
}
i32 main() {
    Node n = new Node;
    object o = n;
    return o.foo();
}
""", "cannot access member 'foo' on object"),

    ("bad_object_as_iface", """
interface IShape {
    i32 area();
}
class Node {
    i32 v;
}
i32 main() {
    Node n = new Node;
    object o = n;
    IShape s = o as IShape;
    return 0;
}
""", "'as' target must be a class type"),

    ("bad_object_weak", """
class Node {
    i32 v;
}
i32 main() {
    Node n = new Node;
    weak object w = n;
    return 0;
}
""", "weak requires a class or interface type"),

    ("bad_object_new", """
i32 main() {
    object o = new object;
    return 0;
}
""", "cannot use 'new' on 'object'"),

    ("bad_object_arg_primitive", """
void foo(object o) { }
i32 main() {
    foo(5);
    return 0;
}
""", "cannot pass 'i32' to 'object' parameter"),

    ("bad_struct_recursive", """
struct A {
    A a;
}
i32 main() {
    return 0;
}
""", "recursively contains itself"),

    ("bad_struct_cycle", """
struct A {
    B b;
}
struct B {
    A a;
}
i32 main() {
    return 0;
}
""", "recursively contains itself"),

    ("bad_object_to_class", """
class Node {
    i32 v;
}
i32 main() {
    Node n = new Node;
    object o = n;
    Node m = o;
    return 0;
}
""", "cast with 'as' first"),

    ("bad_bit_float", """
i32 main() {
    f64 x = 1.5;
    i32 y = x & 2;
    return y;
}
""", "operator '&' requires integer operands"),

    ("bad_bit_not_float", """
i32 main() {
    f32 x = 1.5;
    i32 y = ~x;
    return y;
}
""", "operator '~' requires an integer operand"),

    ("bad_bit_bool", """
i32 main() {
    bool b = true & false;
    return 0;
}
""", "operator '&' requires integer operands"),

    ("bad_bit_class", """
class N {
    i32 v;
}
i32 main() {
    N a = new N;
    N b = new N;
    i32 x = a & b;
    return x;
}
""", "operator '&' requires integer operands"),

    ("bad_bit_compound_float", """
i32 main() {
    f32 f = 3.5;
    f &= 1;
    return 0;
}
""", "compound assignment not supported for this type"),

    ("bad_bit_compound_bool", """
i32 main() {
    bool b = true;
    b &= true;
    return 0;
}
""", "compound assignment not supported for this type"),

    ("bad_const_assign", """
i32 main() {
    const u32 x = 5;
    x = 2;
    return 0;
}
""", "cannot assign to const variable 'x'"),

    ("bad_const_compound", """
i32 main() {
    const i32 x = 5;
    x += 1;
    return 0;
}
""", "cannot assign to const variable 'x'"),

    ("bad_const_inc", """
i32 main() {
    const i32 x = 5;
    x++;
    return 0;
}
""", "cannot modify const variable 'x'"),

    ("bad_const_noinit", """
i32 main() {
    const u32 x;
    return 0;
}
""", "const variable 'x' requires an initializer"),

    ("bad_const_ref", """
void inc(ref i32 v) { v = v + 1; }
i32 main() {
    const i32 x = 5;
    inc(ref x);
    return 0;
}
""", "cannot pass const variable 'x' to ref parameter"),

    ("bad_const_class", """
class Node {
    i32 v;
}
i32 main() {
    const Node n = new Node;
    return 0;
}
""", "const is only supported on primitive value types"),

    ("bad_const_param_assign", """
i32 f(const i32 v) {
    v = 10;
    return v;
}
i32 main() {
    return f(1);
}
""", "cannot assign to const variable 'v'"),

    ("bad_const_field", """
class C {
    const i32 x;
}
i32 main() {
    return 0;
}
""", "const fields are not supported"),

    ("bad_const_ref_const", """
i32 f(ref const u32 x) {
    return 0;
}
i32 main() {
    return 0;
}
""", "ref parameters cannot be const"),

    ("bad_if_no_brace", """
i32 main() {
    i32 x = 2;
    if (x > 1) return 1;
    return 0;
}
""", "expected '{' for if body"),

    ("bad_else_no_brace", """
i32 main() {
    i32 x = 2;
    if (x > 1) { x = 3; } else return 4;
    return 0;
}
""", "expected '{' for else body"),

    ("bad_while_no_brace", """
i32 main() {
    i32 x = 3;
    while (x > 0) x = x - 1;
    return x;
}
""", "expected '{' for while body"),

    ("bad_for_no_brace", """
i32 main() {
    i32 s = 0;
    for (i32 i = 0; i < 3; i = i + 1) s = s + 1;
    return s;
}
""", "expected '{' for for body"),

    ("bad_default_not_literal", """
i32 f(i32 x = y) {
    return x;
}
i32 main() {
    return 0;
}
""", "default parameter value must be a literal"),

    ("bad_default_trailing", """
i32 f(i32 a = 1, i32 b) {
    return a + b;
}
i32 main() {
    return 0;
}
""", "parameter 'b' must have a default value because a previous parameter has one"),

    ("bad_default_ref_param", """
void f(ref i32 x = 1) {
}
i32 main() {
    return 0;
}
""", "ref parameters cannot have default values"),

    ("bad_default_bool_for_int", """
i32 f(i32 x = true) {
    return x;
}
i32 main() {
    return 0;
}
""", "default value for parameter 'x' does not match the parameter type"),

    ("bad_default_string_for_int", """
i32 f(i32 x = "s") {
    return x;
}
i32 main() {
    return 0;
}
""", "default value for parameter 'x' does not match the parameter type"),

    ("bad_default_null_for_int", """
i32 f(i32 x = null) {
    return x;
}
i32 main() {
    return 0;
}
""", "default value for parameter 'x' does not match the parameter type"),

    ("bad_default_int_for_bool", """
bool f(bool x = 1) {
    return x;
}
i32 main() {
    return 0;
}
""", "default value for parameter 'x' does not match the parameter type"),

    ("bad_call_too_few_args", """
i32 f(i32 a, i32 b) {
    return a + b;
}
i32 main() {
    return f(1);
}
""", "too few arguments for 'f' (expected at least 2, got 1)"),

    ("bad_call_too_many_args", """
i32 f(i32 a, i32 b = 2) {
    return a + b;
}
i32 main() {
    return f(1, 2, 3);
}
""", "too many arguments for 'f' (expected 2, got 3)"),

    ("bad_method_call_too_few_args", """
class C {
    i32 v;
    i32 sum(i32 a, i32 b) { return a + b; }
}
i32 main() {
    C c = new C;
    return c.sum(1);
}
""", "too few arguments for 'C.sum' (expected at least 2, got 1)"),

    ("bad_default_method_not_literal", """
class C {
    i32 v;
    i32 f(i32 x = this.v) { return x; }
}
i32 main() {
    return 0;
}
""", "default parameter value must be a literal"),

    ("bad_static_this", """
class C {
    i32 v;
    static i32 f() { return this.v; }
}
i32 main() {
    return 0;
}
""", "'this' cannot be used in a static method"),

    ("bad_static_field_access", """
class C {
    i32 v;
    static i32 f() { return v; }
}
i32 main() {
    return 0;
}
""", "unknown identifier 'v'"),

    ("bad_static_via_instance", """
class C {
    static i32 f() { return 1; }
}
i32 main() {
    C c = new C;
    return c.f();
}
""", "cannot call static method 'C.f' via an instance; use the class name"),

    ("bad_instance_via_class", """
class C {
    i32 v;
    i32 get() { return this.v; }
}
i32 main() {
    return C.get();
}
""", "cannot call instance method 'C.get' via the class name; use an instance"),

    ("bad_static_method_missing", """
class C {
    static i32 f() { return 1; }
}
i32 main() {
    return C.nope();
}
""", "method 'C.nope' does not exist"),

    ("bad_static_field", """
class C {
    static i32 v;
}
i32 main() {
    return 0;
}
""", "static fields are not supported"),

    ("bad_static_private_outside", """
class C {
    private static i32 f() { return 1; }
}
i32 main() {
    return C.f();
}
""", "cannot call private method 'C.f'"),

    ("bad_static_native", """
class C {
    native static i32 f();
}
i32 main() {
    return 0;
}
""", "cannot be both static and native"),

    ("bad_array_member", """
i32 main() {
    i32[] arr;
    arr.push(1);
    u64 n = arr.lenght;
    return 0;
}
""", "array has no member 'lenght'"),

    ("bad_class_field", """
class Foo { i32 x; }
i32 main() {
    Foo f = new Foo;
    return f.xyzzy;
}
""", "class 'Foo' has no field 'xyzzy'"),

    ("bad_struct_field", """
struct Pt { i32 x; i32 y; }
i32 main() {
    Pt p;
    return p.zzz;
}
""", "struct 'Pt' has no field 'zzz'"),

]

# ============================================================
# RUNNER
# ============================================================

def run_negative_test(idx, name, source, expected_substring, extra_files=None):
    testdir = os.path.join(SCRIPT_DIR, "test")
    os.makedirs(testdir, exist_ok=True)

    my_file = os.path.join(testdir, f"_n{idx}.my")

    with open(my_file, "w", encoding="utf-8") as f:
        f.write(source.strip() + "\n")

    if extra_files:
        for ef_name, ef_src in extra_files:
            with open(os.path.join(testdir, ef_name), "w", encoding="utf-8") as f:
                f.write(ef_src.strip() + "\n")

    r = shell(f'"{MYLANG_EXE}" {my_file} nul', cwd=SCRIPT_DIR)
    if r.returncode == 0:
        return False, "mylang exited 0, expected compile failure"

    if expected_substring and expected_substring not in (r.stderr or ""):
        snippet = (r.stderr or "")[-200:]
        return False, f"mylang failed but did not report '{expected_substring}'\n  {snippet}"

    return True, f"mylang exit {r.returncode}"


def run_test(idx, name, source, expected, asan_dll_dir, leak_check=False, native_c=None, extra_files=None):
    testdir = os.path.join(SCRIPT_DIR, "test")
    os.makedirs(testdir, exist_ok=True)
    exedir = os.path.join(SCRIPT_DIR, "build", "test")
    os.makedirs(exedir, exist_ok=True)

    my_file = os.path.join(testdir, f"_t{idx}.my")
    c_file  = os.path.join(testdir, f"_t{idx}.c")
    exe_file = os.path.join(exedir, f"_t{idx}.exe")

    with open(my_file, "w", encoding="utf-8") as f:
        f.write(source.strip() + "\n")

    # Additional source files (e.g. import targets), written next to the test.
    if extra_files:
        for ef_name, ef_src in extra_files:
            with open(os.path.join(testdir, ef_name), "w", encoding="utf-8") as f:
                f.write(ef_src.strip() + "\n")

    # Compile .my -> .c
    leak_arg = " --leak-check" if leak_check else ""
    r = shell(f'"{MYLANG_EXE}"{leak_arg} {my_file} {c_file}', cwd=SCRIPT_DIR)
    if r.returncode != 0:
        # mylang might crash with ASan detection - capture exit code
        return False, f"mylang exit {r.returncode}: {r.stderr[-200:] if r.stderr else ''}"

    # Compile .c -> .exe
    extra_sources = None
    if native_c:
        native_file = os.path.join(testdir, f"_t{idx}_native.c")
        with open(native_file, "w", encoding="utf-8") as f:
            f.write('#include "runtime.h"\n')
            f.write(f'#include "_t{idx}.h"\n')
            f.write(native_c.strip() + "\n")
        extra_sources = [native_file]

    if not compile_c(c_file, exe_file, extra_sources):
        return False, "C compile error"

    # Run
    try:
        exit_code, out, err = run_exe(exe_file, asan_dll_dir)
    except subprocess.TimeoutExpired:
        return False, "timeout"
    output = out + err

    if isinstance(expected, tuple) and expected and expected[0] == "crash_contains":
        # Crash expected, and the panic output must mention the given text
        # (e.g. a file:line from an imported module).
        if exit_code == 0:
            return False, "exit 0, expected crash"
        if expected[1] not in output:
            snippet = output[-300:] if len(output) > 300 else output
            return False, f"crashed but output missing {expected[1]!r}\n  {snippet}"
        return True, f"exited {exit_code & 0xFFFFFFFF} (crash, matched {expected[1]!r})"

    if expected == "crash":
        # Any non-zero exit means the program was terminated (breakpoint/ASan)
        if exit_code != 0:
            return True, f"exited {exit_code & 0xFFFFFFFF} (crash)"
        else:
            return False, f"exit 0, expected crash"

    # Tuple (exit_code, stdout) or plain string (= (0, stdout)): exact stdout match.
    # stderr is ignored for matching (CRT leak dump / MyLang leak report / ASan).
    if isinstance(expected, tuple) or (isinstance(expected, str) and expected != "crash"):
        want_exit, want_out = expected if isinstance(expected, tuple) else (0, expected)
        if exit_code != want_exit:
            snippet = output[-300:] if len(output) > 300 else output
            return False, f"exit {exit_code}, expected {want_exit}\n  {snippet}"
        if out != want_out:
            e = repr(want_out)
            a = repr(out)
            if len(e) > 300: e = e[:300] + "..."
            if len(a) > 300: a = a[:300] + "..."
            return False, f"stdout mismatch\n  expected: {e}\n  actual:   {a}"
        if (TEST_MODE == "debug" or leak_check) and output:
            return True, f"exit {exit_code}, stdout ok\n{output}"
        return True, f"exit {exit_code}, stdout ok"

    if exit_code != expected:
        snippet = output[-300:] if len(output) > 300 else output
        return False, f"exit {exit_code}, expected {expected}\n  {snippet}"

    if (TEST_MODE == "debug" or leak_check) and output:
        return True, f"exit {exit_code}\n{output}"
    return True, f"exit {exit_code}"


def run_suite(leak_check, filters):
    asan_dll_dir = ""
    if TEST_MODE == "release":
        asan_dll_dir = ensure_asan_dll()
        if not asan_dll_dir:
            print("WARNING: running without ASan - tests may still pass")

    passed = 0
    failed = 0
    total = 0

    for i, t in enumerate(TESTS):
        if len(t) == 3:
            name, source, expected = t
            native_c = None
            extra_files = None
        elif len(t) == 4:
            name, source, expected, native_c = t
            extra_files = None
        else:
            name, source, expected, native_c, extra_files = t
        if filters:
            name_lower = name.lower()
            if not any(k in name_lower for k in filters):
                continue
        total += 1
        ok, msg = run_test(i, name, source, expected, asan_dll_dir, leak_check, native_c, extra_files)
        status = "PASS" if ok else "FAIL"
        print(f"[{status}] {name:30s} {msg}")
        if ok:
            passed += 1
        else:
            failed += 1

    for i, t in enumerate(NEGATIVE_TESTS):
        if len(t) == 3:
            name, source, expected = t
            extra_files = None
        else:
            name, source, expected, extra_files = t
        if filters:
            name_lower = name.lower()
            if not any(k in name_lower for k in filters):
                continue
        total += 1
        ok, msg = run_negative_test(i, name, source, expected, extra_files)
        status = "PASS" if ok else "FAIL"
        print(f"[{status}] {name:30s} {msg}")
        if ok:
            passed += 1
        else:
            failed += 1

    if filters:
        all_tests = len(TESTS) + len(NEGATIVE_TESTS)
        print(f"\n{passed}/{total} passed, {failed} failed  (filtered from {all_tests} tests)")
    else:
        print(f"\n{passed}/{total} passed, {failed} failed")
    return failed


def main():
    global TEST_MODE

    parser = argparse.ArgumentParser()
    parser.add_argument("--mode", choices=["release", "debug"], default=None,
                        help="release = ASan + release CRT, debug = no ASan + debug CRT (default: run both)")
    parser.add_argument("--leak-check", action="store_true",
                        help="pass --leak-check to mylang.exe to enable memory leak tracking")
    parser.add_argument("filters", nargs="*", default=None,
                        help="optional test name keywords to filter (case-insensitive)")
    args = parser.parse_args()
    leak_check = args.leak_check
    filters = [f.lower() for f in args.filters] if args.filters else None

    modes = [args.mode] if args.mode else ["release", "debug"]
    total_failed = 0
    for mode in modes:
        TEST_MODE = mode
        if len(modes) > 1:
            print(f"===== mode: {mode} =====")
        # The compiler flags differ per mode (ASan vs debug CRT), so rebuild.
        print(f"Building mylang.exe ({mode})...")
        if not compile_mylang():
            sys.exit(1)
        if not compile_pch_and_runtime():
            sys.exit(1)
        total_failed += run_suite(leak_check, filters)
    sys.exit(0 if total_failed == 0 else 1)

if __name__ == "__main__":
    main()
