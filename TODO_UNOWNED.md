# TODO: MyLang unowned 非拥有引用(类 Swift `unowned(safe)`)

## 状态
✅ 已实现并通过测试(`python test_runner.py` 与 `--mode debug` 各 204/204 passed)。
实现要点与设计偏差:
- 表示与 weak 完全相同(变量存 `WeakRef*`,复用 `weak_count` 份额机制),
  所有份额管理(初始化/拷贝/释放/清理/字段析构)直接并入 weak 分支。
- `codegen_expr` 变为包装器:unowned 类型的 IDENT/MEMBER_ACCESS 读取自动包
  `mylang_unowned_check`;份额管理路径经 `codegen_expr_raw`
  (`CodegenContext.no_unowned_check`)跳过包装。
- `mylang_unowned_check(WeakRef*)`:NULL → panic "unowned reference is null";
  `refcount <= 0` → panic "unowned reference to dead object";否则返回用户指针。
- 顺带修复:语句级强类赋值路径(`codegen_expr_stmt`)原来不排除 weak,
  `w = b`(weak 局部变量重赋值)会走错分支产生类型混乱的 C;现在 weak/unowned
  都落到 `codegen_expr` 的统一弱赋值块。
- 范围限制均已落地:仅类类型、局部声明必须初始化、禁 unowned 返回类型、
  禁 unowned 数组、禁 `new unowned`、禁 `.lock()`、禁 unowned match、
  禁 `as unowned`。
- 测试:9 个正向用例(基本读写/份额拷贝/转强引用/参数/重赋值/字段/
  从 lock 转换/两种死亡访问 crash)+ 9 个负向用例,内联于 test_runner.py。

## 目标

加入 `unowned` 引用:不拥有对象、可像强引用一样直接使用,访问时检查对象是否存活,
已死则 `my_panic`(带行号),而不是像 `weak` 那样返回 NULL 要求 `lock()`。

```mylang
class Node {
    Node parent;              // 强引用:父死子亡(当前唯一选择)
    unowned Node parent;      // 目标:不拥有,直接用,死了 panic
}

void dump(unowned Node n) {   // unowned 参数
    print(n.name);            // 读取时自动插入存活检查
}
```

动机:父子回引用、delegate 这类"引用者必然后死"的结构里,weak 强制满屏
`lock()` + NULL 判断;unowned 把"必须活着"写进类型,违反时是带行号的确定性
panic,失败语义更清晰,且仍内存安全(检查兜底,永不 UAF)。

## 语义设计(范围决策)

1. **只做类类型**:`unowned ClassName`。不做 `unowned interface`(需求极小,
   且需要胖指针版的检查包装,后续真要再加)。
2. **只做 safe 变体**:每次 r-value 读取(方法调用、字段访问、当实参、赋给强引用)
   自动插入 `mylang_unowned_check`,死了 `my_panic`。不做不检查的
   `unowned(unsafe)`。
3. **转换规则**:
   - strong → unowned:隐式(声明初始化、赋值、传参),取一份非拥有份额;
   - unowned → unowned:拷贝份额;
   - unowned → strong:允许(检查 + retain),等价 Swift 的隐式转换;
   - unowned 变量禁止调用 `.lock()`(那是 weak 的伪方法)。
4. **禁止**:`return` unowned 值;`unowned` 数组(第一版,后续可复用
   `MYLANG_ELEM_WEAK_CLASS` 路径再放开);`unowned` 用于 `new`( nonsense )。
5. 允许的形态:局部变量、函数参数、类字段(析构时释放份额)。
6. 清理/释放路径与 weak 完全共用:作用域退出、字段 dtor、数组元素释放都是
   `mylang_weak_release`(份额是同一种东西)。

## 内存模型基础(为什么现在加很便宜)

unowned 需要的全部机制 = "对象内存被非拥有引用钉住 + 死亡标记可读 + dtor 在强
计数归零时即时运行" —— 这正是弱计数嵌入 `ObjHeader` 后已经具备的:

- `weak_count` 语义即"非拥有份额数 + 对象存活时的隐式份额",unowned 份额
  **直接复用同一个计数器**,不新增头部字段(份额的释放规则完全相同:
  归零 ⟹ dtor 已运行 ⟹ 释放内存)。
- runtime 只需新增一个函数:

```c
void* mylang_unowned_check(void* user_ptr) {
    ObjHeader* h = mylang_obj_hdr(user_ptr);
    if (h->refcount <= 0) my_panic("unowned reference to dead object");
    return user_ptr;
}
```

  读取侧:调用者的份额钉住对象内存,所以读 `refcount` 永远安全;
  `refcount == 0` 是死亡判定(与 `mylang_lock` 相同的不变量)。

## 线程语义(单线程精确 / 多线程兜底 / Swift 的做法)

- **单线程(现状)**:检查是精确的。检查与使用之间没有任何东西能改变
  `refcount`;方法调用时被调方入口 retain `this`,调用期间对象不可能死。
- **多线程(将来)**:检查不是同步原语 —— 另一线程可能在检查通过后、使用前
  释放最后一个强引用,对象进入析构。此时读到的内存依然有效(份额钉住,不
  UAF),但对象逻辑上已死 —— 检查退化为"兜底",不再能保证对象存活。
  真正需要并发存活保证的场景必须用 `weak` + `lock()`(CAS 钉住强引用)。
- **Swift 怎么搞**:Swift 的 `unowned(safe)` 同样是"僵尸对象 + 访问时检查
  trap",也**不是**并发安全的。它的答案是:数据竞争本身由语言层面消灭
  (Swift 6 的 Sendable/actor 编译期检查),在此之上 unowned 的检查和单线程
  一样精确;需要跨线程钉住对象就用 weak 转 strong。另外 Swift 还有不检查的
  `unowned(unsafe)`,死了是真悬垂指针(我们不引入)。
- 结论:MyLang 将来引入线程时,unowned 语义**不需要改** —— 精确性来自语言
  的数据竞争策略,而不是 unowned 机制本身。

## 实现要点(按层)

### 1. lexer / token(token.h, lexer.c)
- 新增 `TOK_KW_UNOWNED` 关键字。

### 2. 类型系统(ast.h / symtab 相关)
- 新增 `TYPE_IS_UNOWNED = 0x08000000`(下一个空标志位)。
- `Type` 增加 `is_unowned`(或复用 `is_weak` 路径加分支);类型相等、
  赋值兼容性规则:unowned 与 weak 互不兼容,unowned → 同类 strong 兼容。

### 3. parser(parser.c)
- `unowned ClassName v = expr;` 声明:复用 weak 的解析路径(`TYPE_IS_WEAK`
  的 8 处触点旁加平行分支)。
- 校验:`expr` 必须是类类型(strong/weak/unowned 来源按转换规则);
  非类类型(原始类型、struct、interface、数组)报编译错误。

### 4. runtime(runtime.h / runtime.c)
- 新增 `mylang_unowned_check(void*)`(如上);份额操作复用
  `mylang_weak_init` / `mylang_weak_copy` / `mylang_weak_release`。

### 5. codegen(codegen.c,大头,`is_weak` 75 处触点中需要平行的部分)
- 变量声明:发射 `ObjHeader*`(或直接用类指针类型加 check 包装,二选一,
  推荐与 weak 相同的 `WeakRef*` 表示,读取时 check 后 +1 偏移…… 实现时定)。
  → 决策:与 weak 一样存 `WeakRef*`(即 `ObjHeader*`),读取时
  `mylang_unowned_check` 返回用户指针。
- 初始化/赋值/参数转换:strong→unowned 包 `mylang_weak_init`;
  unowned→unowned 包 `mylang_weak_copy`;所有权的 owned 表达式
  (call 结果)用 `mylang_weak_init_owned`。
- **读取点(r-value)**:任何把 unowned 变量当类值使用的位置,先
  `mylang_unowned_check(...)` 再解引用;方法调用接收者、字段访问、
  实参、return(经检查转换为 strong)、f-string 插值。
- 清理:`CleanupEntry` 增加/复用弱份额释放分支(复用 `is_weak` 分发)。
- 类字段 dtor:释放 unowned 字段份额(复用 weak 字段路径)。
- 编译错误:对 unowned 调用 `.lock()`;`return` unowned 局部(未转换为
  strong 的);`unowned` 修饰接口/数组/原始类型。

### 6. 测试(test/unowned_*.my)
- 基本:声明、读取、方法调用、字段访问、当参数传;
- 转换:strong→unowned、unowned→unowned、unowned→strong;
- 死亡访问:对象死后读取 → 进程 panic 退出(按现有 panic 测试约定断言);
- 形态:参数、类字段(含 dtor 释放份额,debug 模式无泄漏);
- 错误用例:`.lock()`、unowned 接口、unowned 原始类型、return unowned。

### 7. 文档
- `AGENTS.md`:新增 unowned 小节(语法、语义、与 weak 的分工、线程语义说明);
  内存模型节补一句 `weak_count` 同时服务 weak/unowned 份额。

## 工作量估算

约 400~600 行(含测试与文档)。runtime 仅 +~15 行;其余大部分是 weak 路径的
平行分支,真正的新逻辑只有 `mylang_unowned_check` 和读取点插入。

## 预计影响文件

- `src/token.h` / `src/lexer.c`
- `src/ast.h` / `src/symtab.c`(类型标志与校验)
- `src/parser.c`
- `src/codegen.c`
- `src/runtime.h` / `src/runtime.c`
- `test/unowned_*.my`(新增)
- `AGENTS.md`

## 验收标准

- `python test_runner.py` 与 `python test_runner.py --mode debug` 全部通过。
- 新增 unowned 用例通过,包括死亡访问 panic 与 debug 模式无泄漏。
- 现有 weak 用例不回归。
- ASan 压测(tmp_native/weak_lock_race.c)不受影响。

## 后续可选(不在本期)

- `unowned` 数组(`MYLANG_ELEM_UNOWNED`,复用弱元素路径)。
- `unowned interface`(需要胖指针检查包装)。
- 线程化之后:按语言的数据竞争策略复查检查点的精确性(机制不变)。
