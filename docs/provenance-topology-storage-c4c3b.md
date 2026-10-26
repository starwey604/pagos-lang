# 拓扑分段保存、暂停卸载与查询测量：C4c3b

C4c3b 在 [C4c3a](provenance-topology-checkpoint-c4c3a.md) 的符号拓扑/公式
准备 checkpoint 上加入流式分段保存、成功发布后的容量释放、存储遥测和
真实源码测量。原生查询的 [C4c2b 分页](provenance-query-paging-c4c2b.md)
作为独立对照，不把节点读取缓存、查询状态和完整进程 RSS 混为一项。

## 分段拓扑格式

新增显式 `TopologyTask::save_segmented()` 与 `offload()`，默认段上限
64 KiB；已有 `save()` 和 `checkpoint()` 仍使用 C4c3a 的 v3 单对象格式。
TaskStore 恢复自动识别 v3/v4，同一拓扑任务可以显式更新两种格式；原生
查询和符号拓扑之间仍禁止复用任务身份。engine 和记录模型没有改变。

v4 QueryTaskManifest 保存绑定、控制字段、累计工作、未确认页元数据及
六张表的连续目录。每段使用 QueryStateSegment v4，54 字节头包含任务 ID、
表号、首记录和记录数；CAS、manifest 的专用校验及全部身份边界仍需验证。

| 表 | 固定记录 | 记录字节数 |
| --- | --- | --- |
| Queue | node | 4 |
| Formula directory | root、channel、fallback、term begin、term count | 18 |
| Flat terms | node | 4 |
| Calls | node | 4 |
| Local queue | node | 4 |
| Unconfirmed records | kind、node、target、relation、summary、port | 15 |

公式的变长 terms 展平写入单独表；编码直接枚举现有公式/terms，不先生成
完整 v3 字节对象或另一份扁平数组。每个文件及 manifest 受 codec 字节上限
约束，每表记录数受元素上限约束；完整逻辑状态可以大于单个对象上限。
发布前预检全部目录能否容纳，固定段连续性、大小与记录宽度必须一致。

恢复读取 root 一次，按段直接填充最终容器，验证 term 范围连续且总数
吻合，再运行同一套拓扑状态校验、重建哈希索引。它不拼接完整快照字节，
不运行 Stage、不重新准备已完成公式，也不重复累计逻辑工作。
恢复最终仍物化全部查询表和哈希索引；它不是活动拓扑的按需分页后端，
也不承诺恒定恢复时间或总工作集上限。原生来源查询仍使用已有独立分页后端。

分段、manifest CAS 先发布，head 最后按预期 generation 原子替换。
相同段复用既有 CAS；相同状态与段大小再次保存，CAS 新增字节为零，head
仍更新。旧代对象保留，旧 head 继续可读；本轮没有在线 GC、压缩或 mmap。

## 实际释放与独立索引生命周期

```cpp
auto saved = task.offload(store, expected_generation, 64 * 1024);
if (!saved)
    return;
auto memory = task.state_memory();
// memory.offloaded == true；任务持有的所有变长状态表容量为零。
// 下一次 advance/acknowledge/index/checkpoint 会显式读回同一 head。
```

只有完整保存成功才交换释放 queue、visited、公式记录/terms/索引、calls、
local queue/seen、来源族集合、未确认页和 active lease。保留绑定、根、
热进度/统计及持有 TaskStore 存活锁的卸载描述；关闭外部 store 不会使
卸载任务失去清理保护。恢复使用卸载时的 head，不自动换成最新代。

已发布的 FormulaIndex 可能正由调用方或压缩来源 QueryTask 持有。
卸载通过换入空索引解除任务的持有，**不会清空或修改外部索引**。
`state_memory().retained_index` 通过弱引用单独观察这部分仍存活的记录/
terms/索引容量；它不计入任务释放后的 owned 容量。外部引用仍可能使
进程 RSS 保持较高。`index()` 是明确的完整恢复入口，失败时抛 runtime_error。
任务的 stats 引用等不应跨 offload/恢复持有；独立返回的共享索引继续有效。

读取失败保留卸载描述和原 head，advance 返回 Paused/详情错误，
`storage_failure()` 给出具体 CheckpointError；修复文件后可重试。
卸载期间 cancel/resume 的逻辑进度在恢复后保留；未确认页保持原序号和
原记录，确认之前不推进工作。配额、只读、过期 generation 或 head 发布
失败不释放活动容器，不丢弃页，也不把原进度当作完成。提交后故障的调用方
仍应按 TaskStore 契约重新读取 head 确认 generation。

## 容量和 IO 计量

`TopologyStateMemory` 分别报告 queue、visited、公式记录/terms/哈希索引、
calls、local queue/seen、families 和 delivery 的容量。向量按实际 capacity
计，unordered 容器计值对象和 bucket 槽；不计节点链接、分配器开销、
共享控制块、图缓存/lease、目录/编解码临时缓冲及调用方的结果副本。
这些字段不能当作完整堆或进程 RSS，跨任务共享索引也不能重复相加。

`TopologyStorageStats` 只描述最近一次分段保存或恢复，不持久化为逻辑
工作：segment_reads/writes、bytes_read/submitted、cas_bytes_added、
reused_objects、peak_codec_buffers 和 logical_bytes。

- bytes_submitted 包含提交给 TaskWriter 的段和 manifest payload；
  cas_bytes_added 按成功写入前后的配额计数差计量，包含新 CAS envelope，
  不含 head 替换。复用对象不新增 CAS 字节。
- bytes_read 计显式 root/叶对象读取，保存时含旧 root 的绑定校验；
  不含 FileStore 内部 dedup/commit 验证。测量工具另外统计全部任务读写
  请求，包括缺失探测、head 和重复验证；它们不是物理磁盘访问次数。
- peak_codec_buffers 是观测到的保留 root/control 与单段字节向量 capacity
  高水位，不含 CAS envelope 编码缓冲、目录元数据和分配器瞬时扩容。
- logical_bytes 是六表固定记录总字节数，排除控制和目录。用新增 CAS
  字节除以它，只描述这一次保存的格式开销/写入放大，不是整个文件系统
  或设备的写放大。

## 可复现测量

新增 `pagos_query_task_bench` 和
[benchmark_query_tasks.py](../scripts/benchmark_query_tasks.py)。独立进程先
对真实 `.pgs` 运行一次 Stage/Recorder 并发布函数证据；所有查询样本均只
解析/检查及读取证据，Stage 次数为零。四个模式为 native-memory、
native-paged、topology-resident 和 topology-segmented；拓扑另测物理阶段
和公式准备中途两个保存点。每个样本校验恢复前后的全部逻辑计数，并比较
两种原生模式、两种拓扑模式和不同保存点的最终内容/工作量。

```sh
cmake --build --preset release --target pagos_query_task_bench
python3 scripts/benchmark_query_tasks.py --repeats 3 --warmups 1 \
  --date 2026-10-26 --output benchmarks/results/query-task-c4c3b.json
```

prefix/save/restore/continue 独立计时。原任务和旧 view 在恢复前销毁，
新 view 的热元数据准备不计入 restore 时钟。比较保存前/后、恢复及完成
容量；按 advance 边界采样 owned 高水位，目录另报。文件字节包含 CAS
包头、head、标记、旧代以及未发布叶对象；原生分页在保存后继续生成的叶
对象也包含在最终目录中。不得把这个最终目录大小当成初次快照大小。

wait4 RSS 包含整个子进程的前端、证据缓存、保存/恢复、兼容结果/摘要副本
及析构，也可能有 fork/exec 父进程 RSS 下限；不等于 query-only 峰值。
测量按顺序启动全新 Release 子进程，在构建和测试停止后执行，使用暖 OS
页缓存，没有清缓存、绑定核心或固定频率。三次样本只作本机描述性比较，
不作普遍速度收益或 RSS 上限的结论。原始配置、机器/文件系统、版本、
二进制/CMake/源码 SHA、逐样本与分布均保存到报告。

## 本机结果

[原始报告](../benchmarks/results/query-task-c4c3b.json) 包含 24 组、每组
3 次正式样本与 1 次预热，共 72 个测量样本；四份源码另由独立发布进程
运行 Stage。机器为 AMD Ryzen AI 9 HX 470，临时任务目录在 btrfs。
报告保留构建时基线提交和 dirty 标记，并记录本轮源码/二进制 SHA。
以下仅列中位数；完整逐样本和 IQR 见报告。

原生查询的 sampled owned 峰值（字节，分页目录另外计量）：

| 源码 | Memory | Paged | Memory 续跑 ms | Paged 续跑 ms |
| --- | ---: | ---: | ---: | ---: |
| 257 个独立来源 | 39,052 | 108,304 | 0.77 | 0.75 |
| 2,049 个独立来源 | 312,332 | 163,456 | 0.27 | 2.61 |
| 256 步真实函数 | 33,976 | 100,224 | 0.27 | 0.33 |
| 1,024 步真实函数 | 132,664 | 147,312 | 0.11 | 1.34 |

大宽图的分页 owned 容量约下降 48%，另有 792 字节目录；三份较小/链状
输入没有容量收益，固定追加缓冲会占用更多空间。续跑也未显示普遍加速。
这支持将分页视为显式容量控制，不支持默认将小查询切换到磁盘。
最终 native-memory/paged 目录大小不能直接比较快照效率，因为后者还包含
继续执行产生但尚未发布新 head 的叶对象；报告单独列出分页 IO 计数。

公式准备中途的拓扑任务：

| 源码 | 暂停前 owned 字节 | offload 后 | v3/v4 保存 ms | v3/v4 恢复 ms | v4 保存放大 |
| --- | ---: | ---: | ---: | ---: | ---: |
| 2,049 个独立来源 | 250,804 | 0 | 0.73 / 0.87 | 4.34 / 4.34 | 1.03 |
| 1,024 步真实函数 | 61,232 | 0 | 0.32 / 0.45 | 1.02 / 1.02 | 1.10 |

这两组的已计 codec 缓冲高水位分别为 23,380 和 11,860 字节；更小的
frontier 快照因固定头/目录开销，保存放大约为 1.69–1.80。
分段保存没有表现为普遍更快；优势是可跨单对象大小限制，并在成功暂停
后释放任务表。继续遍历后，v3/v4 活动 owned 高水位基本相同，不宣称活动
拓扑内存有界或 RSS 已下降。所有模式的恢复前计数保持不变，最终公式/
来源实例和逻辑工作一致，没有重复计费。

## 验证和剩余范围

专项测试覆盖嵌套/递归拓扑每个阶段的极小段卸载/恢复、未确认页、宽
frontier、共享索引不失效、只读/配额/过期 generation/发布失败、v3/v4
切换与旧代读取、超出单对象上限、损坏目录、重新封装有效 CAS 的错误段
身份、缺段和读取失败后的取消/重试。跨进程矩阵同时覆盖 v3/v4，最终
记录、公式和工作量等价，只读恢复不修改任务或语义证据。

benchmark-smoke 在两份小源码上运行四种模式及两个拓扑保存点，检查
容量释放、分页预算和恢复等价；它是正确性回归，不作为性能测量。
下一轮 C4c3c 接主解释 CLI 的显式继续/暂停、任务保存/恢复及维护，并保留
普通文本兼容。压缩来源执行中及指定调用链 QueryTask 的 checkpoint 仍是
单独扩展；本轮可恢复的是其公式准备任务。

2026-10-26 验证：Debug 全量 391/391 CTest（含 lit），GCC 与
ASan/UBSan 各 390/390 非 lit CTest；本轮新增 6 项分段/卸载单元测试、
扩展跨进程 v3/v4 矩阵与 benchmark smoke。C++ 格式、Python 编译、
41 份 Markdown 链接和 `git diff --check` 均通过。正式 Release 测量在
这些构建/测试结束后串行运行；正确性运行与性能样本分开记录。
