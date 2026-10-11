# Cache Store C1b 实现记录

状态：已实现，日期为 2026-10-12。对应
[迭代计划](cache-implementation-plan.md)的 C1b，依赖
[C1a 协议](cache-identity-c1a.md)。本轮提供独立文件存储与故障恢复，
没有接入 Analyzer 求值或增加 `pagosc` 缓存选项。

## 接口与适用性边界

[store.h](../include/pagos/cache/store.h)提供 `FileStore::lookup` 和
`FileStore::publish`，实现位于 [store.cpp](../lib/cache/store.cpp)。公共
接口使用标准 C++ 和 Pagos 类型，文件操作复用私有 LLVM Support 工具。
存储库不调用 StageAnalyzer，也不依赖 HIR、MIR 或 LLVM IR。

调用方提供规范 QueryKey、`verified_inputs`、`semantic_result` 和具有唯一
名称、种类、负载及 required 标记的 artifact 列表。输入指纹由调用方验证
全部依赖后生成，C2 首版使用保守整模块语义指纹；函数自身摘要不能代替
完整依赖。结果指纹覆盖语义内容，物理编码或可选展示变化不必改变它。

lookup 返回拥有字节缓冲的 Candidate，或带明确原因的 StoreError。
Candidate 只表示存储校验成功。恢复前仍须由适配器解码负载、验证准入、
依赖、资源协议、当前位置和必需证据。存储层不判断纯度，也不证明调用方
提供的语义结果指纹完整。本轮支持 C1a 的 Value、QueryKey 和 SourceAnchor
种类，CAS 验证共同格式/种类头；具体负载语义解码属于调用方。
新资源、快照和模板种类在后续加入明确标签与版本检查。

## 文件索引与格式

root 由调用方显式指定。构造及只读 lookup 不创建目录；首次 publish
才建立所需分片和临时目录。

```text
cache-root/
    queries/
        <query-prefix>/
            <query-hash>.lock
            <query-hash>/<verified-inputs-hash>.manifest
    objects/
        <object-prefix>/<object-hash>
    tmp/
        publish-<unique-suffix>
```

同一查询的依赖代际使用独立 manifest，不将旧输入的不同结果误报为新输入
的确定性冲突。lookup 显式匹配调用方给出的输入指纹，不扫描、猜测适用的
旧代际。历史代际和孤儿回收留给离线维护。

`store_format_version` 当前为 **1**，独立于仍为 1 的 C1a 格式/查询/预算
版本。多字节数显式使用 little-endian，没有 C++ 内存 dump。

```text
CAS:
    PGO1 | u32 version | u8 kind | u64 payload-size
         | 32-byte object-digest | payload
Manifest:
    PGM1 | u32 version | bool conflict | u64 key-size | canonical-key
         | inputs-digest | semantic-result-digest | u32 artifact-count
         | [name, kind, required, u64 payload-size, object-digest]...
         | 32-byte manifest-checksum
```

CAS 寻址摘要对版本、种类、长度和负载做带 domain 的 SHA-256。读取同时
匹配 manifest 引用、对象头及实际内容。大小在分配前检查，读取固定长度
后再验证 EOF，拒绝截断、增长和尾随字节；引用路径只由 Digest 生成。

artifact 按名称排序，名称限 64 字节 ASCII 小写字母、数字、`_` 和 `-`。
拒绝重复名称和无必需对象的普通 manifest；冲突 manifest 无对象。
校验 checksum 后仍检查版本、规范键、当前输入、引用、长度及顺序。
默认限制为单个对象 16 MiB、manifest 1 MiB、最多 32 个 artifact；
QueryKey 同时受 C1a Limits 限制。这些不是总编译器内存或磁盘容量保证。

## 发布、并发与冲突

发布先在 IO 前验证输入，再获取查询锁、读取已有代际，并按以下顺序提交：

1. 在 root 的 `tmp/` 创建唯一文件，完整写入并检查 flush/close 错误。
2. 用不覆盖目标的硬链接发布 CAS；竞争写入者先发布时重新验证内容。
   已发布 CAS 从不原地写入，也不覆盖损坏对象。
3. 所有新对象已发布并验证后，用临时文件原子 rename 发布 manifest。

同键写入使用独占文件锁，lookup 使用共享锁，等待时间有界。锁文件不在
使用期间删除或替换，进程退出由系统释放锁。进程内另有互斥，避免部分
平台的文件锁不能区分线程；首版进程内 lookup/publish 串行。
不同进程、不同查询仍可并发共享同一 CAS 对象。

相同已验证输入、相同语义结果返回 AlreadyPresent，保留已有完整候选。
不同结果返回 DeterminismConflict，并原子保存冲突标记，停止该代际复用。
若写入标记发生 IO 错误，仍报告冲突和 IO 原因，并在当前进程禁用该输入。
完全不可写的文件系统不能保证其他进程看到新标记；调用方必须保留冲突
报告，不能当成成功发布或采用最后写入者结果。

缺失/损坏的必需对象使整个 lookup 失败，不返回部分候选。可选详情失败
时保留引用而 `bytes` 为空，明确表示不可用。缺失对象可重发，损坏 manifest
可原子替换；损坏 CAS 保持未命中，需离线删除后重算。

## 故障恢复与维护

普通失败尽力清理临时文件；突然退出可留下临时文件及已发布孤儿，没有
manifest 就不会成为候选。manifest 发布后失败，重新打开只能看到完整
旧/新候选或未命中。StoreError 区分未找到、缺对象、坏索引/对象、不兼容、
键/输入不匹配、超限、IO、锁忙、冲突和分配失败，并保留可用系统错误码。
LLVM 输出流错误转为返回值，不进入 fatal-error 析构路径。

本机文件系统须支持同文件系统硬链接与原子 rename；不支持时返回 IO
失败。已在 Linux 上验证 Clang 和 GCC。首版不调用 fsync，不承诺断电后
保留最近写入；重启后缺失、截断或校验失败正常未命中。目录删除、不可写、
磁盘满或锁超时同样由调用方回退。

没有在线 GC、容量淘汰、mmap、压缩、数据库或清理 CLI。离线维护必须
停止所有使用该 root 的读写任务，再清理临时文件、历史 manifest 和不可达
对象；不能并发删除锁文件。整目录删除也只在维护窗口执行。

## 验证与下一轮

新增 [cache_store_test.cpp](../tests/unit/cache_store_test.cpp)的 17 个
GoogleTest 和 [独立进程测试](../tests/cache_store_process.py)，覆盖：

- 新进程打开、类型/多对象关联、缓冲所有权、跨查询去重、输入代际、
  可选缺失、重复发布及持久冲突。
- manifest/对象逐字节截断、坏 checksum、未知版本、非法引用、键/输入
  不匹配、超大长度、尾随字节和非普通文件。
- 部分写入、close/发布故障、ENOSPC 注入、目录删除与真实文件系统发布。
- 临时写入、对象发布后、manifest 发布前后的真实进程中断。
- 同键与跨键八进程并发、不同结果竞争、锁超时和锁持有者死亡。

测试中的资源槽只验证物理多对象关联，不是已实现的 ResourceRecipe。

```sh
cmake --build --preset debug
ctest --preset debug
cmake --build --preset gcc-debug
ctest --preset gcc-debug -R '^(Cache|pagos-cache)'
cmake --build --preset asan
ctest --preset asan -R '^(Cache|pagos-cache)'
python3 scripts/check_docs.py
git diff --check
```

Debug 完整回归 219/219 通过，包含原有 lit。GCC 兼容构建及 ASan/UBSan
构建通过，各自缓存专项 39/39 通过（C1a/C1b 共 37 个 GoogleTest 和两个
进程测试）。修改的 C++ 通过 clang-format，文档与差异空白检查通过。

下一轮 C2a 接入纯叶函数标量缓存、资源核算和实验性 CLI/遥测，验证
关闭/冷/热模式语义一致与真实跳过求值。C1b 不提供编译时间改善的证据。
