# C3a：模块 Provenance 快照

2026-10-12 完成，承接 [C3q](provenance-query-c3q.md) 的内存查询。
[接口](../include/pagos/stage/provenance_snapshot.h)提供独立导出、打开、
section 访问和恢复；它保存真实 P0 证据，不参与 Stage 判定。

## 格式与所有权

`ProvenanceSnapshot` payload 使用显式小端 wire records：

```text
Header
  C1 envelope kind/version
  Snapshot format version / P0 record-model version
  Module semantic fingerprint / context fingerprint
  Section directory: kind, required, offset, size
Identity / Root / Summary / Node / Edge / Decision / Site / String
Whole-payload checksum
```

Identity 保存版本化查询上下文，包括目标、编译器兼容身份与模块依赖。
Root 保留绑定/局部契约，Summary 保留作用域名称及值/效果入口，Decision
保留已有稀疏规则、原因和作用域。Site 保存稳定 SourceAnchor，String
保存去重名称与标签；实际偏移属于独立位置对象，不影响语义图字节。
图节点/边保留局部 ID、参数 owner、调用与摘要引用，不编码指针或活动查询。
当前值/效果入口共享原根，没有新增 Comptime 历史。

打开句柄先检查整对象校验和、版本、目录范围/重叠/重复、必需 section
及上下文身份。未知可选 section 可跳过；未知必需能力被拒绝。句柄持有
不可变缓冲，借用 section span 的调用方保留句柄或副本。
恢复再验证记录长度、ID、关系/规则枚举、owner、参数所属定义、摘要端口、
源位置与稀疏决策，并构建新的 FrozenGraph。物理边必须引用先前节点；
SummaryRef/Call 按摘要 ID 引用，合法循环摘要不被当作坏图。

这版完整读取、校验和解码。偏移访问只针对已经打开的缓冲，不代表 lazy
文件 IO、mmap、压缩或可卸载节点池；函数单元和 lease 留 C4a。

## 图与位置分别发布

沿用 FileStore 的 CAS、manifest 与原子发布，新增 ProvenanceSnapshot、
ProvenancePositions payload 种类，以及独立的证据/位置查询命名空间。
模块图的 manifest 只引用必需图对象，按模块语义与目标/版本定位。
位置对象绑定图内容指纹、源码修订、文件名和偏移，按修订另行索引。
同一语义的两份空白修订只产生一个图对象、两个位置对象。

读入时提供当前 checked-AST 的 CacheIdentity 和上下文，通过稳定 Anchor
重新取得当前位置；不会默用首次发布的偏移。缺位置对象可重建，缺图或
坏必需图则返回错误，由调用方选择重新分析。图与位置分别发布，位置
发布失败不删除已经成功发布的图；普通语义缓存不隐式写这些快照。

```sh
build/debug/pagosc --snapshot-write=build/evidence \
  explain-provenance example.pgs
build/debug/pagosc --snapshot-read=build/evidence --max-fuel=0 \
  explain-provenance example.pgs
```

选项仅用于 explain-provenance，读/写互斥。写入先完成正常 Stage 分析，
只导出成功模块；C++ API 也可以导出已有的中止证据与局部失败契约。
读取仍解析、类型检查及验证当前语义身份，直接恢复并查询快照，不运行
StageAnalyzer；零 Stage Fuel 的跨进程用例证明了这一点。显式读取失败
报告错误，不把重新分析伪装为快照命中；也不接受 Stage 缓存控制选项。

## 验证

[单元测试](../tests/unit/provenance_snapshot_test.cpp)比较同步/分 slice 的
值、效果、见证、调用上下文、覆盖和所有 P0 决策，覆盖数组投影、显式
边界、共享函数、相同内部读取、递归、volatile 与中止局部契约。
其他用例验证空白修订的图字节不变、当前位置正确、目标/语义变化拒绝、
句柄和恢复图生命周期、每个截断、坏目录/版本/ID/owner/边、CAS 对象缺失
及只读不写盘。CLI 在不同进程比较输出并验证读入路径没有 Stage 求值。

测量工具见[开发指南](development.md)。Release 报告保存在
`build/benchmarks/evidence-c3.json`，包含 8 组、56 个测量样本、16 个预热
样本，以及来源/二进制/CMake 哈希、机器版本、每次数据及 median/IQR。
环境为 Ryzen AI 9 HX 470、Clang/LLVM 23.1.1；采样时本次构建/测试均已
结束，没有锁定 CPU 频率或控制 OS 页缓存。

以下是实际带 Runtime 参数和大量局部绑定的函数，时间为 median ms：

| 局部绑定数 | 图 payload bytes | 位置 bytes | 编码 | 校验/打开 | 恢复 | 查询 |
| --- | --- | --- | --- | --- | --- | --- |
| 128 | 38,063 | 4,290 | 0.381 | 0.067 | 0.123 | 0.028 |
| 512 | 148,271 | 16,578 | 1.627 | 0.251 | 0.679 | 0.086 |
| 1,024 | 295,239 | 32,962 | 3.296 | 0.552 | 1.500 | 0.191 |

首次测量暴露了每个 Anchor 在线性位置表中查找的重复成本，已改为一次
建立位置索引；上表为修复后的源码/二进制数据。节点仍直接按 ID 定位，
不为每个节点建立哈希表。

512 绑定的独立 CLI 进程中，重新分析的 wall median 为 16.314 ms，
IQR 2.914 ms，peak RSS median 39.16 MiB；整对象读取/恢复为 11.855 ms，
IQR 0.963 ms，peak RSS median 30.32 MiB。这是查看已有证据时跳过 Stage
和旧 RuntimeTrace 构建的测量，不是同次编译峰值下降或 lazy IO 的证明。
磁盘图/位置对象分别为 178,302 / 16,663 字节，包含 CAS envelope；工具的
规范上下文与 CLI 上下文大小不同，因此与上表 payload 大小不混用。

验证：Debug CTest 266/266，包含 lit 390/390；GCC 和 ASan/UBSan 的
缓存/Provenance 定向 CTest 各 120/120、相关 CLI 各 17/17；文档和格式
检查通过。
