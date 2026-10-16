# 函数证据单元 C4a2

状态：2026-10-14 完成 C4a 的第二轮。实现实际特化身份、三个逻辑入口、
单元局部编号、Import、递归 SCC 冻结、根 manifest 发布及必需摘要闭包读取。
[读取视图 C4a1](provenance-view-c4a1.md) 已提供 lease 和物理块查询。
后续 [C4a3](provenance-function-queries-c4a3.md) 已完成读取视图映射、
跨进程实验 CLI、拓扑/决策读取及真实 Stage 大函数测量。以下保留本轮范围。

设计约束见 [Cache Architecture RFC](cache-architecture-rfc.md)，后续安排见
[实现计划](cache-implementation-plan.md)。本轮没有新增 Stage 推断规则，
没有通过函数证据加载跳过 Stage，也不提供构建期卸载或任务 checkpoint。

## 从实际分析捕获身份

提供 QueryContext 的 StageAnalyzer 使用实际 checked AST 和类型表生成
模块/定义身份。Recorder 保留模块语义指纹、上下文和定义身份，不保留 AST
地址；函数特化创建时保存声明的 StableDefinitionId、定义指纹和实际参数。
参数包括类型、Stage 和 Comptime 值；Runtime 参数只保存类型与 Stage，
不保存当前调用的来源、RuntimeTrace ID 或具体实参。

这些数据存入 ProvenanceAnalysis/ProvenanceScope。导出重新核对原始上下文、
模块指纹、稳定定义和定义指纹，不能把旧图配上另一份 checked AST 的身份。
特化名称来自实际声明，不能从显示标签或残留符号推断。

函数键新增 `QueryKind::FreezeFunctionEvidence = 5`，仅支持 Function 定义，
允许 Runtime 参数。键继承 C1a 的有界类型/值协议，可编码标量和固定标量
数组；实际成员限于 Recorder 已创建的作用域，当前 Analyzer 的残留特化
主要采用标量参数/结果，并未为一般数组特化建立函数成员。
不完整身份、未知类型以及无法编码的记录/指针参数在导出时明确失败。
目标、编译器身份和协议版本保留在上下文；记录模型和函数单元版本通过独立
semantic-options 标签与对象 header 绑定。依赖暂用保守整模块语义指纹。
独立对象不等于独立语义失效，也没有新增逐函数增量命中。

没有 QueryContext 的旧 Analyzer 使用方式仍可生成原有内存图及 C3a 模块
快照，但不能导出本轮函数单元。C3a v1 不保存新增特化身份/实例化标记，
恢复后的旧快照也不能伪装成已捕获实际特化的 Analysis 重新导出。
Recorder 的身份捕获失败不改变语言诊断或逻辑预算；后续导出失败。
有 QueryContext 时新增一次 checked-AST 身份遍历；启用 C2 缓存时目前可能
与其身份生成重复。没有把这项开销宣称为性能收益，C4a3 应纳入测量。

## 单元、入口与调用侧证据

[FunctionEvidence](../include/pagos/stage/function_evidence.h) 提供显式
`publish()`、`load()`、目录/成员访问和 `read_detail()`。持久化单元包含：

- Value 入口：函数结果的单元局部根编号。
- Effects 入口：效果通道的单元局部根编号；P0 目前与 Value 共用根。
- Decision 入口：已记录的 StageDecision 列表，保留规则、Stage、理由、
  成员投影、owner 和 SourceAnchor。它不是新求值操作历史或推导的 Stage。

模块单元保存顶层根、局部契约的当前调用实例和调用侧决策。Recorder 对
复制/实参代入产生的决策标记 `instantiated`，导出时放入模块/调用侧单元，
防止把当前调用的实例化决策作为共享函数决策。所有原始根和已记录决策
都有明确去向。调用节点保留当前单元内的实参边及调用位置，callee 用
scope 引用绑定实际特化；不把 caller 来源写入函数键。

每个函数收集值/效果/非实例化决策的物理闭包；不展开 Call 的 callee，也不
枚举动态调用路径。公共物理节点在不同单元中可复制，单元内去重。
局部节点按原图的拓扑顺序重新编号；目标和边偏移只在当前成员内有效。
因此这仍是初版冻结格式，不承诺最小体积或全局结构去重。

`recorded_complete` 只说明该成员的物理闭包没有 Incomplete 节点。
它不等于查询完成、递归求解精度、完整语言证明或成功编译。
分析失败时的部分契约证据可以用于显式解释导出；本 API 不能把它作为
成功 Stage 结果复用。解释覆盖和语义缓存准入分别验收。

## SCC 与 Import

冻结前从 Call、SummaryRef、Parameter owner 及元数据 owner 收集作用域
依赖，用迭代式 SCC 算法处理有限摘要图，避免宿主递归栈随图深度增长。
递归组件整体保存一个摘要对象和一个冷拓扑对象。组件内引用使用成员编号，
不把两个互递归对象的内容哈希互相嵌入。

scope 引用为 32 位：0 是模块作用域；低位编号指当前组件成员，编号从 1
开始；最高位表示 Import，余下位是从 1 开始的 Import 槽。
Import 保存外部摘要 CAS 引用、成员索引和该成员的规范键摘要。
成员和 Import 按稳定键摘要排序，根目录也按稳定键排序。

组件 DAG 从依赖叶子开始发布。根目录包含全部作用域的摘要对象、成员索引
和键摘要，验证闭包时不能只校验顶层对象或相信未经核对的 Import owner。

## 文件格式、发布与读取

新增三种对象，均有 PGC1 公共 header、函数格式版本 1 和 P0 记录模型 1：

```text
EvidenceRoot (kind 9)
    canonical root key / conservative module dependency
    catalog: summary ObjectRef, member index, canonical member key digest

EvidenceSummary (kind 10, one SCC)
    conservative module dependency / cold detail ObjectRef
    Import table
    members: canonical key, node count, Value/Effects ports,
             recorded coverage capability, Decisions, module roots

EvidenceDetail (kind 11, one SCC)
    conservative module dependency
    members: key digest, SourceAnchor/label sites,
             local nodes, local edges, scope references
```

模块根键使用 `$module.units`，模块成员键使用 `$module.unit`，与 C3a 的
`$module` 快照命名空间分离。源位置存为结构 Anchor，重开后关联当前
CacheIdentity；空白/注释修订不改变这些对象，不保存旧源码偏移作替代。

FileStore 新增 `write_object()`，复用原有 CAS 校验、原子叶对象发布和故障
清理。它不发布查询候选。所有叶对象和摘要写好后，才用现有 FileStore
发布含 root artifact 的 manifest。中断可能留下不可达叶对象，不会暴露
半个根；未来离线 GC 必须理解这些对象内部引用，不能只标记 manifest 的
直接 artifact。现有项目没有在线 GC。

`load()` 先核对查询 manifest 的键、输入与根结果摘要，再读取根和全部必需
摘要。检查版本、模型、当前上下文、定义/依赖、规范键、端口/节点范围、
重复键、Import 对象/成员/预期键及决策/契约 owner。缺失或损坏必需摘要
返回明确 StoreError，调用方可回到分析。本轮没有自动重跑 Stage。

热闭包读取不打开任何冷拓扑。它可以列出键、入口、覆盖能力、已记录决策
和模块根；这些元数据不意味着已经求解值/效果来源。
`read_detail()` 独立读取并校验整个组件冷对象，检查局部编号、后向边、
关系、端口大小、Parameter owner 的定义/Anchor/Runtime 槽及 Call 实参数量。
未知版本、坏 owner、截断、大小上限和 IO 错误明确失败。

冷对象缺失/损坏只使详情不可读，已验证的热键、端口和决策仍可访问。
当前原生来源求解还需要冷拓扑；不能把只有热元数据的句柄宣称为精确来源
结果。C4a3 负责把缺失详情映射成查询可用性/覆盖状态，并接入 lease。
`read_detail()` 返回拥有存储的解码结果，目前不做 pin 缓存或自动补写。
读取始终没有发布行为。

默认单对象仍为 16 MiB；codec 同时限制字节和元素数，冻结与加载还限制
全部成员物理闭包的节点总数。一个 SCC 的冷对象可能超过上限，此时明确
拒绝导出；C4a3 已测量受控大函数的实际读取开销；多块大组件留到 C4b。

## 验证与后续

[函数单元测试](../tests/unit/function_evidence_test.cpp) 使用真实 Stage 图，
覆盖不同 Comptime 特化、不同 Runtime caller、嵌套调用、互递归 SCC、
三个入口、全部决策去向、只读热闭包不打开冷对象、缺必需摘要/可选详情、
当前源码 Anchor、目标/依赖变化、容量限制和并发发布。
带正确 CAS 校验和的错误版本、错误 Import 键及指向现有模块成员的错误
决策 owner 仍会被拒绝；旧分析不能绑定到另一份 checked AST。
叶对象测试验证 CAS 共享、无根候选、非法配置和写入失败清理。

测试另完整解码局部节点、Import 和 owner，构造测试用内存 oracle，与原图
比较值/效果来源、最短见证、调用位置链、局部契约失败和递归覆盖。
这只是编码语义等价性验证，不作为 C4a3 的按需查询或内存收益证据。

后续 [C4a3](provenance-function-queries-c4a3.md) 已集成函数格式与
EvidenceView，保留调用上下文和局部编号映射，接入拓扑/决策实验 CLI，
完成独立进程、旧修订、损坏/缺失及真实大函数测量。下一轮进入 C4b 的
Recorder 构建期封块和同次编译释放。

本轮新增 14 个函数单元测试和 2 个 FileStore 叶发布测试。Debug 库/进程
CTest **299/299** 通过，端到端 lit **390/390** 通过；GCC 与 ASan/UBSan
的 Cache、Provenance、StageDecision、EvidenceView、FunctionEvidence 相关
CTest 各 **154/154** 通过。文档检查覆盖 31 个文件，clang-format 和补丁
空白检查通过。
