# C3q1/C3q2：可恢复查询与共享来源公式

2026-10-12 完成。本文记录[缓存实现计划](cache-implementation-plan.md)中
C3q1/C3q2 的内存实现；模块磁盘快照、spill、跨进程 checkpoint 和递归
不动点仍属于 C3a/C4。默认 CLI 和 Stage 的依据没有改变。

## 任务、覆盖与交付

[QueryTask](../include/pagos/stage/provenance_query.h)强持有 FrozenGraph，
保存 BFS 队列、visited、调用上下文、活动节点/边游标、前驱和交付游标。
`begin_query(root, kind, witness)` 开始 Sources 或最短 Witness 查询。
Value/Effects 沿用原有关系过滤和参数替换；没有另行推断 Stage。

`advance` 的 `QuerySlice` 分别限制新激活状态、边扫描、上下文绑定与输出
记录数。扫描包括被过滤的物理边；绑定包括作用域祖先检查、参数匹配及
新调用帧；见证重建也消耗绑定额度，单独计入 `witness_steps`。
入队由当前边操作推进，不用状态额度截断入队。额度不足时保留该操作的
游标，不丢 frontier，不重新从根开始。零额度可以结束已完成的记账，
不能激活新节点、扫描边或创建调用帧；输出额度不决定求解覆盖。

| 信息 | 当前含义 |
| --- | --- |
| `QueryProgress` | Running、Paused、Cancelled、Completed 或 Failed |
| `EvidenceCoverage.precision` | 尚未穷尽为 Unresolved；穷尽且有缺口为 Conservative；否则 ExactForRecordedModel |
| `all_sources` | 已穷尽当前记录模型的可访问来源状态；结合 issues 判断缺口 |
| `witness_found` | 已找到来源，见证重建可能仍在推进，不代表全部来源完成 |
| `issues` | Incomplete、未绑定参数、递归调用截断、缺摘要或旧 API 上限 |
| `DetailAvailability` | 内存数据 Resident；任务分配失败为 Unavailable |
| 页状态 | 交付序号、已交付/待交付来源数及见证游标；无记录页序号为零 |

`stop_token` 可并发请求取消，执行器在每个节点/边/绑定步骤的边界检查。
任务本身只能由一个线程操作。`cancel` 保留状态，显式 `resume` 后可以
继续；Paused 的下一次 advance 自动继续，销毁任务才释放状态。
Failed 为终止状态，需要新建任务。页交付游标在整页成功建立后提交，
分配失败不确认未交付记录；图仍可供其他新任务读取。

新分页接口按 BFS 发现顺序交付，身份是图节点与调用上下文，页不会重新
排序或重复发送。`result()` 返回拥有上下文表的排序副本；解析图 ID 时
还须保留图。当前队列、visited、来源和前驱均留在内存；小页数不等于
总内存上限。Completed 后可以继续取得待交付页，不再增加求解工作。

旧 `FrozenGraph::query` 是任务的一次性兼容包装，仍默认最多接纳一百万
个状态，仍排序来源并给出原边上的 BFS 最短见证，仍用 `complete` 表示
旧的覆盖含义。新的任务没有整次任务上限；暂停与记录缺关系分别报告。
[ProvenanceQueryTask](../include/pagos/stage/provenance_analysis.h)还持有
ProvenanceAnalysis 和 SourceFile 内容/位置快照，独立于 Analyzer、HIR
和调用方输入对象。

## 摘要、作用域与压缩公式

`FrozenGraph::summaries()` / `summary(id)` 枚举不可变函数摘要，Value 与
Effects 入口暂共享已有根。Recorder 注册摘要时捕获函数名，并在语义
事件产生时记录 `StageDecision.scope` 与局部契约的 `ProvenanceRoot.scope`。
`ProvenanceAnalysis.scopes()` 提供决策节点及契约根索引，零作用域为模块。
内联副本归调用方作用域，原函数符号记录保留函数归属；不通过标签或 HIR
地址推测归属，不新增求值历史。

[TopologyTask / FormulaIndex](../include/pagos/stage/provenance_topology.h)
先分页交付物理节点、邻接边及 Value/Effects 摘要链接，然后按 slice 准备
局部依赖公式。物理拓扑包含调用实参和初始化关系，按图节点去重，摘要环
保留有限回边；此阶段不创建调用上下文、不枚举路径、不宣称来源已求全。
只有完成后才发布不可变 FormulaIndex，任务和索引各自持有图。

每个查询根、可达摘要通道根和实参根的公式保留以下原图终端：来源族、
Parameter、Call、SummaryRef 和 Incomplete。结构包装通过迭代遍历消去；
调用保留共享引用，不把被调用公式递归内联。公式按根/通道复用，源族按
节点/通道计数，族数不是具体读取次数或目标执行次数。

| 原有转移 | 公式转移与等价依据 |
| --- | --- |
| Use、Strict、Select、RequireComptime、Aggregate、Projection、Sequence | 按原通道过滤关系，收集去重终端；选择性投影已由记录边表达 |
| Origin、RuntimeBoundary、Effect | 保留原节点和通道；终端自身的相关依赖仍沿原边求解 |
| Parameter | 保留 owner/slot，查询时在当前作用域链匹配，在父上下文查询实参 |
| Call | 保留摘要与实参边；Value 进入值根，Effects 同时保留全部实参工作 |
| SummaryRef | 在同一上下文进入原摘要根，visited 保证有限循环不重复展开 |
| Incomplete、递归 Call | 保留缺口；递归作用域重复时沿原规则截断，并返回 partial |

Sources 查询可以把结构根的出边遍历替换为公式终端转移，实际减少包装
节点访问和扫描。终端需要原有绑定/缺口处理；缺公式、未知规则/关系时
使用原体，未知规则/关系的原因保存在 `DependencyFormula.fallback`。
最短 Witness 使用 C3q1 原边遍历，避免压缩边改变最短路径。
公式准备逐根扫描，各根大量重叠时仍可能有重复准备成本，当前没有线性
总准备成本或压缩一定更快的承诺；准备状态/边、重用和容量单独计数。

`begin_invocation(calls, kind, witness)` 接受从外到内的指定调用链：验证
每个内层调用属于前一摘要的局部依赖公式，保留前缀上下文，从最后一个
Call 查询，包括该调用的未使用实参效果。只实例化选定前缀和查询涉及的
后继；Witness 是该调用起点的最短见证。调用链验证属于工厂的同步准备
工作，计入基准 query 时间；执行阶段的绑定受 slice 控制。
重复函数作用域的调用链明确拒绝，完整递归实例化留给 C4d；单次递归
来源查询仍按原规则返回缺口。查询不同实参或同站点的不同读取，不合并
其具体上下文；压缩查询的上下文编号与未压缩查询不承诺相同。

## 实验入口

```sh
build/debug/pagosc --provenance-query=resumable --provenance-slice=1 \
  --provenance-query-stats explain-provenance example.pgs
build/debug/pagosc --provenance-query=compressed \
  --provenance-query-stats explain-provenance example.pgs
build/debug/pagosc --provenance-query=topology explain-provenance example.pgs
```

默认 mode 为 legacy。只有 explain-provenance 接受这些选项；slice 必须
为正数。CLI 将 N 同时用于四类额度，C++ 接口可分别设置。
resumable/compressed 持续推进并输出来源，topology 仅输出有限
结构。遥测独立输出到 stderr，不混入 AnalysisStats。CLI 不暴露跨进程
续接游标；正常 check、诊断路径与残留执行不增加求解任务。

## 验证与测量

[查询测试](../tests/unit/provenance_query_test.cpp)覆盖百万长链、宽扇入、
40 层普通菱形、SummaryRef 环、递归缺口、缺摘要和未绑定参数、细粒度
恢复/取消、独立输出和见证交付。普通 40 层菱形仅访问 121 状态；共享
40 层调用图有 122 节点、162 条通道摘要链接，表示 2^40 个叶读取实例，
指定调用只产生 41 个上下文和一个来源。4 层图的 16 个具体实例还与独立
笛卡尔调用链模型逐项比较；全实例查询的成本仍随实际实例数量增长。

[真实快照测试](../tests/unit/stage_provenance_test.cpp)检查作用域、局部
失败契约、输入生命周期、scale 的不同实参、内部读取、未用实参工作、
投影和嵌套替换；查询前后的 Stage/HIR、全部 AnalysisStats 与诊断相同，
成功模块继续可降低到 MIR。两项 CLI 回归比较默认与实验输出和失败诊断。

Release 的独立实验工具分别测量 build/freeze、拓扑/公式准备、一个查询
及每个子进程的 peak RSS；RSS 包括全部阶段，不能当作查询保留内存。
公式容量仅含公式记录、终端数组和调用数组，排除哈希表及分配器开销。

```sh
cmake --build --preset release
python3 scripts/benchmark_provenance_queries.py \
  --output build/benchmarks/provenance-c3q.json
```

测量环境为 AMD Ryzen AI 9 HX 470、Clang/LLVM 23.1.1、Linux Release。
报告 `build/benchmarks/provenance-c3q.json` 包含 14 组、98 个计时样本和
28 个预热样本，以及二进制/来源/CMake 哈希、版本、每次结果、median、
IQR、min/max。最终采样期间未同时运行本次构建或测试，没有锁定 CPU
频率或控制 OS 页缓存；小耗时组波动明显，不据此宣称延迟排名。

| 工作负载 / 查询 | 状态数 | 公式准备 median ms | 查询 median ms | 查询 IQR ms |
| --- | --- | --- | --- | --- |
| 1,000,002 节点长链 / resumable | 1,000,002 | 0 | 60.109 | 0.928 |
| 同长链 / compressed | 2 | 93.061 | 0.023 | 0.001 |
| 10,000 来源扇入 / resumable | 10,001 | 0 | 1.004 | 0.546 |
| 同扇入 / compressed | 10,001 | 2.280 | 1.032 | 0.564 |
| 40 层普通菱形 / resumable | 121 | 0 | 0.014 | 0.015 |
| 同菱形 / compressed | 2 | 0.045 | 0.012 | 0.008 |
| 10 层调用图全实例 / resumable | 4,094 | 0 | 0.631 | 0.354 |
| 同调用图全实例 / compressed | 4,094 | 0.025 | 0.644 | 0.359 |
| 40 层调用图 / 指定调用 | 2 | 0.026 | 0.011 | 0.013 |

长链在 245 个执行 slice 后完成，扫描 1,000,001 条边，队列峰值 2；
旧默认包装止于 1,000,000 状态，留下 partial 和零来源。压缩后的索引
保留 100 字节公式/终端数组容量，但首次准备与查询合计约 93 ms，超过
直接 resumable 的约 60 ms；复用已准备索引后才体现包装消除收益。
两者进程 peak RSS 中位数分别约 142.66 与 141.33 MiB，没有峰值下降。

10 层共享调用图全枚举仍创建 2,047 个上下文、返回 1,024 个来源，压缩
不减少这些具体实例。40 层图的有限索引则只有一个来源族、163 个公式
终端、7,308 字节数组容量；指定调用返回一个来源、41 个上下文，扫描一条
摘要依赖，不预先产生 2^40 个实例。索引的保留容量不含哈希表，不能用来
代表整个任务或进程内存。C4b/C4c 才处理构建和求解状态的内存卸载。

构建验证：Debug 全量 CTest 256/256，包含 lit 387/387；GCC 和
ASan/UBSan 的 Provenance/StageDecision 定向 CTest 各 51/51，Provenance
CLI 各 8/8；文档检查 27 文件、clang-format 和 diff whitespace 检查通过。
