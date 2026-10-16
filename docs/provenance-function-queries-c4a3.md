# 函数证据按需查询 C4a3

状态：2026-10-16 完成 C4a 的第三轮。函数格式接入原生来源/效果查询、
最短见证、分页物理拓扑和热决策读取；实验 CLI 可以在独立进程中读取，
不运行 Stage。C4a1–C4a3 完成，下一轮是 C4b 构建期封块与同次编译卸载。
设计见 [RFC](cache-architecture-rfc.md) 和[实现计划](cache-implementation-plan.md)。

## 读取句柄与生命周期

[FunctionEvidenceView](../include/pagos/stage/function_evidence_view.h) 拥有
C4a2 已验证的必需热摘要闭包，构造时不打开任何冷对象。它为每个成员分配
本句柄内的虚拟节点范围和 SummaryId，映射局部 Value/Effects 根、Import、
Call、SummaryRef、Parameter owner、决策和契约；保留实际特化身份与参数。
这些编号不写回 CAS，也不充当跨编译身份。

首次 pin 某节点时，读取、校验并解码所属 SCC 的整个 EvidenceDetail 对象，
将局部编号映射到视图。以 SCC 为最小读取单位，暂不承诺单节点 IO、mmap、
压缩分块或大 SCC 的多块拆分。模块调用侧证据也是一个块：查询 `aa = a(x)`
仍需读取模块块，但不读取无关 `b` 的内部拓扑。所有必需热摘要始终读取；
按需承诺针对冷详情，不表示逐函数独立失效。

EvidenceView 的共享有限缓存保存不可变解码块；NodeLease 同时 pin 节点、
边和当前 SourceAnchor 解析出的源码位置/标签。淘汰后已有 lease 仍有效，
视图和文件删除也不使其悬空。尚有 lease 时允许重用弱引用中的块；共享视图
的解码/缓存操作受锁保护，查询游标属于各任务。缓存额度不限制外部 pin。
Anchor 建立一次索引，避免逐个冷位置线性扫描整份 checked-AST 位置表。

重新打开同一语义对象时，SourceAnchor 关联当前源码偏移。旧句柄及其 lease
继续保留旧修订的偏移，不被新修订覆盖。空白/注释不触发重新发布。

## 查询与失败边界

原生 QueryTask 直接使用 lease 遍历，保留实参代入、调用位置链、效果通道、
最短见证、递归截断和覆盖协议；无需先构造 FrozenGraph。
[EvidenceTopologyTask](../lib/stage/evidence_topology.cpp) 逐页返回物理节点、
边和两个摘要端口链接，支持暂停、取消及恢复。共享 callee 只遍历其物理图，
不会枚举动态调用实例；零 slice 和提前取消不触发 IO。
C3q2 的公式索引/压缩求解仍是内存接口，没有为函数磁盘视图偷偷准备全图。
CLI 拓扑是结构展示，不等于全部来源证明。

决策模式只列出热摘要内已记录的 StageDecision，保留所属作用域。它不是
新的 Stage 推断或完整 Comptime 求值轨迹；函数名相同的全部特化都会列出。

缺失或损坏必需摘要使打开失败。缺失/损坏冷详情、错误版本、codec 上限或
IO 失败使对应来源/拓扑任务失败，详情不可用，不返回假装精确的空来源。
即使所有冷对象都不可读，热决策仍可列出。展示位置在查询后另一次 pin
失败时明确标记不可用并返回失败。读取没有自动修复、补写或 Stage fallback。

## 实验 CLI

对具有绑定 `aa` 和函数 `a` 的当前模块：

```sh
build/debug/pagosc --evidence-write=build/function-evidence \
  --evidence-root=aa explain-provenance example.pgs
build/debug/pagosc --evidence-read=build/function-evidence \
  --evidence-root=aa --evidence-mode=sources --max-fuel=0 \
  --provenance-slice=128 --provenance-query-stats \
  explain-provenance example.pgs
build/debug/pagosc --evidence-read=build/function-evidence \
  --evidence-root=a --evidence-mode=decisions \
  explain-provenance example.pgs
```

`--evidence-mode` 为 `sources`（默认）、`witness`、`topology` 或 `decisions`。
前三种模式的 `--evidence-root` 选择已记录的绑定/契约根；决策模式选择作用域名，
`module` 表示模块。省略选择器时列出全部根/作用域；不存在的名称返回失败。
`--evidence-cache-bytes=N` 默认 4 MiB，允许 0，只约束缓存保留的解码容量。
slice 使用既有 `--provenance-slice`；统计通过 `--provenance-query-stats` 输出。

写入运行正常 Stage、发布函数对象后重新打开查询。读取只解析、类型检查并
验证当前身份，在 Analyzer 之前返回，因此 `--max-fuel=0` 仍能读取已有证据。
两者与 C3a `--snapshot-*` 互斥，读取不接受 Stage cache 配置；旧 compressed/
topology provenance 选项不混入新接口，应使用 `--evidence-mode`。
默认编译路径没有自动函数磁盘读取或 spill。

统计分开列出必需热对象和冷 payload 字节，不含查询 manifest IO 与每个
CAS 对象的 49 字节封装。函数适配器的冷计数记录成功校验/解码的块加载；
它不是包含失败读的系统 IO 计数。cached/live 是解码容器容量及堆标签容量，
不包含热目录/Anchor 索引、查询队列/visited、解码临时副本和分配器开销。
进程峰值 RSS 另行测量，不将这些容量值当作总内存限制。

## 真实 Stage 测量

[测量脚本](../scripts/benchmark_function_evidence.py) 生成并解析/类型检查真实
`.pgs`：两个不同函数 `a`/`b`，各有 N 个命名标量算术步骤，各调用一次。
独立导出进程运行 Stage 并写入文件系统 CAS；读取进程不运行 Stage，只查询
`aa` 两次。lazy 使用 4 MiB 缓存；eager 使用零解码缓存逐块恢复完整图及位置，
释放磁盘句柄后执行同一原生查询。两者的来源及调用路径签名必须一致。
重复调用共享、不同 Comptime 特化、递归和效果另由真实 Stage 单测覆盖。

```sh
cmake --preset release
cmake --build --preset release
python3 scripts/benchmark_function_evidence.py \
  --output build/benchmarks/function-evidence-c4a3.json
```

结果见[原始 JSON](../benchmarks/results/function-evidence-c4a3.json)，包含全部
样本、中位数、IQR、范围、源码/二进制/CMake 哈希、工具版本与 dirty 标记。
每档每模式预热 2 次、测量 7 次；导出样本各用新目录。查询进程共享操作系统
页缓存，不代表物理磁盘冷启动。时间包含语义输出签名计算。
RSS 用 Linux wait4 记录每个子进程高水位，包含启动、AST/类型检查、热元数据、
恢复和查询，不是隔离的查询峰值；未宣称 Stage 构建内存已降低。

2026-10-16 本机 Release：AMD Ryzen AI 9 HX 470、Linux 7.2.10、
Clang/LLVM 23.1.1、Python 3.14.8；未绑核或固定频率，样本按顺序运行。
下表为中位数。热对象固定为 **4 个 / 2,472 字节**；冷读取 lazy 固定为
**2 个对象**，eager 为 **3 个对象**，所有样本的重复查询新增冷读取均为 0。

| N/函数 | 视图节点 | lazy 冷字节 | eager 冷字节 | lazy 保留解码字节 |
| --- | --- | --- | --- | --- |
| 128 | 788 | 57,828 | 113,261 | 51,705 |
| 512 | 3,092 | 223,332 | 444,269 | 200,313 |
| 1024 | 6,164 | 444,028 | 885,661 | 398,457 |

| N | 模式 | 热加载 ms | 完整恢复 ms | 首查 ms | 复查 ms | 证据首响应 ms | RSS MiB |
| --- | --- | --- | --- | --- | --- | --- | --- |
| 128 | lazy | 0.249 | — | 0.372 | 0.024 | 0.635 | 27.67 |
| 128 | eager | 0.368 | 1.011 | 0.062 | 0.029 | 1.495 | 27.75 |
| 512 | lazy | 1.024 | — | 1.993 | 0.150 | 3.048 | 29.30 |
| 512 | eager | 0.680 | 2.527 | 0.149 | 0.114 | 3.372 | 29.48 |
| 1024 | lazy | 0.951 | — | 2.091 | 0.128 | 3.085 | 31.39 |
| 1024 | eager | 0.890 | 4.053 | 0.191 | 0.154 | 5.097 | 32.29 |

证据首响应是每个样本的热加载、视图准备、完整恢复（若有）与首次查询之和，
再取中位数，不含当前源码解析/类型检查/身份生成。lazy 首查包含冷读/校验/
解码，eager 首查在完整恢复后发生，二者不能脱离恢复成本直接比较。
这份 eager 基线通过本轮函数读取器和 Builder 恢复，不是 C3a 整文件 codec
的性能测量。不可变块的解码容量在加载时计算一次，零缓存 pin 重用不重复
扫描所有位置字符串。

最大一档证据首响应为 **3.085 ms / 5.097 ms**，而整个子进程墙钟中位数为
**18.124 ms / 18.200 ms**。首次按需读取避免约一半冷字节，阶段延迟较低；
本轮启动、当前源码检查和测量噪声足以掩盖端到端时间差，不能宣称整体编译
显著加速。RSS 差约 **0.89 MiB**，改善较小，后续仍应按真实应用扩大测量。

独立新目录导出结果如下；其 RSS 包含完整 Stage，与读取进程不是同一阶段：

| N | 原图节点 | Stage ms | 发布 ms | 导出进程 RSS MiB |
| --- | --- | --- | --- | --- |
| 128 | 789 | 1.353 | 1.674 | 29.63 |
| 512 | 3,093 | 9.159 | 5.776 | 50.57 |
| 1024 | 6,165 | 25.987 | 8.832 | 120.41 |

当前仍先完整构建证据；最大一档导出进程峰值约 120 MiB。函数冷存储没有
消除首次分析的 RuntimeTrace、图和元数据开销，此项留给 C4b 单独验收。

## 验收与保留边界

[函数测试](../tests/unit/function_evidence_test.cpp) 与
[跨进程脚本](../tests/lit/Inputs/run_function_evidence.py) 覆盖：

- 值/效果来源、最短见证、调用位置链、递归覆盖及决策与原始内存图一致；
  特化参数、指纹和作用域中的决策数量保持。
- 同一视图并发查询/淘汰；暂停游标持有 lease；旧修订与新修订位置同时有效；
  lease 跨句柄销毁及文件删除仍有效。
- 新进程 `--max-fuel=0` 读取，sources/witness/topology 均只读所需冷块；
  重复输出一致，读取前后全部缓存文件内容和修改时间不变。
- 删除无关大函数详情后当前查询仍成功；损坏可达详情后查询失败而热决策
  可读；删除全部详情仍可读决策；删除必需摘要使加载失败；只读 miss 不建目录。
  错 Import/版本/owner 等格式验证继续由 C4a2 的真实函数对象测试覆盖。

Debug 全量 CTest **305/305**，其中 lit **391/391**；GCC 与 ASan/UBSan
相关 Cache、Provenance、StageDecision、EvidenceView、FunctionEvidence
CTest 各 **159/159**，对应 CLI lit 各 **18/18**。新增 5 个函数查询单测和
1 个跨进程 lit 用例；源码位置测试另扩展旧用例。

本轮仍在完整构建图后冻结并发布，每个 SCC 仍有一个整对象大小上限。
C4b 需处理构建期封块、释放和元数据峰值，不能用本轮 RSS 比较替代其验收。
C4c 的磁盘游标、C4d 的有限域递归求解和 C4e 的解释重放尚未实现。
没有数据库依赖，也没有改变 Stage 规则、Fuel 计费或默认同步查询接口。
