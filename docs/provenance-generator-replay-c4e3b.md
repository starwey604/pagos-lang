# 真实生成器记录与选定元素重放 C4e3b

日期：2026-10-28。承接 [封闭生成器准入](provenance-generator-admission-c4e3a.md)，
完成 [实施计划](cache-implementation-plan.md) 的 C4e3b 库接口与真实 Stage
缓存接入。显式元素 CLI 和跨进程/effect harness 留给 C4e3c。

## 实际记录和能力隔离

准入仍限于内部函数的唯一尾生成器、无语句的纯标量边界和元素表达式，
捕获为完整类型化 Comptime 参数。元素独立性只能从当前 checked AST 取得，
转换、调用、全局读取、索引、嵌套或状态操作均不扩展准入。

实际 Stage 求值成功取得两个 u32 边界后，将 first/last 交给 ResourceCapture。
记录器核对只有一次边界事件、该事件处于边界 Fuel 之后/数组预留之前、
源码位置相同、范围递增且与函数声明数组长度相等。数组资源预留与后续
每个元素的 Fuel 模板仍必须完整匹配。只有成功的纯 Constant 结果才发布
生成器证据；不会由最终数组长度反推边界，也不执行额外边界计算。

新增 QueryKind 7 FreezeGeneratorReplay，能力后缀为
`;generator-replay-evidence.v1`。独立 manifest 必须包含三个必需对象：

- Value：完整类型化数组。
- GeneratorReplayDescriptor（kind 16/v1）：源码边界/元素配方、全部捕获及
  迭代槽。
- GeneratorReplayObservation（kind 17/v1）：描述指纹、真实 first/last 和
  完整类型化数组的预期指纹。

Observation 是固定结构：PGC1/kind 17、版本 u32、描述摘要 32 字节、first
和 last 各 u32、结果摘要 32 字节。必须精确消费，拒绝未知版本或空/逆序
范围。数组值域为 `pagos.generator-replay-value.v1`；联合证据摘要分别
哈希三个规范载荷后，以 `pagos.generator-replay.evidence.v1` 域固定长度
拼接哈希。数组值和源码描述的旧格式保持兼容。

默认 ReadWrite 对实际执行且准入的生成器发布该证据；已有 C2 热值项不会
自动补造 P1 记录。库级 CacheEvidence::GeneratorReplay 可要求数组复用时
同时具备该能力，缺失/损坏正常重新执行，ReadWrite 可升级独立能力项。
旧值模式、标量能力模式和残留模板的复用条件保持不变。该新枚举尚未成为
生产 CLI 选项。

物理 replay_writes/replay_hits/replay_rejected 统计同时覆盖标量及生成器
证据，不改变原 AnalysisStats。Spill 模式沿用现有延迟发布与失败门控；
证据未获准提交时，生成器对象不会单独越过门控发布。

## 独立读取

load_generator_replay 先从 checked AST、匹配 TypeTable 和当前特化键重建
源码准入，再从独立 manifest 读取三件套。逐项比较完整描述、依赖/联合摘要、
观察版本/描述指纹、数组类型/长度和实际数组载荷指纹；返回持有准入凭据
且不含 AST 指针的记录。缺任一必需对象、损坏/过期/错误源码或未知格式
明确不可用，不退回普通 C2 值，也不运行 Stage、运算或写缓存。

加载验证绑定和源码对应性，不证明运算结果。重新封装后具有合法 CAS 和
联合摘要的假值仍须由执行器对实际计算核验，不能把文件完整性当成求值证明。

## 有界单元素执行

execute_generator_replay 接收已准入的记录和数组下标 k。下标相对数组起点，
实际迭代值是 first + k。它先验证记录绑定，然后按顺序执行 begin、end，
分别与真实观察核对；确认递增及长度后，只执行 k 对应的元素配方，与原数组
的第 k 个类型化值比较。迭代值计算在已验证 u32 范围内，不发生溢出。

标量和生成器共用同一个私有纯运算实现，沿用 IntegerValue 的目标宽度、
有符号规则、除零/非法移位错误，不构造虚假的标量函数键或预期结果。
bool、各整数宽度和 usize 捕获、常量元素及同名参数遮蔽均保留准入语义。

独立额度沿用 ScalarReplayExecutionLimits：

- Fuel：每次尝试执行节点消耗 1，边界与元素三次结果核验各消耗 1。
- 工作值：三个配方最大节点数乘 sizeof(ScalarConstant)，复用一个缓冲区。
- 输出：固定 VerifiedGeneratorReplay 对象加选定元素步骤的保留容量，另限
  步骤数。详情范围只选择元素配方的节点，不选择边界节点或其他元素。
- codec：描述/数组校验的字节、元素、字符串与深度单独限额。
- 取消：入口、准入前后、每个节点和每次核验及公开结果前检查 stop_token。

即便不请求详情，也必须执行两个边界及选定元素的完整依赖。失败只返回错误、
统计、阶段及可选节点/整数错误，不公开临时值和未核验详情。一次性重试需
从头开始，没有 checkpoint、Stage 调用、外部效果或缓存写入。

成功结果包含描述摘要、记录数组摘要、k、实际迭代值、选定类型化值、步骤
及独立统计。结果仅证明该元素与记录数组的对应值一致，不宣称其他元素已
重新执行或验证。输入数组仍需整体读取/解码/哈希，受 codec 限制；跳过其他
元素指不执行它们的计算，不能据此宣称端到端 IO/校验开销为常数或提供硬
墙钟期限。输入所有权与 codec 临时载荷不计入执行工作缓冲/输出统计。

## 验证与后续

新增 14 项 GeneratorRecord 测试，覆盖真实范围与结果、跨 AST 生命周期、
完整三件套与旧能力升级、每个必需对象缺失/损坏、重新封装的错误源码/
指纹/范围/值、全部 Fuel 边界、详情子范围、精确工作/输出额度、取消、
算术错误阶段、目标宽度、变量遮蔽和构建/Spill 失败门控。Stage 正常与
不足预算对照核对 11 项逻辑统计、HIR/Provenance、诊断及成功时的残留 MIR。

真实 32,768 元素生成器的首尾查询均只执行 2 个边界节点和 3 个元素节点，
共 8 Fuel，工作值缓冲为 3 × sizeof(ScalarConstant)。两次查询的执行统计
相同；这不是整体数组加载/校验或端到端内存的常数开销声明。

验证结果：

- Debug 全套 483/483，含 lit。
- GCC 串行 482/482，排除 lit。
- ASan/UBSan 482/482，排除 lit。
- 生成器、既有标量、C2 与 Spill 聚焦测试 98/98。
- 文档 53 个文件、clang-format 和 git diff --check 通过。

GCC 沿用串行验收；[C4e2a](provenance-scalar-execution-c4e2a.md) 记录的旧
并行偶发问题未在本轮定位或修复。

下一轮 C4e3c 接入显式元素详情 CLI、独立进程读取以及 effect harness；
仍不改写默认 Stage、构建预算、原残留结果或 C3 P0 图。
