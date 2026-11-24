# 工作台实施与回归 · UI1–UI4

本轮覆盖 Windows 桌面基础工作台。分支为 `codex/solution-orchestration`，改造前的提交为 `c885c48976d49ba2125fed03bef5c54381f6bd5d`。UI5 的方案图编辑、专用编辑器、操作员运行页和界面设计器另行排期。[基线](ui-workbench-baseline.md) · [样例和场景断言](ui-workbench-scenarios.md) · [视觉与布局约定](ui-design-system.md)

## 阶段交付

2026-11-24 最终全量回归通过。桌面 Release **176/176**，无界面 Release **133/133**，分别用时 33.04 秒和 11.03 秒。两套配置使用同一工作区的测试夹具，本机 TCP 和 Modbus 回环用例均已包含。完整 CTest 日志位于各构建目录的 `ui4-acceptance.log`。

| 阶段 | 用户可以验证的行为 | 实现与测试入口 |
|---|---|---|
| UI1 运行与历史 | 顶栏控制整方案，流程栏控制该流程的独立运行或触发。多轮并发时可选择取消对象。方案取消依次显示已请求、报告终态和子流程清理状态。锁定旧轮后，选择另一个模块仍按该次迭代查找。 | [控制器](../source/ui/run_controller.cpp) · [历史选择](../source/ui/module_history_panel.cpp) · [生命周期测试](../tests/ui/run_lifecycle_tests.cpp) |
| UI2 结果与配置 | 上部查看模块图像、流程图像或全部声明输出，下部比较当前判定与历史。流程图像来源按当前文档、流程记忆。总览区分执行关系和端口映射，定位最近一次方案报告的子流程。资源窗口分列草稿定义、会话值和引用。独立配置窗口固定对象，可撤销字段提交并单独调试。 | [结果页](../source/ui/workspace_results.cpp) · [总览](../source/ui/solution_overview.cpp) · [资源](../source/ui/solution_resources.cpp) · [配置](../source/ui/editor_inspector.cpp) |
| UI3 画布与布局 | 临时工具抽屉覆盖画布，固定后可调整宽度。节点文字区分执行状态和业务判定。显式整理只移动当前层级，撤销可恢复原布局。窗口、分割器和面板偏好保存在本机。 | [画布测试](../tests/ui/canvas_tests.cpp) · [工作台交互测试](../tests/ui/workspace_tests.cpp) · [设计约定](ui-design-system.md) |
| UI4 编辑与重跑 | 五路样例从上限 40 调到 50，第五路实测 B 均为 50，判定 NG→OK。A→B 的输入 0、1 各交付一次，值为 6、9。输入 2 使 A 越界，B 启动次数为零。 | [复现和详细比对](ui-workbench-scenarios.md) · [跨进程保存重载](../tests/ui/acceptance_parity_tests.cpp) |

草稿编辑代次用于提示“结果来自此前配置”。撤销回到外观相同的值时仍可保留此提示。只有最近一份整方案报告提供父子关系，较早的独立流程记录仍按其自身快照浏览。资源窗口支持编辑相机、TCP/Modbus 连接和流程触发，共享值与队列提供只读查看。集合值按需分批展开。

## 复现

桌面构建启用 Qt、QtNodes 与 OpenCV，关闭 MVS。无界面配置关闭 Qt、OpenCV 与 MVS。以下命令在仓库根目录的 x64 Developer PowerShell 中运行，DLL 与 Qt 插件路径按[桌面环境说明](s6-acceptance.md#复现)配置。

```powershell
cmake --build out/build/release --parallel 8
ctest --test-dir out/build/release -C Release -j 2 --output-on-failure --no-tests=error -O out/build/release/ui4-acceptance.log
ctest --test-dir out/build/s67-headless -C Release -j 2 --output-on-failure --no-tests=error -O out/build/s67-headless/ui4-acceptance.log
$env:VISIONPRO_UI_STATE_SCREENSHOTS = (Resolve-Path docs/acceptance/ui-workbench).Path
out/build/release/tests/ui/visionpro_workspace_tests.exe -platform windows workspace_visual_states five_lane_selection_and_history
out/build/release/tests/ui/visionpro_acceptance_load.exe -platform offscreen --output docs/acceptance/ui-workbench/ui4-desktop-load.json
```

## 桌面与环境

| 场景 | 实测范围与证据 |
|---|---|
| T15–T16、T20 外观和节点 | 在 2560×1440、96 DPI 的 Windows 主屏上复核 1280×720、1440×800、1920×1080 客户区，字体为 Microsoft YaHei UI。浅色、深色与系统切换保留所选轮次和图像。五路样例保留原坐标，每对相邻测量卡至少留出 4 个逻辑像素。[改造前](acceptance/ui-workbench/ui0-windows-workspace.png) · [改造后](acceptance/ui-workbench/ui4-windows-workspace.png) · [原生截图索引](ui-design-system.md#windows-实测) |
| T17–T19 整理、布局和抽屉 | 画布用例检查作用域、端口交互、撤销及保存重开。临时设置用例检查重启恢复与旧版本、损坏数据回退。工具箱覆盖分类、搜索、Esc、双击与拖放。[画布用例](../tests/ui/canvas_tests.cpp) · [窗口与键盘用例](../tests/ui/editor_tests.cpp) |
| T21、T23 文件与生命周期 | 在方案副本中编辑、保存重开，用独立 CLI 进程对照。可控任务分别延后完成与实际退出，覆盖切换方案、关闭窗口和迟到通知。[样例断言](ui-workbench-scenarios.md) · [生命周期回归](../tests/ui/run_lifecycle_tests.cpp) |
| T24 无界面入口 | 关闭 GUI 和 OpenCV 的 Release 构建独立运行。`dumpbin /dependents` 检查本次 `visionpro_cli.exe`，仅有 Windows 和 MSVC 运行库依赖。 |

125% 和 150% 通过 Windows 平台插件的进程级 Qt 缩放运行。1440×800 客户区分别输出 1800×1000 和 2160×1200 的截图。150% 下 1920×1080 逻辑窗口大于该屏幕可用面积，Windows 报告外框尺寸受限，此项仅有 Qt 客户区渲染记录。物理高 DPI 跨屏拖动、工业相机和实物 PLC 留待对应设备验证。[125%](acceptance/ui-workbench/ui4-windows-qt-1.25.png) · [150%](acceptance/ui-workbench/ui4-windows-qt-1.5.png)

## T22 桌面负载

两次均在本机单独运行 Release 夹具。窗口 1440×900，画布缩放 100%，100 个节点、300 条控制线、18 轮 1920×1080 灰度图。两次分别验证每轮 Blob 面积为 230,400 像素。布局调整后图像视口从 635×393 增至 635×476，画布视口从 701×760 变为 701×727。测量使用模块图像视口的实际重绘事件。[UI0 原始数据](acceptance/ui-workbench/ui0-desktop-load.json) · [UI4 原始数据](acceptance/ui-workbench/ui4-desktop-load.json) · [既有门限](s6-acceptance.md#桌面负载复测)

| 指标 | 样本数 | UI0 p95（ms） | UI4 p95（ms） | 门限（ms） |
|---|---:|---:|---:|---:|
| 画布拖动至重绘 | 20 | 74.40 | 78.58 | 180 |
| 打开至可用 | 5 | 86.79 | 84.43 | 120 |
| 保存至提交 | 5 | 78.00 | 77.67 | 160 |
| 启动至可再次运行 | 18 | 160.19 | 142.95 | 250 |
| 阈值图至重绘 | 18 | 25.98 | 28.18 | 50 |
| Blob 图至重绘 | 18 | 15.36 | 13.80 | 40 |

六项 p95 均在门限内。Private Bytes 相对运行前的最大增量从 120.46 MiB 变为 119.54 MiB，门限 160 MiB。两次在第 18 轮都保留 16 轮历史、48 个图像缓冲，共 99,532,800 字节。清空历史后显示仍持有 2,073,600 字节，释放显示后两次均归零。本次容量为 32 的观察通道丢弃了 1,259 条通知，18 轮结果和控制线仍可逐轮核对。
