# 活动查询分页与磁盘索引：C4c2b

C4c2b 在 [任务存储 C4c2a](provenance-task-store-c4c2a.md) 上加入显式的
活动查询分页。Value/Effects、调用上下文替换和最短见证使用同一遍历规则，
完整队列、去重表和上下文索引无需同时驻留；暂停、保存及新进程恢复不运行
StageAnalyzer，也不重放或重复计费已完成的查询工作。普通查询仍默认使用
内存容器；实验接口在库层，主解释 CLI 和符号拓扑任务留给 C4c3。

## 六张表与存储格式

`QueryTask::begin_paged` 要求显式 TaskStore、新任务 ID 和已验证的函数证据
binding。同一 ID 已有 head 时必须恢复，不能重新开始并覆盖旧任务。

| 表 | 内容 | 磁盘记录字节数 |
| --- | --- | --- |
| Contexts | call、parent、summary owner | 12 |
| Queue/前驱 | node、context、predecessor | 16 |
| Sources | node、context、site | 12 |
| Reversed witness | node、context | 8 |
| Visited index | `(node, context)` 到 queue 位置 | 16 |
| Context index | `(call, parent)` 到 context ID | 16 |

各表保留固定容量追加尾部。满尾部在下一个逻辑步骤前封存为不可变 CAS 段；
索引尾部排序后成为带最小/最大 key 的 run。读取数组先按记录位置定位段，
索引查询先查尾部，再按 key 范围筛选磁盘 run，段内二分查找。六表共享
一个可淘汰的解码 LRU 缓存。访问接口返回记录值，不把段内引用带过淘汰点。

索引目前是多个排序 run，范围可能重叠；最坏情况仍需访问多个 run。
本轮没有 B-tree、LLVM OnDiskHashTable、索引合并、压缩或 mmap，不能
把该实现视为已完成查询性能验收。是否引入这些机制应由后续读放大测量决定。

`QueryStateSegment` 和 `QueryTaskManifest` 使用独立的格式版本 **2**；
查询 engine/记录模型、语义闭包、位置/源码修订和交付协议保持原约束。
段头 54 字节，记录连续，索引目录额外记录范围。旧分段 v1 和单对象
checkpoint v1 仍可按原接口恢复，未实现旧任务到活动分页的自动迁移。
`checkpoint()` 可显式导出有 codec 大小上限的单对象兼容格式。

## 容量预算及边界

`QueryPagingConfig` 默认段上限 16 KiB、读缓存 64 KiB、驻留预算 256 KiB。
六张表均预留相同记录容量，按最大记录宽度 16 字节计算。配置必须容得下
六个尾部及至少一个最大解码段；不足时在写查询对象前返回 LimitExceeded。

`resident_bytes` 限制六表尾部的实际 capacity 字节与解码读缓存之和。
`state_memory()` 分别报告六表 capacity、cached、directory、compatibility
和 peak_buffers；读缓存淘汰后才分配新解码页。分段目录/范围按段数增长，
单独计量并受 manifest codec 上限约束，**不在驻留预算内**。同样不计入的
还有容器链接/分配器元数据、CAS 编解码和 IO 临时缓冲、证据缓存及 lease、
输出页和调用方结果、源码/HIR/MIR。这不是总进程 RSS 的硬上限。
物理 paging_stats 和 peak_buffers 仅计当前加载的后端，恢复时重新开始；
持久化的逻辑 QueryStats 不因此清零。

`result()` 显式产生完整、排序的独立兼容结果；`invocations()` 显式物化
上下文 span，容量计入 compatibility，并在下一次 advance/offload 释放。
需要流式消费时使用 QueryPage 及 `(task, sequence)` 确认协议。

`save()` 封存剩余尾部，写六表 manifest，再以预期 generation 原子发布
head。活动查询提前写出的段不会自动推进 head；崩溃只能回到最后一次显式
保存的逻辑进度，未发布叶对象由既有任务维护策略处理。`offload()` 成功后
释放尾部、缓存、目录、兼容上下文和 active lease，保留任务存活锁；
下次访问恢复原分页配置。旧 head 的叶对象继续保留，支持持有旧代的读者。

## 恢复、故障和只读访问

`restore_paged()` 逐段验证所有查询表、目录、排序 run 与索引双射，并验证
前驱、上下文、来源、见证及交付边界，只保留配置允许的缓存；不重建完整
vector 或内存哈希表。它仍需扫描全部查询状态，**不承诺恢复启动 IO 或时间
恒定**。普通 TaskStore `restore()` 遇到 v2 会选择默认分页配置；自定义
配置应显式调用 restore_paged，恢复配置至少需要容纳已存的最大解码页。

每个逻辑步骤先处理满尾部，再记录轻量回退点。步骤中查询状态的磁盘读取失败时撤回
该步的追加、edge cursor、阶段和工作计数，已经完成的步骤保留。配额不足、
查询状态 IO 失败、缓存/记录限制等通过 `storage_failure()` 返回具体错误并暂停，
不会丢弃 frontier、把旧进度当作完成或永久裁剪依赖边。交付页只有完整构造
成功才推进游标；未确认页读取失败后，恢复仍重送原序号和原范围。
卸载状态读取暂时失败同样可在下一次访问时重试，取消需显式 resume。

只读 TaskStore 可校验及读取快照，无写入；继续遍历可使用固定尾部，
需要封存时返回 ReadOnly 并暂停。调用方可用 `resume_with_storage()`
显式换成同目录、同任务 ID 的可写 handle，或增大磁盘配额。该操作不推进
查询工作、不发布 head，不允许切换到另一任务命名空间。save 失败也不会
隐式改变活动后端的只读/可写属性或配额。

```cpp
auto task = QueryTask::begin_paged(binding, store, root, QueryKind::Value,
                                   false, paging_config);
if (!task)
    return;
auto page = task->advance(slice);
if (task->storage_failure())
    return; // 保留任务；修复存储或显式增加配额后继续。
if (page.sequence)
    (void)task->acknowledge(page.sequence); // 在消费方提交记录后确认。
auto head = task->offload(store, 0);
if (!head)
    return;
auto restored = QueryTask::restore_paged(binding, store, *head, root,
                                          QueryKind::Value, false,
                                          paging_config);
```

语义证据本身不可用仍沿用原查询的 Failed/UnavailableEvidence 行为；
分页状态存储的可重试暂停不改变这一边界。

磁盘配额、原子 head、存活锁和显式清理沿用 C4c2a；本轮没有在线 GC 或
断电持久性承诺。普通语义缓存、Stage 诊断及 HIR/MIR 不受查询分页影响。

## 验证与后续

专项单元测试覆盖 Value/Effects、嵌套调用/递归、最短见证、极小段逐 slice
保存/恢复/未确认页重送、257 来源的大 frontier、固定尾部/缓存容量、
只读暂停和可写重绑、配额暂停后续接，以及恰好发生在 enqueue 中的读取
失败回退。重新封装有效 CAS 校验的索引值、排序和范围损坏必须拒绝恢复。
交付及卸载恢复的 IO 故障验证修复后序号、来源和累计工作保持一致。

跨进程探针在中途、遍历结束和未确认交付处退出，使用新进程、不同 slice
预算恢复分页；结果、调用上下文、累计工作和内存查询对照一致，Stage 次数
为零，原语义证据文件不变。探针每次 advance 校验尾部/缓存预算，保存后
校验实际容量释放。它们是正确性验收，不是性能测量。

C4c3 剩余工作：符号拓扑任务状态的保存/恢复；实际来源与大函数的内存、
目录、磁盘、保存/恢复延迟和读写放大测量；主解释 CLI 的显式分页、续接与
维护入口，以及普通文本结果兼容。C4c 整体尚未完成。

2026-10-16 验证：Debug 全量 377/377 CTest（含 lit），GCC 与
ASan/UBSan 各 376/376 非 lit CTest；C++ 格式、Python 编译、39 份
Markdown 的链接检查和 `git diff --check` 均通过。扩展的任务跨进程测试
在 ASan/UBSan 下约 15 秒完成；此处记录正确性运行，不作为性能结论。

后续 [C4c3a](provenance-topology-checkpoint-c4c3a.md) 已支持符号拓扑/公式
准备的驻留 checkpoint 和跨进程确认续接；尚未复用本页的活动分页后端。
拓扑卸载与测量、主解释 CLI 分别继续拆分到 C4c3b/C4c3c。
