# 工作台视觉与交互约定

工作台使用 Qt Widgets 和 Fusion。窗口、弹窗、工具图标、画布和历史表共同响应系统、浅色、深色三种外观选择。应用颜色集中在 [workspace_appearance.cpp](../source/ui/workspace_appearance.cpp)，尺寸和字体定义在 [workspace_style.hpp](../source/ui/workspace_style.hpp) 与 [workspace_style.cpp](../source/ui/workspace_style.cpp)。

## 阅读与输入

正文使用系统界面字体，Windows 优先使用 Microsoft YaHei UI。标题和节点名称使用较高字重，技术数值可用系统等宽字体。字号使用点，控件、间距和图标使用 Qt 逻辑像素，字体度量参与标题、表格、端口的布局。

| 组件 | 尺寸与行为 |
| --- | --- |
| 面板标题 | 最小高度 32，字体增大时随内容增长。标题和页签共用一行。 |
| 局部控件 | 图像操作最小高度 28，普通间距 8，紧凑排列间距 4。 |
| 工具图标 | 主工具栏图标 18，工具箱图标 24；图标按钮提供名称和工具提示。 |
| 历史表 | 行高至少 28，文字增大时另留 8；测量值、范围、耗时右对齐，缺值显示破折号。 |
| 选择与焦点 | 页签下划线、工具类别的选中标记和键盘焦点使用青色。工具项支持悬停、按下、选中和禁用状态。 |
| 状态 | 业务判定显示 OK／NG；执行失败、超时和取消使用独立文字。颜色辅助识别含义。 |

## 工作区

左侧分类栏只显示有可用工具的类别。点击类别或搜索临时打开抽屉，画布保留当前位置。点击“固定”后可拖动分割器调整宽度。Esc 关闭抽屉，焦点回到打开它的按钮。开发工具由“视图”菜单显式启用。

普通节点首行显示工具图标和模块名称；第二行保留固定高度，序号位于图标下方，执行状态及业务判定位于名称下方。序号跟随所属流程的模块列表顺序，节点、绑定和运行记录继续以领域 ID 标识。长名从中间省略，保留通道编号等后缀；悬停可读全名、调用状态和已发布结果的耗时。同一层级多次调用、尚未选定时显示“请选择调用”。卡片、端口命中、分支文字和连线端点共用画布几何。

“整理当前层级”按控制连接由上而下排列本层模块，同时为分支留出横向间距。整理占用一个撤销步骤，撤销可恢复原坐标及原先缺少的布局记录。含循环控制连接的层级会显示原因。保存的模块坐标随方案读写。

图像来源和锁定说明位于视口上方。底部聚合尺寸、像素格式、指针取样、缩放倍率和视图操作。长说明单行省略，悬停可读全文。多来源显示横向缩略图，分类图显示可滚动图例。右侧结果区可以通过“视图”菜单收起或恢复。

## 布局存储

只有可见界面布局进入用户设置的 `workspace/layout` 组。版本 1 分别记录主窗口与画布子窗口的状态、窗口几何、两处分割器、结果面板、工具抽屉和配置窗口大小。首次启动、旧版或损坏的布局使用默认尺寸。“恢复默认布局”可在打开方案时使用，操作不产生方案编辑记录。

## 验证与截图

`visionpro_canvas_tests` 检查端口和标题几何、当前层级整理和撤销。工作台测试 `toolbox_category_grid_and_search` 检查抽屉与焦点，`remembers_and_resets_workspace_layout` 检查跨窗口恢复和损坏设置回退，`workspace_visual_states` 抓取窗口初始状态、临时抽屉、诊断和全高清尺寸。运行图像、历史和配置窗口见五路检测样例测试 `five_lane_selection_and_history`。

在 Windows Qt 后端运行 `workspace_visual_states` 时，将 `VISIONPRO_UI_STATE_SCREENSHOTS` 指向已有目录。测试记录 Qt 平台、逻辑窗口尺寸、像素尺寸、屏幕、DPR 和实际字体。离屏平台生成的文件使用独立的平台后缀；额外的 `QT_SCALE_FACTOR` 是 Qt 缩放模拟。各轮实测资料保存在 [工作台验收目录](acceptance/ui-workbench/)。

### Windows 实测

主屏为 2560 × 1440，Qt Windows 平台记录 96 DPI、DPR 1 和 Microsoft YaHei UI。主窗口实际测试了 1280 × 720、1440 × 800 和 1920 × 1080 三种逻辑尺寸。截图取自真实 Qt 窗口的客户区。同屏额外设置 `QT_SCALE_FACTOR=1.25` 和 `1.5` 后，1440 × 800 窗口分别生成 1800 × 1000 和 2160 × 1200 像素图像。物理高 DPI 显示器间拖动仍待实机复核。

| 视图 | 窗口中可复核的内容 |
| --- | --- |
| [改造前](acceptance/ui-workbench/ui0-windows-workspace.png) / [改造后](acceptance/ui-workbench/ui4-windows-workspace.png) | 相同五路检测样例，第二轮为 OK。保留原方案坐标，五张测量卡各自留出间隔并显示末尾通道编号。右侧可读本轮图像、尺寸、倍率、实测值和上一轮 NG。 |
| [默认视图](acceptance/ui-workbench/ui4-windows-default.png) / [临时抽屉](acceptance/ui-workbench/ui4-windows-drawer.png) | 左栏展开后，顶部工具栏、画布中的定位模块和右侧图像边缘保持原位置。 |
| [诊断](acceptance/ui-workbench/ui4-windows-diagnostics.png) / [全高清](acceptance/ui-workbench/ui4-windows-fullhd.png) | 诊断停靠在画布下方，右侧保留图像区和结果；1920 × 1080 可在 100% 视图查看更多后继模块。 |
| [紧凑窗口](acceptance/ui-workbench/ui4-windows-compact.png) | 1280 × 720 时仍可读取当前模块、图像尺寸、倍率和历史表中的实测与允许范围。 |
| [浅色](acceptance/ui-workbench/ui4-windows-light.png) / [深色](acceptance/ui-workbench/ui4-windows-dark.png) | 所选轮次、模块强调边界、执行成功与业务 NG 在两种主题中均有文字标识。 |
| [模块配置](acceptance/ui-workbench/ui4-windows-properties.png) | 非模态窗口中并列显示基本参数、运行设置和输出说明，并保留模块调试入口。 |
