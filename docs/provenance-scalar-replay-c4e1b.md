# 真实 Stage 标量重放证据：C4e1b

2026-10-28。本轮将 [C4e1a](provenance-replay-descriptor-c4e1a.md) 的描述
接入真实标量叶函数计算。显式 Stage 缓存写入时，可发布带实际结果的配方；
独立读取重新核对当前已类型检查的源码，不调用 Stage 或执行配方。
普通 C2 值缓存保持兼容，新的能力要求有独立版本和拒绝/重算路径。

本轮没有实现详情解释器。C3/C4a 的图、决策和快照仍为 P0；新增的是
**标量函数体的重放证据 v1**，不是完整 Stage Evidence P1，也不为所有
Comptime 结果提供理由。默认编译关闭 Stage 缓存时不生成或写入这些对象。

后续 [C4e2a](provenance-scalar-execution-c4e2a.md) 已实现独立有界执行与
实际结果核验，详情 CLI 仍待 C4e2b；本页记录源对应性和存储能力的边界。

## 从实际计算记录

ScalarCache 已有的准入要求函数为内部定义、没有语句、只有尾表达式，
参数/结果类型完整，依赖使用保守模块指纹。重放记录进一步限定为：

- 所有实参为带类型的 Comptime 标量；保留完整原始参数槽和具体输入值。
- 函数体仅含参数引用、标量字面量及 C4e1a 支持的一元/二元运算。
- 没有 Cast、短路逻辑、条件、调用、数组/生成器、局部语句、静态存储读取、
  指针、volatile/MMIO 或外部效果。C2 可缓存的 Cast 暂不具有重放能力。

在当前 checked AST、TypeTable 和 CacheIdentity 上建立后序配方，节点携带
各自真实结构锚点、类型和参数槽。字面量使用与 Stage 相同的目标整数解析，
运算操作码由显式映射产生；不把折叠后的输出逆推成配方，也不做额外求值。
真实字面量函数体可以记录 Constant，不能为不支持的函数伪造 Constant。

此时描述只是 draft，尚无预期结果。实际函数体求值成功，且现有
ResourceCapture 对其所有有序 Fuel 事件完整核对后，才将实际 TypedValue
指纹写入描述并发布。记录不产生新的语言 Fuel，不改变原 AnalysisStats、
诊断、HIR、Provenance 或残留 MIR。效果型实参的工作仍在原调用路径执行；
描述只解释函数体和已捕获输入，不宣称解释输入自身的计算历史。

## 独立的版本化能力项

C2 的 EvaluatePureSpecialization（kind 1）仍只保存原 value/resources，
其语义结果指纹与字节格式不变。新证据使用 FreezeScalarReplay（kind 6），
从原特化键派生，并在 semantic_options 后追加
`;scalar-replay-evidence.v1`。两个索引保留同一个已验证依赖指纹。
kind 6 的参数也必须全为 Comptime；未知 kind/旧读取器正常拒绝。

证据候选项有两个必需产物：

| 产物 | 对象种类 | 内容 |
| --- | --- | --- |
| value | Value | 真实 Stage TypedValue，与 C2 共用同一 CAS 对象 |
| replay | ReplayDescriptor（15） | 完整原 kind 1 特化、输入、配方及预期结果 |

scalar_replay_evidence_version=1 控制能力键后缀。描述/配方语义版本仍分别
为 1；证据 semantic_result 用独立域连接两个规范负载的指纹，绑定值和描述。
未知必需产物、可选替代品、缺产物、版本不兼容或结果关联不符均拒绝。

两个 manifest 分别原子发布，没有跨索引事务。C2 已发布但新证据失败时，
普通值缓存仍可用；要求重放证据的路径必须重算。证据自含值引用，不把
“C2 manifest 存在”当作发布完成。构建期 spill 使用既有延期发布机制，
分析/证据失败时两个索引都受同一提交许可保护；临时配额和 pending 容量
也计入新增产物。普通模式沿用 C2 的逐成功函数发布边界，不增加模块事务。

## 要求能力与旧缓存回退

```sh
pagosc --stage-cache=read-write --cache-dir=.pagos/cache \
  --cache-evidence=scalar-replay --cache-stats emit-hir input.pgs
pagosc --stage-cache=read-only --cache-dir=.pagos/cache \
  --cache-evidence=scalar-replay --cache-stats emit-hir input.pgs
```

CacheConfig.evidence 默认 Value，CLI 默认 cache-evidence=value。
ScalarReplay 要求 **C2 标量值复用**同时具备合法的源绑定描述，并且其中值
与 C2 值一致。数组/残留模板沿用各自原有能力契约；选择此选项不将它们
标为可重放。CLI 在 cache=off 时拒绝 scalar-replay 能力要求。

旧值项没有新证据时，默认 Value 策略仍可命中，且不会从旧结果补写描述。
显式要求 ScalarReplay 则正常执行原函数体；read-only 不写入，read-write
成功后发布独立的新证据项。支持外的函数也走原求值，不为满足要求执行
新分支或额外效果。损坏/丢失描述不会被当作能力命中。

原 C2 value/resources 及资源预算验证仍先执行；新证据不能单独用于跳过
Stage 或替代 ResourceRecipe。之后仍按当前会话检查 Fuel/构建额度，不足
时走原诊断路径，不重复收费。物理统计新增 replay-writes、replay-hits、
replay-rejected；原 writes/residual-writes 继续计 C2 产物。replay-hits 表示
证据已验证，可能随后因当前预算不足而放弃整个值命中，不是语言 cache_hits。

## 独立读取与源对应性

[scalar_replay.h](../include/pagos/stage/scalar_replay.h) 提供：

```cpp
auto record = load_scalar_replay(store, checked_module, types, specialization);
```

调用方提供与目标一致的已检查模块/TypeTable，以及原 kind 1 的完整特化键。
接口重建当前身份及受支持的配方，读取独立证据项，验证：

1. 当前定义、签名、参数和完整键匹配；依赖指纹已由当前 checked module
   重新得到。版本、目标、编译器、输入或定义内容变化均隔离。
2. value/replay 两个必需对象和联合语义指纹完整；TypedValue 与描述的类型/
   预期指纹一致，所有结构锚点在当前源码中有效。
3. 描述中的整个类型化节点数组、操作、操作数、字面量、输入槽及位置与
   当前源码重建的配方逐项相同。仅锚点存在或重新计算 CAS 哈希不够。

因此 C4e1a 的纯结构准入在本接口中进一步建立了**源码到配方的对应关系**。
这仍不等于在读取时核算实际运算；C4e2 必须在独立预算内执行并核对结果。
描述和关联值的数学一致性不能由编码/内容哈希代替，不重写原缓存结果。

读取没有 StageAnalyzer、配方执行或隐式写入；返回值拥有描述、TypedValue
及对象引用，不依赖 AST/Analyzer 生命周期。SourceAnchor 允许纯空白编辑
映射到当前位置；原始源码修订锁仅用于查询任务，本接口不使用它。
错误区分 MissingDescriptor、UnsupportedSource、InvalidDescriptor 与
StorageFailure，并保留细分 codec/存储原因。旧 Literal 没有合成回退。

读取仍会完整加载两个对象，源码配方及校验集合也完整驻留；FileStore 的
对象限制和 codec 限制不是总 RSS 上限。本轮不提供分页、重放 Fuel、详情
输出或取消接口，不声称已经完成 P1 的分支/生成器理由或纯计算详情重放。

## 验收

8 项新单元测试覆盖真实 Stage 的参数/字面量/运算及 owned 记录、默认和
要求能力的冷热路径、旧项升级、缺失/损坏、重算联合哈希后的错误配方/
位置/结果关联、未来版本和缺必需摘要、所有 Fuel 边界、bool/整数/32/64
位 usize、源编辑与目标/编译器隔离，以及明确不准入的函数体。
同时核对诊断、全部 AnalysisStats、HIR、Provenance 和 MIR。

独立进程 harness 的 read 分支从不构造 StageAnalyzer，验证跨进程发布/
读取、逐次相同描述、空白位置变化、对象丢失/损坏、旧项能力回退及只读
文件内容/mtime 不变。真实 CLI 测试检查参数拒绝、能力命中和预算错误；
既有 spill 失败案例额外断言新证据也没有发布。

验证：Debug 全量 CTest 444/444（含 lit），GCC 与 ASan/UBSan 各
443/443 非 lit CTest；底层协议目标文件强制重建后的三种构建专项均为
47/47。C++ 格式、Python 编译、49 份 Markdown 及 `git diff --check`
通过。

```sh
ctest --test-dir build/debug \
  -R 'ScalarReplay|pagos-scalar-replay-process' --output-on-failure
```

下一轮 C4e2a 实现纯配方的独立有界执行与结果指纹核验；之后 C4e2b 再接入
显式详情 CLI。新解释不会运行整个模块或隐式执行外部效果。
