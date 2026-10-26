# 递归求解驻留快照与严格恢复：C4d3a

2026-10-27。C4d3a 为 [RecursiveSummaryTask](../include/pagos/stage/provenance_recursive.h)
增加显式绑定构造、checkpoint 和 restore。保存发现方程、SCC 遍历、组件
worklist 和活动参数替换状态；恢复不重新运行 Stage，也不从根重新求解。
本轮是 C4d3 的第一切片，后续分段存储和持久 CLI 尚未完成。

后续 [C4d3b](provenance-recursive-storage-c4d3b.md) 已实现分段 TaskStore、
暂停卸载和存储重试；本页 v5 格式继续兼容，持久 CLI/结果确认仍待 C4d3c。

## 身份与接口

绑定必须来自 FunctionEvidenceView.checkpoint_binding，包含已验证的完整
函数证据闭包、当前位置映射和原始源码修订。任务另绑定调用方提供的唯一
逻辑 task ID、所选 SummaryId 和一个 Value/Effects 通道。SummaryId 仅在
同一闭包的虚拟图中有效，不能作为跨修订选择键。

```cpp
using namespace pagos::stage::provenance;
auto binding = evidence.checkpoint_binding(complete_source_text);
if (!binding)
    return;
RecursiveSummaryTask task(*binding, task_id, summary, QueryKind::Value);
(void)task.advance(256);
auto bytes = task.checkpoint();
if (!bytes)
    return;
auto restored = RecursiveSummaryTask::restore(
    *binding, task_id, summary, QueryKind::Value, *bytes);
```

checkpoint 为 const 操作，不暂停执行、不修改工作计数，也不隐式写文件。
普通 EvidenceView 构造的未绑定任务和 Failed 任务拒绝保存。Running、
Paused、Cancelled、Completed 均可保存；取消状态恢复后须显式 resume。
Completed 恢复后查询结果不再增加求解工作。
同一任务的方法由一个线程调度，快照在原语步骤之间取得；并发取消使用
stop_token，不在另一个线程同时遍历事实集进行保存。

restore 在构造任务表前比较闭包、位置、源码、任务、摘要和通道。语义未变
但原始源码不同也拒绝旧任务，沿用 C4c 的严格续接规则；这不同于 C4d2
显式发起新查询时允许重用语义证据并重绑定位置。

## 格式与游标

快照使用 QueryTaskManifest 的 **v5 驻留格式**，与原生 v1/v2、拓扑 v3/v4
区分。记录 recursive_engine_version、共享 query_engine_version 和
provenance_record_model；求解规则或虚拟布局版本不匹配时拒绝。
末尾使用 pagos.recursive-task.v1 域的 32 字节完整性指纹。

| 部分 | 内容 |
| --- | --- |
| 控制与身份 | 格式/规则版本、闭包/位置/源码、task、summary、channel、progress、phase |
| 工作计数 | steps/slices、节点/链接/组件、求解次数、事实增长、队列峰值 |
| 方程表 | 发现顺序、节点引用、排序事实集、issues、merged、queued |
| 关系表 | 原依赖、摘要及实参链接，保留顺序、Relation 和实参槽 |
| SCC 状态 | seen、显式 DFS 帧/边游标、完成序、组件成员/环标志 |
| 求解状态 | 组件/成员游标、worklist、活动方程/输入/通知用户游标 |
| 参数替换 | 当前输入事实位置、被绑定实参及实参事实位置 |

不序列化 NodeLease、容器迭代器、地址、哈希桶或共享指针。活动 set 迭代器
编码为当前原子值或 end；恢复时在经过验证的相应事实集中重新定位。它
不是 ordinal 排名，避免集合插入后把同一排名误当作原位置。嵌套实参替换
同时保存两个位置，不能只保存外层事实游标。

节点记录从所绑定证据读取；ID 索引、逆向用户、输入/实参数组和摘要目录
按已保存关系顺序重建。组件摘要列表按发现顺序及已完成登记游标重建。
这些是恢复校验和派生索引准备，不计入逻辑 steps，也不重做 SCC 迭代或
参数依赖求解。后续 advance 只增加新执行的步骤；slices 继续表示实际
调用次数，可随调用方调度改变。

## 恢复验证与资源边界

解码要求精确消费全部负载，检查字节/元素配额、枚举/布尔值和完整性。
验证所保存的方程发现顺序及关系前缀与原图一致，不接受孤立的额外方程；
已加载节点与待发现 frontier 分开处理。再检查：

- 事实属于已发现的参数/来源种子域，本地种子和必要缺口没有丢失。
- 事实增长、节点/链接/组件和环计数与实际表一致。
- DFS 帧、边游标、seen、完成序、组件分区和依赖方向相容。
- queued 标志、唯一 worklist 和当前组件一致。
- 活动替换对应合法输入、callee owner/slot 和实际实参；原子位置存在。
- 进度、阶段及尚未使用/已经完成的状态符合其生命周期。

绑定不符为 BindingMismatch；版本不符为 IncompatibleVersion；结构、引用
或校验失败为 Corrupt；超配额为 LimitExceeded。读取已发现证据失败为
UnavailableEvidence。失败返回 expected 错误，不修改原任务，不静默重启
查询或回退到 Stage。修复输入后可再次恢复原快照。

恢复会读取并校验所有已加载节点和相关关系，不是常数时间或纯 head 读取。
尚未加载的 frontier 不为恢复提前读取；之后 advance 访问缺块时仍会 Failed。
所有方程、set、队列和验证临时表仍完整驻留；不保证活动分页、低 RSS 或
有界恢复临时内存。cache::Limits 的 bytes 包含末尾指纹，elements 限制
各容器记录数；这些不是总进程内存或墙钟截止。

## 文件与交付边界

调用方可显式将 bytes 写成 FileStore 的 QueryTaskManifest CAS 对象，并
保存它的 ObjectRef；读取后交给 restore。这里没有自动发布到语义查询
索引，也没有 TaskStore head、代次比较、磁盘配额、卸载或清理协议。

本轮保存执行状态，尚无最终结果的消费确认水位。恢复 Completed 不代表
结果已被消费，不能直接把它当作允许清理的 Completed TaskStore head。
先持久化结果再交付、失败重送和确认回收由后续持久 CLI 统一接入。
[C4d2](provenance-recursive-cli-c4d2.md) 的 continuation_available 仍为 false，
其步骤上限耗尽仍要求从头发起，不能传入本轮实验 probe 的对象摘要续接。

## 验证与后续

单元测试在真实 Stage 导出的自递归、互递归、内部来源及递归完成摘要上，
对双通道的每个原语步骤保存/恢复。覆盖全部七个阶段、取消/恢复、end 和
活动绑定边界；事实、循环拓扑、工作计数和重新编码字节与原任务一致。
结构测试在重算完整性指纹后仍拒绝非法引用、DFS/worklist/替换游标和
不存在的事实位置；另覆盖绑定、版本、截断、尾随字节、配额和证据缺块。

独立进程实验先运行一次 Stage 发布证据，随后分别在七个阶段保存并由另
一进程继续；两种摘要、两个通道共 28 个案例。结果/循环拓扑及逻辑工作
与 oracle 一致，读取不创建索引、TaskStore 或修改证据/对象文件。错误
源码、摘要、通道、缺失及损坏 CAS 对象明确失败。

验证：Debug 全量 CTest 414/414（含 lit），GCC 与 ASan/UBSan 各
413/413 非 lit CTest；C++ 格式、Python 编译、45 份 Markdown 文档及
`git diff --check` 通过。

后续 C4d3 分为：

1. **C4d3b：分段 TaskStore 与暂停卸载。** 流式保存方程/事实/关系和活动
   控制，原子发布 head 后再释放容量；验收配额提高、旧代保留、读写错误、
   活跃锁与维护。分别计量状态、恢复临时内存和写入代价。
2. **C4d3c：持久递归解释 CLI。** 绑定实际特化与通道，接入显式继续、
   取消、只读、最终结果确认/重送及维护；明确双通道任务身份，不把执行
   完成当作已交付或已确认，不改变普通解释和 C4d2 新查询入口的语义。

C4d 尚未全部完成；本轮也没有升级 C4d1 的精度保证或 Stage 推断规则。
