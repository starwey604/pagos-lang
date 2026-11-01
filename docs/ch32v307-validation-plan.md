# 基线稳定与 CH32V307V-EVT-R1 实机验证计划

日期：2026-11-01。探索基线：`llvm` 分支 `277691c`，探索开始时工作区干净。
本文是待执行计划，不代表 CI 已修复、协议已冻结或实机已通过。

执行更新（2026-11-01）：S0 修复提交 `ae92865` 位于
`codex/stabilize-crc-ci` 分支，尚未合并到 `llvm`。
[GitHub Compiler 全矩阵](https://github.com/starwey604/pagos-lang/actions/runs/41270362461)
的 Debug、GCC、ASan、Release 作业均通过，
[Documentation](https://github.com/starwey604/pagos-lang/actions/runs/41270362462)
也通过。本地固定 LLVM 22.1.8 的四配置干净构建各通过 487 项 CTest；
Debug 的 58 个生产代码单元静态检查及 54 项 RV32/QEMU 检查通过。
修复已整理为[草稿 PR #1](https://github.com/starwey604/pagos-lang/pull/1)。
CRC 验证保留原有断言，编译
限时改为 60 秒，HIR 经临时文件读取；完整串行 lit 限时为 600 秒。
S1 已形成[协议基线清单](cache-protocol-baseline.md)、21 份固定字节样本
与 13 项新增测试，验证结果以配套提交的 CI 为准。S2–S3 尚未开始；
正式冻结应以合入后的全绿提交为准。

## 现状与目标

C1–C4 的有限实验闭环已完成，范围见
[缓存实现计划](cache-implementation-plan.md)。当前已有对象发射、最小 C ABI、
指针、volatile、全局整数/数组/记录和段属性，可复用
[QEMU 原生 MMIO 示例](../platforms/qemu-rv32-virt/mmio.pgs)。
CH32 平台目录、芯片启动/链接配置和板级实机证据仍缺失。

目标是先形成可信的基线，再用一个 UART 配置场景验证：已知板级信息在
Comptime 固化，时钟和引脚配置可核验，Runtime 仅保留必要设备访问。
第一轮采用轮询，不将中断、DMA、RTOS 或通用 HAL 作为前置条件。

## S0：修复并确认远端 CI

[探索时的 Compiler 运行](https://github.com/starwey604/pagos-lang/actions/runs/40866667236)
对应探索基线 `277691c`。Debug、GCC、Release 成功；ASan 的 487 个 CTest 中仅
`pagos-lit` 失败，其中 397 个 lit 用例只有 `stage/crc32.pgs` 失败。
具体是 [run_crc32.py](../tests/lit/Inputs/run_crc32.py) 的 `emit-hir`
子进程超过 10 秒；不是顶层 45 分钟作业超时，也未见该失败的 sanitizer 报告。
Documentation 工作流成功。本地通过记录不能替代远端结果。

执行：

1. 固定现有 LLVM 22.1.8 容器，在 ASan 中重复测量 CRC 的 HIR/LLVM
   发射耗时与峰值，比较普通 Debug，并保存失败进程的输出。
2. 区分宿主波动、测试限时不足和真实性能退化。若为测试限时问题，建立
   有限、显式且适配 sanitizer 的编译超时；保留目标程序运行的独立短超时。
   不关闭 leak 检查、不删除 zlib 差分断言，也不靠无限重试掩盖失败。
3. 定向测试通过后完成干净的四配置回归、质量检查和 RV32/QEMU 验证。
   提交后由 GitHub 全矩阵确认；收集日志、工具版本及通过的准确 commit。
4. 更新开发指南的远端状态；冻结候选以实际全绿提交为准。

退出条件：远端四配置及文档全绿，CRC 定向重复测试无超时，原有
checkpoint/预算/损坏恢复验证仍通过。

## S1：冻结 C1–C4 的实验协议基线

冻结的是已支持范围和兼容规则，不承诺语言稳定、全量 P1 解释或完整增量编译。
以代码中的实际版本和 wire 格式建立单一清单，避免把所有协议统称为 v1。

| 层次 | 需要记录的契约 | 重点验收 |
| --- | --- | --- |
| C1 身份/存储 | QueryContext、目标、定义/参数编码、manifest/CAS、发布 | 跨进程同键；版本/目标隔离；坏对象安全拒绝 |
| C2 计算复用 | 标量/数组准入、ResourceRecipe、逻辑计费 | off/cold/hot 的结果、首错、预算、效果一致 |
| C3 快照/模板 | 语义与位置修订、必需证据、参数重绑定 | 当前源码位置；不串调用来源；MIR 验证 |
| C4a/b 证据 | 函数单元/Import、lease、spill、容量与回收 | 生命周期、损坏拒绝、辅助失败发布门禁 |
| C4c/d 任务 | checkpoint 格式、engine、head、ack、最终 receipt | 跨进程续接；页面重交付；修订隔离与只读边界 |
| C4e 纯重放 | descriptor/recipe/evidence/execution 版本、详情状态 | 只执行准入纯配方；核验结果；零额外设备效果 |

入口包括 [codec](../include/pagos/cache/codec.h)、
[key](../include/pagos/cache/key.h)、
[TaskStore](../include/pagos/cache/task_store.h) 与 `include/pagos/stage/`
中的 snapshot、query、recursive、scalar/generator replay 接口。
清单还须覆盖实现中独立的 checkpoint wire 版本与 CLI JSON，不能仅抄头文件常量。

每项注明：版本来源、必需/可选字段、准入范围、不支持情况、失败方式、
兼容政策和已有测试。语义缓存不可用通常回退正常分析；显式查询/重放遇到
绑定不符应拒绝或报告不可用，不能混同为自动重算后继续解释。

先盘点现有跨进程测试，再补缺失的跨版本与固定编码样本；资源协议、目标
或语义能力改变时必须显式失效。明确本轮是否支持旧格式读取；无迁移能力
时明确拒绝，不暗示旧任务可长期续接。基线内部保持确定性与可复现性。

退出条件：协议清单可逐项追到实现和测试，范围内的兼容/拒绝行为可复验，
无未版本化的行为改动。后续板级工作不顺带扩大 C1–C4 准入范围。

## S2：统一缓存成本账本

复用现有 benchmark 脚本，不先搭建新框架。将报告分为计算复用、证据
构建/读取、查询任务、显式纯重放四类，分别记录成本和收益。

现有数据只作历史参考：

- [C2b](cache-array-c2b.md)：8192 元素生成器 check 的 off/cold/hot
  中位数为 9.712/13.929/8.660 ms，范围重叠，不能推广为稳定加速。
- [C3b](cache-residual-c3b.md)：小标量模板热命中跳过函数体，但整体耗时
  8.802 ms 与 off 的 8.768 ms 无加速证据。
- [C4b3b](provenance-working-set-c4b3b.md)：最大图 spill 峰值约降
  7.25 MiB（3.9%），Stage 中位数约慢 24%；小输入没有稳定内存收益。

其中 C2/C3 部分报告只在 `build/benchmarks/`，C4 部分已在
`benchmarks/results/`。发布基线需把采用的原始数据放入可长期访问的位置，
避免文档依赖仅本机存在的文件。

统一记录 commit/dirty diff、二进制和配置摘要、工具链、CPU、文件系统、
目标、输入规模、预算与重复次数。使用同一 Release 二进制做受控比较；
旧报告的 LLVM 23 和 CI LLVM 22 数据不能直接拼接归因。

至少覆盖：off/cold/hot、普通无关编辑/影响定义编辑/位置编辑、冷写入、
热读取、证据记录、首次/重复查询、checkpoint 保存/恢复、标量/单元素重放。
列出 wall 分布、RSS、对象/文件数、字节与 IO、实际跳过的工作和逻辑计费。
区分应用冷缓存与 OS 页缓存；区分逻辑容量、进程峰值和物理设备 IO。

先用小样本确认采集正确，再顺序运行正式样本。Demo 本身也纳入矩阵；
小板级配置即使无缓存收益也接受，收益判断不影响语义正确性验收。
默认保持 cache off、evidence memory，只有实测支持才另提策略调整。

退出条件：有可复跑命令、原始报告、同口径成本表及适用范围结论。

## S3：UART/时钟/板级版本 Demo

场景：同一份 UART 应用按已知板级 profile 构建，输出板名/配置版本、
构建标识、所选时钟与波特率、自检结果，再进行有界轮询回显。
板级版本首先是显式构建输入，不假定芯片可自动识别 PCB 修订。

Comptime 输入：板/配置版本、晶振频率、时钟树、外设选择、引脚映射、
波特率与误差容限。Comptime 产物：合法性判断、寄存器配置值和初始化计划。
Runtime 工作：时钟切换及 ready 检查、寄存器读写、UART 收发与超时处理。
编译期已知的配置值不意味着设备已经就绪；设备状态始终从 volatile 读取。

候选起点为 USART1/115200 和 8 MHz HSE，来自
[Zephyr 板级定义](https://github.com/zephyrproject-rtos/zephyr/blob/main/boards/wch/ch32v307v_evt_r1/ch32v307v_evt_r1_common.dtsi)。
引脚、跳线和实际晶振仍需对照
[WCH 硬件资料](https://github.com/openwch/ch32v307/tree/main/SCHPCB)
与手头板卡核验。USART1 与其他 USART 的总线时钟来源不同，分频计算应对照
[WCH USART 实现](https://github.com/openwch/ch32v307/blob/main/EVT/EXAM/SRC/Peripheral/src/ch32v30x_usart.c)，
不能直接照搬 README 的概念性 `clock_hz / baud`。

分两步实现：

1. 先验证厂商 C/汇编启动与最小 C 串口参考程序，再链接 Pagos 导出的
   纯配置计算函数，核对 ABI、配置常量与生成对象。固定厂商 SDK commit
   及来源/许可，不依赖滚动 main。
2. 在相同启动和链接基座上，将配置计算、RCC/GPIO/USART 的轮询访问
   接入 Pagos，保留最小 C/汇编平台支持。以实际 volatile 访问宽度、
   保留位及寄存器语义为准，不直接复制 QEMU UART 寄存器模型。

拟新增 `platforms/ch32v307v-evt-r1/`，包含 profile、应用、启动/链接说明
及构建/串口采集入口。复用外部工具链链接；核验 ISA/ABI/ELF 属性、
Flash/RAM 划分、gp/sp、data/bss 和 WCH 专有启动要求。
首轮使用整数软浮点边界，避免依赖尚未验证的浮点或中断 ABI。
通用 RV32/QEMU 已通过不等于该芯片启动已通过。

主机侧验收：

- 两套合法 profile 产生预期不同常量，未选配置分支从 residual program
  消失；MMIO 状态读取、写入和收发次数在优化后保留。
- 拒绝非法版本、时钟树、外设/引脚组合、零波特率和不可接受的分频误差。
  先检查现有语言可否表达所需验证；若缺显式配置诊断原语，将其作为独立
  小切片评审，不在示例中假设已有 `assert` 或模块/profile 系统。
- Runtime 值冒充强制 Comptime 配置时，报出依赖路径。
- off/cold/hot 的配置、诊断、MIR/LLVM 与可观察效果一致；只读解释/重放
  不触发 Runtime、MMIO 或外部调用，也不要求当前不准入的 profile 必须命中。

实机侧验收：复位与断电重启均能得到确定的版本/自检输出；data/bss 正确；
UART 有界回显通过；用串口/逻辑分析仪核验波特率，条件允许时用时钟输出
核验时钟。就绪失败必须有限退出或报告，不永久卡在等待循环。
切换配置版本重新构建时，输出与测量符合所选 profile，旧缓存不串配置。
模拟第二个 profile 只能称为配置变体，不声称验证了另一块真实 PCB。

## 交付顺序与硬件前置条件

顺序：S0 → S1 → S2 → S3。S0 收敛后，硬件资料整理和 C 参考固件可
与协议/成本整理交错推进；正式 Pagos 实机结论绑定全绿的冻结基线。
每步保持独立提交，最终保留 ELF/map、构建命令、串口原始日志、测量结果
及源码/工具链身份。普通 CI 做主机/对象/配置测试，实机测试显式运行并
记录设备，不让公共 runner 假装完成板卡验证。

实机实施前需要确认手头板卡丝印/PCB 修订、MCU 型号、晶振、调试器型号
及固件、可用串口/跳线、烧录工具和连接方式。这些不阻塞 S0–S2。
初次探索仅完成现状核验与计划编写；后续执行状态见本文开头。
实机尚未烧录。
