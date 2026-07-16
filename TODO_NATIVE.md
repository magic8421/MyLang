# TODO: method-level `native` (Java-style) + generated header

## 1. 目标

支持方法级 `native`，和 Java 一样：

```mylang
class File {
    void* handle;

    native bool open(u8[] path);
    native i32  read(ref u8[] buf);
    native void close();
}
```

- 方法只有声明，没有 `{}` 体，以 `;` 结束。
- MyLang 编译器生成 `.h`（类型定义 + 函数原型）。
- 用户写 `.c` 实现这些函数，直接调用平台 API。
- 生成 `.c` 里不再重复写 typedef/原型，而是 `#include "<out>.h"`。

**第一阶段只支持类方法 `native`，不支持顶层 `native` 函数、不支持 `native class` 语法糖。**

---

## 2. 生成产物

命令：

```bash
mylang app.my app.c
```

会同时生成：

- `app.c` —— 普通 MyLang 代码的 C 实现。
- `app.h` —— 模块接口头文件（总是生成，为以后 `import` 打基础）。

头文件路径规则：

- 若 `out_path` 以 `.c` 结尾，把 `.c` 换成 `.h`。
- 否则在末尾加 `.h`。

例如 `build/app.c` → `build/app.h`。

---

## 3. `app.h` 里放什么

按顺序写入：

```c
#ifndef MYLANG_APP_H
#define MYLANG_APP_H

#include "runtime.h"

#ifdef __cplusplus
extern "C" {
#endif

/* 1. 前置声明，允许互相引用 */
typedef struct File File;
typedef struct IFoo IFoo;

/* 2. interface 的 vtable / fat pointer / weak pointer 定义 */
typedef struct IFooVTable { ... } IFooVTable;
typedef struct IFoo { ... } IFoo;
typedef struct WeakIFoo { ... } WeakIFoo;

/* 3. class / struct 完整定义 */
struct File {
    void* handle;
};

/* 4. 类型 ID 常量 */
#define MYLANG_TID_File 17

/* 5. destructor 原型（供 C 实现创建对象时使用） */
void _mylang_dtor_File(File* p);

/* 6. 公共函数原型：顶层函数 + 类方法（含 native） */
bool File_open(File* thiz, MyArray path);
int32_t File_read(File* thiz, MyArray* buf);
void File_close(File* thiz);
int32_t _my_main(void);

#ifdef __cplusplus
}
#endif

#endif
```

### 不放什么

- `static` 内部辅助函数：destructor 定义、vtable 常量、weakify/lock 辅助函数。
- `main` 包装器。

### destructor  externalize

当前 destructor 是：

```c
static void _mylang_dtor_File(File* p) { ... }
```

为了 C 实现里能拿到函数指针传给 `mylang_new_object`，要去掉 `static`，并在 `.h` 里声明。

### interface vtable

保持 `static const`，继续放在 `.c` 里。第一阶段允许 native 类实现 interface，但 vtable 引用的方法符号必须由用户 `.c` 提供，否则链接报错。

---

## 4. `app.c` 里放什么

```c
#define _CRTDBG_MAP_ALLOC
#define MYLANG_LEAK_CHECK   /* 若启用 --leak-check */
#include "app.h"
#include <stdio.h>
#include <stdlib.h>
...

/* static 辅助函数、destructor、方法体、vtables、顶层函数、main 包装器 */
```

注意：

- `app.c` 必须 `#include "app.h"`，且 `MYLANG_LEAK_CHECK` 要在 include 之前定义。
- `codegen_class_decl`、`codegen_struct_decl` 里**不再 emit typedef**。
- `codegen_interface_typedefs` 拆成两部：typedef 进 `.h`，lock/weakify 辅助函数留在 `.c`。

---

## 5. ABI 约定

native 方法的 C 签名和 MyLang 生成的普通方法完全一致：

| MyLang | C | 说明 |
|---|---|---|
| `class Foo` 方法 | `RetType Foo_method(Foo* thiz, ...)` | 首参数固定是 `Foo*` |
| 返回 `ClassName*` / 接口 | 被调用方必须 `mylang_retain` 后返回 | 遵循 callee-retains-at-return |
| 参数 `ClassName* x` | `ClassName* x` | 调用方不 retain；想保存需自调 retain |
| `ref T x` | `T* x` |  |
| `weak ClassName w` | `WeakRef* w` |  |
| `InterfaceName i` | 对应 fat pointer struct（按值传） |  |
| `T[] a` | `MyArray a` | 按值传 struct 拷贝 |
| 返回数组 | 禁止 | 现有规则 |

native 实现创建 MyLang 对象的示例：

```c
File* f = (File*)mylang_new_object(sizeof(File), MYLANG_TID_File, _mylang_dtor_File);
f->handle = CreateFileA(...);
return f;   /* 已是 +1，符合 callee-retains */
```

---

## 6. 调用点：暂不做自动 `MY_PUSH`/`MY_POP`

native 方法没有 MyLang 生成的函数体，无法像普通方法那样在函数体开头/结尾做 `MY_PUSH`/`MY_POP`。

如果要在调用点自动补 push/pop，会遇到 native 调用嵌套在表达式里（如 `x = f.read(buf)`）时无法插入语句的问题。

**第一阶段方案**：不做自动 wrap。native 代码里若调用 `my_panic`，stack trace 可能只显示到调用者。如果用户需要精确帧，可以在自己的 C 实现里手动：

```c
MY_PUSH("File.read", "app.my", 45);
/* ... */
MY_POP();
```

后续可考虑为每个 native 方法生成一个带 `line` 参数的静态 wrapper，这里先不实现。

---

## 7. 逐文件改动

### `src/token.h` / `src/token.c`

- 加 `TOK_KW_NATIVE`。
- `token_kind_name` 返回 `"native"`。

### `src/lexer.c`

- `keywords[]` 加 `{ "native", TOK_KW_NATIVE }`。

### `src/ast.h`

- `struct AstNode` 加 `int ast_is_native;`。

### `src/symtab.h`

- `MethodInfo` 加 `int is_native;`。
- 新增 `FuncInfo* symtab_first_func(void);`（用于遍历顶层函数列表）。

### `src/symtab.c`

- `symtab_add_method` 增加 `int is_native` 参数，写入 `MethodInfo`。
- 实现 `symtab_first_func`。
- 更新 `symtab_add_method` 的调用者（仅 `parser.c`）。

### `src/parser.c`

在 `parse_class_decl` 的方法解析循环里：

```c
int is_native = 0;
if (check(p, TOK_KW_NATIVE)) {
    is_native = 1;
    advance(p);
}
Type ft = parse_type(p);
...
if (is_native) {
    expect(p, TOK_SEMI);
    /* body 用 NULL / 空 AST_BLOCK 占位 */
} else {
    AstNode* mbody = parse_stmt(p);
}
...
symtab_add_method(info, fname.text, ft, mc, mpn, mpt, is_native);
mnode->ast_is_native = is_native;
```

### `src/codegen.h`

```c
void codegen_program(AstNode* program, FILE* out, FILE* header,
                     const char* source_file, int leak_check,
                     const char* header_include_name);
```

### `src/codegen.c`

主要改动：

1. `CodegenContext` 加 `FILE* header; const char* header_include_name;`。
2. 新增 header 输出函数：
   - `emit_header_preamble`
   - `emit_header_forward_decls`
   - `emit_interface_header_typedefs`
   - `emit_class_struct_defs_to_header`
   - `emit_struct_defs_to_header`
   - `emit_header_type_ids`
   - `emit_header_destructor_prototypes`
   - `emit_header_function_prototypes`
   - `emit_header_method_prototypes`
   - `emit_header_postamble`
3. `codegen_program` 流程改为：
   - `preinstantiate_generic_types`
   - 完整生成 `.h`
   - 生成 `.c` 前言（含 `#include "app.h"`）
   - emit `.c` 内容
4. `codegen_class_decl`：删除 struct typedef 输出，只输出 destructor + 方法 + vtable。
5. `codegen_struct_decl`：删除 struct typedef 输出（或变为空）。
6. `codegen_interface_typedefs`：拆成 header typedef + `.c` helper 两部分。
7. `codegen_class_destructor`：去掉 `static`。
8. `codegen_method_decl`：先向 header emit 原型；若 `is_native` 直接返回，不向 `.c` emit 函数体。
9. `codegen_func_decl`：向 header emit 原型（`main` 除外），再向 `.c` emit 定义。

### `src/main.c`

- 从 `out_path` 推导 `header_path` 和 `header_include_name`（basename）。
- 打开 header，传给 `codegen_program`。
- 打开失败时报错退出。

### `test_runner.py`

- `compile_c(src, exe, extra_sources=None)`：把 `extra_sources` 加入 cl 命令，并加 `/Itest`。
- `run_test` 增加 `native_c=None` 参数；若不为空，写入 `test/_t{idx}_native.c`，并自动注入 `#include "_t{idx}.h"`。
- 主循环支持 TESTS 条目的可选第 4 元素：
  ```python
  for i, t in enumerate(TESTS):
      if len(t) == 3:
          name, source, expected = t
          native_c = None
      else:
          name, source, expected, native_c = t
  ```

### `AGENTS.md`

新增 **Native Methods** 章节，记录语法、ABI、header 生成、refcount 规则。

---

## 8. 实现顺序（里程碑）

### Milestone 1：Header 重构（无 native 功能）

- 先生成 `.h`，把 typedef / 原型全放进去；`.c` 改为 include `.h`。
- 保持所有现有测试通过。
- 这一步单独提交，避免 native 和 refactor 混在一起。

### Milestone 2：Parser / Symtab 支持 `native`

- 加 token、lexer、ast flag、MethodInfo flag、parser 解析 `native RetType name(...);`。
- 加负向测试：native 方法带体、缺 `;` 等应报错。

### Milestone 3：Codegen 跳过 native 函数体

- `codegen_method_decl` 对 native 只写 header 原型，不写 `.c` 体。
- 确认调用点生成的 `ClassName_method(...)` 能链接到用户实现。

### Milestone 4：测试 runner 支持 extra C 源文件

- 写一个最简单的 native 测试（例如 `Math.add`）验证端到端。

### Milestone 5：文档与清理

- 更新 `AGENTS.md`。
- 全量跑 release + debug 测试。

---

## 9. 测试计划

### 负向测试（加入 NEGATIVE_TESTS）

| 名称 | 预期错误 |
|---|---|
| `bad_native_body` | `native` 方法后写了 `{}` |
| `bad_native_semi` | `native` 方法后缺 `;` |

### 正向测试（加入 TESTS，使用第 4 元素放 native C）

```python
("native_add", """
class Math {
    native i32 add(i32 a, i32 b);
}
i32 main() {
    Math m = new Math();
    return m.add(10, 20);
}
""", 30, """
int32_t Math_add(Math* thiz, int32_t a, int32_t b) {
    return a + b;
}
"""),
```

再增加一个测试 native 方法返回对象 / 使用 `ref` 参数，验证 retain 规则。

---

## 10. 风险与待决定

1. **Generic class 里的 native 方法**：第一阶段禁止。等泛型实例化稳定后再考虑。
2. **native 类实现 interface**：允许，但用户必须实现所有 interface 要求的方法，否则链接报错。
3. **顶层 `native` 函数**：第二阶段再做，语法和类方法一致。
4. **自动 stack trace**：第一阶段不做，用户可手动 `MY_PUSH`/`MY_POP`。
5. **多模块 import**：header 机制已为将来打好基础，但真正的 separate compilation 还需要解决 class destructor / vtable 的重复定义问题（目前 destructor 改 external 后，多个 `.c` 同时定义同名 destructor 会冲突）。
