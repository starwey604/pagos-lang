# Cache Scalar C2a 实现记录

状态：已实现，日期为 2026-10-12。对应
[迭代计划](cache-implementation-plan.md)的 C2a，依赖
[稳定身份](cache-identity-c1a.md)与[文件存储](cache-store-c1b.md)。
首次接入真实 Stage 求值复用，默认关闭；没有性能收益或峰值内存下降的声明。
本页记录 C2a 交付时的切片；当前数组扩展和测量见
[C2b 实现记录](cache-array-c2b.md)。

## 准入范围

[ScalarCache](../lib/stage/scalar_cache.cpp)接到
[StageAnalyzer](../lib/stage/stage_analyzer.cpp)的会话内 miss 路径。
每次仍运行前端、按当前目标检查类型、从左到右分析全部实参；现有会话内
缓存优先。磁盘查询只接受全部实参具有 Comptime 标量常量的 internal 函数。

参数和结果支持 bool、u/i8、16、32、64 及目标已解析的 usize。函数体必须
没有语句，尾表达式只能包含字面量、参数名、纯标量一元运算、整数转换、
非短路二元运算。所有表达式类型都必须是标量，名称必须确认为参数。

普通调用、递归、全局名称、指针/内存操作、数组/记录、局部绑定及契约、
循环、早返回、if 与短路逻辑继续走正常分析。它们的内层纯叶函数可以独立
命中，但不把外层调用当成可恢复查询，也不导入子查询 memoization。
失败的函数体不发布。实参可携带已知值之外的残留工作；工作由当前调用方
保留，不放入函数体缓存。

## 格式与验证

QueryKey 使用 C1a 的定义、类型、值、Stage、目标、compiler build 与语义
选项身份；FileStore 使用已验证的保守整模块语义指纹。修改其他定义或顶层
语句也会失效。空白、注释及数字分隔符变化保持身份，SourceAnchor 在当前
AST 中重新映射，缓存不保存首次调用的绝对区间或 HIR 指针。

一个 manifest 原子关联两个必需对象：

| 对象名 | ObjectKind | 内容 |
| --- | --- | --- |
| `value` | Value = 1 | C1a 规范类型与标量常量 |
| `resources` | ResourceRecipe = 4 | 预算协议、进入深度、特化数、结果位置与 Fuel 检查点 |

ResourceRecipe 使用共同 `PGC1`/format version 1 头及 budget protocol
version 1，依次记录相对进入深度、特化计数、长度限定的 SourceAnchor、
可选结果参数槽、Fuel 数量与有序结构位置。非参数直接返回的结果使用当前
函数尾表达式区间；直接返回参数的结果使用当前实参区间，与原分析一致。
参数槽是定义内索引，不携带调用方身份。

本切片的相对深度和特化数均为 1。正常路径从进入检查点及实际统计差值
取得它们，从 `consume_fuel` 收集实际有序区间；发布前必须与准入树的
完整检查点序列匹配。调用表达式、实参和外层函数消耗不进入本记录。
条件执行暂不准入，因此无需猜测动态路径或省略需要的 StageDecision。

读取先完成 FileStore 校验，再要求必需对象齐全、规范资源记录完全一致、
类型等于当前已检查的返回类型、规范结果指纹匹配。标量解码额外限定为
128 字节和一个元素，避免错误聚合对象在返回类型校验前分配大数组。
资源记录精确比较也拒绝未知版本、尾随/截断数据、错误源码锚点、结果槽及
修改后的成本。结果指纹覆盖规范 value/resources 的长度限定编码。
未知必需 artifact 名称也拒绝恢复，不能静默忽略未来版本的必需证据。

新增 ObjectKind 标签，format/query/budget/store 版本仍为 1。旧读取器会
拒绝未知必需种类；compiler build 身份也防止不同实现错误互用。

## 预算与解释恢复

调用的深度/特化限制仍由原检查点检查。命中在 AnalysisStats 副本中加入
函数体 Fuel、一个特化及本次最大进入深度；恢复常量、通过原
`instantiate_inline`/Recorder 路径构造 HIR、保留实参工作后，再提交统计及
当前会话 memoization。磁盘命中不增加逻辑 `cache_hits`；相同会话内第二次
调用仍使用原缓存命中规则，不重复计入函数体 Fuel 或特化。

Fuel 不足时丢弃副本，从原 miss 路径运行，产生原错误代码、顺序、当前区间
和依赖路径。损坏、缺必需对象、格式拒绝、锁/IO 错误及缓存分配失败也按
正常分析回退。读写模式会尝试原 FileStore 的修复/冲突协议，不在线清理
损坏 CAS；这些故障只进入独立遥测，不产生语言诊断。

本范围的折叠结果在现有 P0 中按 Literal 解释，没有函数体局部契约或迁移
决策。新 HIR 经当前 Recorder 获得绑定、实参工作、来源及契约证据。
这里没有持久化 Provenance 图，也没有新增更丰富的纯求值理由。

## 实验控制与遥测

CLI 使用以下选项，缓存目录必须明确指定：

```sh
build/debug/pagosc --stage-cache=read-write --cache-dir=.pagos-cache \
    --cache-stats explain-stage tests/lit/cache/pure-scalar.pgs
build/debug/pagosc --stage-cache=read-only --cache-dir=.pagos-cache \
    --cache-stats explain-stage tests/lit/cache/pure-scalar.pgs
```

`--stage-cache=off` 是默认值；off 不构建缓存身份或进行缓存 IO。
`read-only` 不创建目录、发布对象或改写已有缓存文件。`read-write` 在成功
求值后发布。仓库根目录 `.pagos-cache/` 已忽略；选择其他目录时自行安排
版本管理规则。公共库通过 [CacheConfig](../include/pagos/stage/cache_config.h)
及可选 QueryContext 注入，不要求 CLI 或 LLVM 后端。

`--cache-stats` 在 stderr 输出单独的 `stage-cache:` 行：lookups、hits、
misses、writes、errors、rejected、budget-fallbacks、body-evaluations 和
bodies-skipped。hits 只计成功提交；misses 计存储未命中/负载拒绝；预算回退
单列。writes 只计新发布，已有相同项不重复计数。body-evaluations 计实际
进入函数体分析（包括不准入函数），bodies-skipped 计恢复后跳过的函数体。
这些是物理工作统计，与 explain-stage 的逻辑 AnalysisStats 分开。
未请求遥测时正常 stdout、诊断和 explain 输出保持不变。

## 验证范围

[单元测试](../tests/unit/stage_cache_test.cpp)比较关闭、冷、热模式的完整
AnalysisStats、HIR、Provenance 与诊断，覆盖全部标量宽度、参数来源重映射、
逐个 Fuel 边界、嵌套深度/特化边界、会话内优先及 Analyzer 重用；也覆盖
语义/依赖/Stage/参数/目标/协议变化、失败求值、不准入体、IO 故障、损坏或
缺对象、错误类型/超大负载及无效/缺失资源记录。

[lit 用例](../tests/lit/cache/)运行独立的关闭、冷读写和热只读编译进程，
比较 check、Stage/Provenance、HIR、MIR、LLVM IR；检查只读文件内容及 mtime
不变，逐预算比较错误，验证实际 body-evaluations 减少。热 LLVM IR 经
Clang 编译执行，验证结果和外部输入的顺序/次数。空白改动命中并重新映射
解释位置，模块语义修改安全失效。

2026-10-12 验证结果：Debug 全套 CTest 233/233（含 382 个 lit 用例）；
GCC 和 ASan/UBSan 缓存专项分别 53/53，并分别通过 3 个新增缓存 lit 用例。
缓存专项包括原协议/存储测试、14 个 StageCache 测试及两个独立进程测试。
clang-format、25 份文档检查及 `git diff --check` 均通过。

数组生成已由 C2b 扩展，残留模板和图快照继续由 C3b、C3a 实施；FileStore、
完整身份构建和逐表达式 Anchor 查找仍有成本。本轮只证明正确复用真实
计算，未测量或宣称标量场景整体加速。
