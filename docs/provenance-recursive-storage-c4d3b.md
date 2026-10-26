# 递归状态分段存储与暂停卸载：C4d3b

2026-10-27。C4d3b 将 [C4d3a](provenance-recursive-checkpoint-c4d3a.md) 的
递归求解状态接入 TaskStore，增加 v6 流式分段、原子 head、暂停容量释放、
严格恢复及存储失败重试。v5 驻留快照继续兼容，求解规则和精度不变。
持久 CLI、结果交付确认及重送留给 C4d3c。

## API 与发布

RecursiveSummaryTask 增加 save_segmented、offload、TaskStore restore，
以及 state_memory/storage_stats/storage_failure。绑定仍是完整闭包、位置、
原始源码、唯一 task ID、实际摘要和单个通道，版本校验沿用 C4d3a。

```cpp
auto saved = task.save_segmented(store, expected_generation, 16 * 1024);
auto released = task.offload(store, expected_generation, 16 * 1024);
auto restored = RecursiveSummaryTask::restore(
    binding, store, head, summary, QueryKind::Value);
```

两个保存调用是不同选择；分别使用当前 head 的 expected_generation。
save_segmented 保留驻留状态。offload 先准备一个空壳，再成功发布新 head，
随后替换并销毁旧工作状态，释放方程、事实树、关系、索引、DFS、组件、
worklist 和活动 lease。保留绑定、逻辑进度/工作计数、存储句柄及 head。
外部结果副本、证据缓存和空 deque 的固定基线不属于被释放的工作 payload。

保存不会先构造完整 v5 字节串；记录直接进入一个叶缓冲，封存为 CAS 对象。
只有最后发布 manifest/head 后，新代才可恢复。已存在 head 的格式、绑定和
状态先校验；读写锁与 expected generation 防止覆盖其他写入者的新代。
旧代、复用对象和未发布对象沿用 TaskStore 配额及显式维护规则。

失败不卸载原工作表，原逻辑步骤/事实不变。head 发布前失败保留旧 head；
如果错误发生在原子替换之后，head 可能已经前进，调用方应重新读取代次。
已保存旧代仍可按原 TaskHead 恢复，不假装 IO 错误自动回滚或删除对象。
没有自动 GC 或 fsync/断电持久性承诺。

## v6 分段格式

manifest 使用 QueryTaskManifest，叶使用 QueryStateSegment。控制区保留
版本、身份、进度、工作计数、各阶段游标及两个活动事实位置；其后是九张
表的总行数与分段目录。每项目录含连续 first/count 和完整 ObjectRef。

| 表 | 固定记录宽度（字节） | 内容 |
| --- | --- | --- |
| 方程 | 22 | 节点、issues、merged/queued、事实起点/长度 |
| 事实 | 13 | 参数或来源族原子，按方程顺序扁平化 |
| 关系 | 14 | 原依赖、摘要/实参、关系和实参槽 |
| 组件 | 13 | cyclic、成员起点/长度 |
| 组件成员 | 4 | 节点 ID |
| seen | 1 | 已访问标记 |
| DFS 栈 | 16 | 方程及邻接游标 |
| 完成序 | 8 | 方程索引 |
| worklist | 8 | 待处理方程索引 |

每个叶有 54 字节头，包含 v6、任务、表号、起点和行数。段大小至少 76
字节，默认 16 KiB，不得超过 codec 单对象上限。manifest 保留完整性指纹，
CAS 再校验对象身份。恢复检查目录连续、对象种类/长度、表号/范围、事实
排序与分区；随后复用 v5 的图、SCC、队列及替换位置验证。

cache::Limits.bytes 约束每个叶和 manifest，不约束整次任务总状态字节。
elements 限制每张表，包括扁平事实/组件成员的总行数；现有对象超过恢复
上限时明确失败。目录过大也须提高单 manifest 上限，本轮没有多层目录。
段大小可改变后重新保存，绑定、通道和规则不能改变。

恢复逐段填充最终容器，不拼接完整 v5 字节串，也不生成完整中间字节快照。
方程、事实、关系及验证临时表仍完整驻留；这不是活动状态分页或总内存
上限。已发现证据仍须校验，冷读取与 C4d3a 相同，不重跑 Stage 或求解步骤。

## 卸载后的访问与失败

progress、stats、state_memory、storage_stats 和 storage_failure 不重新载入。
cancel/resume 在空壳上更新逻辑状态；不会删除记录或自动修改已发布 head。
需要持久化取消时仍须显式保存。Completed 的 cancel 不清空结果。

advance、checkpoint、result、links、components 明确恢复整个状态。读取失败
时保留空壳/存储引用，将可见进度报告为 Paused（取消则保持 Cancelled），
不增加逻辑工作；错误见 storage_failure，不伪装成新的推断缺口。修复对象
或 IO 后再次访问可恢复，成功时清除存储错误。result/links/components 无法
恢复时抛 runtime_error，checkpoint 返回 typed error，advance 返回进度。

引用、span 及计数引用仅保持到下一次 mutation/隐式恢复；调用方需要长期
保留时应复制。卸载空壳持有 TaskStore 存活锁；关闭其他句柄不会使该任务
被维护删除。显式 restore 的调用方负责保留自己的存储句柄。

## 完成、确认与维护

Running/Paused 保存为 Paused，取消保存为 Cancelled。**计算 Completed 保存
为 AwaitingDelivery**，尚不提供把它标为已确认 Completed 的 API。
读 result 或恢复完成状态都不代表消费端已提交结果；默认维护保留该 head。

默认维护只回收不活跃的 Cancelled/Completed；AllInactive 明确丢弃所有
不活跃任务。活动句柄、未知标记和 symlink 继续保护。消费确认、结果重送
及最终回收由 C4d3c 接入；本轮不能直接把 await 状态当作可清理终态。
[C4d2 CLI](provenance-recursive-cli-c4d2.md) 仍是新查询入口，仍报告
continuation_available=false，没有新增隐式保存或 continue 参数。

## 计量与真实函数检查

state_memory 分开报告方程容量、事实 payload、关系/派生邻接、索引、SCC
和 worklist。树链接、分配器开销、deque 基线、证据缓存及外部副本除外。
卸载后这些工作项均为零；它不是总进程 RSS。

storage_stats 记录段读写、payload 读入/提交、CAS 新增/复用、逻辑字节、
目录容量和编解码缓冲峰值。validation_payload_bytes 单独统计恢复验证
临时容器 payload/capacity，仍不包含树链接、分配器或图缓存。失败保存可
包含已尝试/写入的量；失败恢复通过 storage_failure 报错并保留原有统计。

Clang Debug 的一次确定性容量检查：真实源码为接收 Runtime x 的 512 步
加法链，实际记录 1,538 行方程；段大小 2,048，单对象上限 16,384 字节。

| 指标 | 字节或数量 |
| --- | --- |
| 驻留工作表 payload/capacity | 657,422 字节 |
| v5 完整快照 | 84,399 字节，超过本次单对象上限 |
| v6 逻辑记录 | 108,680 字节 |
| 提交 payload / 新增 CAS（含 envelope） | 115,128 / 118,068 字节 |
| 分段数量 | 59 |
| 自有编解码缓冲峰值（保存/恢复较大者） | 5,308 字节 |
| 恢复目录容量 / 验证临时 payload | 4,760 / 113,576 字节 |

测试验证 v5 在该上限下失败，v6 成功；卸载后工作 payload 为零，恢复并
重新编码与原 v5 一致。分段为单对象上限和退出时释放容量提供路径，增加
了描述记录/CAS envelope 的磁盘成本。这不是 Release 速度排名或 RSS 峰值
实验，不能据此宣称普遍内存/延迟收益。

## 验证与下一轮

单元测试比较七阶段及活动替换形态的 v5/v6 状态、逻辑工作和结果；覆盖
配额提高、只读、过期代次、发布前/后错误、旧代保留、v5 升级、缺段修复、
错误目录/表号、取消和未确认完成的维护保护。大函数验证跨单对象上限及
实际释放容量，并分别记录目录和恢复临时 payload。

独立进程对两种摘要、两个通道的七个阶段进行分段保存/卸载和只读恢复，
与 oracle 一致；恢复不改状态/证据文件，默认保护 Paused/AwaitingDelivery，
取消及显式丢弃按策略回收。已有 v5 独立进程案例继续通过。

验证：Debug 全量 CTest 420/420（含 lit），GCC 与 ASan/UBSan 各
419/419 非 lit CTest；C++ 格式、Python 编译、46 份 Markdown 文档及
`git diff --check` 通过。

下一轮 C4d3c 接入持久递归 CLI、单通道任务身份、最终结果确认/重送、
只读和取消。C4d 的该部分尚未全部完成，精度与 Stage 规则保持原有边界。
