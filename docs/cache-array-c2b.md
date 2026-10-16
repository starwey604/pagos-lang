# Cache Array C2b 实现记录

状态：已实现，日期为 2026-10-12。对应
[迭代计划](cache-implementation-plan.md)的 C2b，沿用
[C2a](cache-scalar-c2a.md) 的 Stage 接入、身份、CAS、预算回退与默认关闭策略。

## 准入与恢复

内部适配器仍位于 [scalar_cache.cpp](../lib/stage/scalar_cache.cpp)，现同时
处理标量和一次标量数组构造。internal 函数必须有固定长度的标量数组返回
类型、无语句的函数体；尾表达式是数组字面量或数组生成器。参数仍限定为
全部 Comptime bool/整数标量。元素、生成边界和生成体只准入字面量、参数、
生成索引、非短路标量运算与转换；生成体没有局部语句或普通函数调用。

支持 bool、u/i8、16、32、64 和目标已解析的 usize，保持元素顺序。生成
边界允许标量计算和参数，不另建估算边界的求值器；正常 Stage 求值成功且
实际长度等于声明返回类型后才能发布。生成索引允许遮蔽参数，边界仍在外层
作用域求值。空/倒序范围、长度不匹配、元素求值错误不发布成功项。

普通嵌套特化、递归、数组参数、记录、混合 Runtime 数组、局部绑定/契约、
if、短路及顶层独立生成器查询继续正常分析。数组返回函数的实参必须没有
残留工作：当前这类函数不使用标量残留签名，参数绑定未剥离实参工作，
因此不能仅凭已知值准入。C2a 标量函数原有实参工作恢复行为保持。

命中恢复一个现有所有权规则下的 HIR Constant，不引入常量池或共享可变
负载。Runtime 索引、当前数组引用、边界检查与目标读取仍由当前 HIR/MIR
流程生成。第二次同会话调用使用原 memoization；新数组实参在查询前分析，
即使被调函数会话内命中，也仍扣除实参构造配额。

## 有序资源模板

标量 C2a ResourceRecipe 字节保持。数组在其后增加扩展标签、构造发生的
Fuel 位置、元素数量、逻辑元素宽度、重复后缀起点和次数。Fuel 模板保存
结构位置序列，生成器包括：

```text
Fuel(array)
Fuel(begin expression tree)
Fuel(end expression tree)
ReserveArray(count, width) -> ReserveAggregate(count, width)
Repeat(count):
    Fuel(generator iteration)
    Fuel(scalar body expression tree)
```

数组字面量在根 Fuel 后预约，再按元素顺序访问。普通求值的
ResourceCapture 逐个核对实际 Fuel 区间及预约位置、类型、数量；只在整个
模板、相对进入深度和特化数精确匹配时发布。它使用计数器与模板游标，
不分配随迭代次数增长的检查点数组。测试验证 3 与 1024 次迭代的 recipe
大小相同；压缩的是重复事件模板，而不是把有序资源协议替换为总计数。

热恢复在 AnalysisStats 副本上检查完整 Fuel、数组元素/字节及共享聚合
成员/字节额度，成功计入一次聚合构造。先检查旧数组配额，再检查共享配额，
乘法前检查容量。恢复结果、当前 Recorder 证据和会话 memoization 后一次
提交统计。任一额度不足都丢弃副本，从原函数体重新分析；原检查点决定
错误顺序、区间与部分计数，尤其保留“预约成功后 Fuel 耗尽”的旧行为。
深度与特化入口仍使用原检查；不把磁盘命中当成逻辑会话命中。

## 解码与故障边界

FileStore 校验 CAS、manifest 和保守整模块依赖后，适配器要求两个必需
对象、规范资源模板完全一致和结果指纹匹配。解码先只读取 wire header/type，
核对当前返回形状、数量与精确剩余负载长度，再允许元素 vector 分配。
bool 元素一字节，所有宽度整数采用 C1a 的八字节规范数值，逻辑预算仍按
声明宽度计费。错误形状、类型、截断、尾随数据、构造顺序及重复参数都拒绝。

构造数量和单对象大小仍受现有 codec/FileStore 容量限制；超出缓存准入
容量的程序回到正常求值。没有取消保护、磁盘数组按需解码或内存峰值下降
的承诺。损坏/缺块/IO 故障、读取恢复分配失败不产生新的语言诊断。

## 物理测量

`--cache-stats` 增加纳秒时间字段；没有请求它时不读取阶段时钟。
库调用方可通过 CacheConfig.measure_timings 开启。

| 字段 | 测量边界 |
| --- | --- |
| dependency-ns | 一次完整 CacheIdentity 构建，包含保守模块依赖摘要 |
| query-ns | 准入、Anchor、规范键及资源模板构建 |
| read-ns | FileStore lookup，包含 IO、校验和、manifest 解码与引用验证 |
| decode-ns | 资源验证后的形状/负载检查、值解码和语义结果验证 |
| encode-ns | 发布负载、资源及结果指纹编码；不含 publish |
| publish-ns | FileStore publish，包含 IO、锁、校验和与发布协议 |
| resource-ns | 命中的预算副本核算 |
| restore-ns | 命中的 HIR/Recorder/会话项恢复及提交 |
| evaluate-ns | 调用 miss 路径中实际函数体分析的包含时间；嵌套函数会重叠 |

时间只属于遥测，不进入键或语言预算。这些边界不能相加当成全部编译时间，
read/publish 也不是纯硬件 IO 时间；完整前端、身份/输出与进程启动仍有成本。

[benchmark.py](../scripts/benchmark.py) 增加 `--cache-modes=off|cold|hot|all`、
可重复的 `--case` 和 `--command`，加入
[封闭生成器](../benchmarks/closed-generator.pgs)。每个样本是独立 Release
编译进程；cold 每次清空专用缓存，hot 先独立预热后只读，轮次轮换测量顺序。
这不表示清空 OS page cache。通过 Linux wait4 获取每个子进程的峰值 RSS，不使用累计的子进程最大值，
也不要求安装 GNU time；wall 时间包括启动、输出捕获和 1 ms 轮询开销。
JSON schema 2 保存所有预热/正式样本、阶段
遥测、缓存文件数量/字节、输出指纹、机器、工具链、预算、源码/二进制身份
及工作区脏状态。关闭/冷/热输出指纹必须一致。

```sh
cmake --build --preset release
python3 scripts/benchmark.py --cache-modes=all \
    --output build/benchmarks/cache-c2b.json
python3 scripts/benchmark.py --cache-modes=all \
    --case benchmarks/closed-generator.pgs --command check \
    --output build/benchmarks/cache-c2b-generator.json
```

实际测量结果和验证记录见下节；完整样本保存于忽略的 build/benchmarks。

## 测量结果与验收

2026-10-12 在 AMD Ryzen AI 9 HX 470、Linux 7.2.9、Clang/LLVM 23.1.1、
CMake 4.4.4、Python 3.14.7 上运行 Release。源码基线 HEAD 为 `62a6915`，
工作区有未提交的 C2a/C2b 与文档修改；实际二进制 SHA-256 为
`5692a1fa5e9a8dc6ed4b384dacc765731e9a07c0519ccb1e0d6d66cfa007f69b`。
完整源码、配置、预算、机器与输出身份以本机报告
`build/benchmarks/cache-c2b.json` 为准。

共 7 个用例 × 3 个命令 × 3 个缓存状态，441 个正式样本、126 个预热样本，
另保存 21 个 hot seed 样本。每组预热 2 次、正式重复 7 次，采样期间没有
并行构建/测试。所有用例均验证关闭/冷/热 Stage 输出与逻辑统计一致，
每条命令的三种缓存状态输出指纹相同。

8192 元素封闭生成器的 wall 中位数如下，单位为毫秒：

| 命令 | off | cold | hot |
| --- | --- | --- | --- |
| check | 9.712 | 13.929 | 8.660 |
| emit-mir | 14.894 | 10.762 | 9.744 |
| emit-llvm | 14.904 | 12.858 | 11.847 |

`check` 的 min–max 分别为 8.622–11.807、9.662–17.133、6.544–10.783 ms。
短进程样本波动明显；cold emit-mir 比 off 快不能解释成发布缓存的收益。
这个样本证明真实复用，尚不足以给整体编译加速作稳定承诺。

该生成器所有 hot 正式样本均为 hits=1、bodies-skipped=1、
body-evaluations=0；off/cold 均实际求值一次。冷热磁盘负载均为 4 个文件、
66,435 字节，其中 2 个 CAS 对象、1 个 manifest 和 1 个锁文件。
`check` 各阶段时间的独立中位数，单位为微秒：

| 阶段 | off | cold | hot |
| --- | --- | --- | --- |
| 模块依赖身份 | 0 | 47.63 | 48.84 |
| 查询/资源模板构建 | 0 | 17.46 | 11.11 |
| FileStore 读取/校验 | 0 | 35.47 | 198.28 |
| 负载解码/结果验证 | 0 | 0 | 209.02 |
| 负载编码 | 0 | 327.69 | 0 |
| FileStore 发布 | 0 | 383.25 | 0 |
| 命中资源核算 | 0 | 0 | 0.58 |
| 命中恢复 | 0 | 0 | 5.60 |
| 实际函数体分析 | 2590.10 | 4007.02 | 0 |

Cold 还包含逐步核对资源模板的成本。Hot 跳过函数体，但仍读取、校验、
解码整个数组；没有按需元素读取。分项中位数不能相加得出 wall 中位数，
也不包含全部前端/输出开销。

反例同样保留：1024 个小标量特化的 `check` wall 中位数为
10.874/54.053/28.746 ms（off/cold/hot）；即使命中 1024 次，逐查询读取
和验证仍超过原求值成本。不准入用例还需付完整身份构建的额外成本。
因此本轮保持默认关闭，不扩大普通调用/聚合准入，不把磁盘复用宣称为
构建期内存架构的完成。峰值 RSS 原始样本已记录，未据此建立内存收益门槛。

验证结果：

- Debug 全套 CTest 240/240；最后单独复验 lit 全集 385/385。
- GCC 和 ASan/UBSan 缓存专项各 60/60，缓存 lit 用例各 6/6。
- 新增 7 个 StageCache 单元测试与 3 个数组 lit 用例。覆盖多宽度/bool、
  计算边界、同会话命中、新实参数组、全部 Fuel 边界、精确/减一/零构造
  配额、旧数组与共享配额首错、空/倒序/错长度/元素失败、准入回退、
  当前位置/生成索引遮蔽、重复模板大小和损坏负载/资源记录。
- 独立进程对比 check、Stage/Provenance、HIR、MIR、LLVM IR 和只读内容/
  mtime；嵌套 Runtime 函数里的纯数组命中保持深度、特化及证据一致。
  Runtime 索引 0/3 正确，4/UINT32_MAX 与关闭缓存一样触发陷阱。
- clang-format、Python 语法、26 份文档检查与 `git diff --check` 通过。

后续实际开发入口为 C3q1。细粒度依赖、批量小查询 IO、残留模板、证据快照、
构建卸载和纯详情重放按 RFC 各自推进；本轮没有提前实现这些能力。
