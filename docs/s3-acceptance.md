# S3 整体验收证据

本矩阵覆盖 S3 的 26 项必验子范围。场景的阶段归属、退出条件与全量复现记录分别见 [需求 §9](requirements.md#9-验收场景与阶段矩阵c10)、[开发 §6.5](development.md#65-验收覆盖与退出条件) 和 [S3.9 整体验收记录](development.md#820-s39-与-s3-整体验收记录)。

## 场景证据

| 场景 | 已验证行为 | CTest 入口 |
|---|---|---|
| A01 | 同类型计数节点使用各自的初值和重置策略，实例重建后每轮状态重新初始化、会话状态继续累计。清单往返恢复两种节点配置 | `workflow.state_lifetimes`、`workflow.debug_state_isolation`、`infrastructure.solution_roundtrip` |
| A02 | Init 在状态域内只执行一次。显式重置恢复目标初值，其他节点继续累计。改名和布局保留代次，改变初值建立新代次，在途调用持有旧代次至退出 | `workflow.state_lifetimes`、`workflow.state_acceptance` |
| A03 | 改名及配置往返保留稳定身份。复制分配新 ID 并重映射内部引用。删除造成的悬空引用有定位，所选范围的无效依赖在调用前拒绝 | `workflow.copy_subgraph`、`workflow.copy_solution`、`workflow.validation_bindings`、`workflow.execution_scope`、`infrastructure.solution_roundtrip` |
| A04 | 结果和绑定按会话、运行、作用域、逻辑调用与尝试精确定位，旧运行结果不能作为本轮结果。裁剪帧身份保留至后台退出，固定请求保留工件关联标识 | `core.identities`、`workflow.run_isolation`、`workflow.execution_chain`、`workflow.execution_fail_fast`、`workflow.async_completion`、`workflow.async_lifetime` |
| A05 | 分别识别合法的零、空 List、Optional None 与声明允许的 NotProduced。错误类型、缺必需输出、未声明缺失及非必需 Invalid 输出均报告诊断 | `workflow.completion_contract`、`workflow.execution_chain`、`workflow.execution_fail_fast`、`workflow.external_result_delivery` |
| A06 | 末节点从非相邻来源读取字段及固定索引，将 Int64 显式转换为 Float64 后参与计算，来源选择器可追溯。错误类型定位到绑定 | `workflow.execution_chain`、`workflow.sibling_dependencies`、`workflow.validation_bindings` |
| A07 | 普通索引读取空 List 时定位到出错输入。失败、取消和超时来源按各自错误处理，Optional 或缺失策略仍报告原错误 | `workflow.execution_fail_fast`、`workflow.resolver_availability` |
| A08 | 只读解析保留全局值、版本和更新来源，缺失或版本不符时报错。预览只读快照，队列接收属于显式操作。清单往返保留变量初值和队列定义 | `core.shared_definitions`、`workflow.resolver_domains`、`workflow.run_isolation`、`workflow.execution_chain`、`workflow.execution_fail_fast`、`infrastructure.solution_roundtrip` |
| A09 | 五路共享只读输入，每路独立产出图像，原像素保持不变。裁剪视图在取消和后端收尾期间保持有效，历史索引淘汰后在途消费者仍可读取原图像 | `workflow.fork_join`、`workflow.async_cancellation`、`workflow.async_lifetime`、`workflow.result_retention` |
| A11 | 五路测量和条件各调用一次，乱序完成后汇合逻辑及模拟发送各一次。重复完成和退出被拒绝，输出顺序遵循数据绑定。磁盘方案保存前后的五路交付与控制一致 | `workflow.fork_join`、`app.cli_roundtrip` |
| A12 | 命名出口按位多选，部分未选和内外层零选传播关闭。零到达跳过汇合和发送。选中分路失败结束等待并保留失败诊断。命令行验证多选、单选和零选的保存往返 | `workflow.branch_selection`、`workflow.branch_fail_fast`、`app.cli_roundtrip` |
| A13 | 兄弟节点先取得控制许可，再等待实际来源提交，追溯记录来源先派发。逆序引用及控制与数据联合等待环在实例创建前拒绝 | `workflow.execution_scope`、`workflow.sibling_dependencies` |
| A16 | 同步和模拟异步按同一完成校验处理结果。取消后的迟到输出不发布，操作、实例、输入及资源租约持有至实际退出 | `core.tool_execution`、`workflow.execution_chain`、`workflow.async_completion`、`workflow.async_cancellation`、`workflow.async_lifetime` |
| A17 | 技术成功且判定 NG 时继续调度。技术失败没有可用算法输出，FailFast 跳过后继并解除等待中的兄弟分路 | `workflow.execution_chain`、`workflow.execution_fail_fast`、`workflow.fork_join`、`workflow.branch_fail_fast` |
| A18 | 取消先于提交时拦截迟到结果，取消晚于提交时保留已接受结果。重复完成、旧运行及错误尝试身份的通知无法再次推进控制。计算中已写入的状态按声明保留 | `workflow.async_completion`、`workflow.async_cancellation`、`workflow.state_acceptance` |
| A19 | 托管与外部资源按显式路径基准读取，固定文件核验摘要。缺工具或资源可诊断加载，有问题的执行范围被拒绝。命令行核对托管资源来源、缺失诊断和未知字段保存保护 | `infrastructure.solution_roundtrip`、`infrastructure.file_resource_execution`、`infrastructure.solution_diagnostic_loading`、`infrastructure.solution_save_failures`、`infrastructure.solution_save_cleanup`、`app.cli_roundtrip`、`app.cli_diagnostics` |
| A20 | 资源写入、临时清单和实际替换失败均保留可执行的旧方案。并发写入拒绝陈旧基线，保留首个提交。活动租约使清理延期，清理错误在提交后独立报告 | `infrastructure.solution_save_failures`、`infrastructure.solution_save_conflict`、`infrastructure.solution_save_cleanup` |
| A22 | 历史容量 0 和 1 下精确索引按策略释放，已持有的结果和图像可越过历史淘汰、会话关闭及引擎销毁继续读取 | `workflow.result_retention`、`workflow.session_lifecycle` |
| A23 | 不同固定请求竞争同一流程时只受理一个。Busy 请求不改变活动输入或排入后续运行，下一次缺输入报错。异步调用持有原图像直至退出 | `workflow.run_isolation`、`workflow.atomic_admission`、`workflow.async_lifetime` |
| A25 | 模拟后端复用图像、文本和 Binary 缓冲后，已保留结果仍可读取。输出校验区分空值和允许的不产出，拒绝坏长度、正计数空指针、缺必需槽及非必需 Invalid | `workflow.result_retention`、`workflow.completion_contract`、`workflow.external_result_delivery`、`workflow.execution_fail_fast` |
| A27 | 重复和迟到通知按运行身份拦截。慢订阅容量不足时淘汰旧事件并记录计数，结果查询及唯一逻辑终态照常完成。等待中的观察者可注销 | `workflow.async_completion`、`workflow.async_cancellation`、`workflow.run_observations`、`workflow.session_lifecycle` |
| A28 | 损坏清单、缺失资源或工具、能力不符的候选保留原活动配置。切换与关闭停止旧会话受理，并等实际退出后发布新会话或最终关闭 | `workflow.session_lifecycle`、`infrastructure.file_resource_execution`、`infrastructure.solution_diagnostic_loading` |
| A29 | 同一异步受理运行可按通知身份查询实际结果，并通过句柄同步等待完成与实际退出。等待到期保持原状态，收尾期间同流程仍 Busy | `workflow.run_observations`、`workflow.sync_run_lifecycle`、`workflow.async_lifetime` |
| A30 | 单节点使用稳定输入身份准备显式值或默认值并记录来源，只调用目标，不补跑上游或后继。缺失输入及订阅参数有诊断，隔离调试不修改活动状态 | `workflow.independent_plan`、`workflow.independent_execution`、`workflow.debug_state_isolation` |
| A31 | 模拟调用成功码 0、输出成功码 1 和业务判定分别映射。未知错误域及原码可查询，模块与算法耗时分别保留来源、单位、未知值及实测零 | `workflow.completion_contract`、`workflow.external_result_delivery` |
| A32 | UTF-8 文本、Binary 和中文路径往返保留。模拟结果按完整 Schema 映射，非法 UTF-8、遗漏输出项及版本/能力不符有诊断 | `core.tool_registry`、`workflow.external_result_delivery`、`infrastructure.solution_roundtrip`、`infrastructure.file_resource_execution` |

各子阶段记录保留固定夹具、检查数及当时的 Release 日志。S2 的 48 项 CTest 和 14 项子范围作为回归基线，包含 `core.failure_exit` 预期失败入口。见 [S2 整体验收](development.md#810-s27-与-s2-整体验收记录)、[S3.1–S3.8 实现记录](development.md#812-s31-实现与验证记录)。

## 证据边界

S3 的运行证据来自同步和受控异步模拟工具、固定文件及内存夹具。文件保存故障证据来自 Windows 本地固定 NTFS。S4 接续 OpenCV 真实算法，设备、厂商 SDK、连续运行及重试按需求矩阵的后续阶段验证。见 [需求 §6](requirements.md#6-第四阶段无-gui-视觉闭环) 和 [需求 §9](requirements.md#9-验收场景与阶段矩阵c10)。
