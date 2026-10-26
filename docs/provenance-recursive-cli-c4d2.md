# 显式递归摘要解释与函数证据：C4d2

2026-10-26。C4d2 将 [C4d1](provenance-recursive-c4d1.md) 的有限域求解器
接入经验证的函数证据读取和独立 CLI。它查询一个实际特化的形式依赖、
来源/效果族及循环拓扑，普通解释与 C4c 原生持久任务沿用原有行为。
本轮不新增证据格式，也不改变 Stage 或求解规则。

## 列出和选择摘要

先显式发布函数证据并列出摘要目录：

```sh
pagosc --evidence-write=.pagos/evidence --evidence-mode=summaries \
  explain-provenance input.pgs > summaries.json
pagosc --evidence-read=.pagos/evidence --evidence-mode=summaries \
  --max-fuel=0 explain-provenance input.pgs
```

目录只读取已验证的 hot closure 和当前源码位置元数据，不打开冷拓扑。
它列出实际记录的函数特化，不包含模块作用域，也不为尚未分析的函数
推测摘要。FunctionEvidenceView.summary_catalog 提供对应的虚拟 ID 和
完整 QueryKey 指纹；revision 提供闭包、当前位置和原始源码指纹。

每项包含 name、key 以及类型、Comptime/Runtime、约束和已知值参数。
值使用文本展示，Runtime 值为 null；constraint 为 RuntimeConstraint
的数值，0 为无约束，1 为显式 Runtime 边界。key 是包含目标、编译器、
定义和 typed stage arguments 的完整特化键指纹，不是函数名或裸节点号。

按唯一名称求解：

```sh
pagosc --evidence-read=.pagos/evidence --evidence-mode=recursive-summary \
  --evidence-summary=rotate --max-fuel=0 \
  explain-provenance input.pgs > recursive.json
```

同一函数有多个已记录特化时，名称选择明确报歧义。可以先过滤目录，
然后用其中一项的完整 key：

```sh
pagosc --evidence-read=.pagos/evidence --evidence-mode=summaries \
  --evidence-summary=scale explain-provenance input.pgs
pagosc --evidence-read=.pagos/evidence --evidence-mode=recursive-summary \
  --evidence-summary-key="$SUMMARY_KEY" --evidence-channel=value \
  explain-provenance input.pgs
```

name/key 二选一，key 为 64 个十六进制字符。不存在的选择不回退到其他
特化。摘要模式不接受 evidence-root 或 query-task 选项，也不接受显式
legacy/compressed/topology 查询模式；只有 explain-provenance 可使用。
它不是绑定调用方实参后的具体来源查询，两个 caller 的输入不进入该
Runtime 特化键；需要实例上下文时继续使用原生 sources/witness 查询。

## 结构化输出、精度及循环依据

stdout 是一条完整 JSON 对象加换行，schema 1。type 为 summary-catalog
或 recursive-summary，独立于 C4c 的 page/status 与确认协议。

| 公共字段 | 含义 |
| --- | --- |
| evidence.closure | 经验证的函数证据根对象指纹 |
| evidence.positions | 当前源码锚点到位置的映射指纹 |
| evidence.source_revision | 完整原始源码指纹，包括空白 |
| local_ids | true；输出中的节点 ID 仅在该 evidence 绑定内有效 |
| stage_runs | 显式发布为 1；独立读取为 0 |
| continuation_available | false；本轮不保存求解状态 |
| restart_required | 步骤预算未完成时为 true |

目录记录包含 summaries；求解记录包含选中的 summary 和 results。
默认分别计算 Value 和 Effects，可用 evidence-channel=value/effects/both
选择。每个通道的记录包括：

- progress、fixed_point、precision、issues 和命名的 gaps。
- instances_merged、concrete_sources_complete，以及形式参数/来源族 atoms。
- work：steps、slices、nodes、links、components、cyclic_components、
  evaluations、facts_added 和 queue_peak。
- 完成后的 proof_available、nodes、links 和 components。

Parameter 原子用 owner 的完整特化键及 slot 表示。SourceFamily 原子引用
节点，来源位置和规则见 nodes。节点描述还包括参数 owner/slot、调用
callee 键，以及已有决策的 stage/rule/reason 和 recursive_completion。
slot 按残留函数的 Runtime 参数从零编号；目录 arguments 则按原声明顺序
包含 Comptime 和 Runtime 参数。对应源码参数时须先筛出 Runtime 项，
不能直接用 slot 索引完整 arguments。例如 scale(4, x) 中 slot 0 对应 x。
这些字段是原记录，不是解释器重新推断的 Stage。不同调用点有不同节点；
调用共享的子图仍保留各自链接，族集合合并不能冒充具体实例列表。

links 包含 dependency、summary 或 argument，保留原 Relation；argument
的 slot 是实参位置。组件包含节点及摘要键，cyclic 表示有限方程环。
原节点规则与链接可以检查参数替换和循环结构；它们不是动态执行的最短
来源路径，也不证明递归会终止。完整拓扑可能包含未使用的 Value 实参，
不据此声称结果依赖每条结构边。

精度沿用 C4d1 的保证：精确只针对声明记录模型中的形式依赖投影；内部
来源族跨调用发生实例合并时为 Conservative；缺关系或未支持情况为
Unresolved。issues 是 RecursiveIssue 位掩码，gaps 给出对应名称，不能
将其与原生 CoverageIssue 混用。concrete_sources_complete 始终为 false。
即使 fixed_point=true，仍须查看 precision；空集合不提升为 Comptime。

## 步骤、失败与读取边界

provenance-slice 控制每次 advance 的原语步骤数，默认持续至不动点。
可显式设置每个通道的总上限：

```sh
pagosc --evidence-read=.pagos/evidence --evidence-mode=recursive-summary \
  --evidence-summary=rotate --recursive-max-steps=256 --provenance-slice=32 \
  explain-provenance input.pgs
```

recursive-max-steps 默认 all，也接受 0。预算耗尽时输出 Paused/Unresolved
和已有事实前缀，不提供尚未完成的 proof 数组；它不表示已有覆盖完整。
两个通道各有独立上限。提高预算后须从头发起，本轮没有可恢复任务 ID、
已确认页或磁盘状态。不得将该退出状态当作 C4c 的持久继续令牌。

| 退出码 | 含义 |
| --- | --- |
| 0 | 目录读取或不动点计算完成；不保证 precision 精确 |
| 3 | 本次预算耗尽，restart_required=true |
| 1 | 选择、证据、绑定、位置、输出或运行失败 |
| 2 | 参数组合或数值错误 |

独立 evidence-read 仍解析/类型检查当前源码并验证目标、编译器和完整
hot closure，随后直接查询，不构造 StageAnalyzer。零步骤查询也执行这些
校验。空白修订可复用语义证据，但绑定新的 positions/source_revision；
语义或目标不匹配则拒绝。key 与节点号不能跨绑定任意复用。

无论读目录还是求解，读取都不写证据或任务目录。冷块缺失/损坏明确失败，
没有 Stage fallback；未选择且不可达的冷函数不必读取。多通道的求解和
位置渲染在写 stdout 前准备完成，后一个通道失败时不会留下前一个通道
的成功 JSON；输出流自身中断仍可能产生截断文本。

求解及 proof 仍驻留内存，最终输出也需缓冲。证明位置渲染、已知值展示、
元数据校验和 JSON 编码不计入 steps，不是总 RSS、输出字节或墙钟预算。
provenance-query-stats 在 stderr 报告 metadata/cold/cache/live 读取量，
不会把解释工作加入 Stage AnalysisStats。

## 验证与后续

真实独立进程覆盖摘要目录、特化键歧义、双通道及单通道、不同 slice、
零/有限步骤、源修订、目标/语义变化、不可达大函数缺块、可达缺块、hot
closure 缺失和失败 Stage 的 stdout。所有读取用 max-fuel=0，检查证据
文件内容与修改时间不变；目录与零步骤的冷读取计数为零。

实际 Stage 图的单元测试按特化参数匹配原内存作用域及磁盘虚拟作用域，
对比形式槽、带位置的来源族、精度、缺口、合并标志及拓扑计数。覆盖
参数轮换、互递归、递归内部输入、递归完成边界、选择性投影与 Effects。
虚拟节点/摘要 ID 重映射不参与跨后端身份比较。

验证：Debug 全量 CTest 409/409（含 lit），GCC 与 ASan/UBSan 各
408/408 非 lit CTest；C++ 格式、Python 编译、44 份 Markdown 文档与
`git diff --check` 通过。

C4d2 的显式解释验收范围完成。下一轮 C4d3 为目录、SCC、worklist 及
活动替换游标引入版本化持久续接，复用 C4c 的绑定、发布、配额和确认
基础；当前输出不因此宣称持久化或具体递归调用实例求全。
