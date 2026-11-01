# S1 固定协议样本

这些 `.hex` 是 S0 全绿实现 `ae92865` 的协议字节样本；S1 仅增加测试，
未修改生产编码器。生成环境为固定 CI 镜像的 Clang/LLVM 22.1.8。
context 使用 `tests/unit/cache_test_input.h` 的固定 workspace/module、
RV32 目标和 `test-build-v1`，避免绑定滚动 CLI compiler build identity。

每个空白分隔的两位十六进制数代表一个字节。独立值/键/锚点、图/位置和
重放描述直接用于旧字节解码测试。checkpoint 样本绑定其测试源码、证据
闭包及固定 task ID；不能用于其他输入或真实用户任务。

`*-store.hex` 是 test-only 目录束：按相对路径字典序排列，每项是 UTF-8
相对路径、NUL、u64 小端内容长度、原始文件内容。不含 `.lock` 生命周期
文件；包含 CAS 封装、查询 manifest 或 TaskStore head。它不是产品磁盘
格式，不提供给用户迁移。束比较固定所有引用和负载，随后使用正常 loader
验证当前存储可恢复；独立 checkpoint fixture 另行验证旧字节读取。

程序与选择步骤由各 `FrozenBaseline*` 测试显式定义。标量/数组/残留缓存
各自使用独立空目录；任务存储在固定逻辑步保存，递归 receipt 束包含前一
代仍保留的 CAS。fixture 中没有实际临时目录、宿主地址或墙钟值。

测试中的 `protocol_fixture.h` 只有读取/比较能力。不要自动更新样本；
编码改变应按[协议基线](../../../docs/cache-protocol-baseline.md)做兼容
评审，明确升级或拒绝政策后，保留必要旧样本并增加新样本。
