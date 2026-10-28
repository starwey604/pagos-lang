# 显式标量重放详情 CLI：C4e2b

2026-10-28。本轮将 [C4e1b](provenance-scalar-replay-c4e1b.md) 的只读源绑定
记录和 [C4e2a](provenance-scalar-execution-c4e2a.md) 的有界执行接入
`pagosc explain-replay`。命令在 StageAnalyzer 创建前分流，独立选择已记录
的函数特化，核验数学结果后才呈现选定步骤及当前源码位置。
普通编译/解释接口保持既有行为，C3 图仍为 P0。

## 先记录实际计算，再独立解释

源文件中必须实际计算过所选特化，例如：

```pagos
fn square(x: u32) -> u32 { x * x }
let answer = square(7);
```

显式发布本编译器、目标和源码代次下的标量证据：

```sh
pagosc --target=riscv32-unknown-elf --stage-cache=read-write \
  --cache-dir=.pagos/cache --cache-evidence=scalar-replay check input.pgs
pagosc --target=riscv32-unknown-elf --max-fuel=0 \
  --replay-read=.pagos/cache --replay-function=square --replay-arg=7 \
  explain-replay input.pgs
```

首条命令才运行正常 Stage。第二条解析/检查当前源码、验证记录并执行纯
配方，不运行 Stage、整个模块或外部效果，不写缓存或生成查询任务。
max-fuel=0 是可验证这一点的普通 Stage 限制，不控制重放预算。

replay-function 选择当前唯一函数定义；每个 replay-arg 按声明顺序对应一个
完整原始参数。整数为符合该参数宽度/符号性的十进制值，可含下划线；bool
只接受 true/false。不解析表达式、类型后缀、调用或转换，零参数函数不传
replay-arg。所有输入都为显式 Comptime 快照，usize 用选定目标的宽度解释。
函数名、类型化输入、目标、编译器与定义共同构成完整特化键，输出其指纹；
不同实参不会误选同一名字下的另一特化，也不扫描目录猜测“最新”项。

定义存在但没有保存所请求的计算时返回 missing-descriptor。旧 Literal、
C2 只有值的旧项、不支持的 Cast/分支/短路/调用/数组函数体，不会被逆推成
可重放历史。需要新记录时由调用方明确重新执行发布命令；读取没有隐式
Stage fallback。此接口只解释函数体及已捕获输入，不解释输入自身的历史。

## 预算与步骤选择

| 参数 | 默认 | 含义 |
| --- | --- | --- |
| replay-fuel | 100000 | 节点尝试加最终核验的独立 Fuel |
| replay-value-bytes | 8 MiB | 执行值数组 payload |
| replay-output-records | 4096 | 允许保留的详情记录数 |
| replay-detail-bytes | 1 MiB | C4e2a 成功结果对象和详情容器 payload |
| replay-render-bytes | 1 MiB | 最终 stdout JSON 的 UTF-8 字节，包含换行 |
| replay-first | 0 | 从零开始的首个详情节点 |
| replay-count | all | 保留此后所有节点，或显式 N（0 表示仅核验） |
| replay-stats | 关闭 | stderr 准备/节点/Fuel/呈现字节统计 |

示例只保留 square 的 Multiply 结果：

```sh
pagosc --target=riscv32-unknown-elf --replay-read=.pagos/cache \
  --replay-function=square --replay-arg=7 --replay-first=2 --replay-count=1 \
  --replay-output-records=1 --replay-fuel=4 --replay-render-bytes=8192 \
  explain-replay input.pgs
```

选择范围只影响保留/呈现，仍完整计算依赖并核对最终值。3 节点的真实 square
记录需 4 个 Fuel；count=0 仍执行和核验，只省去详情。范围/额度不满足时
明确返回不可用，不静默截断后宣称全量成功。数值额度允许 0，没有无限 Fuel
选项；Stage 的 max-fuel 等参数与这些限制独立。

所有 JSON 在有界私有缓冲中完成，只有完整响应才写 stdout。呈现超额时
整个响应被丢弃，stdout 为空，stderr 报 render byte limit；小到装不下错误
响应的额度也不例外。不会为了返回错误而突破字节上限。输出 IO 失败可能
使传输被截断，但缓存不变，可重新调用；没有持久继续或交付确认协议。

source/codec 准入仍受原有完整对象限制，其临时内存不计入执行值 payload。
呈现额度不限制 stderr 诊断或进程 RSS；源码解析/类型检查/身份及记录验证
也不按节点 Fuel 收费。普通查询任务、snapshot、函数 evidence、启用的 Stage
缓存/缓存目录及 spill 配置与 explain-replay 互斥；replay 选项不用于其他命令。

## 输出与精度边界

stdout 是一条 schema=1、type=scalar-replay 的 JSON 对象及换行。
成功 status=verified，包含：

- stage_runs=0，evidence_version、execution_version、recipe_semantics；
- specialization 的函数名、完整原 kind 1 特化键指纹和类型化实参；
- descriptor 的规范配方指纹，source 的文件路径及当前原始文本修订指纹；
- 已核验 result，当前结果 site 的字节 begin/end 及一基 line/column；
- selection 的 first/count/total/complete，以及独立 work 统计；
- details 的节点号、操作名、类型化计算值、当前 site、操作数节点号和
  input_slot（非参数节点为 null）。

所有标量 value 均是字符串，附明确 type，例如 i8/-7、bool/false 或
u64/18446744073709551615，避免 JSON 消费方的数值精度损失。
selection.complete 只表示是否选中全部配方节点；局部窗口也完成了数学核验。
步骤是本次重放算得的值，不冒充此前逐操作保存的完整历史。

源位置来自当前 SourceAnchor 映射。纯空白变化可复用相同特化/描述指纹，
但 source.revision 及绝对偏移/行列会更新；定义、模块依赖、目标或编译器
变化则不能误读旧代次。位置遵循 SourceManager 的字节偏移/列契约。

不可用响应 status=unavailable，包含 reason、work、可选 node/integer_error，
不包含 result 或 details。具体原因包括 missing-descriptor、unsupported-source、
missing-object、corrupt-object、版本/证据绑定不符、各项额度、算术失败或
result-mismatch。后一种情况即使缓存值和指纹一起被改写，也必须由实际
计算发现，不用新值覆盖原缓存。continuation_available=false；不可用响应
restart_required=true，表示下一次从头开始，不保证相同输入将变得可用。

退出码：0 已核验；3 不可用或额度不足；130 取消；2 参数/实参错误；
1 源码/目标/类型检查或其他操作/输出失败。前置普通诊断和参数错误只写
stderr，过小的呈现额度及取消边界也允许 stdout 为空。

SIGINT 处理器只设置保证无锁的原子标记，由独立 watcher 转发 stop_token。
在执行边界、呈现步骤之间及最终输出前检查取消；不在信号处理器中访问
编译器、存储或执行 stop 回调。正在进行的源码/记录准入与 IO 不保证立即
中止；这不是墙钟 deadline。最终输出检查点前观察到取消时不交付 verified
值；输出已经开始后的传输不能撤回，缓存始终不变。

## 验收与后续

独立 CLI 进程测试覆盖多个实际特化、顺序参数和零参数、bool/负值/64 位值、
32/64 位 usize、源位置/空白重绑定、UTF-8 路径的精确呈现字节边界、全部
额度与范围错误、缺/损坏证据、旧值项与不支持函数体、结果和指纹一起篡改、
输出设备失败、SIGINT 取消。成功/失败均检查缓存内容及 mtime 不变，
max-fuel=0 读取成功，以及当前源码含 Stage 错误时也不隐式运行 Stage。
测试用的篡改 fixture 不链接进 pagosc。lit 另覆盖参数隔离和真实成功输出。

验证：Debug 全量 CTest 456/456（含 lit），GCC 串行非 lit 与 ASan/UBSan
并行非 lit 各 455/455；C++ 格式、Python 编译、51 份 Markdown 和
`git diff --check` 通过。C4e2a 记录的 GCC 并行旧用例不稳定仍保留跟踪，
本轮没有将其标为已解决。

```sh
ctest --test-dir build/debug -R 'pagos-scalar-replay-cli-process' \
  --output-on-failure
```

C4e 的标量记录、读取、执行、核验和显式详情闭环已完成。生成器独立元素
重放仍未实现，下一轮 C4e3a 先建立封闭捕获/元素独立性的源码准入依据。
完整 P1 分支理由、C3 图集成与持久详情任务仍属后续扩展。
