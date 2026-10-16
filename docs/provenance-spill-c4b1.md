# 构建期证据封块 C4b1

状态：2026-10-16 完成 C4b 第一轮的独立存储设施。新增分段追加构建器、
临时 spill、构建期旧块 lease、摘要延后绑定和额度/失败协议。
后续 [C4b2](provenance-recorder-spill-c4b2.md) 已接入 Recorder/CLI，
以下保留本轮范围；完整 C4b 的真实构建峰值验收仍未完成。
本轮不声称降低真实 Stage 构建峰值。设计与余下验收见
[Cache Architecture RFC](cache-architecture-rfc.md) 和
[实现计划](cache-implementation-plan.md)。

## 接口与存储

[EvidenceBuilder](../include/pagos/stage/evidence_builder.h) 是单任务追加构建器。
它与原有 Builder 并存，不改变 FrozenGraph 的引用/span 生命周期。
`add()` 产生图内 NodeId；`pin()` 返回拥有节点及边存储的 NodeLease。
活动块被 pin 时返回单节点及其边的不可变快照，后续追加/封存不会使其悬空；
已封存块返回共享不可变解码缓冲，不暴露可淘汰块的永久裸引用。

`seal()` 可以显式调用，追加也会在逻辑节点/边字节阈值、codec 上限或驻留
缓冲额度要求下提前封块。一个节点可以大于块阈值，但必须满足单对象编码
和驻留额度；不能把单节点边列表任意截断。直接边只允许指向已存在的节点。
物理块使用 C4a1 的 EvidenceBlock v1，边目标仍是同一图内的全局编号，
块内的边偏移是局部编号。这不是跨编译函数身份，也不突破 32 位 ID 容量。

Memory 模式只保留内存段，无文件 IO；Spill 模式在 CAS 原子发布成功后移除
构建器对该活动块的所有权，实际释放 node/edge vector 容量。
旧块读取有独立有限缓存和弱引用：外部 lease 仍 pin 时可以重用，缓存淘汰
不影响已有 lease。构建器本身不支持并发修改，冻结视图支持并发只读查询。
本构建器不建立全图去重索引；原 Builder 的 SelectiveIntern 模式仍保持原状。

`reserve_summary()` 创建独立目录槽，Call/SummaryRef/Parameter 可以在摘要
尚未绑定时写入物理块，支持有限递归和跨块前向摘要引用。`define_summary()`
只允许绑定一次，目标必须是本图已有节点；物理块不需要回写。
`freeze()` 检查所有预留摘要已绑定，封存最后的活动块，然后返回 BuiltEvidence：
构建统计及共享 EvidenceView。冻结后的来源、效果、见证和物理拓扑直接从
该视图读取，不先恢复整图。整个过程中不发布语义查询 manifest。

示意接口如下，错误分支应由调用方处理，不是新 Stage 规则：

```cpp
provenance::EvidenceBuildConfig config;
config.mode = provenance::EvidenceBuildMode::Spill;
config.directory = "build/evidence-spill";
config.block_bytes = 64 * 1024;
provenance::EvidenceBuilder builder(config);
auto node = builder.add(provenance::Rule::Origin, site_id);
// Check node before use; reserve/define/pin/seal also return expected.
auto frozen = std::move(builder).freeze();
// frozen->view supports QueryTask and EvidenceTopologyTask.
```

## 临时目录和 IO 边界

Spill 必须显式选择模式和父目录，不能从语义缓存目录推导，也没有全局默认
写入位置。构造和空图冻结不创建目录；第一次写入尝试才创建唯一的任务目录。
不同构建器的任务目录隔离，所有权只覆盖各自任务目录，不删除父目录或兄弟
数据。FileStore 的叶对象发布复用完整 CAS 校验、临时文件及原子发布协议。

任务目录由 RAII 管理。构建器销毁或最终冻结视图销毁时尽力清理自己的目录；
视图存活时目录继续可读，已解码 lease 跨目录清理仍有效。异常退出/进程崩溃
可以留下孤立任务目录；新构建器不会发现或恢复它们，也不会把它们当作成功
语义缓存。离线过期目录维护和真实崩溃进程验收留到后续轮次。

读取失败不会自动修复或补写。构建期发现旧块缺失、损坏、错误版本或 IO
失败后，构建器进入不可用状态并拒绝冻结。冻结后发生文件丢失或损坏，则
对应 QueryTask 失败、详情不可用；冻结不会预先重新扫描所有已发布冷对象，
因此它不能保证文件在后续查询期间一直可读。

## 额度与失败

配置分开管理：

| 配置 | 范围 |
| --- | --- |
| `block_bytes` | 每段逻辑 node/edge 字节阈值，默认 64 KiB |
| `resident_bytes` | 构建器拥有的活动、内存封存段和读缓存容量，默认 64 MiB |
| `cache_bytes` | 已封存块的读缓存，默认 1 MiB，可为 0 |
| `disk_bytes` | 尝试发布的 CAS payload + 49 字节封装，默认 256 MiB |
| `store` | 单对象 codec/大小上限及可注入 IO 故障 |

缓存会先被淘汰，再考虑提前封块释放活动容量。外部 lease 保留的缓冲单独
计入 live/peak，不受缓存或构建器所有权额度强制回收。临时编码/解码副本、
目录/摘要、分配器和文件系统元数据不在驻留缓冲额度内，**这不是进程 RAM
硬限制**。冻结视图的缓存额度还会扣除不可淘汰内存段占用。

写入失败或磁盘额度不足时，默认 KeepMemory 策略保留当前内存段，并记录
非致命 spill_error；后续追加仍必须满足驻留额度。Unavailable 策略直接返回
辅助 EvidenceError。内存额度耗尽、单节点超过 codec 上限或旧块不可读时，
返回持久的不可用状态，释放构建器持有的图缓冲，已有 lease 不受影响。
后续追加、摘要操作和冻结返回相同失败，不能发布一份缺块的成功图。

每次真正写入前先预留 payload/封装额度。即使对象已经发布、API 随后报错，
这次尝试仍计费；失败前未实际占用的空间也可能被保守计入。这保证孤立叶
对象不会绕过磁盘额度，但不等于精确的文件系统占用。

非法引用、非法 owner、重复摘要绑定和未绑定摘要冻结属于调用方/构建器
一致性错误，仍抛出逻辑异常，不能被内存回退吞掉。追加、摘要预留、读取、封存和冻结中的 IO、codec
容量及分配失败返回独立 EvidenceError，不产生 E2001 或其他语言诊断。Recorder 接入后如何
在此状态下继续 Stage 并阻止缺必需证据的缓存发布，仍是 C4b2 的工作。

## 计量与验收

EvidenceBuildStats 分别报告活动/内存封存/读缓存容量、live/peak、目录和摘要
vector 容量、释放活动容量、成功写入及保守磁盘计费、构建期重读对象/字节。
live 包含外部 lease 保留的解码块和活动节点快照；peak 是保留缓冲的采样
高水位，不是进程 RSS。站点/根/决策、weak HIR 表、特化参数和 RuntimeTrace
还不属于这个存储层，其实际容量计量和释放必须在 C4b2/C4b3 另行完成。

[16 个单测](../tests/unit/evidence_builder_test.cpp) 覆盖：

- 默认内存模式及显式内存模式不触发文件操作；20,001 节点跨块链在追加
  期间释放封块容量，构建器拥有的缓冲保持在配置额度内；冻结查询按块读取。
- 与原 Builder 比较值/效果来源、最短见证、调用上下文、递归截断、控制边、
  契约、聚合投影与 Runtime 边界的初始化效果。
- 递归摘要在封块后才绑定；活动节点快照跨增长、缓存淘汰、外部 pin 超额度、
  视图复制/销毁及目录清理；取消/恢复拓扑与并发查询。
- 部分写入失败的内存回退、严格不可用策略、磁盘/驻留/codec 额度耗尽、
  发布后故障的孤立对象计费、独立任务目录和清理。
- 旧块缺失/损坏、任务目录删除、构建期/冻结视图的读取 IO 故障、无修复写入；
  非法配置、引用及未绑定/重复摘要与辅助存储错误分开处理。

验证结果：Debug 全量 CTest **321/321**（其中 lit **391/391**）；GCC 与
ASan/UBSan 的 Cache、Provenance、StageDecision、EvidenceView、FunctionEvidence、
EvidenceBuilder 相关 CTest 各 **175/175**。Release 的新构建器测试 **16/16**，
Debug 更新后的相同测试也 **16/16**。文档链接、clang-format 与补丁空白检查
通过。

本轮容量断言来自存储层单测，不是实际函数编译或 RSS 收益实验。后续
[C4b2](provenance-recorder-spill-c4b2.md) 已接入 Recorder/ProvenanceAnalysis、
不可用状态与发布边界，并提供独立 spill CLI；C4b3 完成全部工作集计量、
真实 Stage 释放与 Release 构建峰值验收后，才能标记完整 C4b 完成。

后续 [C4b3b](provenance-working-set-c4b3b.md) 为临时任务补充生命周期锁
和显式死亡目录回收，并完成大图构建峰值实验验收。本页容量与文件描述
记录最初 C4b1；当前任务另有 26 字节版本锁标记，不计入 CAS 逻辑额度。
