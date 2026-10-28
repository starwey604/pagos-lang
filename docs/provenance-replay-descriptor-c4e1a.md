# 纯标量重放描述与绑定验证：C4e1a

2026-10-28。C4e1a 定义可独立保存的 ReplayDescriptor，作为后续关键摘要
记录和纯计算详情重放的数据契约。本轮提供规范 codec、类型/引用准入、
当前语义绑定验证和显式文件系统 CAS 存储；尚未让 Stage 生成描述，也没有
执行配方或新增 CLI。C4e1b 接入实际记录，C4e2 实现有界重放。

后续 [C4e1b](provenance-scalar-replay-c4e1b.md) 已接入真实 Stage 记录、
版本化的独立能力项和非执行式源码对应性验证；本页记录描述层的边界。

## 描述和准入边界

描述保存完整 EvaluatePureSpecialization QueryKey，包括定义及其语义指纹、
目标 triple/CPU/features/data layout/指针宽度/端序、编译器构建、语义选项、
查询/预算版本，以及全部带类型的 Comptime 实参。verified_inputs 记录调用方
已经验证的依赖指纹；初始适配仍可使用保守的整个模块指纹。Runtime 实参、
残留特化和其他 QueryKind 均拒绝。

还保存结果 SourceAnchor、结果类型、预期 TypedValue 指纹，以及自包含的
纯标量配方。配方为按依赖顺序排列的节点数组，引用是从零开始的索引，
严格指向先前节点。最后一个节点为结果，所有节点必须通过依赖贡献于结果；
自环、前向引用、无用节点、非法输入槽及错误元数拒绝。验证和可达性扫描
使用迭代过程，长配方不依赖宿主调用栈。

每个节点保存固定 wire 操作码、类型、所在定义内的结构位置，以及该操作
需要的输入槽、常量或操作数引用。Input 槽索引完整原始 Comptime 参数列表，
值由 QueryKey 提供；Constant 保存规范的标量值。节点不保存指针、AST/HIR
引用或此前执行轨迹，也不要求调用方永久记录每次求值操作。

版本 1 允许规范 bool、u8/u16/u32/u64、i8/i16/i32/i64，以及目标宽度的
usize。usize 与同宽无符号整数仍为不同类型；非规范冗余类型字段拒绝。
数组、record、pointer、未定类型和其他宽度暂不准入。

| 操作 | 输入约束 | 结果 |
| --- | --- | --- |
| Input / Constant | Comptime 槽 / 类型匹配的常量 | 声明的标量类型 |
| Negate / BitNot | 一个整数 | 相同整数类型 |
| Not | 一个 bool | bool |
| Add / Subtract / Multiply / Divide / Remainder | 两个相同类型整数 | 同类型整数 |
| BitAnd / BitOr / BitXor / ShiftLeft / ShiftRight | 两个相同类型整数 | 同类型整数 |
| Equal / NotEqual | 两个相同类型标量 | bool |
| Less / LessEqual / Greater / GreaterEqual | 两个相同类型整数 | bool |

操作码独立于 syntax 枚举编号，Input=1、Constant=2，依次至 GreaterEqual=21。
完整定义见 [replay_descriptor.h](../include/pagos/stage/replay_descriptor.h)。
没有 Call、分支、Select、循环、短路逻辑、转换、全局/环境读取、地址、
解引用、volatile/MMIO 或外部效果入口。后续 Stage adapter 对无法表达的
函数必须报告不可用，不将其替换成伪造的 Literal 配方。

静态准入不计算操作，因此不会在本轮执行常量除法或检查动态移位量。
未来解释器必须复用既有目标整数规则，使用独立预算，处理除零、非法移位、
有符号除法溢出等失败，并核对实际结果指纹后才返回详情。

## 当前绑定与验证职责

validate_replay_descriptor 的调用方传入描述（可为空）、当前 CacheIdentity、
独立确认的特化键/依赖指纹，以及已有语义结果 TypedValue。验证包括：

1. 对完整对象执行 codec 准入和 bytes/elements/string_bytes 限制；检查描述、
   配方语义、查询及预算版本，不接受未来版本的形似负载。
2. 当前特化键和依赖指纹完全匹配。函数定义身份、定义语义指纹、参数顺序/
   类型与结果类型必须与当前定义一致。
3. 预期 TypedValue 的类型和指纹与描述一致。指纹包含整数宽度、符号性、
   usize 标记及具体值，使用独立的 pagos.replay-value.v1 域。
4. 结果锚点属于相同定义/语义指纹，结果节点位置与结果锚点一致；每个节点
   的结构位置均能在当前定义中解析。缺位置、过期定义或其他模块均拒绝。

CacheIdentity 由已经成功类型检查的源码构建，调用方负责当前依赖及结果的
验证。此 API 只验证描述结构与当前绑定；**成功不证明配方来自源码，也不
证明运算结果等于预期值**。源程序到配方的对应关系由 C4e1b 的记录 adapter
建立，真实运算与结果核验由 C4e2 完成。FileStore 的完整性检查也不能代替
这两层语义要求，手工构造的数据不会自动成为 Stage Evidence P1。

锚点遵循 C1 的语义位置契约：空白变化可复用配方并解析到新位置，定义内容
变化则失效。本轮没有复用递归查询任务的原始源码修订锁；配方不含可继续的
源码游标。所有数据持有独立值，AST 和 Analyzer 销毁后仍可验证。

空描述明确返回 MissingDescriptor；旧 Literal/value 对象不解码成描述，不从
折叠后的 49 推测此前是 square(7) 或其他计算。ReplayError 区分不兼容版本、
错误格式、绑定不符、非法配方/引用/锚点、不支持类型/操作、结果不符、额度
不足和内存分配失败；适用时附底层 codec 错误及节点索引。

## 格式与存储

新增 ObjectKind::ReplayDescriptor=15。使用现有 PGC1 包头与 FileStore CAS：

```text
PGC1 / cache format version / object kind 15
Descriptor version u32 = 1
Scalar recipe semantics u32 = 1
QueryKey blob (u64 byte length + canonical encoded key)
SourceAnchor blob (u64 byte length + canonical encoded anchor)
Verified inputs digest (32 bytes)
Result type (canonical scalar type)
Expected TypedValue digest (32 bytes)
Node count u32
Node records
  operation u8 / type / structural position u32
  Input: original parameter slot u32
  Constant: canonical scalar value
  Unary/Binary: one/two earlier node references u32
```

整数均按显式小端编码，解码要求完整消费，不使用宿主结构体布局。
blob 长度受 bytes 限制，参数/节点数受 elements 限制；解码前检查长度与剩余
负载，不按未验证的长度分配。depth 不限制平坦配方长度。完整描述指纹使用
pagos.replay-descriptor.v1 域，包含操作、引用、位置和所有绑定信息。
独立 codec 不额外提供校验和；持久内容完整性由 CAS envelope 校验，语义
合法性仍由描述解码/绑定验证负责。

显式保存示例：

```cpp
auto bytes = encode_replay_descriptor(descriptor);
if (!bytes)
    return;
auto object = store.write_object(cache::ObjectKind::ReplayDescriptor,
                                 std::move(*bytes));
// Later: read_object, decode_replay_descriptor, validate_replay_descriptor.
```

本轮只扩展 FileStore 的对象种类白名单，不发布新的语义候选项，不改变
Stage 缓存命中或现有 Evidence 能力版本。旧读取器拒绝新对象种类，已有
对象格式不变。描述和既有语义项的关联、缺描述时的能力准入/正常重算路径
由 C4e1b 完成；可选详情丢失不能编造历史或覆盖已有值。

codec 与验证会完整驻留描述、规范编码和工作集合；额度是对象/元素限制，
不是总 RSS 保证。没有活动执行、Fuel 消耗、取消或分页接口；这些属于后续
重放器，不能把当前编码验证称为已完成纯计算重放。

## 验收

12 项单元测试覆盖规范往返与指纹隔离、全部操作码及声明整数宽度/符号性、
32/64 位 usize 和 bool、无效槽/引用/类型/字段、未使用节点和未知操作、
逐字节截断/版本/尾部/恶意长度与计数、输入/目标/编译器/定义/结果变化、
源码空白重定位和过期位置、旧 Literal 缺描述、20,000 节点迭代验证，以及
文件系统 CAS 往返与对象丢失。测试明确区分静态准入与后续语义结果核验。

验证：Debug 全量 CTest 435/435（含 lit），GCC 与 ASan/UBSan 各
434/434 非 lit CTest；最终测试括号修正后的 Debug 专项 12/12。
C++ 格式、48 份 Markdown 及 `git diff --check` 均通过。

```sh
build/debug/tests/pagos_cache_tests --gtest_filter='ReplayDescriptor.*'
```
