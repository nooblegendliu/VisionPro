# 工作台场景复测 · UI4

2026-11-24，Windows x64、Release、OpenCV。五次 CLI 运行与 [UI0 记录](ui-workbench-baseline.md) 一致。两个样例目录与 UI0 基线提交 `c885c48976d49ba2125fed03bef5c54381f6bd5d` 的 Git 差异为空。每次复测都从原样例复制，保留原图及相对资源路径。

## 五次 CLI 复测

| 样例及输入 | UI0 | UI4 实测 | 退出码 |
|---|---|---|---|
| 五路检测，第五路范围 [10,40] | B=50，NG | B=50，`passed=false`，NG | 0 |
| 五路检测，仅将第五路上限改为 50 | B=50，OK | B=50，`passed=true`，OK | 0 |
| A→B，`index=0` | B 收到 6，走合格后继 | A、B 各一轮，`area=6 → fifth=6`，合格处理执行 | 0 |
| A→B，`index=1` | B 收到 9，走不合格后继 | A、B 各一轮，`area=9 → fifth=9`，不合格处理执行 | 0 |
| A→B，`index=2` | A 越界，B 跳过 | 一轮 A，`IndexOutOfRange`，B 列入 `skipped`，交付列表为空 | 1 |

两次五路检测中，`Judge lane 5.value` 的来源都指向本轮 `Measure lane 5.blue` 的实际调用。工作件坐标中的 2×2 ROI 起点为 (22,7)，按本轮位姿平移 (+3,+2) 后，相机像素起点为 (25,9)。两个 CLI 进程各自建立会话，历史锁定和跨轮回看通过桌面用例检验。

实际调用的可执行文件为 `out/build/release/source/app/visionpro_cli.exe`。以下命令在仓库根目录复现，`$inspection` 和 `$graph` 为两个样例的临时目录副本。运行间只修改副本中的 `Judge lane 5.maximum` 常量。构建所用 OpenCV 的 DLL 目录需在 PATH 中。

~~~powershell
$cli = 'out/build/release/source/app/visionpro_cli.exe'
$flow = '5ca4f87a-ea1e-4d62-89b0-000000006910'
$entry = '5ca4f87a-ea1e-4d62-89b0-000000006310'
& $cli run --solution "$inspection/solution.json" --workflow $flow
$manifest = Get-Content -Raw "$inspection/solution.json" | ConvertFrom-Json
$inspectionFlow = $manifest.workflows | Where-Object id -eq $flow
$judge = $inspectionFlow.nodes | Where-Object name -eq 'Judge lane 5'
$upper = $judge.bindings | Where-Object { $_.target.id -eq 'maximum' }
$upper.sources[0].value.value = 50.0
$manifest | ConvertTo-Json -Depth 100 | Set-Content -Encoding utf8 "$inspection/solution.json"
& $cli run --solution "$inspection/solution.json" --workflow $flow
foreach ($index in 0, 1, 2) {
    & $cli run-solution --solution "$graph/solution.json" --entry-input "$entry/index=$index"
}
~~~

本机原始 stdout 位于 `out/diagnostics/ui4-scenarios/inspection-{40,50}.json` 和 `graph-{0,1,2}.json`。已逐项读取报告中的 `bindings`、`effective_roi`、`inputs`、`public_result`、`deliveries`、`skipped` 和诊断码。样例原始清单 SHA-256 如下。

| 样例 | SHA-256 |
|---|---|
| [五路检测](../samples/s6-inspection/solution.json) | `c655700950048432824397e3f8a1d2a9ee1deb750c96a2f615403c4ef8d5ecaf` |
| [A→B](../samples/f3-solution-graph/solution.json) | `0896ae7178b738cf4e680886bc19bf6a1c77b22f880bca1c918add8dca73a326` |

## T01–T14 自动化覆盖

下表记录测试函数里的实际断言。一次 CTest 成功只证明该目标中已写出的断言通过。运行汇总、原生 Windows 和负载结果见[综合验收](ui-workbench-acceptance.md)。

| 场景 | 自动化断言和覆盖范围 |
|---|---|
| T01 命令作用域 | 五路用例点击局部运行，再按 F5 重跑。配置窗口用例从画布另选模块后用 Ctrl+F5 提交并核对目标。两项均通过 Windows 原生窗口测试。`runs_linked_solution_and_shows_delivery` 点击顶栏运行整方案，`workspace_cancel_shortcut_keeps_its_flow` 在浮动详情获焦时发送 Shift+F5。 |
| T02 锁定旧轮并取消 | `local_cancel_uses_explicit_active_round` 锁第一轮，在第二轮也活动时明确选择第二轮，核对两个句柄的状态；仅独立运行时顶栏禁用。`solution_cancel_waits_for_child_exit` 核对顶栏取消期间所浏览的旧轮不变。 |
| T03 单次命令提交 | 五路局部按钮启动后运行数为 1，第二次命令后为 2，日志各有一条完成事件；整方案按钮触发的报告包含预期的两条子运行和一次交付。 |
| T04 方案取消和实际退出 | `solution_cancel_waits_for_child_exit` 延后子运行的 `mark_exited()`，依次核对已请求取消、取消报告且仍在清理、子运行退出并恢复准入。 |
| T05 文档替换和迟到结果 | `replace_and_close_with_pending_physical_exit` 替换文档后让旧调用完成、退出；检查新面板身份为空，随后只显示新会话。另有窗口销毁后到达的完成回调。 |
| T06 草稿配置过期 | 五路测试编辑已运行模块，验证旧范围和来源提示；再次运行并选到最新轮，提示消失。循环测试在锁定历史时删除所选模块，旧调用与输出仍显示，出现此前配置提示；撤销后草稿中恢复同一模块。 |
| T07 三个结果视图的来源 | 五路测试在 NG、OK 和历史首轮之间切换，将显示图像与运行图像比较，并核对判定。循环测试按所选调用核对完整输出字段 `area_pixels=6`。 |
| T08 跨模块保持迭代 | `loop_invocation_history` 回看 0→1→0，切到同层模块后仍显示第 0 次调用的值 6。另一个模块在迭代 0 被真实条件分支跳过，在迭代 1 才执行。锁迭代 0 时图像为空、完整输出显示跳过，锁迭代 1 时同一模块有可定位的调用。 |
| T09 无效、未产出和淘汰 | `invalid_output_diagnostic` 制造单端口无效值，核对完整输出页与详情里的诊断和端口身份；越界 ROI 用例核对五个失败输出和清图。五路测试把历史容量降为 1，锁定旧轮仍能读图，解除选择后只剩最新轮。引擎夹具另断言淘汰索引返回 `ResultRetired`，持有的句柄仍可读。 |
| T10 完整输出和按需展开 | 核对五路测量的五个声明输出、端口 ID 和显示名；结构体在展开时读取字段。`pages_large_output_collection` 用 150 项实际输出，初始子项为零，每次展开 64 项，中间保留“显示更多”，最后显示第 149 项。 |
| T11 流程图像通道和手动预览 | `flow_image_remembers_skipped_source` 选择 NG 支路图像，OK 轮该通道缺图，回看首轮复原，换文档清除选择。`window_previews_and_edits_symbolic_roi` 核对手动预览后撤销栈仍干净。 |
| T12 查看方案资源 | `browses_resource_definitions_and_edits_an_endpoint` 查看时无运行会话、无编辑记录，相机仍可被独立租用。五路队列测试在非空时打开资源窗并刷新，核对收发计数和消息身份。`reproduces_saved_communication:modbus` 在回环服务端取样，打开、选择、刷新、关闭 PLC 定义后，连接数和请求数均为 0；显式执行写寄存器流程后各为 1。 |
| T13 方案关系和子运行追溯 | `overview_links_only_its_own_children` 在两次整方案运行中间插入独立 A，从总览进入第二次 B，排除独立 A，再回看第一次 B 的 `fifth=6` 及所属报告已不可用提示。 |
| T14 配置对象和撤销 | `properties_dialog_reuses_selection_and_clears_on_replacement` 开着判断模块配置时点选测量模块，核对配置窗口仍指向判断模块，编辑并撤销，调试提交的目标仍是原模块；双击另一模块后配置对象才切换。 |

断言所在的测试文件为[工作台](../tests/ui/workspace_tests.cpp)、[编辑器运行](../tests/ui/editor_run_tests.cpp)、[运行生命周期](../tests/ui/run_lifecycle_tests.cpp)、[图像预览](../tests/ui/visual_tests.cpp)和[样例行为对照](../tests/ui/acceptance_parity_tests.cpp)。
