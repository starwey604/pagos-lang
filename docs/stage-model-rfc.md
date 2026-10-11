# Pagos Stage Model RFC

Pagos 将 Stage 作为组织程序的核心语义：已知信息可以提前求值，未知信息留在
残留程序中，程序员可以约束阶段边界，并了解每项推导的原因。同一个业务函数
应当自然适应 Comptime 与 Runtime 输入，阶段变化时仍遵循相同的计算规则。

> Every computation has a stage. Every stage has a reason.
> Every boundary can be explained.

这项承诺还需要行为保持原则：合法的阶段迁移应当保持程序的结果与可观察行为。
类型、布局、外部效果和目标表示等边界可能使迁移失败，编译器需要说明原因。

本 RFC 整理已有语义，并提出有限的精度扩展。当前编译器行为仍以
[阶段规范](staging-semantics.md)和对应测试为准；下文明确标注尚未实现的内容。
Provenance 的数据结构、算法和性能方案留待单独研究，本轮只定义解释需求。
各代码块按独立的 `.pgs` 片段理解；标注失败的例子用于说明边界。

## 阶段与存储命名

阶段名称统一为 `Comptime / Runtime`，阶段绑定使用 `comptime let` 和
`runtime let`。`static` 专用于程序期存储声明，写权限继续由 `const / mut`
说明。`comptime let` 要求结果已知，产生结果所需的 Runtime 工作仍然保留。

| 形式 | 职责 |
| --- | --- |
| `Comptime` | 值的编译期已知性 |
| `Runtime` | 需要由残留程序产生或承载的值 |
| `comptime let` | 要求结果编译期已知 |
| `runtime let` | 显式建立 Runtime 边界 |
| `static const / static mut` | 程序期存储及写权限 |

除明确标注的收敛扩展外，示例的预期行为沿用现有语义。

## 四个核心概念

| 概念 | 含义 | 一个例子 |
| --- | --- | --- |
| Stage | 按语言规则能否在编译期确定值 | `10 + 20` 是 Comptime `30` |
| Stage Contract | 程序必须满足的阶段要求或显式边界 | `comptime let n = 16;` 要求结果已知 |
| Stage Residualization | 利用已知信息生成正确的残留程序 | 已知时钟与未知波特率留下运行时除法 |
| Stage Provenance | 值的阶段、契约和残留工作的推导原因 | 除法依赖 Runtime 波特率 |

这四项共同回答：知道什么、要求什么、留下什么，以及为什么。
What-if Analysis 是将来的查询能力，不作为基础语义的组成部分。

## Stage 表示值的已知性

Stage 描述编译器按照语言规则，在当前程序、目标配置和允许的编译环境下，
能否确定一个值。保留 `Comptime <= Runtime` 二元模型：

- `Comptime`：值可以在编译期确定，并可用于允许的编译期计算。
- `Runtime`：该值需要由残留程序产生或承载，编译期计算不能观察其具体值。

```pagos
let a = 10;
runtime let b = 20;
let sum = a + b;
```

`a` 是 Comptime `10`，`b` 和 `sum` 是 Runtime。即使 LLVM 最后将 `sum`
优化为立即数 `30`，Pagos 的阶段判断仍然不变。

Comptime 是语言语义保证，Runtime 不证明后端无法优化。纯粹的严格算术运算按照
操作数阶段传播：任一操作数是 Runtime，结果就是 Runtime。短路、分支选择和
字段投影按各自规则处理；“尽可能 Comptime”指遵循这些明确规则。

例如，当前规则仍将 `runtime_value * 0` 判为 Runtime。不能因为后端能够化简，
就让 `comptime let` 的成败取决于优化等级、LLVM 版本或编译器尝试了多少证明。

### Stage 与类型分别描述值

类型描述可以进行哪些操作；Stage 描述值何时可知。普通类型不需要写成
`Comptime<u32>` 或 `Runtime<u32>`。

```pagos
let fixed: u32 = 115_200;
runtime let selected: u32 = 115_200;
```

两者类型都是 `u32`，阶段不同，可以传给同一个普通函数。Stage 也不决定
可变性、存储位置或是否存在一条机器指令。

```pagos
static const limit: u32 = 100;
let observed = limit;
```

`limit` 的初始化值必须是 Comptime，但读取全局存储得到的 `observed` 是 Runtime。
`static const` 描述存储权限，没有将每次读取变成编译期常量。

## Stage Contract 让程序员控制边界

默认推断只受语言规则约束。需要硬性保证时，程序员可以要求 Comptime，或主动
建立 Runtime 边界。

| 绑定形式 | 语义 | 结果或失败原因 |
| --- | --- | --- |
| `let x = e;` | 按规则推断 `e` 的阶段 | 保留合法的推导结果 |
| `comptime let x = e;` | 要求结果为 Comptime | 报告契约失败及原因 |
| `runtime let x = e;` | 检查 `e` 后建立 Runtime 边界 | 后续不能观察该绑定的 Comptime 值 |

```pagos
let inferred = 4 * 4;
comptime let required = inferred;
runtime let deferred = inferred;
comptime let invalid = deferred;
```

`inferred` 和 `required` 是 Comptime `16`。`deferred` 是 Runtime，最后一行
失败。`comptime` 不要求编译器加大优化力度；`runtime` 也不要求额外分配内存。

显式 Runtime 边界必须保留，包括对聚合值的投影：

```pagos
record Config { baud: u32 }
runtime let config = Config(baud: 115_200);
comptime let baud = config.baud;
```

最后一行按当前语义失败，未来的有限收敛规则也应继续尊重这个边界。
程序员主动隐藏的 Comptime 信息不能被投影分析重新暴露。

### 必要位置可以有隐式契约

一些要求来自语言结构，不需要额外的阶段关键字。例如固定长度数组生成器的
边界必须是 Comptime，因为编译器需要确定构造长度。

```pagos
let squares = [for i in 0..4 { i * i }];
runtime let count = external_input();
let invalid = [for i in 0..count { i * i }];
```

`squares` 是 Comptime 数组，第二个生成器失败。诊断应说明“生成长度要求
Comptime”，并指出 `count` 的 Runtime 来源。这与元素类型不匹配是不同的问题。

当前函数没有 `comptime n` 参数或依赖长度返回类型。本 RFC 不新增这些语法；
以后如果类型布局依赖参数，该要求应接入相同的契约解释。

### Comptime 结果与纯初始化是不同要求

`comptime let` 约束结果。全局初始化还要求没有 Runtime 工作。

```pagos
fn known_after_read() -> u32 {
    external_input();
    7
}

comptime let answer = known_after_read();
static const invalid: u32 = known_after_read();
```

单独看这两种使用：Comptime 绑定可以得到已知结果 `7`，同时保留一次读取；
全局初始化必须失败，因为它无法在纯 Comptime 初始化中执行这次读取。

契约解释应分别给出“值必须已知”“不得含有残留工作”等要求及其来源，
避免只用一个 Comptime 标签表达所有合法性条件。

## 普通函数默认具有阶段多态

阶段多态表示同一个普通函数可以按照调用点的输入知识进行部分求值，
无需为编译期和运行时分别编写业务实现。

```pagos
fn uart_divisor(clock_hz: u32, baud: u32) -> u32 {
    clock_hz / baud
}

let clock_hz = 80_000_000;
let fixed = uart_divisor(clock_hz, 115_200);
runtime let baud = external_input();
let dynamic = uart_divisor(clock_hz, baud);
```

`fixed` 是 Comptime `694`；`dynamic` 是 Runtime。后者仍利用已知的时钟，
残留程序计算 `80_000_000 / baud`，并保留必要的除零检查。

结果取决于函数实际使用的输入，不是所有参数阶段的简单合并：

```pagos
fn first(value: u32, unused: u32) -> u32 { value }
comptime let result = first(7, external_input());
```

`result` 是 Comptime `7`，但未使用的第二个参数仍按顺序求值，读取发生一次。
函数体不依赖这个参数的值，不等于调用方可以跳过它的计算。

这适用于普通 Pagos 函数。`extern fn` 与 `export fn` 的固定 C 边界调用仍是
Runtime，即使参数已知或导出函数体返回常量；外国代码不会在求值器中执行。
Runtime 递归当前也采用保守的结果规则，不承诺通过终止或固定点分析找出常量。

## 控制流决定哪些计算发生

控制依赖表示某个结果或某项工作取决于路径选择。条件已知时，编译器只求值
选中的路径；条件未知时，残留程序保留选择。

```pagos
comptime let selected = if true { 7 } else { external_input() };
let skipped = false && (external_input() != 0);
```

`selected` 是 Comptime `7`，`skipped` 是 Comptime `false`，两处读取都不发生。
未选中的代码仍需通过语法分析、名称解析和类型检查。

```pagos
runtime let flag = external_input() != 0;
let selected = if flag { 7 } else { 9 };
```

`selected` 是 Runtime，因为它的值取决于 `flag`。两个分支各自的常量仍是
Comptime；Runtime 控制不意味着其中每一项纯计算都必须变成未知值。
这个例子中两条路径结果不同，后面的相同常量收敛提议也不会使它成为 Comptime。

循环遵循相同区分：Comptime 边界可以在编译期展开，Runtime 边界留下循环。
例如 `[for i in 0..4 { i * i }]` 的边界与各元素已知；
`for i in 0..count { external_input(); }` 在 Runtime `count` 下保留循环，
循环索引也是 Runtime。循环残留工作与其后一个常量结果的已知性分别判断。

早返回还会决定后面的代码是否执行。例如函数在 Runtime 条件下选择
`return 7` 或继续到结果 `9`，调用结果依赖这个控制条件。目前即使两条返回
路径都是 `7`，调用结果也保守地保持 Runtime；首轮收敛不扩展这种情况。

## 聚合值保留局部知识

字段精度表示一个对象包含 Runtime 数据时，其他字段仍可以保留 Comptime 知识。
整体阶段不能代替每个字段的判断。

```pagos
record UartConfig { base: usize, baud: u32 }
let config = UartConfig(
    base: 1073745920usize,
    baud: external_input()
);
comptime let base = config.base;
let baud = config.baud;
```

`config` 整体是 Runtime，`base` 是 Comptime，`baud` 是 Runtime。构造时读取
波特率的工作仍需执行，即使后面只使用 `base`。这里的地址数值只说明已知性，
不授权编译器访问宿主或目标设备。

数组也保留元素知识：

```pagos
let table = [11, external_input()];
comptime let first = table[0];
let second = table[1];
```

`first` 是 Comptime `11`，`second` 是 Runtime，构造过程的读取仍只发生一次。
当前这些知识可以经过别名、参数和直接返回传递；显式 Runtime 边界以及
Runtime 分支或返回路径的选择仍阻止投影恢复 Comptime。

例如，`runtime let index = 0; table[index]` 当前仍是 Runtime。
运行时索引即使选择全相同的元素，也不属于本 RFC 首轮收敛扩展的范围。

## Residualization 分开处理结果与工作

残留化利用已知信息提前计算，并生成仍需在目标程序中执行的工作。
分析需要分别回答值是否已知，以及为了得到该结果还必须执行什么。

```pagos
fn write_uart(p: *mut u32) -> u32 {
    volatile *p = 42;
    7
}

export fn configure(p: *mut u32) -> u32 {
    comptime let result = write_uart(p);
    result
}
```

`configure` 内部的 `result` 是 Comptime `7`，但调用必须保留一次 volatile
写入，以及适用的地址检查。Comptime 结果可以参与后续编译期计算，不能成为
删除设备写入的理由。

残留工作还包括产生 Runtime 值的计算、检查、循环和控制流，不只包括外部
副作用。例如读取未知波特率后进行除法，需要留下除法与必要检查；反过来，
一个 Runtime 结果也不要求把已知时钟的计算留到运行时。

### 效果决定操作是否允许执行

效果描述操作对程序外部或目标存储的访问要求。值的 Stage 不能独自决定
效果是否合法，也不能提供所需授权。

```pagos
let address = 4096usize as *const u32;
let sample = volatile *address;
```

`address` 可以是 Comptime，`sample` 是 Runtime。数值地址已知不能让编译器
在编译期读取设备。普通目标内存访问也保留运行时语义；volatile 另有可观察
访问要求，不隐含同步或内存屏障。

构建期文件与环境能力尚未实现。以下是沿用阶段规范的概念示例，`build_u32()`
代表需要授权、并记录输入依赖的 `build.fs` 操作：

```pagos
runtime let enabled = external_input() != 0;
let value = if enabled { build_u32() } else { 0 };
```

这个程序应因 Runtime 控制下的构建期效果失败。编译器不能先读取文件，再把
运行时分支当作普通数值选择。未来即使分支值可以收敛，效果合法性仍须检查。

## 阶段迁移保持计算规则与可观察行为

把一个输入从 Comptime 改为 Runtime，在其他对应输入相同、且程序满足阶段契约
和目标执行要求的情况下，应保持结果及语言定义的可观察行为。可以提前计算
更多或更少的工作，业务计算规则保持一致。

例如，前面的 `uart_divisor` 使用 Comptime `115_200`，与运行时提供同一个
波特率，都应得到 `694`。换成运行时输入并不保证输入值不变；值变了，结果
可以按同一除法规则变化。

行为保持需要覆盖以下情况：

- 实参按从左到右的顺序求值，语句遵循源码顺序。
- 一个绑定表示一次求值，别名和重复使用不重复执行该计算。
- 未选中的分支与短路操作数不执行，选中的效果仍按原有条件执行。
- 丢弃结果不能绕过必要检查、陷阱或控制流。
- 特化复用可以共享代码，不能合并独立的运行时调用或读取。

```pagos
let snapshot = external_input();
let doubled = snapshot + snapshot;
let fresh = external_input();
```

这里有两次读取：`doubled` 重复使用第一次读取的结果，`fresh` 执行第二次。
编译器既不能把第一次读取执行两遍，也不能把第二次读取合并进第一次。

```pagos
fn quotient(n: u32, d: u32) -> u32 { n / d }
runtime let denominator = external_input();
quotient(10, denominator);
let result = 7;
```

虽然除法结果被丢弃，除零时的运行时陷阱仍需保留。若直接调用
`quotient(10, 0)`，Comptime 求值应报告编译错误。这是同一非法运算规则在两个
阶段的处理方式；行为保持不要求编译器接受已经证明非法的程序。

Comptime 求值也必须使用目标整数宽度与算术规则，不能继承宿主 C++ 的偶然行为。
例如 `255u8 + 1u8` 在两个阶段都按 `u8` 回绕得到 `0`。

编译预算限制求值和特化的成本。超出 fuel、递归深度或特化预算时应报告限制，
不能静默退化为 Runtime。例如一个 Comptime 调用链耗尽 fuel，错误原因是
求值预算，而不是突然出现了未知输入。

### 跨阶段的数据必须能在目标中表示

跨阶段持久化表示把 Comptime 数据嵌入目标程序，而不是把编译器的宿主对象
直接交给目标。

```pagos
let squares = [0, 1, 4, 9];
runtime let index = external_input();
let selected = squares[index];
```

Comptime 数组可以嵌入目标，Runtime 索引执行带边界检查的读取。具体采用
立即数、只读表或其他表示由编译器决定。

未来的宿主文件句柄、编译器语法对象和能力令牌不能按这个方式嵌入；目标符号
地址需要目标重定位。能在编译期得到一个对象，不代表能将它作为运行时数据。

## 拟议扩展是有限的阶段收敛

阶段收敛表示通过明确的有限规则，证明某个结果不随 Runtime 选择变化，
从而保留 Comptime 知识。它不要求证明任意两个程序等价。

当前规范规定 Runtime 条件选择的结果为 Runtime，即使分支常量相同。
下面是拟议行为，尚未实现：

```pagos
runtime let flag = external_input() != 0;
comptime let answer = if flag { 7 } else { 7 };
```

拟议规则允许 `answer` 为 Comptime `7`。条件读取仍发生一次；不能因为结果
已知而删除求值它所需的工作。

首个扩展只处理两个分支均正常继续、没有退出当前函数的控制流，且结果具有
相同类型与相同 Comptime 常量的情况。常量相等按 Pagos 的类型和值规则判定，
不调用后端优化或一般程序等价性证明。

分支效果可以不同，但必须继续保留选择关系：

```pagos
export fn choose_write(p: *mut u32, flag: u32) -> u32 {
    comptime let answer = if flag != 0 {
        volatile *p = 1;
        7
    } else {
        volatile *p = 2;
        7
    };
    answer
}
```

拟议规则允许内部 `answer` 为 Comptime `7`，仍按照 `flag` 选择并执行一次
写入。合并结果不能合并两条写入，也不能提前执行未选中分支的检查或效果。

### 字段收敛在标量规则之后研究

```pagos
record UartConfig { base: usize, baud: u32 }
runtime let revision = external_input();
let config = if revision == 1 {
    UartConfig(base: 1073745920usize, baud: 115_200)
} else {
    UartConfig(base: 1073745920usize, baud: 57_600)
};
comptime let base = config.base;
let baud = config.baud;
```

当前 `base` 的 Comptime 要求失败。拟议规则在同一名义记录类型内逐字段比较：
`base` 可以是 Comptime，`baud` 仍是 Runtime，整体配置仍是 Runtime。
这只比较字段常量，不新增记录的普通结构相等运算。

首轮不扩展 Runtime 索引、早返回合并、循环终止证明或递归固定点推导。
显式 `runtime let` 和 C 边界继续保持原有要求。字段知识如何通过参数、返回
和缓存传递，需要在实现前明确，不能让精度只偶然依赖某一种 HIR 形状。

## Provenance 定义解释需求

Stage Provenance 表示推导结果的因果证据。Stage Provenance Graph 是这些
关系的概念视图；内部数据结构与查询算法尚未选定。

解释需要区分数据依赖、控制依赖、显式阶段边界和契约要求，并将效果与残留
工作的原因单独表达。不能将所有因果关系压成一条变量名路径。

```pagos
runtime let revision = external_input();
runtime let baud = external_input();
let clock = if revision == 1 { 80_000_000 } else { 40_000_000 };
let divisor = clock / baud;
comptime let config = divisor;
```

这个失败至少需要解释：`revision` 通过控制选择影响 `clock`，`baud` 通过
数据依赖影响除法，`config` 建立 Comptime 契约。一个简短诊断可以展示一条有用
路径，但完整查询不能将它误认为唯一来源。

显式 Runtime 边界本身就是不能向上观察 Comptime 值的原因，同时可以解释其
初始化过程。效果来源与值来源需要分开：`first(7, external_input())` 的
读取属于残留工作，不是结果 `7` 变成未知值的原因。

Comptime 结果也需要理由。例如字段投影得到常量、Comptime 分支选择和未来的相同
常量收敛，应能分别解释。完整依赖证据不等于最小修改方案；解释两个来源
不自动证明必须修改两者，后续 What-if 还需要假设与重分析。

### 数据结构与算法另行研究

当前 `RuntimeTrace` 已提供单路径解释与符号参数替换，可以继续作为过渡。
本 RFC 不决定将它替换为什么，也不预设节点存储、共享方式或图遍历算法。
实现前需要比较语义覆盖和编译成本，重点回答：

- 普通编译需要保存哪些证据，详细解释查询还需要做哪些工作？
- 大型聚合、重复特化和长依赖链怎样控制记录与查询的时间、内存成本？
- 缓存如何保留可替换的参数来源，避免查询得到上一次调用的证据？
- 递归和共享依赖怎样得到有限、确定的解释？
- 精简或汇总证据时，如何明确说明解释范围，避免将省略的来源当成不存在？

例如，同一个 `scale(4, runtime_value)` 的两次调用可以共享残留函数，
但第一次的解释必须指向第一次的输入，第二次也必须指向自己的输入。
共享代码不能共享错误的调用来源。

性能研究可复用[现有测量方法](m3-preparation.md#p1可重复的性能基线)与
`scripts/benchmark.py`，再补充多来源、长链和递归用例。分别观察普通编译与
解释查询的耗时、峰值内存和规模增长，先测量再决定表示方案。

[独立原型实验](provenance-prototype.md)比较字符串路径、追加式 ID 图和选择性
去重，记录调用上下文、字段投影及查询成本。后续已将构建与只读接口分开，
并[接入真实 Stage 分析](provenance-analysis.md)，保持本 RFC 的语义边界；
实际编译成本、递归来源求解及缓存仍需继续测量与研究。

## Stage 由契约控制并通过工具观察

程序员使用普通绑定、Comptime 要求和 Runtime 边界控制 Stage，通过编译器与
Analyzer 观察推导结果。普通业务逻辑暂不增加 `stageof` 或编译期上下文判断，
避免阶段变化直接切换业务算法。

例如，无论波特率来自常量还是运行时读取，`uart_divisor` 都计算同一个除法。
将来如果引入阶段专用实现，需要单独定义等价要求或允许的行为差异。

期望的工具解释可以同时展示四项信息，下面仅示意内容，不规定 CLI 格式：

```text
result:
  Value Stage: Comptime
  Known Value: 7
  Reason: write_uart returns the constant 7
  Contract: comptime let result requires a known value; satisfied
  Residual Work: one volatile write, with required address checks
```

当前 `pagosc explain-stage` 主要报告顶层绑定的 Stage、单条 Runtime 路径和
分析统计。另有 `pagosc explain-provenance` 查询已记录的多来源和残留工作，
并明确标记部分覆盖。完整 Comptime 推导历史及更精确的递归解释仍是后续目标；
本 RFC 不规定查询语法或输出格式。

## 后续推进与验收

先确认上述语义边界，再研究 Provenance 的表示与算法；实现选择需要同时满足
解释需求和可接受的编译成本。结构化解释落地时优先保持现有 Stage 判断，
收敛扩展作为单独的语义工作更新规范、示例与测试。

| 工作 | 应能验证的行为 |
| --- | --- |
| 整理现有语义 | 区分 Comptime 值、纯初始化、Runtime 边界与残留工作 |
| 研究 Provenance | 对多来源、缓存、聚合和递归比较正确性与成本 |
| 统一契约与解释 | Comptime 与 Runtime 都有理由，要求与执行工作分别说明 |
| 标量收敛 | 相同常量可知，条件求值、分支效果与陷阱仍正确 |
| 字段收敛 | 相同字段可知，不同字段 Runtime，显式边界不被穿透 |

测试应同时检查推导和执行。例如相同分支常量的用例，要检查 Comptime 契约成功，
还要在两个条件输入下验证选中的写入；多来源用例要检查控制与数据原因，
不能只检查出现了一个 Runtime 字样。优化后的输出可以验证行为保持，
不能反过来决定 Stage。

本方向暂不增加新的阶段层级、阶段泛型、Stage 普通类型包装、构建能力 API
或业务代码阶段反射。下一个需要深入解决的问题是 Provenance 的实现成本；
有限收敛的知识表示与传播边界随后明确。

## 当前实现参考

- [阶段规范](staging-semantics.md)：当前传播、控制流、效果与嵌入规则。
- [HIR](../include/pagos/hir/hir.h)：值阶段、常量和 `RuntimeTrace`。
- [阶段分析器](../lib/stage/stage_analyzer.cpp)：绑定、投影、调用和分支分析。
- [字段精度用例](../tests/lit/stage/record-config.pgs)：跨别名与调用保留知识。
- [Runtime 分支边界](../tests/lit/stage/record-branch-boundary.pgs)：当前不收敛。
- [Comptime 结果与 volatile 工作](../tests/lit/codegen/volatile.pgs)：保留写入。
- [诊断约定](diagnostics.md)：当前错误信息和依赖路径的展示要求。
- [路线图](roadmap.md)：编译器与硬件配置实验的推进背景。
