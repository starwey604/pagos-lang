# Stage Evidence P0 迭代计划

状态：P0 已实现。工程探索基线为 `7f44ea0`，日期为 2026-10-11。
本计划对应 Stage Model 讨论中的 P0，与已完成的
[M3 前置准备 P0](m3-preparation.md#p0模拟平台与工具链确认)不是同一项工作。

## 目标与边界

让 `StageAnalyzer` 在决定 Stage 时主动提供规则依据，
`ProvenanceRecorder` 将依据转成证据图，工具读取并解释证据。
图查询仍不参与 binding-time analysis、Comptime 求值或 residualization。

本轮先迁移四类决策：`RuntimeBoundary`、`RuntimeSelection`、
`MemberProjection`、`ConservativeFallback`。普通严格运算、效果和执行结构
暂时沿用现有记录方式。完整 Comptime 推导、普通函数特化解释、阶段收敛、
递归不动点、What-if、LSP、跨修订缓存及诊断路径替换另行迭代。

P0 应保证 Stage、已知值、残留行为和现有错误诊断不变。
它改变证据的产生位置，不增加语言功能。

## 实施前的工程现状

- [StageAnalyzer](../lib/stage/stage_analyzer.cpp)负责推断与部分求值；
  `analyze_binding` 创建显式边界，`analyze_if` 处理分支，
  `analyze_field` / `analyze_index` 恢复成员知识。
- [Recorder](../lib/stage/provenance_analysis.cpp)中的 `construct` 根据 HIR
  种类重建 `RuntimeBoundary`、`Select`、`Projection` 等关系。
  `finish` 还根据 `recursive` 和函数体 Stage 推导“recursive completion”。
- [私有接口](../lib/stage/provenance_recorder.h)已有 `projection` 和
  `loop_index` 主动事件。前者只记录成功恢复常量的投影，并会跳过纯 Comptime
  聚合；Runtime 成员、被边界阻止的投影仍由 HIR 重建。
- [Builder/FrozenGraph](../include/pagos/stage/provenance_graph.h)已经分离。
  [快照](../include/pagos/stage/provenance_analysis.h)拥有图、位置与绑定根，
  不依赖 HIR 生命周期，但还没有独立的语义决策规则接口。
- [真实 Stage 单元测试](../tests/unit/stage_provenance_test.cpp)已有 14 项，
  覆盖多来源、实参替换、成员精度、效果、递归及快照生命周期。
  这些测试主要断言来源集合，尚未验证决策规则来自哪个语义分支。

因此不需要重建图存储或查询系统。主要修改应集中在 Stage 与 Recorder
之间的接口，以及快照中的可查询决策摘要。

## 现有规则清单

| 决策 | 实际语义点 | 当前结果及证据要求 | P0 范围 |
| --- | --- | --- | --- |
| 常量与纯运算求值 | `analyze_expression`、`analyze_binary` 等 | 已知结果可为 Comptime，工作单独保留 | 完整理由后续处理 |
| 严格运算传播 | `make_runtime` 的调用方 | Runtime 操作数影响结果 | 普通运算暂留 HIR 记录 |
| 显式 Runtime 边界 | `analyze_binding` | 任意合法初始化值变为 Runtime，值查询不穿透 | 主动记录 |
| Runtime 分支选择 | `analyze_if` | 条件影响结果，相同分支常量目前也为 Runtime | 主动记录 |
| 逻辑短路 | `analyze_binary` | 也可生成 HIR `If`，但条件不一定 Runtime | 同步审计与标明依据 |
| 成员投影 | `analyze_field`、`analyze_index` | 已知成员、Runtime 成员、不可穿透聚合分别处理 | 主动记录 |
| Runtime 索引 | `analyze_index` | 保留数组和索引依赖及边界检查 | 保持语义，不恢复常量 |
| 递归调用完成性 | `analyze_call`、`make_residual_call` | 常量尾结果不能证明递归完成，调用保持 Runtime | 主动记录保守原因 |
| 早返回与循环控制 | `analyze_function_body`、循环分析 | 独立的返回控制与循环规则 | 保留当前实现与回归 |
| C ABI、内存和外部输入 | 调用、指针和存储分析 | 固定 Runtime 来源及相应效果 | 保留，测试与保守原因的区别 |

`ConservativeFallback` 首先只覆盖已经存在的递归完成性规则。
Runtime 控制、Runtime 索引和 C ABI 边界不能被笼统归入“分析不够精确”。
预算耗尽仍是错误；查询截断或中止摘要仍是解释不完整，均不是新的 Stage 理由。

## 接口设计与落地

采用明确的语义事件，先保持 Recorder 为 Stage 内部接口。
事件参数表达 Analyzer 已经确定的依据，Recorder 不再重新选择规则。
以下名称是设计示意，实施时可沿用现有命名风格：

```text
runtime_boundary(result, initializer, binding_site)
runtime_selection(result, condition, then_value, else_value)
member_projection(result, aggregate, selection, member, index)
conservative_runtime(function_or_call, RecursiveCompletion, body)
```

实际私有接口为 `runtime_boundary`、`selection`、`projection` 与
`conservative_completion`。快照的 `StageRule` 标识上述四类语义，
另用 `OperandPropagation` 区分已知条件的短路操作数传播。
`StageReason` 细分成员投影状态与 `RecursiveCompletion`。
`decisions()` 枚举稀疏摘要，`decision(NodeId)` 查询特定节点。

投影的 `selection` 应明确区分已知常量、选中 Runtime 成员、被不透明聚合
阻止的选择和 Runtime 索引。若存在实际成员 HIR，由 Analyzer 提供；整个聚合
已知时记录简短常量依据。Recorder 不再调用 `Builder::project` 去决定成员
是否可知，避免在图层重复定义 `Reference`、`Sequence`、边界和分支的透明性。
已选成员提供值依赖，整个聚合初始化和索引求值提供工作依赖。

语义规则标识与图存储规则分开：`ProvenanceAnalysis` 已增加稀疏的
决策摘要表，以图内 `NodeId` 关联结果 Stage、规则、源码位置和必要的子原因。
依赖继续放在图边中，不为每个表达式复制路径或全部来源集合。
P0 不给所有 HIR 节点增加 `StageDecision`，也不把图 ID 放进 AST 或 MIR。

现有 `Rule::Select` 同时服务分支、循环索引及返回控制，
`Rule::RuntimeBoundary` 同时服务显式边界、C 调用和递归完成性，
因此它们不能直接充当语义规则标识。保守原因必须有结构化标识，不能仅靠
“recursive completion”字符串识别。可以暂留现有图终端表示以保持查询行为，
工具通过决策摘要区分显式边界、真实输入和保守判定。
若以后新增独立图规则，再单独验证查询和基准工具的兼容性。

绑定根可能是 `Use` 或 `RequireComptime`，结果还可能被 `Sequence` 包装。
测试和工具应能读取关联决策节点，不能假定绑定根本身就是决策。
本轮通过 C++ 快照接口提供规则查询；默认 CLI 输出保持当前格式。
完整解释视图的格式留到后续设计。

## 必须处理的传播路径

1. **包装与身份**：事件在新结果发布给绑定或查询之前记录。
   `Reference` 和 `Sequence` 保留同一个决策及残留工作，不能因事件复制 HIR
   而重复执行读取。一个表达式身份不能被重新标成另一条互斥决策。
2. **内联实例化**：`instantiate_inline` 复制 HIR 并替换符号参数。
   原表达式身份上的事件不会自动出现在副本上；需在现有实例映射中迁移证据，
   保持参数替换和 DAG 共享，不能把首次调用的实际输入写入共享决策。
3. **局部契约**：`concrete` 会复制被实参替换的图路径，需同步保留决策摘要。
   函数分析失败后，局部 Comptime 契约仍应能解释实际实参。
4. **函数摘要**：共享摘要保留符号依据，每次调用保留独立上下文。
   递归回边第一次出现时函数体可能尚未完成；Analyzer 在定义完成时明确
   提交完成性判定，Recorder 的 `finish` 只发布已有判定。
   不得仅因调用遇到未完成函数体就断言其最终是保守常量结果。
5. **快照与生命周期**：构建映射继续验证弱身份，防止临时 HIR 地址复用。
   冻结后只保留 ID 和自有数据，Analyzer、AST、HIR 销毁后仍可读决策。
   纯成员投影新增证据应保持摘要规模，不保存完整 Comptime 求值历史。

短路尤其需要独立核对：`true && runtime_bool` 当前可能生成条件为 Comptime
的 HIR `If`，结果 Runtime 来自被选中的操作数。
它不能被标成“Runtime 条件导致选择”。可使用一个窄的操作数传播标识处理
这种既有构造；这不要求本轮迁移所有严格运算。
显式边界和递归完成性也分别表示可见性契约与保守规则，不能混成外部读取。

## 实施顺序与完成情况

| 步骤 | 修改范围 | 可独立审阅的完成条件 |
| --- | --- | --- |
| 0. 固定基线 | 工具链、现有测试与 Release 基线 | 在支持的 LLVM 完整 SDK 下复现当前行为 |
| 1. 建立事件与摘要接口 | Recorder 私有头、实现、快照公有头 | 明确规则身份、所有权、包装及复制规则；接入显式边界 |
| 2. 迁移选择与投影 | 分支、短路、字段和索引决策点 | Analyzer 提供选择依据；覆盖已知、未知和边界受阻成员 |
| 3. 迁移保守完成性 | 调用、定义完成、摘要发布 | 保守原因由 Analyzer 提供；移除 `finish` 的独立推断 |
| 4. 汇总验收 | 单元、lit、文档、成本对比 | 决策一致，执行与诊断回归通过，新增成本有记录 |

步骤 1 到 3 都应同时处理其触及的内联、实参替换和快照生命周期。
不把传播正确性留到最后补齐。每一步确认新路径后移除对应的事后猜测；
已迁移事件缺失应能被一致性检查发现，不能默默回退并伪造同一语义规则。
未迁移规则的 HIR 记录路径继续明确保留。

上述迁移已完成。`construct` 对缺少语义事件的四种 HIR 节点报内部一致性
错误；`finish` 不再根据函数体 Stage 或 `recursive` 标志创造完成性依据。
Analyzer 在结果、实际成员或函数定义确定时提交对应事件。

内联实例化复用现有 HIR 实例映射，图替换符号参数并复制决策；同一次实例中
共享依赖只复制一次，不同调用的残留工作身份保持独立。
不含迁移决策的普通折叠常量无需额外复制证据。
局部契约只复制实际改变的路径，并同步迁移其摘要。

## 验收用例

| 用例 | Stage 与理由 | 来源、残留行为和反例 |
| --- | --- | --- |
| `runtime let x = 7` 与直接外部读取 | 都是 Runtime，前者为显式边界 | 原因类别不同；边界值查询不穿透，初始化效果保留 |
| Runtime 条件选择 `1/2` 或 `7/7` | 均为 RuntimeSelection | 控制来源正确；`comptime` 要求仍失败，不引入收敛 |
| 短路、静态条件及带返回分支 | 依据与实际语义一致 | 不虚构 Runtime 条件，未选效果不执行，返回控制不丢失 |
| 混合记录与数组的已知成员 | Comptime、MemberProjection | 无其他成员的值来源；构造读取仍执行一次 |
| Runtime 成员及不透明聚合 | Runtime、对应投影依据 | 只选实际成员；显式边界、分支与返回作用域不被穿透 |
| Runtime 索引 | Runtime | 不因元素相同恢复 Comptime，检查和索引效果不丢失 |
| 递归工作后返回 `7` | Runtime、RecursiveCompletion | 不冒充外部输入或显式边界；效果递归查询仍可为 partial |
| 数据依赖递归与纯 Comptime 递归 | 保持当前 Stage | 不把所有递归一律标成完成性回退，预算错误仍报错 |
| 缓存复用、嵌套内联和局部契约失败 | 原规则经实例化仍可查 | 第二次调用指向自身实参，内部读取按调用上下文区分 |
| 快照独立持有及重复分析 | Stage 与摘要一致 | 销毁输入后可查询；下一轮不继承上一轮身份 |

优先扩展现有 Stage Provenance 单元测试，直接断言结构化规则、Stage 和图边
关系，而非只检查标签文字。lit 使用已有 `%run_residual`、`%run_optimized`
和 volatile/C harness，检查两条分支、读取数量、效果顺序以及陷阱。
已有负例应继续确认 `E2001` 与原 dependency path。
如果图构造辅助接口有变更，再补对应的图层单元测试。

在规定工具链中运行 Debug 全量 CTest；GCC、Release、ASan/UBSan 运行触及的
专项与 CLI 用例。格式、clang-tidy、文档和差异空白检查随改动完成。
复用 [编译基准](../scripts/benchmark.py)做同机 Release 前后对比，记录普通
编译耗时与 RSS；必要时补一个含大量成员投影的真实程序，观察节点与摘要增长。
独立图实验不代表实际编译成本，不设没有测量依据的百分比性能门槛。

## 本轮探索的验证与环境

最初的探索只形成计划，未修改 C++、语言规范、测试或 CLI。
初始工作树干净，最新提交已提供真实 Stage provenance 快照。

本机 Clang 与 `llvm-config` 为 23.1.1。运行 `cmake --preset debug` 时，
CMake 自动找到 `/opt/rocm/llvm/lib/cmake/llvm` 的 LLVM 22.0.0git，随后因缺少
RISC-V 后端失败。初始版本检查也不接受系统 LLVM 23。

随后按用户要求使用本机 LLVM 23.1.1 验证：显式选择
`/usr/lib/cmake/llvm`，将允许范围扩为 LLVM 22/23，适配目标特性名称的
`Key` / `key()` API 及 lit 23 的外部 shell 显式启用选项。
Debug 构建和全部 171 项 CTest 通过，包含 170 项 C++ 单元测试与 lit 回归。
修改的 C++ 文件通过 clang-format 和 clang-tidy；文档与差异空白检查通过。
本机 Debug 已缓存正确的系统 LLVM 路径，可作为 P0 后续开发环境。

固定 CI 的 LLVM 22.1.8 镜像和脚本契约保持不变；探索阶段未重跑 LLVM 22、
GCC、Release、Sanitizer 或独立 QEMU RV32 验证。
同机 Release 基线已在 P0 实施前保存。配置与验证命令见
[开发指南](development.md#configure-and-build)。

## P0 实施验证

实施前的 Release 编译器保存在 `build/p0-baseline/pagosc`，包含上一轮的
LLVM 23 适配；本轮使用相同 Clang/LLVM 23.1.1 和 GCC 16.2.1。
新增 9 项决策一致性单元测试，并加强递归与 C ABI 的类别区分断言。
新增 lit 用例检查嵌套内联投影、Runtime 分支、实参求值及优化后的执行行为。

Debug 完整回归通过 180 项 CTest，包含 379 个 lit 用例。
Release、GCC 和 ASan/UBSan 各通过 34 项 Provenance/决策专项，
以及 27 个相关 lit 用例；Sanitizer 验证保持泄漏检查开启。
此次没有重跑 LLVM 22 固定 CI 或独立 QEMU 固件验证。

成本测量复用 `scripts/benchmark.py`，新增
[大量成员投影](../benchmarks/member-projections.pgs)工作负载。
它通过 1024 个混合配置的已知字段生成 Comptime 表，同时保留每次构造的读取。
基线与新编译器使用相同源码、预算和目标，交替采样；原始结果保存在
`build/benchmarks/p0.json`，不将进程 RSS 当作纯证据图内存。
每组预热两次、采样七次，共六组工作负载、三种编译模式。
本机没有 `/usr/bin/time`，本轮复用脚本的工作负载与采样流程，临时用
Linux `wait4` 读取单次子进程 RSS，以 1 ms 轮询等待并保留 30 秒超时。
JSON 的 `measurement` 标明该采集方式；未修改脚本默认的 GNU time 采集器。

大量成员投影的结果如下；时间包含进程启动和等待误差，不能据此判断提速：

| 模式 | 基线中位时间 | P0 中位时间 | 配对时间比 | 基线 / P0 峰值 RSS |
| --- | --- | --- | --- | --- |
| `check` | 17.31 ms | 17.26 ms | 1.000 | 37408 / 37728 KiB |
| `emit-mir` | 24.66 ms | 24.61 ms | 1.086 | 38660 / 38548 KiB |
| `emit-llvm` | 25.66 ms | 23.57 ms | 1.001 | 42376 / 42252 KiB |

其他工作负载的配对时间比在 0.894 到 1.134 之间，进程启动和毫秒级波动
占比明显。此次样本没有显示稳定的退化，也不构成大程序成本或图内存的结论。
六组源码的 `check`、`explain-stage`、`emit-hir`、`emit-mir` 和 `emit-llvm`
共 30 项输出，与基线的 stdout、stderr 逐字节一致。
修改的 C++ 通过 clang-format 和 clang-tidy；文档及差异空白检查通过。

## 后续迭代

P0 完成后，先插入 [Cache Architecture RFC](cache-architecture-rfc.md)，
明确语义查询、稳定身份、依赖验证、残留工作、预算及证据存储边界。
该 RFC 先规划纯 Comptime 结果的缓存实验，不把磁盘数据库作为 P1 前提。
[实施计划](cache-implementation-plan.md)将 C1–C3 拆成六轮，默认采用文件
索引、CAS 与分段二进制证据快照；先完成协议及真实计算复用的验证，
不等待数据库、mmap 或压缩选型。
[C1a](cache-identity-c1a.md)已交付稳定身份与规范编码，
[C1b](cache-store-c1b.md)已交付文件存储。下一轮为 C2a 纯标量复用；
Analyzer 的求值与本节 P0 证据记录路径保持原状。
再补 P1 的有限 Comptime 理由：已选静态分支、纯运算摘要、
调用结果和特化的依据，并测量记录成本。随后单独实现相同标量常量的分支收敛，
新增语义规则、更新规范、检查两种条件下的效果与陷阱。
字段收敛需要再明确知识如何跨参数、返回和缓存传递。

递归来源求解、批量查询成本和 What-if 都可继续利用现有图，
但不作为 P0 的完成条件。每一轮应保持同一个原则：阶段依据由作出该判断的
语义组件提供，存储与解释组件不另行定义 Stage 语义。
