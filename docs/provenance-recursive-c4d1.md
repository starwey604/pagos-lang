# 有限递归摘要域与内存求解：C4d1

2026-10-26。C4d1 实现显式 C++ 实验接口
[RecursiveSummaryTask](../include/pagos/stage/provenance_recursive.h)。
输入是一个不可变 EvidenceView、一个 SummaryId 和 Value/Effects 通道；
输出是该摘要的有限符号依赖关系。构造不运行 Stage，也不展开调用帧。
普通解释 CLI、原生 QueryTask 的递归截断和 C4c checkpoint 格式保持原有
规则。C4d1 不包含 CLI 或求解状态 checkpoint；C4d 尚未全部完成。

后续 [C4d2](provenance-recursive-cli-c4d2.md) 已接入显式函数证据 CLI；
本文的内存规则和精度保证继续适用，求解状态 checkpoint 仍留给 C4d3。

## 抽象域与准入

令 P 为记录图中出现的 `(owner, slot)` 形式参数，O 为当前通道的静态
来源节点族。每个节点 n 的状态是有限乘积：

```text
D(n) = (dependencies ⊆ P ∪ O, issues ⊆ Issue, instances_merged ∈ Bool)
```

集合以包含关系排序，issues 用按位或，merged 只允许 false → true。
所有转移单调，从空集和节点本地种子开始，求最小不动点。动态递归深度
不属于域，因而没有通过增加展开预算逼近无限调用树。最坏情况下每节点
保留整个原子域，空间可达 O(V × |P ∪ O|)，本轮没有总 RSS 上限。

| 记录规则 | 转移 |
| --- | --- |
| Parameter | 产生 owner/slot 原子；owner 必须指向已发布摘要 |
| Origin、RuntimeBoundary | Value 产生来源族，并继续访问相关依赖 |
| Effect | Effects 产生效果族，并继续访问相关依赖 |
| Literal、Use、Strict、Select、RequireComptime、Aggregate、Projection、Sequence | 合并相关边的依赖、issues 和 merged |
| SummaryRef | 在当前作用域复制目标通道根，不建立调用帧；允许引用环 |
| Call | 读取被调摘要，对其自身 owner 的形式槽逐个替换为相应实参依赖 |
| Incomplete | 保留可读依赖，加入 IncompleteNode 缺口 |

Value 排除 Effect/Initializer 边；Effects 跟随所有关系。Call 的 Value
只替换真正被使用的槽；Effects 另合并所有实参工作，不能遗漏未用实参
的效果。投影和选择以原记录边为准，不重新分析源程序可行路径，不将
Initializer 加回 Value。参数替换同样使用当前通道，沿用原查询规则。

Call 的来源族保持为有限静态族，跨调用时设置 instances_merged，因为
原集合不再区分该族的动态调用实例。形式槽替换不因参数重命名而合并
来源实例；实参已有的 merged 会继续传播。SummaryRef 不引入实例合并。
两个调用点的来源族集合可以相同，有限关系图仍保留两个调用点。

用到的实参槽缺失、参数 owner 无效、缺摘要、Incomplete、未知规则或
关系均报告具体 issues。被调摘要引用其他 owner 的参数时报告 ForeignScope，
不猜测祖先调用环境。最终结果中非所选摘要 owner 的形式参数也构成该
缺口。未使用的 Value 实参不把其缺口传入结果，但其物理依赖仍会被读取
以建立有限方程目录。证据读失败是任务 Failed，不能返回“精确空集合”。

## 精度保证

精确子集的对象是**记录图的形式依赖投影**：一个形式槽是否存在某个
有限推导，沿已记录的关系及调用替换到达该槽。它不是 Runtime 可行路径、
读取次数、顺序、终止性，或所有具体调用上下文的来源清单。

在无缺口且无来源实例合并的子集中，普通边和 SummaryRef 是集合并，
Call 是形式槽的集合替换。这些正向规则的有限推导与从底开始的单调迭代
对应：每次加入的原子都有有限规则推导；每个有限推导也会在某次迭代中
出现。有限域保证迭代终止，得到与所有有限调用展开之并相同的形式关系。
因此参数置换、自递归和互递归可返回 ExactForRecordedModel。只有引用环、
没有任何终端的方程可得到空的精确形式关系；这不表示程序会返回。

| 状态 | precision |
| --- | --- |
| 尚未到不动点、取消、读取失败或存在支持缺口 | Unresolved |
| 完成且无缺口，但经过来源族实例合并 | Conservative |
| 完成、无缺口、未合并实例 | ExactForRecordedModel |

fixed_point 与精度独立。concrete_sources_complete 始终为 false，避免
将形式关系误当作原生 Source/context 列表。返回的 Parameter 原子是摘要
输入，不是尚未绑定的查询错误；不提供未经验证的具体调用结果。

例如 `F(x,y,z,w) = x ∪ F(y,z,w,x)` 的精确不动点是四个形式槽。
一次递归截断不能得到该关系；含内部 external_input 的递归则只返回
有限输入族及环，明确标 Conservative。RecursiveCompletion 所记录的
RuntimeBoundary 仍是 Value 来源族，不从摘要求解反向修改 Stage 决策。

## SCC、步骤与循环依据

任务首先按需读取所选通道的方程闭包。Call 保存目标摘要链接和带槽号的
实参链接；SummaryRef 保存同上下文链接；其余边保留原 Relation。
原生参数/调用规则不消费的边不建立求解依赖。
首轮直接为原图节点建立方程，不要求预先完成 FormulaIndex，也尚未复用
其压缩公式；包装节点及方程准备成本仍存在，不据此宣称性能提升。

随后用显式栈执行两遍 DFS，求包括摘要引用和实参的方程 SCC；不使用
C++ 递归栈。组件按依赖优先顺序求解，组件内单调 worklist 仅在事实、
issues 或 merged 增长时重新调度用户。每次转移保留输入游标、原子迭代器、
实参替换游标和通知用户游标，不用整张集合复制充当一步。

`advance(N, stop_token)` 最多消费 N 个原语步骤，涵盖节点激活、边读取、
DFS、组件登记、单原子替换与队列操作。零额度暂停且不做推断工作。
取消保留全部内存状态，resume 后继续；计数除 slices 外不因切片策略
变化或暂停而重复累计。一步可包含 EvidenceView 解码、集合操作及分配，
该预算不是墙钟截止或 IO 字节上限。任务状态及结果副本均驻留内存。
任务由一个线程操作；并发请求取消使用 stop_token。

components 提供节点、摘要根所属组件及 cyclic 标记；links 与 evidence
提供原规则、原摘要身份、关系和实参位置。它们是可检查的有限循环拓扑，
不伪造一条终止的动态路径，也不是到任意原子的最短来源见证。静态方程
目录包含未用 Value 实参，因此结构环本身也不能证明结果依赖该环。

```cpp
using namespace pagos::stage::provenance;
RecursiveSummaryTask task(analysis.evidence(), summary, QueryKind::Value);
while (task.progress() == QueryProgress::Running ||
       task.progress() == QueryProgress::Paused)
    (void)task.advance(256);
auto relation = task.result(); // 显式复制；检查 fixed_point/precision/issues。
```

EvidenceView 的 memory/segmented 后端均可使用。节点 lease 仅在发现该
节点期间持有，之后保留方程记录；没有恢复 FrozenGraph 的要求。读取失败
为终止 Failed，新任务可在修复后重新求解；本轮不承诺 IO 游标重试协议。

## 验证与下一轮

[单元测试](../tests/unit/provenance_recursive_test.cpp)覆盖自递归参数轮换、
互递归 owner/slot 替换、SummaryRef 环、内部输入/效果族、常量值与递归
效果分离、未用实参、全部缺口种类、分段视图与读取失败、不同调用点及
一万个摘要构成的环。独立的有界调用展开解释器使用显式形式环境，核对
小递归例子多种展开深度的集合，与方程迭代分别实现。

在一个完整小任务的每个原语步骤边界暂停/取消/恢复，结果、步骤数、事实
增长、求解次数和循环拓扑均与不中断运行相同。真实 Stage 图验证递归
完成边界仍为 Runtime；查询前后 HIR、诊断和全部 AnalysisStats 不变，
成功程序仍可降低到 MIR。默认解释和旧来源上下文仍由原测试覆盖。

本轮验证：Debug 全量 CTest 406/406（含 lit），GCC 与 ASan/UBSan 各
405/405 非 lit CTest；C++ 格式、43 份 Markdown 文档和
`git diff --check` 均通过。

后续分轮：

1. **C4d2：显式解释入口与实际证据验收，已完成。** 见
   [实现记录](provenance-recursive-cli-c4d2.md)。接入经验证的函数证据闭包和
   摘要选择，输出符号精度、缺口及循环依据；明确具体调用实例与摘要形式
   槽的边界，扩充真实递归、控制/投影和双通道案例。不得默认替换原生
   来源查询或清除旧截断标志。
2. **C4d3：求解状态持久续接。** 为方程目录、SCC 阶段、工作集及活动
   替换游标定义版本化 checkpoint，绑定原始证据/源码及求解域版本；接入
   C4c 的确认、配额和清理协议。损坏/绑定拒绝和跨进程结果/计数等价验收
   后才标记 C4d 的续接完成，不能直接序列化容器迭代器或复用旧任务格式。

完整 LSP、具体递归调用路径枚举及 C4e 纯详情重放继续独立安排。
