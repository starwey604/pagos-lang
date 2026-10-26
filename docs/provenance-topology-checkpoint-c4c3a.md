# 符号拓扑与公式准备 checkpoint：C4c3a

C4c3a 补齐 [C4c 计划](cache-implementation-plan.md#c4c查询中间状态卸载与跨进程-checkpoint)
中的符号拓扑状态，沿用 [C4c1](provenance-checkpoint-c4c1.md) 的修订绑定
和交付确认协议，以及 [C4c2a](provenance-task-store-c4c2a.md) 的任务 head、
配额和生命周期。它保存的是物理拓扑及有限符号公式的准备任务，不枚举
调用实例，也不增加 Stage 推断规则。

## 同一任务覆盖两种证据后端

`TopologyTask` 和 `FormulaIndex` 现在通过 EvidenceView 读取内存图或
已验证的函数证据。物理阶段仍交付 Node、Edge、Value/Effects SummaryLink；
随后按 slice 准备每个根/通道的局部 DependencyFormula。它们保留来源族、
Parameter、Call、SummaryRef 和缺口，按原规则过滤结构边，不内联被调用
公式。递归摘要仍保留有限回边，来源求解精度沿用原查询规则。

构造任务不读取冷块，遍历时通过 lease 保持当前节点及邻接边有效。
`FrozenGraph::begin_topology()` 和原内存公式查询继续使用同一算法。
只有 Completed 才发布不可变 FormulaIndex；它独立持有证据，原任务和
FunctionEvidenceView 对象析构后仍可查询。磁盘索引支持 begin_sources
及指定调用链 begin_invocation；graph() 仅对内存后端有效，磁盘后端抛出
logic_error，调用方继续通过原 EvidenceView 或 QueryTask::evidence() 解析
节点。指定调用链工厂需要的冷证据不可读时抛出 runtime_error。

## 任务格式和绑定

`TopologyTask(binding, task_id, root)` 显式启用 checkpoint；普通构造不保留
未确认页、不写磁盘。binding 仍只能由 FunctionEvidenceView 根据完整源码
创建，绑定同一证据闭包、位置对象和原始源码修订；空白变化也拒绝恢复。
调用方为新逻辑任务分配新 ID。

格式使用 QueryTaskManifest **版本 3**，与原生来源/见证任务的驻留 v1、
分页 v2 区分；独立 topology_engine_version 当前为 1，同时记录 Provenance
记录模型版本。任务 ID 不能在原生查询和符号拓扑之间转换，保存时验证旧
head 的绑定及任务种类。bytes 和 TaskStore 两种恢复接口使用同一格式：

| 部分 | 保存内容 |
| --- | --- |
| Identity | 格式、engine、记录模型、闭包/位置/源码/任务指纹、根 |
| Physical | queue、cursor、active、edge cursor、summary port cursor |
| Formula | 根/通道目录、已产生 terms、fallback、当前公式序号 |
| Preparation | 当前 local queue、cursor、edge cursor、active |
| Work | 节点、物理边/摘要链接、公式状态/边、重用、完成公式/term/来源族数 |
| Delivery | 页序号、确认位置、完整未确认记录及该页的进度 |
| Integrity | 专用域 `pagos.topology-task.v1` 的校验，加 CAS envelope 校验 |

没有地址、容器布局、哈希桶或 lease；visited、formula IDs、local seen 和
来源族集合在恢复时重建。fallback 编码为固定枚举，恢复为静态说明文本。
容量字节不是逻辑工作，不持久化；formula_capacity_bytes 在完成时按实际
容器 capacity 计算，完整索引恢复后重新计算，哈希桶仍不计入。

## 恢复和交付

恢复先按 ObjectRef 大小拒绝超出预算的对象，再验证长度、校验、版本及
调用方预期绑定，检查所有目录、根/通道、
唯一 ID、游标、活动状态、累计工作和交付边界。已处理物理节点的邻接长度
及摘要端口验证边/链接累计数，公式终端和未确认记录也核对原证据。
未处理 frontier 先核对 ID 范围，不因此遍历其依赖。
恢复会读取校验所需冷块，但不重放物理遍历、不重新准备已完成公式、不运行
StageAnalyzer，也不重复累计 TopologyStats 的逻辑工作。

checkpoint 模式采用 stop-and-wait：advance 在 acknowledge 前重送相同
`(task, sequence)` 和 records，不接受新的逻辑工作预算。收到 stop_token
会取消任务，仍保留未确认页。消费方提交记录及持久 receipt 后再确认，
确认位置在下一次显式保存时持久化；允许崩溃重送，由消费方按任务/序号
去重，不承诺无需确认协议的 exactly-once。

```cpp
TopologyTask task(binding, task_id, root);
auto page = task.advance(slice);
if (page.progress == QueryProgress::Failed)
    return;
if (page.sequence)
    (void)task.acknowledge(page.sequence); // 在消费方提交记录后确认。
auto head = task.save(store, expected_generation);
if (!head)
    return; // 内存游标、未确认页和累计工作仍保留。
auto resumed = TopologyTask::restore(binding, store, *head, root);
```

TaskStore 更新先发布完整不可变任务对象，再以预期 generation 原子更新
head。遍历/公式准备完成但仍有未确认页时为 AwaitingDelivery，确认并保存
后才成为 Completed；取消需显式保存才修改持久状态。只读 handle 可完整
恢复并继续驻留任务，期间不写文件；再次保存需要可写 handle。旧代 head
仍独立可读，存活锁、显式回收、配额和发布失败语义沿用 TaskStore。

任务证据不可读时 advance 返回 Failed 和具体 failure，失败任务不可保存；
恢复缺块、损坏、绑定/版本不符时返回 CheckpointError，不回退到 Stage 或
偷偷从头创建新查询。保存的 IO/配额/只读/过期 generation 错误不改变
活动游标及未确认页。

## 本轮边界与后续

本轮符号拓扑队列、去重集、公式目录/terms 和准备表仍完整驻留；checkpoint
是受 codec 字节/元素上限约束的单对象，默认 16 MiB/1,000,000 元素。
**没有拓扑任务的分段分页或 offload，也不宣称拓扑工作集下降**。
[C4c2b](provenance-query-paging-c4c2b.md) 的六表活动分页仍只用于原生
Value/Effects 及最短见证任务，不自动套用于可变长度的公式表。

恢复后的 FormulaIndex 可启动压缩来源查询；该 QueryTask 的执行中状态与
指定调用链状态仍不支持 checkpoint，既有 InvalidState 拒绝保持不变。
EvidenceTopologyTask 仍是只读物理拓扑入口，本轮未替换主解释 CLI，也未
增加自动写文件、后台清理或 LSP 接口。

后续拆分：C4c3b 补拓扑任务分段保存/暂停释放及容量计量，对原生分页和
拓扑任务测量状态内存/磁盘、保存/恢复延迟与读写放大；C4c3c 接入显式主
解释 CLI 的继续/暂停、任务保存/恢复及维护，并保持普通文本兼容。压缩
来源执行状态的持久化另列扩展，不因本轮公式准备可恢复而宣称已经支持。

## 验证

单元测试在嵌套调用与递归图的每个 slice 保存/恢复，包括物理节点/边、
两个摘要端口及公式 local queue/active/edge cursor。最终记录序列、公式
目录/terms/fallback、来源结果和所有逻辑工作计数与不中断任务比较。
未确认页恢复后以零预算重送，重送不增加工作；额外验证只读、取消恢复、
AwaitingDelivery、旧代读取、过期 generation、发布失败、任务种类冲突、
错误源码/根/版本、重新校验后的非法游标/公式/交付记录、超限引用读取前
拒绝、缺冷块和独立索引生命周期。
原有内存拓扑测试继续覆盖未知规则 fallback、SummaryRef 环、40 层调用图、
指定调用链和最短见证。

真实进程探针覆盖未开始、物理遍历中途、公式准备中途及完成后退出，
在只读新进程中用不同 slice 预算继续。公式内容、来源实例及逻辑工作与
oracle 一致；按交付序号去重后的拓扑记录无遗漏/重复，Stage 次数为零，
恢复期间任务文件及原语义证据内容不变。空白修订明确拒绝续接。

2026-10-26 验证：Debug 全量 384/384 CTest（含 lit），GCC 与
ASan/UBSan 各 383/383 非 lit CTest；新增 6 项 checkpoint 单元和独立
拓扑跨进程矩阵通过。C++ 格式、Python 编译、40 份 Markdown 的链接检查
及 `git diff --check` 均通过。这些是正确性检查，不作为性能验收。

后续 [C4c3b](provenance-topology-storage-c4c3b.md) 已加入显式 v4 分段保存、
暂停卸载和查询测量；本页的 v3 编码保持兼容，拓扑恢复仍物化完整状态。
