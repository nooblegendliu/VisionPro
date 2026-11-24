# 工作台改造基线 · UI0

2026-11-24 在 `codex/solution-orchestration` 的 `c885c48976d49ba2125fed03bef5c54381f6bd5d` 采集。开始时工作区有两个未跟踪文件，即本机 `CMakeUserPresets.json` 和执行计划。回归与负载使用修改 UI 前构建的桌面二进制。默认、抽屉和诊断三张图从同一提交另行重建后补采。

## 环境与复现

Windows x64 10.0.26300，MSVC 19.51，Ninja Release。桌面目录 `out/build/release` 使用 Qt 6.11.1、OpenCV 和 QtNodes，关闭 MVS。无界面目录 `out/build/s67-headless` 关闭 Qt、OpenCV 和 MVS。在 x64 Developer PowerShell 中运行以下命令，原生截图用例另指定 Qt 的 Windows 平台插件。

```powershell
cmake --build out/build/release --parallel 6
ctest --test-dir out/build/release -C Release -R '^visionpro_(workspace|editor_run|run_lifecycle|editor|inspector|canvas|image_view)_tests$|^app.cli_solution_graph$' --output-on-failure --no-tests=error
out/build/release/tests/ui/visionpro_workspace_tests.exe -platform windows five_lane_selection_and_history
out/build/release/tests/ui/visionpro_acceptance_load.exe -platform offscreen --output docs/acceptance/ui-workbench/ui0-desktop-load.json
```

桌面基线 8/8 通过。无界面 Release 构建成功，133 项中 130 项在沙箱内通过，3 项 Winsock 回环用例遇到 10013；允许本机回环后，原三项均通过。未连接物理设备。

补采时通过 `git archive` 导出上述提交的 `source/ui`、`source/app`、`include` 和 `samples/s6-inspection`。逐字节核对了全部 118 个文件，其中 55 个位于 `source/ui`。在 `out/diagnostics/ui0-c885c48` 隔离编译 UI，与原 Release 的核心、工具和 QtNodes 静态库链接。截图夹具创建独立的临时 Qt 设置，打开同一五路检测工作台，用鼠标点击“搜索”和“诊断”。原生窗口 1440×800，DPR 1，逻辑 DPI 96，字体 Microsoft YaHei UI。抽屉展开后画布变窄，两侧测量节点部分超出可视区域；诊断展开后占用画布下方区域。[默认工作台](acceptance/ui-workbench/ui0-windows-default.png) · [工具抽屉展开](acceptance/ui-workbench/ui0-windows-drawer.png) · [诊断展开](acceptance/ui-workbench/ui0-windows-diagnostics.png)

~~~powershell
$archive = Resolve-Path 'out/diagnostics/ui0-c885c48'
$evidence = Resolve-Path 'docs/acceptance/ui-workbench'
& "$archive/build/ui0_capture.exe" -platform windows "$archive/samples/s6-inspection" $evidence
~~~

## 改造前能力

| 区域与源码入口 | 基线结论 | 状态 |
|---|---|---|
| [运行命令](../source/ui/editor_runs.cpp)、[方案操作](../source/ui/solution_run_actions.cpp)、[控制器](../source/ui/run_controller.cpp) | 整方案可运行和取消，独立流程、触发排空、单模块调试有各自入口。顶栏停止仅打开菜单，工作台取消使用所示轮次。 | 部分实现 |
| [工作台历史](../source/ui/module_history_panel.cpp)、[运行图像](../source/ui/run_image.cpp) | 单模块可跟随或锁定具体轮次和循环调用。切换模块会清空所锁定的调用行。历史摘要取部分输出，尚无独立的完整输出页和按流程指定的展示源。 | 部分实现 |
| [方案编辑器](../source/ui/editor_window.cpp)、[资源与参数](../source/ui/editor_inspector.cpp) | 流程页、设备编辑器、共享数据报告可用。缺少方案关系总览和统一的资源入口。独立属性窗口跟随画布选择，字段能提交到撤销栈。 | 部分实现 |
| [工具箱](../source/ui/workspace_toolbox.cpp)、[主题](../source/ui/workspace_appearance.cpp)、[画布](../source/ui/control_scene.cpp) | 已有明暗主题、缩放导航、分层画布和持久化节点位置。抽屉展开时占用画布宽度。缺少固定状态、显式自动整理及整套布局恢复入口。 | 部分实现 |
| 文档替换与结果身份 | 现有测试覆盖迟到完成、历史淘汰及固定快照。旧结果与当前未保存参数之间缺少可见提示。 | 部分实现 |

## 固定样例

五路检测的同一图像在两轮中均测得 B=50。第五路上限从 40 改到 50 后，判定由 NG 变为 OK。回看首轮仍显示 [10,40] 和对应 ROI。工作区用例在方案副本中执行，并用 CLI 对照。[Windows 工作台](acceptance/ui-workbench/ui0-windows-workspace.png) · [紧凑窗口](acceptance/ui-workbench/ui0-windows-compact.png) · [属性窗口](acceptance/ui-workbench/ui0-windows-properties.png) · [深色](acceptance/ui-workbench/ui0-windows-dark.png) · [浅色](acceptance/ui-workbench/ui0-windows-light.png)

对 `samples/f3-solution-graph/solution.json` 执行 `run-solution --entry-input 5ca4f87a-ea1e-4d62-89b0-000000006310/index=N` 并指定 `--solution`。N=0、1 均退出 0，各有两条子运行，B 分别收到 6、9；N=2 退出 1，诊断为 `IndexOutOfRange`，只有 A 启动，B 被跳过。

## 桌面负载

100 节点、300 条控制线、18 轮 1920×1080 Mono8 图像，历史容量 16，观察容量 32。拖动到重绘、打开、保存、运行到可再次运行、阈值图和 Blob 图重绘的 p95 依次为 **74.40、86.79、78.00、160.19、25.98、15.36 ms**。Private Bytes 最大增量 120.46 MiB。第 18 轮保留 48 个独立缓冲、99,532,800 字节；清空历史后显示持有 2,073,600 字节，再释放显示后为 0。[原始测量](acceptance/ui-workbench/ui0-desktop-load.json)
