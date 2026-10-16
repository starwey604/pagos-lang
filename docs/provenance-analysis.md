# Provenance 构建与真实 Stage 分析

Provenance 已接入 `StageAnalyzer`。每轮分析使用自己的 `Builder`，完成后将
只读 `ProvenanceAnalysis` 快照发布到 HIR 模块。图记录已有的阶段判断，不参与
Comptime 求值、Stage 推断或残留程序生成。

这一步承接[独立原型实验](provenance-prototype.md)，先验证构建、查询和实际
程序之间的对应关系。当前继续使用追加式存储和原有 `RuntimeTrace` 诊断；
没有增加分支收敛规则；证据图可显式导出/恢复模块快照，编译任务调度器仍为后续工作。

## 决策点提供语义依据

[Stage Evidence P0](stage-evidence-p0.md)将四类核心依据迁移到 Analyzer
实际作出判断的位置：显式 Runtime 边界、分支选择、成员投影和递归完成性。
Recorder 消费明确的语义事件，不再通过 HIR 的 `RuntimeBoundary`、`If`、
`Field` 或 `Index` 种类猜测这些规则。缺少对应事件会报告内部一致性错误。
普通严格运算、效果和返回控制的结构记录继续沿用现有实现。

快照增加 `decisions()` 和 `decision(NodeId)`。稀疏的 `StageDecision` 保存
结果 Stage、`StageRule`、源码区间、`StageReason` 和可选成员序号。
依赖仍由图边保存；未迁移运算和结构包装没有决策条目。
绑定根经 `Use`、`RequireComptime` 或 `Sequence` 到达实际决策节点，查询
见证也可以携带该节点及调用上下文。所有数据由快照拥有，不保留 HIR 指针。

| 语义规则 | 依据 |
| --- | --- |
| `RuntimeBoundary` | 用户显式隐藏初始化值的 Comptime 知识 |
| `RuntimeSelection` | Runtime 条件选择结果，包括相同常量的两条分支 |
| `OperandPropagation` | 已知条件的逻辑短路结果依赖被选中的 Runtime 操作数 |
| `MemberProjection` | 已知成员、Runtime 成员、不透明聚合或 Runtime 索引 |
| `ConservativeFallback` | 递归工作后的已知尾结果不能证明调用完成 |

投影的实际成员由 Analyzer 提供，Recorder 不再调用图层的投影透明性判断。
已知成员即使来自纯 Comptime 聚合，也保留一个简短决策摘要；没有重建完整
求值历史。聚合初始化、索引求值和边界检查仍与值依赖分别记录。

递归完成性事件由 Analyzer 在函数定义完成时提交，`finish` 只发布已有摘要。
保守判定的图终端暂沿用 `Rule::RuntimeBoundary`，但其语义条目是
`ConservativeFallback / RecursiveCompletion`。C ABI 边界和外部来源继续用
原有图规则，不能仅凭图终端种类把它们与显式边界或保守判定混为一谈。
查询预算与递归展开的 `partial` 标记只表示解释覆盖，不产生 Stage 决策。

内联实例化替换符号参数并复制相应证据和决策，保留 DAG 身份和各次调用的
工作身份；局部契约的实参替换同步复制改变路径上的决策。残留函数摘要继续
共享符号依据，查询时绑定当前调用的参数。默认 CLI 输出和旧诊断格式保持不变。

## Builder 与 FrozenGraph

[图接口](../include/pagos/stage/provenance_graph.h)将构建和读取分开：

| 对象 | 所有权与职责 |
| --- | --- |
| `Builder` | 单个任务独占，追加节点、建立调用、发布函数摘要 |
| `FrozenGraph` | 共享不可变节点池、边池和摘要表，只提供读取与查询 |
| `QueryResult` | 拥有本次查询的来源、调用上下文、覆盖状态和可选见证 |

例如，构建一个显式边界及其使用：

```cpp
using namespace pagos::stage::provenance;
Builder builder;
const auto origin = builder.add(Rule::RuntimeBoundary, 1);
const auto use = builder.add(Rule::Use, 2, std::array{Edge{origin}});
const auto graph = std::move(builder).freeze();
const auto result = graph.query(use);
// result.sources 包含 origin；builder 已被消费，不能继续修改。
```

冻结检查所有已预留的摘要都已发布，然后转移底层存储；不会复制整个图。
构建期的去重索引随 Builder 释放。`node()` 与 `edges()` 在构建期间返回的
视图可能因扩容失效；冻结后的视图在共享存储存活期间保持稳定。

节点和边仍使用连续数组及 32 位 ID。ID 仅在所属图中有效，不是跨编译轮次的
身份。位置标签保存在独立表中，节点不拥有字符串或 HIR 指针。

后续可将不同 Builder 分配给不同工作线程；已冻结的图可以由多个线程同时
查询。查询的队列、已访问表和调用上下文都属于本次查询，不写入共享图。
发布到其他线程仍需正常的任务队列、future 或其他同步机制。当前编译器自身
仍是串行分析，尚未实现跨图引用或跨任务合并。

## 一轮实际工作流程

1. `StageAnalyzer::analyze()` 创建新的私有记录器和 Builder。
2. 绑定分析完成后，记录其 HIR 值和契约根。保留顶层绑定、局部 `comptime`
   要求及静态存储初始化要求，普通局部绑定按被使用的 HIR 依赖记录。
3. 记录器按表达式身份复用证据。遍历使用显式工作栈；弱引用检查身份，避免
   丢弃的临时 HIR 地址被重新分配后误命中，也不保留全部临时 HIR。
4. 函数分析注册符号参数及摘要槽；调用节点记录实际参数。成员投影和 Runtime
   循环索引在 Stage 作出对应判断时补充证据。
5. 分析结束后填充残留函数摘要、冻结图，并释放构建期映射。HIR 模块持有
   `shared_ptr<const ProvenanceAnalysis>`；工具可以单独保留这个快照。
6. 需要解释时才进行 BFS 查询，以 `(NodeId, 调用上下文)` 去重。

快照拥有图、绑定名称、源码字节区间和位置标签，查询不依赖原来的 Analyzer、
AST、类型表或 HIR 表达式存活。例如，先保留 `module->provenance`，再销毁
模块和 Analyzer，仍然可以查询其绑定根。当前快照不保存源码文本或文件名；
将字节区间显示为行列时，需要工具保留对应源码快照。

## 多来源与调用摘要

```pagos
fn increment(x: u32) -> u32 { x + 1 }
let a = external_input();
let b = external_input();
let first = increment(a);
let second = increment(b);
let both = first + second;
```

两次调用共享符号摘要 `x + 1`。查询 `second` 时，`x` 在该调用上下文中绑定
到 `b`，不会继承首次分析的 `a`。查询 `both` 能得到两个来源，而现有
`RuntimeTrace` 诊断仍保留第一条路径。

若函数自身调用 `external_input()`，同一个源码位置在两次调用中代表不同的
运行时读取。来源通过 `(来源节点, 调用上下文)` 区分，查询结果拥有上下文表，
可以沿父关系解释调用位置。局部 `comptime` 契约则在当次函数分析时绑定实参，
因此失败后也能查询到实际输入，不会只剩未绑定参数。

## 值来源与残留工作分开查询

```pagos
record Config { base: u32, baud: u32 }
let config = Config(base: 4096, baud: external_input());
comptime let base = config.base;
```

`base` 的值来源为空，Stage 为 Comptime；效果查询仍包含构造 `config` 时的
读取。成员投影记录选中字段的数据依赖，以及整个构造的初始化依赖。值查询
跳过初始化与效果边，效果查询保留它们。显式 Runtime 边界保持不透明。

```pagos
fn set(p: *mut u32) -> u32 { volatile *p = 42; 7 }
comptime let result = set(4096usize as *mut u32);
```

`result` 没有 Runtime 值来源，但有一次 volatile 写入。已知返回值不会删除
该写入；已有 LLVM IR 和执行测试继续检查残留行为。效果证据还覆盖普通内存
访问、C ABI 调用、Runtime 循环及可能陷阱的算术或数组索引。这里的效果结果
是已记录效果及检查的集合，不能当作动态执行次数或执行顺序；顺序仍由 HIR/MIR
维护。共享 HIR 与调用上下文决定去重身份，不能按相同文本合并执行。

`static` 存储初始化同时需要 Comptime 值和纯求值。例如已知返回值带写入，
仍不能成为合法存储初始化。图中的 Comptime 契约只表示值要求；效果查询与
原有纯初始化检查共同解释这一限制。

## 查询入口与覆盖范围

```sh
build/debug/pagosc explain-provenance source.pgs
```

上面的 `set` 示例输出类似：

```text
binding result: Comptime
  contract: Comptime
  value sources: 0 (complete)
  residual effects: 1 (complete)
    volatile store @<begin>:<end>
```

`@begin:end` 是源码字节区间；函数摘要内的来源还显示 `via` 调用位置。存在
Stage 错误时，命令先展示已经记录的根，再输出原有诊断，并返回失败状态。
语法或类型检查失败时不会进入 Stage，也没有 Provenance 快照可展示。
`explain-stage`、诊断路径和现有 HIR/MIR 输出保持原有格式。

`residual effects` 计数效果证据，未计数所有纯 Runtime 运算。例如
`runtime let x = 20; let y = x + 10;` 的 `y` 有 Runtime 值来源，但效果数为零；
运行时加法仍由残留程序处理。已在 Comptime 验证的常量索引不额外报告运行时
越界检查；Runtime 索引则保留对应检查。

`complete` 只保证相对于已记录证据的查询覆盖。默认查询最多访问一百万个
状态；调用方可设置预算。预算耗尽、未绑定参数、被中止的函数摘要或递归调用
展开截断时，结果标为 `partial`。查询一条最短见证也不声称覆盖完整来源集合。

例如 `count(n - 1)` 的递归摘要仍保留有限结构，但查询遇到重复的函数作用域
就截断调用上下文展开；递归参数和效果尚未采用 SCC 不动点算法。递归工作后
返回已知常量时，还记录“recursive completion”边界，反映现有 Stage 对完成性
的保守判断，而不会把空来源误解释为 Comptime。

当前保留已残留的计算、边界、控制依赖和成员投影。已经折叠的纯 Comptime
算式通常只留下“known value”，不重建整个求值历史。未执行的 Comptime 分支
不进入证据图。仍不通过复杂等价性证明推断 Stage，也没有阶段 What-if 求解。

## 成本与后续工作

普通编译构建证据并冻结，但不执行完整来源查询。函数摘要按残留定义共享；
纯 Comptime 成员访问无需额外记录整份聚合。局部契约的参数替换只复制实际
改变的证据路径，不展开被调用函数摘要。

当前保留 `RuntimeTrace` 与图两份表示，构建期还有表达式身份映射，实际内存
成本包含位置字符串、根表、容器容量和查询临时空间。独立原型中的 24 字节
节点及 8 字节边不能直接等同于整个分析的内存成本。旧实验表也不能当作本次
真实编译的性能数字。

下一步应测量大型实际程序的 Stage 构建时间、冻结后内存及查询规模，再决定
如何替换旧诊断路径、汇总重复证据和切分任务。缓存应优先保存昂贵的分析
产物；图块缓存、失效判断及磁盘读取成本需要单独实验，当前图 ID 不承诺跨
轮稳定。

[Cache Architecture RFC](cache-architecture-rfc.md)进一步规划统一缓存底座，
将查询失效依赖与 Stage Provenance 分开，并定义精简证据、当前调用重新
绑定及未来冷热分块的边界。现有 FrozenGraph 的连续池和稳定视图契约保持
原状；真正卸载需要另行设计块句柄及读取生命周期。
[C3a](provenance-snapshot-c3a.md)已实现模块级分段二进制
快照与独立源码位置映射；[C3b](cache-residual-c3b.md)验证闭合标量模板的
关联恢复。它不等于完整增量分析或已经实现证据卸载。
已实现 C3q1，解决内存图查询的暂停/恢复和覆盖状态，C3q2 验证了
共享函数摘要及压缩调用拓扑，见[实现记录](provenance-query-c3q.md)。
C3a 格式已落地。[C4a1](provenance-view-c4a1.md) 已实现物理块读取视图、
lease 和原生查询按需读取。[C4a2](provenance-units-c4a2.md) 捕获实际特化
身份并冻结函数 Import/SCC、决策和必需摘要闭包；
[C4a3](provenance-function-queries-c4a3.md) 已接入函数视图和跨进程实验 CLI，
支持来源、见证、拓扑与热决策读取，并提供真实 Stage 大函数测量。
[C4b1](provenance-spill-c4b1.md) 和 [C4b2](provenance-recorder-spill-c4b2.md)
已接入同次编译临时 spill、原生视图及辅助失败/发布边界。
[C4b3b](provenance-working-set-c4b3b.md) 完成工作集计量/释放、真实崩溃
回收及大图构建峰值实验验收；默认仍为 memory。C4c–C4e 继续推进查询状态
checkpoint、有限域
递归解释和纯计算重放。默认同步入口保留一百万状态上限、旧 complete 与文本，
实验任务可以分 slice 继续推进。RFC 区分记录模型/求解精度、任务进度、详情可用性及展示进度，
说明每类步骤的保存位置，不把一次查询暂停或细节未展开变成 Stage 错误。
P1 的有限 Comptime 规则需独立产生摘要，当前纯折叠常量没有完整操作历史，
因此持久化现有图不能自行恢复这些历史。
[C1a](cache-identity-c1a.md)已有 checked-AST 语义指纹和 SourceAnchor
编解码/当前位置映射；C3a 使用它们独立于源码偏移持久化图内 NodeId 和位置
关联。图内 ID 仍不承诺跨语义修订稳定。

回归测试覆盖冻结与消费、并发只读查询、快照生命周期、多来源、缓存复用、
成员精度、副作用、局部契约、返回控制和递归部分覆盖。见
[图单元测试](../tests/unit/provenance_test.cpp)、
[真实 Stage 测试](../tests/unit/stage_provenance_test.cpp)及
[命令行示例](../tests/lit/stage/provenance-multiple-sources.pgs)。

初次接入验证包含 14 项真实 Stage 测试、11 项图测试和 5 个新增 lit 用例。
Debug 完整回归通过 171 项 CTest，其中 lit 运行 378 个用例。GCC、Release 与
ASan/UBSan 配置通过更新后的 Provenance 专项及 CLI 用例；独立基准工具的
84 组参数组合也通过 ASan/UBSan 检查。格式、clang-tidy、文档链接、Python
语法及差异空白检查一并通过。
