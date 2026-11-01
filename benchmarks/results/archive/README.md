# 历史成本报告

此处保留此前只位于本机 `build/benchmarks/` 的原始报告，元数据和样本
保持原样：

- `cache-c2b.json`：C2b 的 off/cold/hot 计算复用测量。
- `evidence-c3.json`：C3 快照阶段、CLI 读取及残留模板复用测量。
- `provenance-c3q.json`：C3q 合成图的可恢复查询与公式准备测量。

这些数据使用各自记录的源码、二进制、工具链与采样配置。S2 使用新的
统一 Release 构建重新采样；历史报告用于追溯，不与 S2 样本合并计算收益。
