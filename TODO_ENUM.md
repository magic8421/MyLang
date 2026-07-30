# TODO: 枚举（Rust 风格带值枚举 / tagged union）

## 状态

2026-07 /grill 讨论后确定：排在 struct 泛型之后做。泛型落地后此线升级为
泛型枚举（Option<T> / Result<T, E>），语法届时不需要变。

2026-07 路线一（简单枚举，C++ enum class 风格）已实现。路线二（带 payload
的 tagged union）尚未开工，本文件剩余条目即为路线二范围。

## 已达成的结论

- 目标场景：(a) SDL 事件 / 状态机（SDL_Event 在 C 里就是 tag+union）。
  **不需要泛型**，变体类型具体写。(b) Option/Result 错误处理等泛型回来再说。
- 表示法：C 的 `struct { i32 tag; union { ... } u; }`，与运行时天然对齐。
- 值语义（栈上、可内嵌），不是 boxed——这是和 interface+match 那条
  "穷人的 sum type"的本质区别（还有穷尽检查）。
- payload 里有 boxed 指针 / string / interface 时，复用 struct 那套
  retain/release 钩子模式，钩子按 tag 分派（每种枚举各生成各的）。

## 路线一已定决策（v1，已实现）

- 语法：`enum Key { Up, Down, Left = 10, Right }`，仅单元变体；
  `Variant(...)` / `Variant {...}` 报专门错误 "payload enums are not yet
  supported"（为路线二保留语法）。变体值可显式指定（可负），否则从 0 自增。
- 访问：仅 `Key.Up`（scoped，等价 C++ `enum class`），变体名不进入普通
  作用域；同名局部变量遮蔽枚举名（与 class 静态调用同规则）。
- 底层类型固定 `i32`；C 端发射 `typedef enum Key { Key_Up = 0, ... } Key;`
  （变体 C 名 = 枚举名_变体名，与 `Class_method` 命名惯例一致）。
- 强类型（照抄 bool 严格规则模式）：枚举与整数、不同枚举之间不隐式转换
  （`enum_mismatch` 挂在与 `bool_mismatch` 相同的 5 个边界：调用实参、
  赋值 x2、变量初始化、return）；显式 `as` 双向转换（`k as i32`、
  `code as Key`），C 端直接强转，无运行时检查。
- `==`/`!=` 允许同枚举比较；算术、关系运算、位运算、复合赋值、`++`/`--`
  一律拒绝。
- match：臂写 `Key.Up =>`，常量折叠成变体值比较，lower 为 if 链；
  **不做穷尽检查**（留给路线二）。
- 枚举可用于：局部变量、函数参数/返回值、`ref` 参数、struct/class 字段、
  数组元素（`Key[]`，MyArray of int，走 MYLANG_ELEM_PRIMITIVE，无钩子）。
- 声明完全进 symtab（`EnumInfo`，仿 `StructInfo` 注册表 + 每个变体的
  payload 字段表），不产生 AST 节点；`EnumInfo` 的字段表 v1 恒空、
  `has_payloads` 恒 0，为路线二预留。
- 枚举是纯编译期类型，不分配运行时 type_id（已验证：type_id 只在
  class/interface/object 运行路径被读取——`mylang_new_object`、`as` 转换、
  match 类型臂、vtable concrete_type_id——值类型路径不读）。
- v1 不支持：枚举方法、`const Key`、枚举值做默认参数、f-string 直接插值
  （用 `as i32`）。

## 尚未决策的分支（路线二开工时再定）

- ~~变体语法：元组式 `Key(u32)`、结构体式 `Move { x: i32, y: i32 }`、还是
  都要；单元变体怎么写。~~ 单元变体语法已定（路线一）；路线二只需定
  payload 两种形态是否都要。
- match 怎么扩：变体模式的绑定语法（`Key k =>`、`Key(code) =>`、
  `Move { x, y } =>`）、是否强制穷尽（覆盖全部变体可省 else，
  symtab 知道全部变体，检查可做）。路线一的常量臂形式保留。
- 默认值 / 零值：第 0 号变体当零值？
- 枚举上的方法（impl 块）做不做。
- ~~与现有 match 的关系：整数臂、类型臂之外的第三种形态。~~ 已定：
  第三种形态 = 枚举变体常量臂（路线一）；路线二再加绑定模式臂。
- `match` 里 payload 含引用时的绑定是否参与引用计数（建议：绑定=借用，
  不参与，和现有 match 类型臂一致）。
- 路线二的 C 表示法：带 payload 的枚举改发 `struct { i32 tag; union {...} u; }`，
  `c_type_str` 按 `EnumInfo.has_payloads` 分叉；类型系统层（`TYPE_ENUM`）不变。

## 依据

现有 match：类型臂（接口/boxed）、整数字面量臂、枚举变体常量臂、else。
struct 钩子机器（retain/release/copy/clone 四个 + 数组元素钩子）可直接
改造复用。
