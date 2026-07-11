#!/usr/bin/env python3
"""MyLang test runner. Compiles .my sources, runs them, verifies results."""

import subprocess
import sys
import os
import tempfile
import shutil

SCRIPT_DIR = os.path.dirname(os.path.abspath(__file__))
MYLANG_EXE = os.path.join(SCRIPT_DIR, "mylang.exe")
VSPATH = r"C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat"

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
    """Copy ASan DLL to project root so executables can find it."""
    dll_path = find_asan_dll()
    if not dll_path:
        print("WARNING: ASan DLL not found, ASan may not activate")
        return ""
    dest = os.path.join(SCRIPT_DIR, os.path.basename(dll_path))
    if not os.path.exists(dest):
        shutil.copy2(dll_path, dest)
    return os.path.dirname(dll_path)  # return dir for PATH

def shell(args, **kw):
    return subprocess.run(args, capture_output=True, text=True, shell=True, **kw)

def find_python():
    for name in ["python.exe", "python3.exe"]:
        p = shutil.which(name)
        if p:
            return p
    return sys.executable

def compile_mylang():
    """Build mylang.exe with MSVC + ASan."""
    srcs = "src\\token.c src\\ast.c src\\lexer.c src\\symtab.c src\\parser.c src\\codegen.c src\\main.c"
    cmd = f'call "{VSPATH}" >nul 2>&1 && cl /nologo /std:c11 /fsanitize=address /Zi /W3 /Fe:mylang.exe {srcs}'
    r = shell(cmd, cwd=SCRIPT_DIR)
    if r.returncode != 0:
        print("FAIL: mylang compilation failed")
        print(r.stdout)
        print(r.stderr)
        return False
    return True

def compile_c(src, exe):
    """Compile generated C code with MSVC + ASan."""
    cmd = f'call "{VSPATH}" >nul 2>&1 && cl /nologo /std:c11 /fsanitize=address /Zi /Fe:{exe} {src}'
    r = shell(cmd, cwd=SCRIPT_DIR)
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
int main() {
    int x = 10;
    int y = x + 5;
    if (y == 15) {
        return 100;
    }
    return 0;
}
""", 100),

    ("while_sum", """
int main() {
    int i = 1;
    int s = 0;
    while (i <= 10) {
        s = s + i;
        i = i + 1;
    }
    return s;
}
""", 55),

    ("if_else", """
int main() {
    int x = 3;
    if (x > 5) {
        return 1;
    } else {
        return 42;
    }
}
""", 42),

    ("new_class", """
class Point {
    int x;
    int y;
}
int main() {
    Point p = new Point;
    p.x = 10;
    p.y = 20;
    return p.x + p.y;
}
""", 30),

    ("class_pass_to_func", """
class Point {
    int x;
    int y;
}
int sum(Point a, Point b) {
    return a.x + b.x + a.y + b.y;
}
int main() {
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
    int x;
    int y;
}
Point make(int a, int b) {
    Point p = new Point;
    p.x = a;
    p.y = b;
    return p;
}
int main() {
    Point p = make(7, 3);
    return p.x + p.y;
}
""", 10),

    ("class_assign", """
class Point {
    int x;
    int y;
}
int main() {
    Point p = new Point;
    p.x = 10;
    Point q = p;
    q.x = 20;
    return p.x + q.x;
}
""", 40),

    ("bounds_dynamic_oob", """
int main() {
    int[] arr = new int[3];
    arr[0] = 1;
    arr[5] = 0;
    return 0;
}
""", "crash"),

    ("bounds_fixed_oob", """
int main() {
    int[3] arr;
    arr[0] = 1;
    arr[10] = 0;
    return 0;
}
""", "crash"),

    ("bounds_in_bounds", """
int main() {
    int[] arr = new int[10];
    arr[5] = 42;
    return arr[5];
}
""", 42),

    ("neg_and_not", """
int main() {
    int x = 5;
    int y = -x;
    if (y != -5) return 0;
    if (!(x == 4)) {
        return 10;
    }
    return 0;
}
""", 10),

    ("compare_ops", """
int main() {
    int a = 3;
    int b = 5;
    int r = 0;
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
int mul(int a, int b) { return a * b; }
int add(int a, int b) { return a + b; }
int main() {
    return add(mul(2, 3), mul(4, 5));
}
""", 26),

    ("char_literal", """
int main() {
    char c = 'A';
    int x = 65;
    if (c == x) return 42;
    return 0;
}
""", 42),

    ("new_array_expr_size", """
int main() {
    int n = 5;
    int[] arr = new int[n + 2];
    arr[6] = 99;
    return arr[6];
}
""", 99),

    ("nested_if_while", """
int main() {
    int x = 0;
    int i = 0;
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
int id(int x) { return x; }
class Counter {
    int value;
    int get() { return this.value; }
    void set(int v) { this.value = v; }
    int inc() { this.value = this.value + 1; return this.value; }
}
int main() {
    Counter c = new Counter;
    c.set(10);
    int a = c.get();
    int b = c.inc();
    return id(a + b);
}
""", 21),

    ("method_thiz_alias", """
class Box {
    int x;
    Box set(int v) {
        this.x = v;
        return this;
    }
    int get() { return this.x; }
}
int main() {
    Box b = new Box;
    b.set(7);
    Box c = b.set(3);
    return b.get() + c.get();
}
""", 6),

    ("method_multi_param", """
class Point {
    int x;
    int y;
    void move(int dx, int dy) {
        this.x = this.x + dx;
        this.y = this.y + dy;
    }
}
int main() {
    Point p = new Point;
    p.x = 1;
    p.y = 2;
    p.move(10, 20);
    return p.x + p.y;
}
""", 33),

    ("method_void_no_return", """
class Log {
    int total;
    void add(int v) { this.total = this.total + v; }
}
int main() {
    Log l = new Log;
    l.add(3);
    l.add(4);
    return l.total;
}
""", 7),

    ("method_return_captured", """
class Box {
    int v;
    Box set(int x) { this.v = x; return this; }
}
int main() {
    Box a = new Box;
    Box b = a.set(5);
    Box c = a.set(7);
    return b.v + c.v;
}
""", 14),

    ("method_return_discarded", """
class Box {
    int v;
    int total;
    Box set(int x) {
        this.v = x;
        return this;
    }
    Box incTotal() {
        this.total = this.total + 1;
        return this;
    }
}
int main() {
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
    int v;
    Box set(int x) { this.v = x; return this; }
}
int main() {
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
    int v;
    int get() { return this.v; }
}
int main() {
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
    int v;
}
int sum(Box[] arr) {
    return arr[0].v + arr[1].v;
}
int main() {
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
    int v;
}
Box[] make() {
    Box[] a = new Box[2];
    a[0] = new Box;
    a[0].v = 5;
    a[1] = new Box;
    a[1].v = 7;
    return a;
}
int main() {
    Box[] x = make();
    return x[0].v + x[1].v;
}
""", 12),

    ("class_array_bounds", """
class Box {
    int v;
}
int main() {
    Box[] a = new Box[2];
    return a[5].v;
}
""", "crash"),

    ("class_array_replace", """
class Box {
    int v;
}
int main() {
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
    int v;
}
int main() {
    Box[] a = new Box[4];
    int i = 0;
    while (i < 4) {
        a[i] = new Box;
        a[i].v = i;
        i = i + 1;
    }
    int s = 0;
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
    int v;
    int get() { return this.v; }
}
int main() {
    Box[] a = new Box[2];
    a[0] = new Box;
    a[0].v = 5;
    return a[0].get();
}
""", 5),
]

# ============================================================
# RUNNER
# ============================================================

def run_test(idx, name, source, expected, asan_dll_dir):
    testdir = os.path.join(SCRIPT_DIR, "test")
    os.makedirs(testdir, exist_ok=True)

    my_file = os.path.join(testdir, f"_t{idx}.my")
    c_file  = os.path.join(testdir, f"_t{idx}.c")
    exe_file = os.path.join(testdir, f"_t{idx}.exe")

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
    if not os.path.exists(MYLANG_EXE):
        print("Building mylang.exe...")
        if not compile_mylang():
            sys.exit(1)

    asan_dll_dir = ensure_asan_dll()
    if not asan_dll_dir:
        print("WARNING: running without ASan - tests may still pass")

    total = len(TESTS)
    passed = 0
    failed = 0

    for i, (name, source, expected) in enumerate(TESTS):
        ok, msg = run_test(i, name, source, expected, asan_dll_dir)
        status = "PASS" if ok else "FAIL"
        print(f"[{status}] {name:30s} {msg}")
        if ok:
            passed += 1
        else:
            failed += 1

    print(f"\n{passed}/{total} passed, {failed} failed")
    sys.exit(0 if failed == 0 else 1)

if __name__ == "__main__":
    main()
