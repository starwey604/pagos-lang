# Provenance 读取视图 C4a1

状态：2026-10-14 完成 C4a 的第一轮。实现 manifest 引用读取、物理证据块、
lease 和原生查询的按块访问。完整函数证据单元、Import 和根 manifest 的
闭包校验仍属于 C4a2/C4a3，不能把本轮标为完整 C4a 或函数级缓存命中。

后续 [C4a2 函数单元协议](provenance-units-c4a2.md) 已实现特化身份和
Import/SCC 的热闭包验证；[C4a3](provenance-function-queries-c4a3.md)
已完成函数格式的按需查询、跨进程 CLI 和真实 Stage 测量。以下保留本轮范围。

设计约束见 [Cache Architecture RFC](cache-architecture-rfc.md)，后续安排见
[实现计划](cache-implementation-plan.md)。本轮不改变 Stage、默认缓存模式、
C3a CLI 的整图恢复或 C3q2 拓扑/公式准备入口。

## 存储读取边界

[FileStore](../include/pagos/cache/store.h) 新增 `lookup_references()` 和
`read_object()`。前者只读取并验证 manifest 的校验和、规范键、已验证依赖
和冲突标记，返回没有 bytes 的对象引用；后者读取一个完整 CAS 对象，检查
种类、大小、摘要、payload header 和版本。两者不写入目录或对象。

`lookup()` 仍加载全部必需对象，沿用语义缓存候选的原有契约。
`lookup_references()` 成功不代表必需对象存在，更不代表摘要闭包可复用。
下一轮的函数适配器必须先校验热摘要闭包，再允许冷详情按需读取。
无关必需冷块的删除不影响本轮的定向查询；这不是 Stage 复用准入规则。

新增 `ObjectKind::EvidenceBlock = 8`。物理块格式为：

```text
PGC1 header / ObjectKind
Block version = 1 / Record model = 1 (P0)
First graph-local NodeId / Node count
Node records: rule, site, local edge offset/count, payload, owner
Edge count / Edge records: graph-local target, relation
```

节点记录 21 字节，边记录 5 字节。物理块采用连续图内节点编号，边偏移从
当前块起点计算；target 和 summary owner 仍是所属整图内的编号。
第一轮没有把图内编号提升为稳定函数身份，也没有设计互相依赖的内容哈希。
函数单元的局部编号和 Import 重映射由 C4a2 解决。

单块第一次读取校验整个 CAS 对象，随后完整解码该块；不会扫描其他块。
这不是 mmap、压缩块或块内随机 IO。查询 slice 约束原有逻辑工作，
不约束一次完整对象校验所花的 IO 和 CPU 时间。默认单对象仍受 FileStore
和 codec 字节上限约束；调用方负责选择块大小。

## 视图和所有权

[EvidenceView](../include/pagos/stage/evidence_view.h) 提供 memory/disk
adapter、`pin()`、摘要入口和读取计量。

memory adapter 保留原 FrozenGraph，共享原来的 Node 引用和 Edge span。
旧 `FrozenGraph::node()`、`edges()` 和查询 `graph()` 的生命周期契约不变。
disk adapter 构造时检查目录连续性、对象种类、编号容量和摘要根范围，
不打开冷对象。目录和摘要元数据由调用方提供；本轮尚无持久化且经过
语义身份验证的根目录协议。

`pin()` 返回拥有不可变存储的 NodeLease。节点引用和边 span 只有在 lease
或其副本活着时才有效。缓存淘汰、视图销毁和文件删除不使已有 lease 失效。
默认解码块缓存上限 4 MiB；LRU 淘汰缓存所有权。块超过上限或上限为零时，
当前 lease 仍可持有它，lease 释放后容量才真正释放。`evict()` 清空缓存；
尚有 pin 的块可以由 weak 引用再次借用，避免重复读取。

计量区分 `cached_bytes` 和包括被 pin 的已淘汰块的 `live_bytes`。
字节是解码后的节点/边 vector capacity，不包括目录、摘要、分配器开销、
查询队列和上下文。读取计量提供请求、命中、成功完整 CAS 读取次数和
payload 字节数；CAS 外壳额外占每对象 49 字节。读取失败的底层系统调用
字节数不在成功 payload 计量中。不能据此宣称编译器总内存上限或峰值改善。

共享视图用 mutex 串行化冷读取与缓存发布。查询任务各自拥有队列、游标和
调用上下文；线程间只共享不可变块。C4a1 不要求并行解码同一个块。

## 查询和失败

[QueryTask](../include/pagos/stage/provenance_query.h) 可以直接接收
`shared_ptr<const EvidenceView>`。构造、零预算首次暂停和提前取消不会读盘。
遍历使用 lease，跨 slice 保留当前节点和边游标；参数替换另外 pin 调用块，
保留现有 owner 匹配、调用帧、来源、效果和最短见证规则。

`graph()` 只适用于 memory adapter；disk 查询通过 `evidence().pin()`
解析节点。磁盘原生查询不先恢复 FrozenGraph，也没有隐式触发 StageAnalyzer。
压缩公式/拓扑准备仍使用内存图，未迁移到磁盘读取视图。

对象缺失、内容损坏、版本不兼容、读取失败和容量上限通过 EvidenceError
区分。查询转为 `Failed`，页的 details 为 `Unavailable`，coverage 增加
`UnavailableEvidence`，不会将部分来源宣称为完整结果。已交付页仍属于原
任务；失败后 `resume()` 不偷偷重启。合法递归摘要环沿用当前保守 cut，
不产生存储错误；有限域递归求解仍是 C4d。

本轮没有更改 Recorder，也不冻结/读取 StageDecision 或 SourceAnchor。
跨源码修订、三个逻辑摘要入口、详情能力与 Import 闭包的验收留到 C4a2/3。

## 验证

[视图测试](../tests/unit/evidence_view_test.cpp) 在文件系统 CAS 上发布
两个嵌套函数的参数/效果块、8193 节点的无关大块，以及两次调用所在块。
来源/效果查询及最短见证使用单状态、单边、单绑定、单输出 slice 与内存
查询比较，检查来源、路径、调用上下文、工作量和 coverage。

定向查询首次只读 2 个对象，payload 合计 **334 字节**；重复查询复用已加载
对象，无关大块的 **213042 字节**不被读取。统计由测试断言约束，不是 RSS
或端到端性能测量，也不是来自真实 Stage 编译的函数单元失效实验。

另覆盖零缓存额度、暂停游标与淘汰、lease 跨视图销毁/文件删除、并发查询
与淘汰、提前取消、合法递归环、缺块、CAS 损坏、带有效 CAS 校验和的块
版本/owner/边错误、截断/多余字节、目录和摘要根错误、codec 上限及只读 IO
失败。FileStore 的独立测试确认引用读取不打开对象，原 lookup 仍拒绝缺少
必需对象，并且显式读取校验种类、大小和摘要。

验证结果：Debug 全量 CTest **284/284**，其中 lit **390/390**；
GCC 与 ASan/UBSan 的 Cache、Provenance、StageDecision、EvidenceView
相关 CTest 各 **138/138**。新增 15 个视图测试和 3 个 FileStore 测试；
文档链接、clang-format 和补丁空白检查通过。
