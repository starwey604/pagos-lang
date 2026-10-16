# C3b：符号标量残留模板与必需证据

2026-10-12 完成。使用 [C3a 快照](provenance-snapshot-c3a.md)、已有文件
CAS 和 C2 资源协议，复用实际已分析的符号 HIR，再走原有 MIR lowering
和验证。沿用 `--stage-cache` 控制，默认 off。

## 准入与身份

首批是内部、非递归、无语句的闭合标量函数：bool/声明整数/usize 参数
与结果，常量、参数、严格 unary/binary 算术和比较、scalar cast。
支持这些运算已有的 Runtime 检查证据；逻辑短路、If、局部契约、普通嵌套
调用、外部/global 符号、指针/内存、数组/记录和循环继续走正常路径。
需要保存新 Stage 事件或子查询协议后，才能扩大模板范围。

TypedStageArguments 保存 Comptime 类型和值、Runtime 类型和符号槽，
包含目标与编译器/协议身份，使用保守模块依赖。Runtime 实参的来源、
首次调用位置和参数 trace ID 不写入共享身份。相同 factor 的 scale
共享一个定义，不同 factor 创建不同残留特化。

## 发布与恢复

一个 manifest 原子关联三个必需对象：

| 对象 | 内容 |
| --- | --- |
| ResidualTemplate | 实际 HIR DAG、类型、常量、运算、符号参数槽、稳定位置 Anchor、符号 trace 路径 |
| ProvenanceSnapshot | 归一化的函数符号证据、参数 owner 与值/效果入口 |
| ResourceRecipe | 已观察到的 Fuel 顺序、单层入口和一次残留特化的预算规则 |

模板不序列化 HIR 地址、会话参数编号或调用方来源。静态参数节点保存槽，
恢复时使用当前的已剥离工作值；必需符号证据将其位置归一到参数声明。
因此位置与证据不会绑定首次调用的输入。节点以先前局部 ID 引用，保留
共享计算身份；不复制成多个效果执行。

只有正常分析成功、观察资源完全匹配且残留特化预留成功后才发布。
失败或活动递归组件不发布。缺对象、坏版本/类型/槽/关系、目标/依赖变化
或对象组合的语义指纹不符，全部回退到正常分析。

命中仍按原顺序分析每个实参，包括未用实参和已知值附带的 Runtime 工作。
进入作用域时创建当前会话参数与 trace，恢复模板并验证类型、局部引用和
符号证据；必需图还与恢复模板的归一化结构逐项对应。支持子集没有新增
Stage 事件，模块 Recorder 在当前定义/调用中重建结构关联；不重跑函数
体的 Stage 求值，不从空来源推断 Comptime。

资源回放先检查当前剩余 Fuel、残留特化与深度，随后采用原来的定义发布、
残留计数和会话 memoization 路径。预检失败不部分扣除缓存资源，正常
求值产生原有失败位置和诊断。已知结果的内联路径仍正常回退。

## 遥测与验证

cache-stats 增加 residual-lookups、residual-hits 和 residual-writes。
原有 hits/bodies-skipped 也包含模板命中；语义 AnalysisStats 的 Fuel、
最大深度、特化数量、会话命中和构造计数保持原含义。decode 时间包含模板
重绑定与证据验证，evaluate 只计真实函数体分析，不将热命中的读入算成求值。

[测试](../tests/unit/stage_cache_test.cpp)覆盖不同 scale 实参、整数宽度、
bool/cast、Comptime 折叠、Runtime 除法/移位检查、每个 Fuel 边界、残留
特化/深度限制、当前空白修订和缺必需证据。off/cold/hot 比较 HIR、
Provenance、诊断及全部 AnalysisStats，并验证成功模块可降低为 MIR。
跨进程 lit 还比较 LLVM/MIR 输出并运行热缓存残留程序，验证调用方输入
和未用实参工作执行正确、read-only 未改写缓存文件。

这版验证了实际跳过函数体，不代表完整增量编译、一般效果模板、递归模板
或直接持久化 SSA MIR。

Release 独立进程测量见 `build/benchmarks/evidence-c3.json` 与
[C3a 记录](provenance-snapshot-c3a.md)。对两个相同 factor 的 scale 调用，
7 次测量、2 次预热的 check wall median/IQR 为：

| 模式 | median ms | IQR ms | 实际函数体分析 | 模板命中 |
| --- | --- | --- | --- | --- |
| off | 8.768 | 1.146 | 1 | 0 |
| cold | 8.795 | 1.050 | 1 | 0 |
| hot read-only | 8.802 | 1.146 | 0 | 1 |

样本确认热缓存跳过函数体，逻辑残留特化仍为一份、会话复用一次。该小函数
求值仅数微秒，进程启动、依赖检查、文件读取与证据验证占主要成本；此次
测量没有整体加速证据，因此维持默认 off。decode 包含证据重建验证，
未来扩大到昂贵模板时仍需重新衡量收益。

验证：Debug CTest 266/266、lit 390/390；GCC 和 ASan/UBSan 的相关
定向 CTest 各 120/120、CLI 各 17/17。所有成功模板经过 MIR 验证，
跨进程执行检查确认当前输入和未用实参工作没有被共享定义合并。
