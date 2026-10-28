# 显式生成器元素 CLI C4e3c

日期：2026-10-28。承接 [真实生成器记录与重放](provenance-generator-replay-c4e3b.md)，
完成 [实施计划](cache-implementation-plan.md) 的 C4e3c：显式元素选择、
跨进程读取、有界 JSON、取消，以及真实主机效果/残留程序对照。

## 使用方式

```sh
pagosc --target=riscv32-unknown-elf --stage-cache=read-write \
  --cache-dir=/tmp/pagos-cache --cache-evidence=generator-replay \
  emit-hir input.pgs
pagosc --target=riscv32-unknown-elf --max-fuel=0 \
  --replay-read=/tmp/pagos-cache --replay-function=table --replay-arg=7 \
  --replay-element=1 --replay-first=0 --replay-count=all \
  explain-replay input.pgs
```

示例函数为 `fn table(x: u32) -> [u32; 4] { [for i in 2..6 { i * x + 1 }] }`。
必须先由正常 Stage 调用 table(7)，成功发布数组计算证据。跨进程解释该
特化的数组下标 1 时，实际迭代值为 3，已核验结果为 u32/22。

新选项 `--replay-element=N` 为显式的零基数组下标，解析范围为 u32；超出
数组长度返回 invalid-selection。数组函数必须提供该选项，标量函数不接受
该选项。没有隐式“第一个元素”，也不把普通数组字面量当成可重放生成器。

--replay-arg 按声明顺序重复，包含全部 Comptime 捕获，连同未使用参数。
整数为对应目标类型的十进制文本，bool 只能是 true/false，不接收表达式。
配方准入仍沿用 C4e3a 的封闭标量白名单，不扩大到转换、分支、索引、调用、
全局/volatile 读取、状态或嵌套生成器。

--cache-evidence=generator-replay 是正常 Stage 缓存的能力要求，需同时启用
缓存和指定目录。旧 C2 数组值可继续在 value 模式命中；要求新能力而缺项时，
ReadOnly 正常求值，ReadWrite 可重新求值并升级独立能力项。explain-replay
与 Stage 缓存、快照、函数证据、spill 和查询任务写入配置互斥，不会升级缓存。
读取仍在 checked AST/目标确定后、构造 StageAnalyzer 之前分派。

## 结果与额度

沿用标量 CLI 的执行 Fuel、工作值、详情记录数、详情载荷和呈现字节选项，
以及 --replay-first/count。详情窗口只选择该元素配方的节点，不选择数组
元素区间；即便详情为空，也必须核验两个边界并执行所选元素的完整配方。

stdout 为 schema=1、type=generator-replay 的一条 JSON 和换行。成功包含：

- status=verified、stage_runs=0、verification_scope=selected-element，
  无 continuation，独立执行/证据/描述/生成器/标量语义版本。
- specialization 的完整类型化捕获及原纯特化键指纹，descriptor 和记录数组
  的 recorded_result 摘要。
- 当前 source 路径及原始文本修订摘要；generator、iterator 和 element 的
  当前源码位置，沿用 SourceManager 的字节偏移、一基行/列。
- element_index、实际 iteration_value、iterator_slot 和已核验 bounds。
- 单个已核验 result；值为带 type 的字符串，保留 bool、负值及 u64 精度。
- selection 的节点窗口，以及区分 bound_nodes/element_nodes 的 work。
- details 的操作、值、位置、依赖节点和 input_slot/input_kind；输入明确
  标注 capture 或 iterator，其他节点为 null。

verification_scope 仅承诺所选元素与记录数组对应值一致。selection.complete
只表示选中了该元素配方的全部节点，不表示所有数组元素已重放。原数组
仍需整体读取/解码/哈希，受 codec 限额；执行工作值和输出统计不含调用方
输入所有权或 codec 临时载荷，不宣称端到端恒定开销或硬墙钟期限。

缺证据、源码不支持、损坏/过期记录、错误范围、额度不足、算术失败或结果
不符返回 unavailable、reason、phase、work 和可选 node/integer_error。
phase 区分 admission/begin/end/element；失败不输出 result、details、bounds
或未核验迭代值。未知函数及源身份失败同样使用生成器响应类型。

UTF-8 呈现额度包含完整 JSON 及换行。超额不发送已构建的部分响应，stdout
允许为空，诊断在 stderr。退出码沿用标量：0 成功，3 不可用/限额，130
取消，2 参数错误，1 源码/目标/类型或其他操作/输出错误。SIGINT 复用无锁
标记和 stop_token 桥接，在执行、步骤呈现及输出前检查；已开始的输出无法
撤回，源码/记录准入和 IO 不保证立即中断。缓存始终只读。

标量请求的 schema/type/字段及选项保持兼容。磁盘沿用 C4e3a/C4e3b 的
kind 16/17 和 kind 7/v1 能力索引，不修改格式。保守 compiler build identity
包含驱动源码；升级编译器后旧记录可能不可用，需显式正常编译重新发布。

## 验收

跨进程 CLI 用例覆盖多特化、完整捕获、同名迭代变量、零参数、bool/负值/
u64/usize、32/64 位目标、节点窗口、所有 Fuel 边界、内存/输出/UTF-8 精确
字节边界、缺失/损坏记录、重新封装的假值/范围/源码/未来版本、旧 C2 值项
及显式升级、当前源码含 Stage 错误、输出设备失败和 SIGINT。成功与失败
均核对缓存文件哈希和 mtime，--max-fuel=0 仍可读取。源位置测试覆盖 UTF-8
路径和空白编辑后的字节偏移。4,096 元素的真实 CLI 查询对比首尾执行统计。

effect harness 将相同源码以关闭缓存、首次构建、只读复用三种方式生成主机
程序，链接每次均读取受控输入并写日志的真实 C observe 函数，调用参数
顺序必须均为 9、10、11、12、38，结果为 38；残留 LLVM 文本逐字相同。
observe 将调用记录到文件；解释成功、Fuel/输出失败以及尝试解释 effectful
生成器期间，输入与
日志文件内容/mtime 和缓存均不变。该测试证明当前已支持路径的效果契约，
不意味着开放了外部函数、MMIO 或受控 IO 的 Comptime 重放。

验证结果：Debug 全套 485/485（含 lit）；GCC 串行非 lit 与 ASan/UBSan
并行非 lit 各 484/484。新增生成器 CLI/effect 及既有标量 CLI 的独立进程
聚焦测试 3/3；加强后的受控读写 effect 用例在三种构建下再次通过。
聚焦 lit 3/3，C++ 格式、Python 编译、54 个 Markdown 和 git diff --check
均通过。GCC 仍沿用串行验收，C4e2a 记录的旧并行偶发问题未在此轮解决。

有限标量/封闭生成器的显式重放闭环完成。完整 P1 分支理由、C3 图集成和
持久详情任务仍是后续扩展；当前 C3 图继续为 P0。
