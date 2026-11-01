# C1–C4 实验协议基线 S1

日期：2026-11-01。实现起点为 S0 全绿提交 `ae92865`，冻结范围是现有
C1–C4 子集的编码、准入、恢复和交付契约。S0 与 S1（`cb58c7f`）已合入
本地 `llvm`，对应代码已通过四配置 CI。本文件是协议清单的
统一入口，细节与实验历史保留在各轮实现记录中。

这不是语言稳定性、公共 C++ ABI、完整 P1 解释、任意版本升级迁移或全量
增量编译承诺。S1 不扩大缓存或重放准入，不改变现有 wire 字节。

## 兼容政策与变更规则

- 基线固定逻辑字段顺序、位宽、标签、哈希域、必需对象与验证顺序。
  不允许用 C++ 对象内存布局直接落盘。序号按附属协议解释，不能只看
  共同头部的 format=1 就把不同对象或任务模式当成同一种格式。
- 原始整数采用小端编码；共同 `PGC1` 头为四字节 magic、u32 格式号、
  u8 ObjectKind。长度及类型编码以 [codec](../lib/cache/codec.cpp) 和
  [wire](../lib/cache/wire.h) 为准。哈希是 SHA-256，输入为 u64 小端
  domain 长度、domain 字节、payload；域字符串也是兼容契约。
- 改变字节含义、顺序、计费规则、引用布局或求解语义时，必须同时评审
  对应格式/engine/预算/能力版本、哈希域、读写器和测试。不能只修改
  编码器与解码器后重新生成样本。仅增加版本字段而不接入键/验证也不够。
- 新增标签只使用新编号，不重用或重排已有编号。当前有直接编码 HIR/
  运算枚举的内部模板，修改这些枚举同样需要模板版本及兼容评审。
- 默认采用精确版本匹配。只有下表明确列出的读取分派属于支持范围；
  没有自动磁盘迁移或任意历史版本支持。未知格式不能猜测字段继续读取。
- 相同格式不保证不同编译器构建可复用。QueryContext 还绑定保守的
  compiler build identity、semantic options、完整目标描述及协议版本。
  CLI 升级后旧记录可能不可用；需显式正常编译重新发布。任务只能在
  原证据闭包、位置映射和源码修订内续接，不能把新源码悄悄绑定到旧任务。

## 版本与对象清单

下列值均为当前实际编码，不是统一的产品版本。`record model=1` 指 P0，
不代表完整求值历史或完整语言证明。

| 协议 | 当前值与读取范围 | 实现入口 |
| --- | --- | --- |
| 共同 payload 格式 | `PGC1`，format=1 | [codec.h](../include/pagos/cache/codec.h)、[codec.cpp](../lib/cache/codec.cpp) |
| 查询/预算协议 | query=1、budget=1，读取精确匹配 | [key.cpp](../lib/cache/key.cpp) |
| FileStore 对象/manifest | `PGO1`/`PGM1`，store=1 | [store.cpp](../lib/cache/store.cpp) |
| SourceAnchor | 共同头 kind 3、format=1；无独立附加版本 | [cache_identity.cpp](../lib/stage/cache_identity.cpp) |
| ResourceRecipe | kind 4、budget=1；数组扩展标签 1，残留扩展标签 2 | [scalar_cache.cpp](../lib/stage/scalar_cache.cpp) |
| 残留模板 | kind 7、template=1 | [residual_template.cpp](../lib/stage/residual_template.cpp) |
| 模块快照/位置 | kind 5/6、provenance format=1；图 record model=1 | [provenance_snapshot.cpp](../lib/stage/provenance_snapshot.cpp) |
| 分块证据 | kind 8、evidence block=1、record model=1 | [evidence_view.cpp](../lib/stage/evidence_view.cpp) |
| 函数证据单元 | kind 9/10/11、function evidence=1、record model=1 | [function_evidence.cpp](../lib/stage/function_evidence.cpp) |
| 原生驻留查询 checkpoint | kind 12、checkpoint=1、query engine=1、record model=1 | [provenance_query.cpp](../lib/stage/provenance_query.cpp) |
| 原生分段/分页任务 | kind 14 manifest、kind 13 segment 的模式版本分别为 1/2；共同 control 仍绑定 checkpoint/engine/model | [provenance_query.cpp](../lib/stage/provenance_query.cpp)、[query_storage.cpp](../lib/stage/query_storage.cpp) |
| 拓扑驻留/分段任务 | manifest 3/4，segment=4；topology engine=1、record model=1 | [provenance_topology.cpp](../lib/stage/provenance_topology.cpp) |
| 递归驻留/分段/最终 receipt | manifest 5/6/7，segment=6；recursive engine=1、query engine=1、record model=1 | [recursive_codec.h](../lib/stage/recursive_codec.h)、[recursive_checkpoint.cpp](../lib/stage/recursive_checkpoint.cpp)、[recursive_storage.cpp](../lib/stage/recursive_storage.cpp) |
| TaskStore head | kind 14、task store=1；任务 ID/generation/status/manifest 引用与校验和 | [task_store.cpp](../lib/cache/task_store.cpp) |
| 标量重放 | kind 15，descriptor=1、scalar recipe semantics=1；evidence=1、execution=1 | [replay_descriptor.h](../include/pagos/stage/replay_descriptor.h)、[scalar_replay.h](../include/pagos/stage/scalar_replay.h)、[scalar_replay_execution.h](../include/pagos/stage/scalar_replay_execution.h) |
| 生成器重放 | kind 16/17，descriptor=1、closed generator semantics=1、scalar semantics=1、observation/evidence=1、execution=1 | [generator_replay.h](../include/pagos/stage/generator_replay.h)、[generator_replay_record.h](../include/pagos/stage/generator_replay_record.h)、[generator_replay_execution.h](../include/pagos/stage/generator_replay_execution.h) |
| 显式解释 CLI | JSON/JSONL schema=1，按 type/mode 分派；无协商接口 | [query_tasks.cpp](../tools/pagosc/query_tasks.cpp)、[recursive_summaries.cpp](../tools/pagosc/recursive_summaries.cpp)、[scalar_replay.cpp](../tools/pagosc/scalar_replay.cpp) |

ObjectKind 1–17 依次是 Value、QueryKey、SourceAnchor、ResourceRecipe、
ProvenanceSnapshot、ProvenancePositions、ResidualTemplate、EvidenceBlock、
EvidenceRoot、EvidenceSummary、EvidenceDetail、QueryCheckpoint、
QueryStateSegment、QueryTaskManifest、ReplayDescriptor、
GeneratorReplayDescriptor、GeneratorReplayObservation。
QueryKey 的 kind 1–7 依次是 EvaluatePureSpecialization、SpecializeResidual、
FreezeEvidence、FreezeEvidencePositions、FreezeFunctionEvidence、
FreezeScalarReplay、FreezeGeneratorReplay。两组编号不可混用。

TaskStore 的 kind 14 head、查询 manifest 与递归 manifest 不是同一布局；
读取入口和命名空间决定其含义。TaskStore 根下 `session.lock` 的
`pagos.query.task.v1` 标记用于 lease/清理识别，不是可忽略的任意文件。

原生 TaskStore restore 支持 manifest 1/2；显式 restore_paged 只读 2。
拓扑 raw checkpoint 读 3，TaskStore restore 分派 3/4。递归 raw
checkpoint 读 5，TaskStore restore 分派 5/6/7；旧 5/6 的未确认完成结果
隐含 sequence=1，7 保存已确认最终 receipt。各模式仍须通过原绑定及
完整性验证，不能互相替代。TaskStore head 与 CAS 封装有独立版本校验。

## 身份、必需负载和失败行为

| 范围 | 准入与必需负载 | 不支持或验证失败 |
| --- | --- | --- |
| C1 | 明确 workspace/相对模块路径、定义身份、结构指纹、TypedStageArguments；目标 triple/cpu/features/DataLayout/指针宽度/端序；值为 bool、已解析整数或具体非空标量数组 | 记录/指针值及未解析类型不编码为可命中结果；坏版本/长度/标签在分配及枚举转换前受限拒绝 |
| C1b | 不可变 CAS、规范查询键与 verified inputs、必需 artifact 名称/种类/内容；manifest 原子发布，语义冲突禁用复用 | 缺必需对象拒绝候选；可选详情缺失不破坏语义身份；只读 miss 不创建目录；存储本身不替 Stage 证明 payload 语义 |
| C2a | internal 纯叶标量、全部类型化 Comptime 实参；`value` 和 `resources` 必需 | 普通调用/控制流/内存/记录等不作为外层缓存查询，正常分析；实参携带的 Runtime 工作仍在调用方执行 |
| C2b | 封闭标量数组字面量/单层生成器/有限投影；有序 Fuel、构造预留与重复后缀 | 计算边界必须实际成功且长度匹配；空/倒序/失败不发布；预算不足回退正常分析产生同一首错 |
| C3a | 图、独立位置对象、语义与 context 指纹、section directory；当前八种图 section 均为必需 | 未知必需 section 拒绝，未知可选 section 可跳过；恢复需当前源码映射；位置修订不等同语义修订 |
| C3b | 已准入的非递归封闭标量残留模板、resources、必需符号证据；当前实参/参数槽重绑定并通过 MIR 验证 | 缺必需证据不使用模板；不持久化 AST/HIR 指针或任意 SSA MIR，不恢复一般效果/递归/聚合模板 |
| C4a/b | 函数/SCC 身份、Import 闭包、摘要及按需 detail；证据 lease 与临时 spill 生命周期 | 摘要结构缺失不同于尚未展开 detail；容量/IO 为辅助失败，按现有门禁阻止不完整语义缓存发布，不成为新的 Stage 理由 |
| C4c/d | task ID、原证据闭包/位置/源码修订、root/summary、channel/mode、engine、队列/公式/SCC 状态、pending delivery | 绑定/未知版本/损坏/缺对象明确拒绝；显式恢复不偷偷重跑 Stage；generation 竞争失败可重试但不能覆盖另一任务 |
| C4e | 原特化键、完整捕获、verified inputs、SourceAnchor、纯 recipe、记录结果；生成器另有源推导独立性、真实 bounds 和 observation | 缺旧描述报告不可用，不从最终 Literal/数组编造历史；有界执行核对结果，失败不返回临时 result/details，也不覆盖语义值 |

准入细节以 [C2a](cache-scalar-c2a.md)、[C2b](cache-array-c2b.md)、
[C3b](cache-residual-c3b.md) 及对应重放实现记录为准。普通缓存模式下，
缺失、损坏、版本/类型/资源不符或 IO 失败通常安全回退正常分析；这不是
显式 explain/read/restore 的规则。显式入口返回其 typed error、诊断或
unavailable，不进行隐式修复写入或将新分析冒充原任务续接。

ResourceRecipe 的 Fuel、深度、特化数与构造计费维持原有逻辑。命中先在
副本预检资源，失败不部分扣费，正常求值负责诊断位置与首错。物理缓存
命中/IO/耗时遥测不改变 AnalysisStats 的语义计数。预算、查询工作量、
证据容量和呈现额度是不同协议，不把逻辑容量宣称为总 RSS 上限。

## CLI schema 1 与交付规则

JSON 字段顺序不是契约；记录 type、字段类型、必需/条件字段及枚举含义是
契约。兼容增加可选字段时消费者忽略未知字段；删改必需字段、类型或既有
状态含义必须升级 schema 并同步测试。不识别的 schema/type/mode 不能
被当成成功结果。遥测和辅助 reason 允许扩展，消费者保留原值并按已知
状态处理，不把新 reason 猜成 verified。

| 输出类型 | 必需语义及条件字段 | 既有规范与测试 |
| --- | --- | --- |
| `page` + `status` JSONL | schema/type/task；page sequence/records/progress；status generation/state/mode/root/channel/storage/stage_runs；原生 coverage 与拓扑 structural/sources_solved 分开 | [查询 CLI](provenance-query-cli-c4c3c.md)、[进程测试](../tests/query_cli_process.py) |
| `recursive-result` + `status` | result sequence=1、证据修订绑定和有限摘要；status 的任务状态/确认水位；最终 receipt 持久化后可释放大表 | [递归任务 CLI](provenance-recursive-tasks-c4d3c.md)、[进程测试](../tests/recursive_task_cli_process.py) |
| `scalar-replay` | schema/type/status/stage_runs；verified 才有 result/details/selection 和独立版本/工作统计；unavailable 有 reason，无成功详情 | [标量 CLI](provenance-scalar-cli-c4e2b.md)、[进程测试](../tests/scalar_replay_cli_process.py) |
| `generator-replay` | verified 另有 selected-element 范围、element_index、iteration_value、bounds 与 descriptor/语义版本；失败不带未核验 bounds/result/details | [生成器 CLI](provenance-generator-cli-c4e3c.md)、[进程测试](../tests/generator_replay_cli_process.py) |

消费者持久提交记录后，按 `(task, sequence)` 确认和去重。stdout 成功不
等于已确认；未确认页重送同一序号，不重复推进或计费；没有消费端 receipt
协议就不承诺 exactly-once。节点/上下文 ID 只在所绑定快照内有效。
completed、coverage/precision、详情可用性和呈现完成度必须分别解读。
拓扑 completed 不代表具体来源求解完毕，有限递归摘要也不是具体来源列表。

重放整数及 bool 用带 type 的字符串表示，保留 u64 精度。重放只执行已
准入纯配方，禁止 Runtime、MMIO、volatile、外部调用或文件/环境输入。
重放退出码保持 0 verified、3 unavailable/限额、130 取消、2 参数错误、
1 源码/目标/类型或操作失败；输出额度太小、取消或前置诊断允许 stdout
为空。查询入口的退出/维护行为继续按其自身 CLI 文档，不能套用重放码表。

## 固定样本与测试门槛

[固定样本目录](../tests/Inputs/protocol-baseline/README.md) 保存从上述实现
产生的规范十六进制字节，使用固定测试 context，而非真实 CLI 的滚动
compiler build。测试不自动更新样本：同构 round-trip 之外，还需要旧字节
解码与当前源码绑定，防止编码器/解码器同时漂移。

| 契约 | 测试入口 |
| --- | --- |
| 值/键/Anchor 固定字节、未知版本、长度与标签 | [codec 单元](../tests/unit/cache_codec_test.cpp)、[key 单元](../tests/unit/cache_key_test.cpp)、[跨进程身份](../tests/cache_identity_process.py) |
| manifest/CAS 必需对象、冲突、原子发布与只读边界 | [store 单元](../tests/unit/cache_store_test.cpp)、[独立进程恢复](../tests/cache_store_process.py) |
| C2/C3b 固定存储与 off/cold/hot 结果、资源、首错、效果等价 | [StageCache](../tests/unit/stage_cache_test.cpp)、[lit 缓存 harness](../tests/lit/Inputs/run_cache.py) |
| C3 图/位置固定字节、未知必需/可选 section、当前位置恢复 | [snapshot 单元](../tests/unit/provenance_snapshot_test.cpp)、[跨进程 harness](../tests/lit/Inputs/run_snapshot.py) |
| C4 函数闭包、lease、spill/清理和辅助失败 | [function 单元](../tests/unit/function_evidence_test.cpp)、[spill 单元](../tests/unit/stage_spill_test.cpp)、[进程测试](../tests/stage_spill_process.py) |
| 原生/分页/拓扑固定状态与恢复、绑定/格式拒绝、确认续接 | [query checkpoint 单元](../tests/unit/query_checkpoint_test.cpp)、[跨进程 checkpoint](../tests/query_checkpoint_process.py)、[拓扑](../tests/topology_checkpoint_process.py)、[TaskStore](../tests/unit/task_store_test.cpp) |
| 递归 5/6/7 固定状态、最终 receipt、损坏拒绝 | [recursive checkpoint 单元](../tests/unit/recursive_checkpoint_test.cpp)、[跨进程恢复](../tests/recursive_checkpoint_process.py)、[持久 CLI](../tests/recursive_task_cli_process.py) |
| 重放固定描述、未来版本/重新封装伪造拒绝、零额外效果 | [标量描述](../tests/unit/replay_descriptor_test.cpp)、[生成器描述](../tests/unit/generator_replay_test.cpp)、[记录](../tests/unit/generator_replay_record_test.cpp)、[effect harness](../tests/generator_replay_effect_process.py) |

格式变更的最小评审材料：受影响清单项、版本/哈希域处理、旧格式读取或
明确拒绝政策、固定样本差异，以及正反恢复和跨进程验证。不能仅以重新
生成样本后测试全绿作为兼容性证据。本文和固定样本不替代更广工作负载
测试，也不证明恶意数据的任意语义真实性或一般编译器内存上限。
