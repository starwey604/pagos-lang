# Provenance 查询 checkpoint：C4c1

C4c1 实现查询任务的有界、跨进程快照和确认后续接。它是 C4c 的第一轮，
尚未实现队列/visited 分段卸载、可变任务 manifest、主 CLI 续接入口或
符号拓扑任务的 checkpoint。默认编译与普通 `QueryTask` 不写磁盘，
普通查询的结果页也不要求确认。

## 绑定与格式

`FunctionEvidence::load` 验证语义根及完整的摘要/Import 闭包；
`FunctionEvidenceView::checkpoint_binding(source_text)` 在此基础上创建
不可由调用方任意填充的 `QueryCheckpointBinding`。调用方必须传入生成当前
`CacheIdentity` 的完整源码文本。绑定包含：

- 已验证的 `EvidenceRoot` CAS 摘要。根递归引用摘要与 detail，成员键包含
  模块语义、特化参数、target、compiler build 和语义选项。
- 当前完整 SourceAnchor 位置映射的指纹。函数格式没有独立的位置 CAS
  对象，这里使用模块指纹、按原次序排列的全部 anchor 与偏移的规范编码。
- 完整源码文本的修订摘要。空白或注释修改也使查询任务失效，即使语义根
  可复用。恢复不会把旧偏移迁到新源码。

Checkpoint 单独使用 `ObjectKind::QueryCheckpoint`，保存公共 wire header、
格式版本、查询 engine 版本、P0 记录模型、上述绑定、调用方分配的任务 ID、
根/Value 或 Effects 通道/最短见证模式和状态正文，末尾附完整校验摘要。
格式版本或 engine/虚拟 ID 映射规则改变时必须升版本。

正文保存 active 阶段、edge cursor、frontier 与已访问状态、调用帧、
见证前驱、结果、交付游标、未确认页范围、确认序号和累计工作计数。
队列的已访问部分及 frontier 共用一份 append-only 记录，恢复时重建
visited/context 哈希索引，不保存容器内存布局或 AST 地址。

图节点编号和上下文编号都只在精确绑定的闭包/任务内部有效，不能单独用作
跨修订身份。恢复同时接受调用方预期的任务 ID、根、通道和模式，任一不符
均返回 `BindingMismatch`，不会自动开始新查询。

## 显式保存与恢复

```cpp
// opened 是已经验证的 FunctionEvidenceView；source_text 是当前完整源码。
auto binding = opened.checkpoint_binding(source_text);
if (!binding)
    return;
QueryTask task(*binding, unique_task_id, root, QueryKind::Value, false);
auto page = task.advance({64, 256, 64, 16});
// 消费方先持久保存结果及 (unique_task_id, page.sequence) 收据，之后确认。
if (page.sequence && consumer_committed(page))
    (void)task.acknowledge(page.sequence);
auto bytes = task.checkpoint();
if (!bytes)
    return;
// 由调用方显式选择独立的任务目录。write_object 原子发布不可变叶对象。
auto reference = task_store.write_object(cache::ObjectKind::QueryCheckpoint,
                                         std::move(*bytes));
// 将成功返回的 ObjectRef 交给恢复进程；失败时 task 仍可在内存中继续。
```

新进程先重新打开同一证据闭包，再以同一源码创建 binding，读取指定
`ObjectRef`，调用 `QueryTask::restore(binding, task_id, root, kind, witness,
bytes)`。恢复验证校验、版本、绑定、所有编号/范围、调用帧父链、队列去重、
前驱、active 阶段、见证链及交付边界；只有通过后才返回可用任务。

恢复会读取上下文调用节点、已报告来源和 active 节点所需的冷块，检查
记录关系并重新获取 lease；尚未访问的 frontier 保持按需读取。缺少 active
块时恢复明确失败；未来 frontier 块缺失在实际读取时使任务 `Failed`，
不能宣称来源完整。恢复不运行 StageAnalyzer、不重放已扫描边，累计工作
计数也不增加；后续可使用不同 slice。取消的快照仍为 `Cancelled`，调用
`resume()` 后才继续。

## 交付与所有权

通过 binding 构造的任务使用单个未确认页的 stop-and-wait 协议：

1. 非空页携带调用方分配的任务 ID 和递增序号；空页序号为零。
2. 未确认时再次 `advance()` 原样重送该页，包括其原来的进度字段。
   不执行新查询工作，也不因新的 output slice 截断旧页。取消会改变
   `progress()` 的当前状态，但不改写已交付页的原进度字段。
3. `acknowledge(sequence)` 接受当前页，旧确认可重复，未来序号拒绝。
   确认改变内存状态；只有后续成功保存并选择新 checkpoint 后，恢复才会
   使用这个确认位置。
4. Checkpoint 保存未确认页的来源/见证范围及原页进度。进程在交付与保存
   确认之间退出时可重送，消费方必须持久保存 `(task, sequence)` 收据去重。
   新逻辑查询必须分配新任务 ID。

这不是端到端 exactly-once 保证；消费副作用与收据的事务性由调用方负责。
恢复旧 checkpoint 也可能重送旧页。未确认页只保留范围，不再复制结果表。
普通任务保持现有无确认接口和同步兼容行为。

`checkpoint()` 只编码字节，失败不修改游标；`FileStore::write_object` 可
发布该专用对象，却不创建语义 QueryKey/候选或任务 head。调用方显式使用
独立任务目录、保留 ObjectRef、选择恢复版本，并管理其生命周期。本轮
没有可变任务索引、自动清理或 fsync/断电持久性承诺。已发布叶对象在任务
完成或取消后仍归调用方；不能把它放进 Recorder 临时 session 清理范围。

## 边界与验收

默认 codec 限制是 16 MiB、单表 1,000,000 条；编码或解码超限明确拒绝。
本轮查询表全部驻留，编码另分配完整字节缓冲，恢复重建完整队列和哈希表；
保存/恢复成本随任务状态增长。尚无工作集下降或恒定恢复延迟的结论。
FormulaIndex、初始 invocation path、legacy 丢弃上限及失败任务不能编码为
本轮 checkpoint。当前公开 opt-in 构造器只创建原生 Value/Effects 任务。

专项单测覆盖逐 slice 重开证据、调用替换、递归截断、Effects、最短见证、
2,049 路物理扇入的中间 edge cursor、取消恢复、未确认页重送、确认边界、
并发读取、写盘失败后内存续跑、冷块缺失、长度/版本/目标/修订不符及
重封校验后的非法状态边界。

`pagos-query-checkpoint-process` 在独立保存进程退出后，以不同 slice 在新
进程继续。128 个真实函数调用的数组用例覆盖两种模式、7 个退出位置、
确认/未确认两种情况，共 28 组；检查来源、调用上下文、最短见证、累计
工作与不中断查询一致，按交付身份去重后无遗漏或重复。恢复探针只解析和
类型检查当前位置绑定，不走 StageAnalyzer；另外检查空白修订、损坏/缺失
checkpoint 对象均拒绝。测试探针是实验入口，尚未新增 `pagosc` 续接参数。

下一轮 C4c2 应先引入分段状态对象、原子任务 manifest/配额/离线清理，
再测量 queue、visited、contexts、前驱的内存/磁盘占用、写入放大和恢复
延迟；符号拓扑任务和主解释 CLI 的迁移继续单列验收。

2026-10-16 验证：Debug 全量 352/352 CTest（含 392/392 lit），GCC
351/351 非 lit CTest。ASan/UBSan 首轮非 lit 的 350 项通过，跨进程组合
超过原 60 秒总时限；将该矩阵总时限调为 180 秒后，专项 9/9 通过，
跨进程组合用时约 114 秒，各子进程仍有独立 20 秒超时。最后的测试宏
花括号兼容修正另以 Debug 8/8 专项检查。Markdown、本轮 C++ 格式、
Python 编译和 `git diff --check` 均通过。这些时长是正确性测试运行记录，
不是 C4c 的性能或内存验收数据。

后续 [C4c2a](provenance-task-store-c4c2a.md) 已加入分段任务存储、原子 head、
暂停状态容量释放、配额与显式清理；
[C4c2b](provenance-query-paging-c4c2b.md) 已加入活动分页与磁盘索引。
