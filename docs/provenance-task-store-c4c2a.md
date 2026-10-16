# 查询任务分段保存与暂停卸载：C4c2a

C4c2a 在 [C4c1](provenance-checkpoint-c4c1.md) 的精确修订绑定、游标和
确认协议上，加入独立任务存储、分段编码、原子 head、配额及显式清理。
暂停任务可以在保存成功后释放查询表容量，新进程也可按 head 恢复；
恢复不运行 StageAnalyzer、不重放已扫描边、不重复累计查询工作。

本轮是 C4c2 的第一步。**活动查询仍使用完整的内存队列和哈希表**；
重新执行查询前会恢复全部查询表。C4c2b 才实现活动期间的按段驻留及磁盘
visited 索引。本轮不宣称活动查询峰值 RSS 有界或已经降低。

## 独立任务目录与发布

```text
<explicit-task-parent>/
  pagos-query-<task-id>/
    session.lock      # 识别标记与共享存活锁
    writer.lock       # 独立的写事务锁
    head              # generation、状态、manifest ObjectRef、校验
    objects/xx/<hash> # 本任务的不可变 CAS 对象
    tmp/              # 写入中的文件；强杀可能留下残片
```

`cache::TaskStore` 是显式开启的任务索引，不创建语义 QueryKey/候选。
图证据与 semantic cache 不在该命名空间；Recorder 临时目录也使用不同
的前缀和标记。默认编译、普通查询和打开只读任务均不创建任务文件。
关闭 handle 不删除已保存任务。

一个任务 ID 只代表一个逻辑查询；新查询必须分配新 ID。更新带调用方预期
的 generation，零表示尚无 head。`begin` 获取独立写锁并核对 generation，
过期更新返回 `StaleGeneration`；读者仍可在写入期间读取旧 head。
查询适配器另外核对旧 manifest 的闭包、源码、根/通道/模式和版本，不能
用相同任务 ID 把 head 切到另一查询目标。

`QueryTask::save` 先按固定记录数写各段，再写 manifest CAS，最后原子
rename 更新 head。未变的完整段、末段和 manifest 可直接复用已有 CAS。
head 可见时，其所有段已发布；替换前失败保持旧 head。替换后进程退出，
新 head 完整可读。发布后的故障回调可能报告 IO 失败而 head 已更新；
此时内存状态仍保留，调用方应重新读取 head 决定下一次期望 generation。

旧代 CAS 保留到显式清理整个任务。这样持有旧 head 的读者不会被在线
删除破坏。恢复使用调用方选定的 head，不自动迁移到另一源码或任务。
本轮没有 fsync/断电持久性、在线跨代 GC 或数据库事务承诺。

## 分段格式与恢复

新增独立对象种类 `QueryStateSegment` 与 `QueryTaskManifest`。C4c1 的
单对象字节格式和 API 继续兼容。任务 manifest 记录同一套版本、绑定、
active 阶段、edge cursor、交付范围、确认位置和累计工作计数，并列出：

| 表 | 固定记录 | 记录字节数 |
| --- | --- | --- |
| Contexts | call、parent、summary owner | 12 |
| Queue/visited/前驱 | node、context、predecessor | 16 |
| Sources | node、context、site | 12 |
| Reversed witness | node、context | 8 |

Queue 的已访问部分和 frontier 共用一份规范记录，visited 哈希索引在
恢复时重建。本轮不保存 STL/LLVM 容器布局，也不实现可直接查询的磁盘
哈希索引。每段带自己的格式版本、任务 ID、表种类、首记录位置和记录数。
manifest 的目录要求各表从零开始、连续、无重叠，引用大小和固定记录
宽度吻合；数量、字节上限、CAS 校验、段身份及全部查询状态边界均需验证。
编码在写任何段前先检查整张目录能否容纳，避免明知无法发布仍写大量叶对象。

默认分段字节上限 64 KiB；单个对象与 manifest 仍受 codec 限制，表记录
数也受 `Limits::elements` 限制。编码只额外持有一个分段缓冲和 manifest
目录，不先构造完整 C4c1 字节对象；CAS 层另有有界编码/校验缓冲。
恢复逐段读取，但最终仍重建所有向量、visited/context 索引和 active lease。
任一段缺失、损坏或引用非法都使恢复失败，不发布半份可用查询状态。

## 暂停任务释放容量

```cpp
auto store = cache::TaskStore::open(task_parent, task_id,
                                   cache::TaskAccess::ReadWrite);
if (!store)
    return;
auto old = store->head();
if (!old)
    return;
const auto generation = *old ? (**old).generation : 0;
// task 是使用同一 task_id 与 FunctionEvidenceView binding 创建的任务。
auto saved = task.offload(*store, generation);
if (!saved)
    return; // 游标仍在内存中，未因保存失败丢弃。
auto memory = task.state_memory();
// memory.offloaded 为真；queue/visited/contexts/context_index/sources/witness
// 所报告的容器容量均为零。之后 advance 会先恢复这些表。
```

`save` 保留内存状态；`offload` 只在 head 发布成功后，以空容器交换释放
六类向量/哈希容量，并释放 active evidence lease。它保留绑定、根、进度、
覆盖状态、累计工作、确认元数据，以及持有生命周期锁的 TaskStore handle。
`state_memory()` 只计这些容器容量，不是进程 RSS；源码/HIR、图读取缓存、
目录/控制对象、调用方持有的页和分配器开销都不在其中。

下一次 `advance` 会先读回同一 head；小 slice 不会被用于重算旧工作。
卸载期间 `cancel/resume` 修改的当前进度在读回后仍保留。`result()` 和
`invocations()` 是显式兼容物化入口，也会恢复全部表，缺失时抛出
`runtime_error`；`advance` 则返回 `Failed`/详情不可用并保留具体存储错误。
跨进程可以用只读 TaskStore 和相同 binding/head 调用 `QueryTask::restore`。

## 配额与生命周期

配额按任务目录内普通文件的逻辑长度计量，包括 CAS 的 49 字节 envelope、
标记、head、旧代对象及强杀遗留的 tmp；另为 head 替换的临时副本预留
127 字节。复用已验证对象不重复计费。文件系统块、目录项和 inode 元数据
不计入该上限；它不是物理设备使用量或总进程内存上限。
写锁保护配额检查与对象/head 发布；已超限的残片会阻止下一次写事务，
不会绕过配额继续累积。IO、超限、只读或过期 generation 不改变原查询语义。

所有 TaskStore handle（包括只读）持有共享存活锁，卸载任务也保留该锁；
写事务另持独占写锁。同进程 handle 共用存活锁 fd，关闭其中一个 handle
不会意外释放其他 handle 的进程级锁。最后一个 handle 关闭与新 handle
并发打开时，注册表要等旧 fd 关闭后才允许新建 fd；显式维护取得独占
存活锁后才可删除。

`cache::reclaim_tasks(parent)` 默认只清理已无 handle 的 `Cancelled` 和
`Completed` 任务。未发布 head、暂停以及等待结果页消费确认的任务保留。
遍历完成但交付未结束时，head 使用 `AwaitingDelivery`，不能因完成遍历
就当作生命周期结束。内存中取消不会自动写盘；需要再次保存取消状态。
`TaskCleanupPolicy::AllInactive` 是显式丢弃所有不活跃任务的选项，包含仍可
续接的任务。取消任务可能仍含旧结果，明确清理意味着放弃继续消费它们。

未知/不完整标记、symlink 命名空间和活动 handle 始终跳过；没有启动扫描、
默认编译清理或隐式任务取消。已识别但无 head 的强杀残片，可用 AllInactive
回收；更早的未知初始化残片保留离线检查。本轮维护接口在库层，`pagosc`
续接/维护参数与主解释入口的迁移仍留后续。

## 验证与后续

专项测试覆盖极小分段下逐 slice 的值/效果与见证续接、页重送、2,049 个
独立来源构成的大 frontier、实际容量归零、全部调用/来源/见证表释放、
复用不变对象、非法目录、缺段、配额、只读、generation 冲突和故障时内存续跑。
跨进程探针覆盖 head 原子替换前后强杀、只读无写入、取消恢复、读者存活锁、
同进程关闭其他读者后锁仍有效、默认保留未完成任务和显式回收。
所有恢复与继续结果、调用上下文和累计工作同不中断查询比较；语义证据
文件内容不受任务保存、取消或清理影响。

下一轮 C4c2b：活动任务按段驻留、磁盘 visited/context 索引、读缓存与物理
容量限制，以及恢复不需要同时物化全部查询表。C4c3 再补符号拓扑任务、
正式测量写入放大/恢复延迟/工作集和主解释 CLI 的接入。

2026-10-16 验证：Debug 全量 368/368 CTest（含 392/392 lit），GCC 与
ASan/UBSan 各 367/367 非 lit CTest。新增任务存储跨进程测试分别约
2 秒和 6 秒完成；这些是正确性运行记录，不作为性能验收。C++ 格式、
Python 编译、38 份 Markdown 的链接检查和 `git diff --check` 均通过。

后续 [C4c2b](provenance-query-paging-c4c2b.md) 已实现显式活动查询分页、
磁盘排序索引与有界解码缓存。v1 保持本页所述恢复行为；v2 流式校验全部
状态但不物化完整表，目录和兼容物化结果不计入尾部/缓存预算。
