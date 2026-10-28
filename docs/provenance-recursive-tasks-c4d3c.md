# 递归摘要持久任务与结果确认：C4d3c

2026-10-28。[C4d3b](provenance-recursive-storage-c4d3b.md) 的分段求解状态
已接入 `pagosc`。每个任务绑定一个实际函数特化和一个通道，跨进程继续时
保留 SCC、替换位置和累计工作量。达到固定点后，完整状态同时充当不可变
结果收据；消费确认与求解完成分开，未确认结果不会被默认维护删除。

本轮保持有限形式依赖/来源族的精度边界，不修改 Stage、残留程序或
RecursiveCompletion，也不替换普通原生来源查询中的递归截断。

## 开始、继续与检查

先发布证据并取得真实特化目录：

```sh
pagosc --evidence-write=.pagos/evidence --evidence-mode=summaries \
  explain-provenance input.pgs > summaries.json
pagosc --evidence-read=.pagos/evidence --evidence-mode=recursive-summary \
  --evidence-summary=rotate --evidence-channel=value \
  --query-dir=.pagos/tasks --query-action=start \
  --provenance-slice=4096 --query-max-slices=1 --max-fuel=0 \
  explain-provenance input.pgs
```

同名多个特化时，使用 `--evidence-summary-key=HEX`。名称仅用于选择，
快照最终绑定实际特化所在的虚拟 SummaryId、完整证据闭包、位置映射、
原始源码和编译器/求解器版本。空白变化也拒绝续接。恢复失败不运行 Stage
或隐式开始新任务。现有任务先以只读句柄完成严格恢复，再打开写入句柄。

任务 ID 为随机的 256 bit 摘要，显示为 64 个十六进制字符，
由 stderr 和 status 输出；也可用 `--query-id=HEX` 显式指定。已有 head
的 ID 不可重新 start。续接和取消必须提供 evidence-read 与 query-id。

```sh
pagosc --evidence-read=.pagos/evidence --evidence-mode=recursive-summary \
  --evidence-summary=rotate --evidence-channel=value \
  --query-dir=.pagos/tasks --query-id="$TASK_ID" --query-action=resume \
  --query-max-slices=all --recursive-max-steps=10000 --max-fuel=0 \
  explain-provenance input.pgs
pagosc --evidence-read=.pagos/evidence --evidence-mode=recursive-summary \
  --evidence-summary=rotate --evidence-channel=value \
  --query-dir=.pagos/tasks --query-id="$TASK_ID" --query-action=resume \
  --query-read-only --max-fuel=0 explain-provenance input.pgs
```

`provenance-slice` 是每次 advance 的原语步骤数；query-max-slices 默认 1，
可为 0 或 all。持久模式的 recursive-max-steps 是**本次命令额外允许的步骤**，
不是累计上限，默认 all；两个预算取先到者。零预算不改写已有状态。
只读模式只检查或重放，禁止步骤/切片预算和确认，也不增加 slices。

持久模式必须显式选 evidence-channel=value 或 effects，每个任务只有一个
通道。省略或选择 both 均拒绝。它只接受 query-storage=memory：执行和恢复
仍完整驻留，保存使用分段 CAS。query-channel、resident/cache buffer 选项和
小于 76 字节的 segment-bytes 拒绝；后者默认 16 KiB。query-disk-bytes 与
query-max-records 沿用任务配额。省略 query-dir 的 C4d2 新查询仍支持 both，
预算不足仍返回 restart_required，不自动保存任务。

## 输出与消费协议

stdout 为 JSON Lines：零或一条 recursive-result，然后一条 status。
未完成时仅有 status；它包含 task、generation、state、summary_key、channel、
acknowledge、acknowledged、stage_runs、累计 work、read_only 和 offloaded。
continuation_available 表示接口具备持久续接能力，不表示仍有工作待执行；
restart_required 为 false。日志与物理读统计在 stderr。

最终 recursive-result 包含 schema=1、task、sequence=1、evidence 修订绑定、
local_ids、实际 summary 描述和单通道 result。result 沿用 C4d2 的 atoms、
precision、gaps、work、nodes、links、components 和既有决策字段。
未完成的事实前缀不作为最终结果交付，达到固定点也不自动提高精度。

完成状态的所有语义表和工作计数固定，因而自身就是不可变结果收据。
重放由该状态和绑定证据确定性渲染，无须再保存一份 JSON 文本。收据中不含
本次 stage_runs、head generation 或只读标记；这些字段仅在 status 中。
同一 `(task, sequence)` 的结果行逐字节相同；调用方用这对键去重。
重放不调用 advance，不重新求解，也不改写 head 或 CAS。

开始先发布初始状态；执行完成后先在内存渲染完整结果，再原子发布对应状态，
最后写 stdout。渲染/保存失败不输出部分 JSON；输出管道故障或进程在发布后
退出时，已保存的结果可由下一进程重送。这里依赖已有 TaskStore 的原子替换
契约，不新增 fsync 或断电持久性保证。

调用方成功保存结果后显式确认：

```sh
pagosc --evidence-read=.pagos/evidence --evidence-mode=recursive-summary \
  --evidence-summary=rotate --evidence-channel=value \
  --query-dir=.pagos/tasks --query-id="$TASK_ID" --query-action=resume \
  --query-ack=1 --max-fuel=0 explain-provenance input.pgs
pagosc reclaim-queries .pagos/tasks
```

未完成任务不能确认；最终结果仅接受序号 1。确认成功后 status.state 为
completed，acknowledged=1、acknowledge=null，此后不再输出结果行。
重复确认幂等，不增加步骤或代次。stdout 成功写出绝不自动确认。
未确认完成状态为 awaiting-delivery，默认清理跳过；已确认完成或明确取消的
非活动任务才可默认清理。所有活动句柄仍阻止清理。

query-action=cancel 保存取消状态，之后可显式 resume。完成后拒绝 cancel，
避免将未确认收据变成可默认清理的取消状态。要放弃它，应明确使用
`reclaim-queries DIR --discard-resumable`；该选项也会放弃未完成工作。

退出码：0 为已确认完成或已取消，3 为仍待执行或待确认，1 为操作错误，
2 为参数错误。独立读取 stage_runs=0；显式 evidence-write start 为 1，
其最终结果与之后的只读重放仍相同。

## 确认水位与兼容

库增加 pending_sequence、acknowledged_sequence 和 acknowledge_result。
后者只改变内存水位，调用方必须成功 save_segmented/offload 才能报告确认。
存储失败保留 resident 状态；不根据内存水位推断磁盘已确认。

未确认状态仍写 v6；旧 v5/v6 的 completed 状态隐含待确认序号 1。
确认后写 v7 QueryTaskManifest：保持相同绑定与 control，在 control 末尾增加
u64 水位 1，随后为九表目录；各 leaf 仍为 v6，因此确认复用已有 CAS 段。
v7 只允许 completed 进度与 Completed head，其他水位/状态组合拒绝。
恢复先验证身份、状态和水位，再读取分段。写入还禁止用旧 resident 状态
降低已保存水位，即使调用方提供了最新 generation。

v5 的单对象 checkpoint 无水位字段，因此已确认任务拒绝导出 v5，以免丢失
消费状态；使用分段保存。旧读取器遇 v7 明确拒绝版本，不解释为 v6。
确认提交也可能在 head 原子替换后报告 IO 失败，调用方须重新读取 head；
不能假设失败意味着未发布。旧不可变 head 的结果仍可读取，但不能回滚当前
head 的确认水位。

## 验收与资源边界

单元覆盖确认前拒绝、非法序号、重复确认、v7 恢复、旧 head 重放、
确认水位回退、错误水位/状态、配额失败和替换后失败的恢复。独立进程测试
覆盖真实 Stage 特化的自递归、互递归和内部输入，双通道逐项对照新查询；
暂停/取消后恢复、累计额外预算、源修订/通道/特化/目标不符、只读文件不变、
逐字节重放、输出失败、损坏后修复和确认前后维护。

验证：Debug 全量 CTest 423/423（含 lit），GCC 与 ASan/UBSan 各
422/422 非 lit CTest；额外的显式 evidence-write 首次交付/独立读取逐字节
对照通过。C++ 格式、Python 编译、47 份 Markdown 及 `git diff --check`
均通过。

执行/恢复、验证集合和结果 JSON 缓冲仍完整驻留；本轮不宣称活动分页、
常量恢复时间或结果输出的内存上限。复用 C4d3b 的单对象/记录上限与临时
容量计量，JSON 还会占用额外内存。大型结果流式交付和完整 LSP 集成另行迭代。

```sh
ctest --test-dir build/debug \
  -R 'RecursiveCheckpointTest|pagos-recursive.*process' --output-on-failure
```
