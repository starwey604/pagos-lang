# 可恢复解释 CLI：C4c3c

C4c3c 将默认 `pagosc explain-provenance` 切到可续接的内存查询任务，保留
普通小查询的文本、来源排序及诊断格式。每轮使用 slice 继续，默认不再
把旧同步接口的一百万总状态上限当作永久 partial；默认编译、check 和
emit 不启动来源查询，也不隐式创建任务文件。`--provenance-query=legacy`
和旧同步 C++ 接口仍是兼容入口。

显式持久化入口使用 [C4c1](provenance-checkpoint-c4c1.md) 的绑定/确认协议、
[C4c2b](provenance-query-paging-c4c2b.md) 的可选原生分页，以及
[C4c3b](provenance-topology-storage-c4c3b.md) 的拓扑分段卸载。查询只读取
所绑定的不可变证据，不重新推断 Stage、不修改 HIR/MIR 或 AnalysisStats。

## 普通文本与显式任务

```sh
pagosc explain-provenance input.pgs
pagosc --provenance-slice=256 --provenance-query-stats \
  explain-provenance input.pgs
```

普通路径保持完整文本输出，不使用磁盘任务。需要跨进程暂停时，明确发布
函数证据，或在一次显式 evidence-write 中直接启动任务：

```sh
pagosc --evidence-write=.pagos/evidence --evidence-root=answer \
  explain-provenance input.pgs
pagosc --evidence-read=.pagos/evidence --evidence-root=answer \
  --query-dir=.pagos/tasks --query-max-slices=4 \
  explain-provenance input.pgs > first.jsonl
```

持久化任务一次只绑定一个唯一 root 和一个通道。需要
`--evidence-root=NAME`；名称不存在或匹配多项时拒绝，不创建新任务目录。
原生 sources/witness 默认查询 value，effects 使用 `--query-channel=effects`。
`--evidence-mode=topology` 准备两个通道的有限拓扑/公式，不接受单通道或
paged 模式。decisions 列表和 compressed/指定调用链执行不使用此入口。

只在给出 `--query-dir` 时使用任务存储；新任务默认生成随机 256-bit
身份，即 64 个 hex 字符的任务 ID。输出中的 task 字段可用于后续命令。
也可用 `--query-id=HEX` 自行提供 256-bit ID，但必须为每个新逻辑查询分配
新值，清理后也不要复用消费端已经记录过的身份。已存在 head 的 ID 不能
以 start 覆盖，必须 resume 或选择新 ID。

## 页交付与继续

显式任务输出 JSON Lines schema 1；每次调用最多一条 page，随后一条
status。没有结果页时只输出 status。普通文本输出不受影响。

| 记录 | 主要字段 |
| --- | --- |
| page | task、sequence、mode、channel、progress、records；原生另有 coverage |
| status | task、generation、state、root、mode、channel、storage、read_only、offloaded、acknowledge、stage_runs、work |
| 原生 status | coverage.precision、issues、all_sources、witness_found |
| 拓扑 status | structural=true、sources_solved=false；work 包含公式准备计数 |

原生 records 包含 source/witness、位置和从内到外的调用链；拓扑 records
包含 node/edge/summary-link、端点、关系和通道。node/context 是所绑定
任务快照内的局部引用，不能脱离 task 或跨修订作为稳定身份。消费端按
`(task, sequence)` 去重，不应按位置或裸节点号丢掉合法实例。

page.progress / status.state 描述执行及交付进度，coverage 描述求解精度。
递归等记录模型缺口可能使 completed 仍为 conservative；最短见证完成
不等于求全来源。拓扑 completed 只表示结构及符号公式准备完毕，不表示
所有具体读取实例都已求解。issues 保留 CoverageIssue 位掩码。

CLI 先保存初始 head，随后执行逻辑 slice；生成页后先成功保存未确认页，
再写 stdout。stdout 不会自动确认页。消费端先持久提交记录及 receipt，
再把输出中的 task 和页序号填入 TASK_ID / PAGE，明确确认并继续：

```sh
pagosc --evidence-read=.pagos/evidence --evidence-root=answer \
  --query-dir=.pagos/tasks --query-id="$TASK_ID" --query-action=resume \
  --query-ack="$PAGE" --query-max-slices=all \
  explain-provenance input.pgs > next.jsonl
```

没有 `--query-ack` 的 resume 重送未确认页，序号和 page 内容保持不变，
不推进查询工作。确认位置随下一次 head 保存持久化；若确认或新 head
提交失败，旧页仍可能重送，消费端沿用原 receipt 去重。没有消费端提交/
确认协议时不承诺 exactly-once。输出失败或进程在保存后退出，也可重送。
自动化调用方可预先提供新 ID，以便失败时无需从 stderr 找回任务 ID。

`--query-max-slices` 默认 1，0 表示不执行新的逻辑 slice，all 表示持续到
首个结果页或任务完成；有未确认页时始终先等待确认。`--provenance-slice=N`
继续同时设置 states/edges/bindings/output 的单次额度。它不是 IO 或页面
渲染时间的硬截止，深调用链的位置渲染也不属于推断工作计数。

| 退出码 | 含义 |
| --- | --- |
| 0 | 完成，或已成功保存取消状态 |
| 3 | 成功保存，但仍有工作或交付/确认待处理 |
| 1 | 证据、存储、绑定、输出或运行失败；诊断在 stderr |
| 2 | 参数错误 |

执行完但仍有未确认/未交付结果时 head 为 AwaitingDelivery。partial 执行
也可能已有未确认页，此时 head 仍为 Paused；status.acknowledge 和实际 page
指出本次需确认的序号。generation 是乐观更新版本，不是页序号。
结构化状态始终含工作计数；`--provenance-query-stats` 另在 stderr 输出函数
证据的 metadata/cold/cache 计量，不混入 Stage AnalysisStats。

## 恢复校验、只读和取消

resume/cancel 必须使用 `--evidence-read` 和任务 ID，不接受 evidence-write。
仍需解析、类型检查当前源码以建立模块身份，但不构造 StageAnalyzer，
即使 `--max-fuel=0` 也可继续已发布的证据查询。模块身份、目标/编译器
build、闭包、位置映射、原始源码、根/通道/模式必须匹配；空白修订也拒绝。
构建指纹包含 CLI 实现及头文件，默认配置变化也会使旧证据失效。
不存在的任务不会通过 resume 创建目录。

恢复先以只读 handle 校验绑定与确认请求，成功后才打开可写 handle。
错误源码、目标、模式、后端或未来确认号不改变任务 head。并发更新时，
过期 generation 的写入失败且不输出未提交页；不回退到 Stage 或偷偷
创建一个新查询。恢复所需证据/段缺失或损坏也明确失败。

只读查看/重放：

```sh
pagosc --evidence-read=.pagos/evidence --evidence-root=answer \
  --query-dir=.pagos/tasks --query-id="$TASK_ID" --query-action=resume \
  --query-read-only explain-provenance input.pgs
```

只读模式只展示已存页及状态，不确认、不执行新逻辑工作、不写文件，
不接受 query-max-slices 或 query-ack。它仍执行恢复校验，可能读取冷块；
不宣称只读查看仅需读取 head，拓扑状态仍需完整恢复。报告的 work 是
所存快照的计数，不把只读预览计入新的工作。

`--query-action=cancel` 保存未完成任务的取消状态，之后可显式 resume。
执行进程在每个切片前检查 head 代次；其他进程取消后，运行中的查询在
下一切片边界退出，且不输出未提交页。最终保存也须通过代次比较，避免
覆盖其他写入者已发布的状态。
取消不删除记录或证据。遍历已经完成时，不用 cancel 清空待确认页；应
确认后正常回收，或明确选择下面的丢弃操作。

## 存储策略与维护

默认 `--query-storage=memory`，仅在保存/退出时分段卸载；原生 sources/
witness 可显式选择 paged，拓扑保持驻留准备。恢复须选择原来后端；
不支持在 CLI 中将 memory 执行状态转换为 paged，反之亦然。

| 参数 | 默认值及作用 |
| --- | --- |
| query-disk-bytes | 256 MiB，本任务目录逻辑文件配额，含旧代与临时文件 |
| query-segment-bytes | 16 KiB，保存分段；paged 也用于新追加段 |
| query-max-records | 1,000,000，每张持久状态表上限；可在恢复时提高 |
| query-resident-bytes | 256 KiB，paged 追加尾部与解码缓存的容量预算 |
| query-cache-bytes | 64 KiB，paged 解码读缓存 |

这些参数均使用 `--名称=N`。记录上限必须在 1..UINT32_MAX，段大小须在
72 字节到 codec 单对象上限之间。paged 配置需容纳已有的最大解码段；
物理预算可以改变，查询身份及规则不能改变。memory 执行不因保存记录
上限而提前丢弃依赖，不能保存时明确报错并保留上一次 head。

resident/cache 不包含目录、图缓存/lease、编解码缓冲、输出页及调用方
结果，不是总进程 RSS。拓扑恢复仍完整物化。内存/分页取舍及写入代价见
[C4c3b 测量](provenance-topology-storage-c4c3b.md)，不因新 CLI 声称普遍加速。

配额或记录上限不足时不输出未提交页；提高预算或修复存储后，从原 head
继续。只有保存成功才卸载任务表。后台没有在线 GC、自动清理或 fsync/
断电持久性承诺；显式维护沿用 TaskStore 的生命周期锁：

```sh
pagosc reclaim-queries .pagos/tasks
pagosc reclaim-queries .pagos/tasks --discard-resumable
```

默认仅回收不活跃的 Completed/Cancelled，保留 Paused、AwaitingDelivery
及尚无 head 的可恢复命名空间。discard-resumable 明确放弃所有不活跃任务，
包括待确认或未完成结果；活动 handle、symlink 和未知标记始终跳过。
任务卸载后以及 stdout 等待消费端时，进程仍持有存活锁。关闭 handle 不
删除任务，也不修改语义证据目录。

## 验证与后续

真实 pagosc 进程测试覆盖 value/effects、witness、符号拓扑、memory/paged、
跨进程确认续接、只读页重送、零工作暂停、取消/回收、存储/绑定错误、
记录与磁盘预算提高后的恢复、缺段、随机新身份、阻塞 stdout 的存活锁
及运行中任务的跨进程取消。
恢复使用 max-fuel=0 验证没有 Stage 路径；显式 evidence-write 启动仅运行
一次 Stage，失败时不混入普通文本或创建任务文件。工作计数与不中断的大
slice 对照一致（slices 属调用次数，允许随调度改变），有序见证/拓扑及
消费端按页去重后的来源无遗漏或重复。分页渲染使用单个 invocation 值
访问，不为每页强制物化整张上下文兼容表。

普通文本与显式 legacy/resumable 的兼容比较及参数错误使用 lit/FileCheck；
新增单元测试验证分页上下文值访问和无兼容容量物化。C4c 的原生来源/见证、
符号拓扑准备与显式 CLI 实验范围已闭合。压缩来源执行/指定调用链的中途
checkpoint、完整 LSP/交互 UI 及未来递归求解器状态仍不在本轮；C4d/C4e
继续负责有限域求解及纯详情重放，不改变本轮已有的精度边界。

2026-10-26 验证：最终独立 Debug 全量 393/393 CTest（含 lit），GCC 与
ASan/UBSan 各 392/392 非 lit CTest；C++ 格式、Python 编译、42 份
Markdown 链接及 `git diff --check` 通过。首轮并行 Debug 中，既有
CacheStore.MissingRequiredObjectsPreventCandidatesAndCanBeRepublished 的
首次发布断言曾失败；单项连续 100 次复测和独立完整回归均通过，原因未
复现。本轮没有修改文件存储实现，保留这项验证记录供后续追踪。
