# 有界纯标量配方执行与结果核验：C4e2a

2026-10-28。[C4e1b](provenance-scalar-replay-c4e1b.md) 已验证当前源码与
配方对应，本轮增加独立的一次性执行器，完成实际运算与记录值的一致性核验。
成功后才返回重放的值和选定详情；失败不公开未核验的值前缀，不修改缓存。
这是标量函数体的纯重放，C3 图仍为 P0，正式详情 CLI 留给 C4e2b。

## 使用与保证

公开接口见
[scalar_replay_execution.h](../include/pagos/stage/scalar_replay_execution.h)：

```cpp
auto record = load_scalar_replay(store, checked_module, types, specialization);
if (!record)
    return;
auto result = execute_scalar_replay(
    *record, {.fuel = 10000, .output_records = 1},
    {.first = record->descriptor.nodes.size() - 1, .count = 1});
// Only success contains a verified TypedValue and retained root-node detail.
```

load_scalar_replay 建立源码/定义/目标/输入绑定，execute_scalar_replay 则重新
验证描述结构、记录值的类型/指纹，再按后序数组执行输入、常量和纯运算。
调用方须先使用读取接口建立源对应性；手工组装的记录也可用于数学验证，
但执行器不能凭它证明配方来自当前源码或历史 Stage。

输入读取明确的 Comptime 参数快照，常量使用描述中的规范值。整数运算
复用 IntegerValue.apply/compare/negate/bit_not，遵循已有目标宽度、符号性和
usize 标记；bool 运算只处理已验证的 bool。没有调用、分支、Cast、循环、
内存/环境读取或效果入口，不调用 StageAnalyzer，也不重新执行整个模块。
没有新缓存写入或默认解释路径接入。

最终 TypedValue 必须同时匹配描述的 expected_result 指纹和关联记录值。
例如配方 square(7) 正确而记录值被改为 50，即使其记录指纹也改成 50，
执行仍得出 49 并返回 ResultMismatch。返回错误不覆盖原缓存，也不借此改变
Stage 推断或原有诊断。成功表示本次重放在声明的标量模型中与记录结果一致，
不声称这是此前执行时逐操作保存的历史轨迹。

## 工作量、内存和输出

scalar_replay_execution_version=1 定义独立解释成本模型：每个尝试执行的节点
收取一个 Fuel，完成后再收取一个最终核验 Fuel。输入/常量也收费，失败的
算术节点已消耗一个 Fuel；未获 Fuel 的节点不执行，不预报其算术错误。

真实 square(x) 的记录通常是两个 Input 节点和一个 Multiply，共 3 节点、
4 个 Fuel。全量详情为 7、7、49；只选 Multiply 仍完整计算三个节点及核验。
这不是原 AnalysisStats.fuel_consumed 或 ResourceRecipe 的成本重放，二者
都不受本执行器影响。

| 限制 | 默认值 | 契约 |
| --- | --- | --- |
| fuel | 100,000 | 节点尝试与最终核验的总额度 |
| value_bytes | 8 MiB | 全部执行值数组 N × sizeof(ScalarConstant) |
| output_records | 4,096 | 请求保留的步骤记录数 |
| output_bytes | 1 MiB | sizeof(VerifiedScalarReplay) 加详情容器容量 payload |
| codec | 既有 cache::Limits | 描述准入的对象/元素/字符串等限制 |

详情选择是 ScalarReplayDetailRange：first 为从零开始的首节点，count 缺省
表示其后所有节点，显式 0 表示不保留详情。越界范围拒绝，不能用溢出计数
绕过配额。输出范围仅控制保留内容，不跳过执行依赖或最终结果验证。

value_bytes 和请求输出量先检查，超额时不执行节点；输入描述及规范编码
的准入随后进行。输出容器预留后还检查实际 capacity，执行中不扩容。完整
准入编码在配置执行值数组之前释放；值数组为平坦定长存储，不递归展开。
输出计的是内存 payload，尚不包含 JSON/源码标签的呈现字节。

count=0 时仍须容纳固定成功结果，可使用 output_records=0；它验证数学
结果而不保留步骤。默认选择全部节点，大函数超过详情额度时须显式选择
较小窗口或不保留详情，不能静默截断并称全量输出成功。

这些额度不包含调用方持有的记录、codec 准入临时集合、分配器开销和输出
呈现，也不是进程 RSS 上限。codec 仍完整编码/验证描述，其工作不按节点
Fuel 收费，受自身 bytes/elements 等限制；没有常量时间的零 Fuel 准入保证。
零 Fuel 通过准入后不配置执行值/详情容器，不计算节点。

## 取消、失败和交付

接口接受 std::stop_token。在入口、准入后、每个节点/最终核验之前及成功
返回前检查取消；不执行用户回调，不提供墙钟 deadline 保证，codec 操作与
单次内存配置不会被中途打断。已取消 token 在读取不合法描述之前就返回。

这是一次性接口，没有暂停状态或持久继续令牌。Fuel 不足、取消或额度失败
后再次调用从头开始，使用新的独立预算，不累计旧运行的工作或公开旧前缀。
C4c/C4d 的持久查询/结果确认协议没有被复用成隐式重放任务。

返回类型为 expected<VerifiedScalarReplay, ScalarReplayExecutionError>：

- 成功：绑定的规范描述指纹、已核验 TypedValue、选定的 ScalarReplayStep
  （节点索引与自带类型的标量值）及工作统计。节点的操作/源位置从同一
  指纹绑定的原描述读取；返回值不持有 AST、Analyzer 或缓存句柄。
- 错误：InvalidDescriptor、InvalidRecordedValue、InvalidSelection、
  FuelExhausted、ValueMemoryLimit、OutputLimit、Cancelled、ArithmeticFailure、
  ResultMismatch 或 OutOfMemory。适用时附描述错误、IntegerError 和节点号。
  不含部分计算值或详情数组。

统计报告成功计算的节点数、已消费 Fuel、已暂存详情数，以及已配置工作
容器的 payload 计量。算术错误的尝试计入 Fuel 而不计入成功节点；错误时
暂存详情计数只是进度信息，不是已交付的值。预检/最终核验失败可无节点号。
所有临时工作容器在返回时释放或移交成功结果。

本轮保留既有整数语义：普通加减乘、取反和负号按宽度处理；除零、非法
移位（包括负有符号移位量）、有符号最小值除以或取余 -1 返回对应错误。
解释失败不是新的源语言错误，也不会触发整个 Stage 作为隐式 fallback。

## 验收与后续

11 项单元测试覆盖全部支持运算与现有 VM 在各宽度/符号性下的对照、bool
和 32/64 位 usize、边界位模式、算术失败及其收费、结果与指纹共同被改写
后的数学不符、非法描述/记录/范围、逐 Fuel 边界、工作值/输出容量边界、
预取消和从头重试、20,001 节点平坦执行，以及真实记录在编译器销毁后的执行。
真实 Stage 样本还确认既有 HIR/统计不变，缓存内容与 mtime 不因执行改变。

独立进程 harness 扩展 execute 分支：先读取 C4e1b 证据再执行，不构造
StageAnalyzer。32/64 位真实记录核对结果、节点、详情和精确 Fuel；Fuel
不足时 stdout 为空，重复运行结果相同，缓存文件不变。现有发布/读取与
损坏/缺失/版本绑定检查继续保留，生产 pagosc 尚无详情执行命令。

验证：Debug 全量 CTest 455/455（含 lit），ASan/UBSan 非 lit 454/454，
GCC 串行非 lit 454/454；最终测试引用绑定修正后的 Debug 专项 31/31。
C++ 格式、Python 编译、50 份 Markdown 及 `git diff --check` 通过。

GCC 的两次 `--parallel 4` 回归分别在既有
RecursiveCheckpointTest.ReceiptPublicationFailuresAndMalformedWatermarks
（对象写入）和 StageSpillTest 的 compressed 图恢复用例出现一次失败。
前者随后独立连续 20 次通过，后者独立重跑通过；完整串行回归通过。
并行不稳定的根因尚未定位，保留为后续回归跟踪项。

```sh
ctest --test-dir build/debug \
  -R 'ScalarReplayExecution|pagos-scalar-replay-process' --output-on-failure
```

下一轮 C4e2b 接入显式详情 CLI、特化选择、源位置和已核验步骤呈现，并对
呈现字节另设额度；独立读取/重放不得运行整个 Stage，不把旧 Literal 或
不支持的函数体伪装成可重放，也不默认对所有缓存值执行解释。
