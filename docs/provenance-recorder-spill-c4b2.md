# Recorder 临时 spill C4b2

状态：2026-10-16 完成 C4b 第二轮。分段构建器接入真实 Stage Recorder，
ProvenanceAnalysis 支持按块读取、辅助不可用状态及显式兼容恢复；实验 CLI
可以在同一次编译中 spill，并在证据失败时继续 Stage 和残留生成。
本轮结束时完整 C4b 仍待 C4b3 的工作集计量、释放与真实构建峰值验收。
设计见 [RFC](cache-architecture-rfc.md) 和[实现计划](cache-implementation-plan.md)，
底层协议见 [C4b1](provenance-spill-c4b1.md)。

## Recorder 和分析句柄

StageAnalyzer 新增独立 EvidenceBuildConfig 参数。默认 Memory 继续使用旧
Builder；其 spill 目录和缓冲额度不隐式生效。只有显式 Spill 才使用
EvidenceBuilder，不能从语义 cache-dir 推导写盘权限。

Recorder 中复制契约、代入参数、复制内联决策和检查已记录节点的路径均使用
持有存储的读取句柄；spill 分支持有 NodeLease。内存分支仍保留旧 Builder，
内部读取快照不改变节点、站点、决策或特化的身份和顺序。
摘要在函数体可能尚未完成时预留，在 finish 中绑定；物理块可以先封存。

[ProvenanceAnalysis](../include/pagos/stage/provenance_analysis.h) 提供：

- `evidence()`：共享不可变读取视图。普通 spill 编译和原生来源/效果/见证
  查询不构造完整 FrozenGraph；ProvenanceQueryTask 同时拥有分析与源码。
- `available()` / `failure()`：已知的记录或存储失败，不是新的 Stage 判断。
  它不预先保证外部文件永不丢失；查询仍会校验实际加载的对象。
- `construction_stats()`：C4b1 构建期缓冲、目录、摘要、封存和重读统计。
- `validate_details()`：逐块完整校验，用于发布准入；流式保留上一 lease，
  即使零缓存也不逐节点重复解码同一块。不恢复 FrozenGraph。
- `graph()`：兼容原裸引用/span 接口。spill 分析第一次显式调用时完整恢复，
  结果只建立一次，并发调用得到同一份存储。`graph_materialized()` 可检查
  是否发生该恢复。已经取得的图引用不会因为磁盘文件删除而失效。

原生 legacy/resumable 来源与效果展示使用 evidence 视图；legacy 保留原有
一百万状态兼容上限。spill topology 使用可暂停的物理遍历，不准备公式。
显式 compressed 查询仍使用旧公式索引，因而会恢复完整内存图。
C3a 快照和 C4a 函数格式的显式导出也使用兼容图；CLI 会报告 materialized。
这些操作不能作为“始终按块读取”或“编译内存保持有界”的证据。

## 证据失败与语义发布

磁盘失败可按 C4b1 策略回退至内存。如果驻留额度耗尽、严格策略拒绝写盘
失败，或构建期间旧块读取失败，Recorder 停止记录，保留辅助失败信息并释放
其构建器、活动调用帧和 weak HIR 索引。StageAnalyzer 的求值、诊断、Fuel、
特化/构造配额和 HIR/MIR 生成继续运行。

失败分析的 sidecar 保留已记录的元数据前缀，明确标为不可用；它不是完整
决策列表。其视图返回 typed EvidenceError，QueryTask 报 Failed、详情不可用，
不会把不可读证据解释为空来源或 Comptime。图兼容恢复失败抛出带原错误的
EvidenceUnavailable；原生解释展示标出 unavailable。非法 owner、重复摘要
绑定等一致性错误不会被这个存储错误边界吞掉。

在显式 spill 模式下，C2a/C2b 标量/数组与 C3b 残留模板的候选先编码并暂存。
Stage 结束、无语言错误且证据完整校验成功后，才逐项用现有 FileStore 原子
发布。这样较早完成的函数不会在后续 spill 失败前先留下成功缓存项。
失败分析或已知不可用证据会丢弃全部待发布项；读取现有语义缓存仍沿用
原依赖、资源和符号证据校验。Memory 模式保持原有发布时机。

待发布队列最多 4096 项、16 MiB artifact payload；超过上限只跳过该次发布，
不改变 Stage。该限制不包含键、容器和分配器开销，尚不是总内存额度。
`blocked-writes` 统计被拒绝/丢弃的候选数，`pending-payload-peak` 记录队列
payload 大小高水位。只读/off 模式没有新增语义缓存发布或隐式 spill。
C3a/C4a 的显式导出也拒绝已知不可用证据；不发布不完整成功根。

## 实验 CLI

```sh
build/debug/pagosc --evidence-spill=build/evidence-tmp \
  --spill-block-bytes=65536 --spill-resident-bytes=8388608 \
  --spill-cache-bytes=1048576 --spill-disk-bytes=268435456 \
  --spill-on-error=memory --spill-stats check example.pgs
build/debug/pagosc --evidence-spill=build/evidence-tmp \
  --spill-block-bytes=256 --spill-stats --provenance-query=resumable \
  --provenance-slice=128 explain-provenance example.pgs
```

`--evidence-spill=DIR` 显式启用同次编译临时存储，接受相对或绝对路径。
其余 spill 选项必须与此选项一起使用。block-bytes 必须为正；resident/cache/
disk 字节数允许 0；失败策略为 `memory`（默认）或 `unavailable`。
各额度仍具有 C4b1 的范围，不包含所有站点/根/决策、RuntimeTrace、解码临时
副本和外部 pin。默认没有 compiler-wide 内存硬上限。

spill 可用于 check、各 emit、explain-stage 和 explain-provenance；
不能与绕过 Stage 的 snapshot-read/evidence-read 同用。显式 snapshot-write/
evidence-write 可以与 spill 同用，但会触发兼容图恢复并增加内存占用。

证据不可用时，check/emit 的退出状态由正常 Stage/后端结果决定，编译产物
仍可执行。explain-provenance 无法提供所需详情时返回失败并输出辅助状态，
不新增语言诊断。Stage 原本失败时仍保留原错误码、位置和依赖路径。

spill-stats 在命令结束时写入 stderr，分别报告已知可用性/错误、是否恢复
内存图、构建节点/块/字节、驻留/缓存/live/peak、目录/摘要、释放容量和读取。
`query-reads` 是冻结视图计数，也包含发布校验、显式导出或兼容恢复的读取，
不是求值次数。query-cache/query-live 单列冻结视图的已加载缓冲容量；构建统计是 finish 时的
快照。它们不能简单相加当作整个进程 RSS。
临时任务目录随最后一个分析句柄/查询视图释放而清理；父目录和其他任务
数据保留，原有 lease 在任务目录删除后仍有效。

## 验证与剩余工作

[12 个 Stage 单测](../tests/unit/stage_spill_test.cpp) 使用真实解析/类型检查/
Stage 图，验证大函数和重复调用、嵌套/内联、局部 Comptime 契约、成员投影、
显式 Runtime 边界、互递归、来源/效果/最短见证及全部逻辑预算与内存模式
一致。512 步函数在构建时封存超过 100 块、释放实际数组容量，普通查询
没有恢复整图；这些是容量和行为断言，不是 RSS 改善结论。

另验证暂停任务跨 Analyzer/HIR 销毁、并发兼容恢复只建立一次、写盘回退、
严格失败、驻留/磁盘额度耗尽、旧块读取故障不替换原契约诊断；较早准备的
标量和残留候选在后续失败时全部拒绝发布。成功的 spill 仍能发布并在只读
模式命中。两个显式持久格式与原内存编码的对象内容一致；缺冷对象时拒绝
导出，不创建成功候选。

[跨进程 CLI 用例](../tests/lit/Inputs/run_stage_spill.py) 比较各命令输出，
对内存、正常 spill、证据不可用三种模式分别编译 LLVM IR 并执行，核对输入
读取顺序和结果；验证真实相对路径写盘、临时清理、读/off 缓存 IO 契约、
后期失败的发布抑制以及错误 flag 组合。

验证结果：Debug 全量 CTest **333/333**，其中 lit **392/392**；GCC 和
ASan/UBSan 的相关 CTest 各 **187/187**，对应 CLI/cache/provenance lit 各
**19/19**；Release 的 EvidenceBuilder 与 StageSpill 测试 **28/28**。
新增 12 个 Stage spill 单测和 1 个独立进程 lit 用例。文档检查覆盖 34 个
Markdown 文件，clang-format 与补丁空白检查通过。

本轮结束时完整 C4b 尚未完成。C4b3 需要计量并处理站点/根/决策、特化参数、weak HIR、
去重目录、RuntimeTrace、待发布队列与分配器的实际保留；还需真正崩溃进程
及孤立目录维护验收。同机 Release 的真实 Stage 构建/查询 RSS、总 IO 与延迟
测量必须覆盖这些工作集，出现可复现的构建峰值改善后才完成 C4b。

后续 [C4b3a](provenance-trace-c4b3a.md) 已实现 RuntimeTrace 前缀共享与
真实 Stage 前后测量。该轮确认路径表示改善，但尚未确认 spill 相对 memory
的峰值收益；完整工作集与崩溃验收继续留到 C4b3b。

后续 [C4b3b](provenance-working-set-c4b3b.md) 已完成工作集计量/回收、
真实进程崩溃/维护和大图构建峰值实验验收，完整 C4b 实验范围完成。
本页原有门槛和未完成项描述记录 C4b2 交付时的状态。
