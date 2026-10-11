# Cache Identity C1a 实现记录

状态：已实现，日期为 2026-10-12。对应
[迭代计划](cache-implementation-plan.md)的 C1a，设计依据为
[Cache Architecture RFC](cache-architecture-rfc.md)。本轮交付稳定身份和规范
编码，Analyzer 的求值、诊断、残留输出及会话内 memoization 路径保持原状。

## 接口与依赖

- [codec.h](../include/pagos/cache/codec.h)：对象种类、协议版本、SHA-256
  摘要、限制与错误，以及 `TypedValue` 的有界编码/解码。
- [key.h](../include/pagos/cache/key.h)：模块/定义身份、目标描述、
  `QueryContext`、`StageArgument` 和 `QueryKey`。
- [cache_identity.h](../include/pagos/stage/cache_identity.h)：从 checked AST
  生成定义/整模块语义指纹、当前位置映射及函数查询键；独立编解码 Anchor。
- [StageAnalyzer](../include/pagos/stage/stage_analyzer.h)：持有可选且只读的
  `QueryContext`；现有调用方可以继续省略此参数。

`Pagos::cache` 的公共 API 只使用标准 C++ 与 Pagos 类型，LLVM SHA-256
工具留在私有实现中。Stage 适配器读取 sema side tables，不依赖 LLVM IR
或 codegen。C1a 本轮没有 CacheIndex、CAS 或 IO；后续
[C1b](cache-store-c1b.md)已扩展此库提供文件存储。

## 规范 wire 格式

format、query、budget 协议版本当前均为 **1**，不是永久 ABI。每个独立对象
以 `PGC1`、little-endian `u32` 格式版本和 `u8` 对象种类开头。
种类为 Value、QueryKey 或 SourceAnchor。整数和枚举逐字段编码，不保存
C++ 对象内存、padding、地址或宿主 `size_t`。字符串使用 `u32` 字节长度。
摘要对长度前缀 domain 与 payload 做 SHA-256；不同协议使用不同 domain。

类型编码覆盖 bool、声明的 8/16/32/64 位有符号与无符号整数、目标已解析的
32/64 位 `usize`，以及正长度固定标量数组。类型标识含宽度、符号、
`usize` 标签、元素类型和长度。bool 使用 0/1；整数使用 `u64` 位模式，
窄整数高位必须为零；数组保持元素顺序。记录、指针、void/never、未知、
错误、延迟长度及非规范类型返回明确错误。类型和值不匹配同样拒绝。

默认限制为 16 MiB 编码对象、1,000,000 个元素、65,536 字节字符串及
256 层 AST 遍历深度。解码先验证长度和剩余字节，再分配数组/字符串；
拒绝未知版本、错误种类/标签、截断、超限、非法 bool 和尾随字节。
调用方可以降低限制。对象大小限制与 Analyzer 的逻辑资源预算相互独立。

## 稳定身份与语义指纹

`StableDefinitionId` 由 workspace 身份、规范相对模块路径、定义种类和
名称组成，定义内容摘要另存。相对路径拒绝绝对路径、`.`/`..`、反斜杠、
驱动器语法及 NUL。workspace 是调用方给定的身份，不在 codec 中探测文件。

适配器按定义和 AST 子节点的固定顺序生成摘要，side tables 只做查询，
不序列化其容器迭代顺序。指纹包含解析后的类型/引用、参数和字段顺序、
绑定契约、运算、分支/循环、linkage、global 存储与 volatile 属性。
不含源码区间、AST 地址或 Provenance NodeId。缺失类型或属于另一个模块
的 global 引用拒绝生成身份；调用方须提供已成功检查的 AST 和匹配类型表。

纯空白、注释及整数分隔下划线不改变摘要。`1 + 1` 和 `2` 保持不同指纹，
防止同值消除资源结构差异。局部名称、字面量拼写和定义顺序允许保守失效，
不承诺 alpha-renaming 或代数等价。定义内容改变保留定义 ID，但改变摘要。
修改其他定义改变整模块指纹，未修改函数自己的指纹可以保持不变。

QueryKey 含 query kind、完整 QueryContext、定义 ID/摘要及有序实参。
Comptime 实参必须含匹配常量；Runtime 实参只含类型和约束，禁止夹带已知
常量。纯求值查询只接受全 Comptime 实参，残留查询可描述混合 Stage。
`make_query_key` 验证函数签名、结果类型准入及 `usize` 与目标宽度一致性，
但不证明函数纯度，也不表示这些查询已经参与 Analyzer。

函数键不包含整模块指纹。C1b/C2 的 manifest 必须独立记录并验证保守
整模块依赖；C1a 的函数摘要不能代替对被调用函数、record 或 global 的
依赖验证。精细依赖查询仍在后续范围内。

## SourceAnchor 与 QueryContext

Anchor 为定义 ID、该定义的语义摘要和定义内 preorder 位置。独立 wire
record 不包含旧 span；`CacheIdentity::resolve` 在本次源文件建立的映射中
查找当前位置。纯空白/注释修改后位置可以重新绑定；一般结构编辑改变
定义摘要，旧 Anchor 拒绝恢复。映射拥有身份和 span，不借用 AST/HIR 指针，
可在 AST、HIR 与 Analyzer 销毁后继续使用。

[driver](../tools/pagosc/main.cpp)在 TypeChecker 前使用已解析目标布局，
随后把 triple、CPU、features、data layout、指针宽度和端序交给 Stage。
兼容身份由 CMake 对源码、构建配置和工具链版本做保守摘要，包含未提交
源码修改；不使用时间戳。语义选项和 query/budget 版本也进入键。
这不是跨编译器二进制兼容的承诺，协议扩展仍需版本和准入检查。

当前 driver 用工作目录与相对源路径作为模块身份，目录外的源文件退回
父目录与文件名。路径解析失败时不提供上下文，正常编译仍继续。跨目录
移动或从另一个工作目录编译允许失效；支持缓存重定位需要未来明确配置
workspace 身份。独立 API 的调用方可以提供稳定的显式身份。

## 验证与下一步

新增 20 个 GoogleTest 和 1 个独立进程测试。覆盖所有整数宽度、标量数组、
混合 Stage、目标/版本变化、类型及长度错误、逐字节截断、变异后的规范
再编码、Anchor 重映射、生命周期与不完整类型表。独立进程重新解析源码，
验证重复运行、明确身份下改变工作目录、空白编辑、函数编辑、无关定义
修改及 RV32/RV64 的身份行为。

```sh
cmake --build --preset debug
ctest --preset debug
cmake --build --preset gcc-debug
ctest --preset gcc-debug -R '^(Cache|pagos-cache-identity-process)'
cmake --build --preset asan
ctest --preset asan -R '^(Cache|pagos-cache-identity-process)'
python3 scripts/check_docs.py
git diff --check
```

Debug 完整回归 201/201 通过，包含原有 lit 测试；GCC 兼容构建通过，
GCC 与 ASan/UBSan 的本轮专项测试各 21/21 通过。修改的 C++ 通过
clang-format 检查，文档链接与差异空白检查通过。

后续 [C1b](cache-store-c1b.md)已交付文件 manifest、内容寻址对象、
原子发布与故障恢复。
本轮没有磁盘命中、性能收益或 Provenance 快照恢复；资源协议、纯度准入、
记录/指针负载、缓存 CLI 和实际跳过求值分别在后续迭代实现。
