# 第五阶段验收与交接

2026-11-16，S5.6 综合验收通过。三条固定流程完成 **9 对** GUI/CLI 结果对照，桌面生命周期回归和本机负载门限通过。验收范围见 [需求 §7.2](requirements.md#72-验收要求) 和 [开发与验证 §9.3](development.md#93-验收基准与证据)。

## 环境与构建

Windows x64 Release `/MD`，系统构建号 26300.9974，AMD Ryzen 9 7945HX，32 个逻辑处理器，15.69 GiB 物理内存。构建使用 MSVC 19.51.36248.0、CMake/CTest 4.4.0、Ninja 1.13.2。GUI 使用 Qt 6.11.1 Widgets、QtNodes 3.0.16（`7c6341a`）和 OpenCV 4.14.0-pre。[构建与复测命令](development.md#329-s56-release-构建与验收)

| 验证 | 结果 |
|---|---|
| Release，GUI 和 OpenCV 开启 | 全量 **116/116** 项 CTest 通过，Qt 集成用例使用 offscreen。视觉 CLI 的 DLL 导入包含 OpenCV，未导入 Qt |
| Release，GUI 和 OpenCV 关闭 | 独立构建 **92/92** 项 CTest 通过。`visionpro_cli --help` 返回 0，CLI 的 DLL 导入无 Qt 或 OpenCV |
| Windows 原生 Qt 交互与显示 | `windows` 平台复测通过双入口对照、4 个生命周期用例和 5 个图像/ROI 用例。自动化检查窗口、控件交互、Blob 叠加和图像坐标 |
| 部署启动 | `VisionProGui` 安装包在移除开发套件查找路径后使用随包 DLL 创建流程窗口，正常关闭并返回 0 |

## GUI/CLI 结果对照

`visionpro_acceptance_parity_tests` 从 S4 方案另存临时副本，字节对比确认 12×10 UInt8 PNG 和 9×9 UInt8 BMP 被复制到新包，原方案清单保持原样。方案格式、四个视觉工具配置及 Blob Record Schema 均为版本 1。文件读取、阈值、Blob 和整数范围判断分别使用 `visionpro.image.read_file`、`visionpro.image.threshold`、`visionpro.image.blob` 和 `visionpro.judge.integer_range`。五路汇合使用现有模拟收集和交付工具。[固定方案](../samples/s4/solution.json) [输入清单](../samples/s4/inputs.json)

| 流程 | 原始配置 | GUI 编辑并保存后 | 布局保存与重载 |
|---|---|---|---|
| 双区域单链（4 次调用） | Blob 面积 6、9，总面积 15，判断从 `count` 读取 2，判定 OK | 检查器把来源改为 `components[0].area_pixels`，读取 6，判定 NG。连通域和总面积保持一致 | 读图节点位置恢复，编辑后结果一致 |
| 五路阈值汇合（18 次调用） | 汇合和交付各一次，按绑定次序得到 `[2,2,2,0,0]`，各路判定为 `[OK,OK,OK,NG,NG]` | 第四路阈值由 210 改为 150，交付 `[2,2,2,2,0]`，第四路判定变为 OK | 读图节点位置恢复，编辑后结果一致 |
| 旋转 ROI 继承（5 次调用） | 阈值掩膜 7 点，Blob 面积 6，质心 `(5,4)`，判定 OK | ROI 的 X 修正从 +1 改为 −1。阈值掩膜仍为 7 点，Blob 面积 7，质心 `(3,4)`，判定 NG | 读图节点位置恢复，编辑后结果一致 |

每组从桌面运行并由独立 CLI 进程加载同一磁盘方案运行。两边分别核对修订、调用顺序、控制边状态、技术状态、判定、输出摘要、诊断及来源。每条结果来源都指向本轮较早的实际调用，继承 ROI 另核对来源端口和区域 ID。逐轮验证会话、快照、轮次、执行作用域、调用及尝试身份后，对照时只忽略各进程生成的会话、快照、轮次和调用标识。布局阶段还核对新修订及保存的坐标，跨修订比较时只额外忽略文档修订号。CLI 图像摘要包括尺寸、格式和样本统计，固定像素与叠加位置由图像/ROI 用例校验。[双入口测试](../tests/ui/acceptance_parity_tests.cpp)

## 交互与生命周期

| 范围 | 回归证据 |
|---|---|
| 编辑与持久化 | `visionpro_canvas_tests`、`visionpro_editor_tests`、`visionpro_inspector_tests` 覆盖控制分叉/汇合、命名出口、拖动、统一撤销/重做、跨作用域诊断、非相邻绑定、Group 边界、复制与往返。`infrastructure.solution_transfer` 及保存事务用例覆盖资源目标、冲突和写入失败保护。编辑器测试确认损坏清单保留当前方案、未知内容按只读处理 |
| 同会话发布与资源 | `visionpro_run_lifecycle_tests` 在首节点等待时将后继参数从 7 改为 11，资源从修订 1 改为 2，并覆盖旧路径上的文件。旧轮后继仍生成像素 `[7,17,17,7]`，下一轮生成 `[11,31,31,11]`。应用服务回归验证改名和布局保留兼容状态、发布失败保留活动快照以及不兼容绑定创建新状态代次 |
| 图像与 ROI | `visionpro_image_view_tests` 与 `visionpro_visual_tests` 验证灰度/BGR、带步长的裁剪、像素及根坐标、缩放和平移。对照固定夹具的有效成员、继承/修正/排除、掩膜和边界。Blob 包围框与质心叠加在选中调用的真实二值输入上 |
| A27 观察与窗口 | 通知容量 1 时可观察到超容，隐藏面板期间业务完成并实际退出，重开后按身份查询。历史淘汰后当前图像的像素和来源仍可读。切换和关闭等待旧轮退出，旧通知不进入新面板。直接销毁窗口后，从保留的运行句柄核对迟到的成功结果及实际退出 |
| A30 单节点调试 | `visionpro_editor_run_tests` 核对一次性输入仅调用目标，保留持久绑定、撤销历史和会话状态，缺输入或无效草稿明确失败。`visionpro_visual_tests` 用一次性 PNG 只运行 Blob，输出面积 4，并在面板检查该次图像 |

A27 使用可控制的模拟异步工具制造取消、迟到和面板停顿时序。原生窗口的交互与显示使用 Qt 自动化验证。人工观感与相机、设备及厂商 SDK 验收各按相应环境开展。

## 负载测量

`visionpro_acceptance_load` 生成真实的 **100 节点、300 条控制线、1920×1080 UInt8 灰度 PNG** 方案。读图、阈值、Blob 三个视觉节点加 97 个模拟加法节点。夹具灰度为背景 24、矩形 220，矩形位置及尺寸 `(320,240,640,360)`，阈值 128。每轮均验证全部 100 次调用和 300 条控制线成功推进，Blob 有一个连通域，面积为 230,400 像素。

正式采样在 Qt offscreen 的 1440×900 窗口运行，DPR 为 1。执行阶段画布视口 210×538，图像视口 1420×188。打开和保存各 5 次，画布在 100% 缩放下完成 20 次合成鼠标拖动，核对命中节点、布局提交和后续重绘。各流程执行和图像显示均采样 18 次。计时使用单调时钟，p50 和 p95 取从小到大第 `ceil(N×p)` 项。

| 计时起止 | 样本数 | 试测 p95（ms） | 正式 p50（ms） | 正式 p95（ms） | 正式最大值（ms） | p95 门限（ms） |
|---|---:|---:|---:|---:|---:|---:|
| 画布按下至提交布局后的重绘 | 20 | 112.30 | 65.31 | 119.86 | 126.03 | 180 |
| 打开方案至可用 | 5 | 65.31 | 63.86 | 66.77 | 66.77 | 120 |
| 触发保存至新修订提交 | 5 | 94.21 | 77.63 | 78.90 | 78.90 | 160 |
| 触发流程至实际退出且界面可再次启动 | 18 | 154.88 | 78.29 | 95.88 | 95.88 | 250 |
| 显示阈值图像至图像区域重绘 | 18 | 28.06 | 26.57 | 28.91 | 28.91 | 50 |
| 显示 Blob 图像至图像区域重绘 | 18 | 16.85 | 15.21 | 15.99 | 15.99 | 40 |

历史默认保留 16 轮，观察容量为 32。正式采样中累计丢弃 1,260 条观察通知，全部执行和图像查询成功。图像内存按去重后的原图、二值图、掩膜缓冲区分别计入，采用有效存储字节数。进程采样取 Windows Private Bytes 和 Working Set，包含 Qt、OpenCV 及其他进程内存。

| 采样点 | 历史轮数 | 历史图像字节 | 显示额外持有字节 | Private Bytes（MiB） | Working Set（MiB） |
|---|---:|---:|---:|---:|---:|
| 首轮前 | 0 | 0 | 0 | 94.00 | 50.15 |
| 第 1 轮后 | 1 | 6,220,800 | 0 | 109.54 | 66.57 |
| 第 16 轮后 | 16 | 99,532,800 | 0 | 207.83 | 164.92 |
| 第 18 轮后 | 16 | 99,532,800 | 0 | 208.91 | 166.04 |
| 清空历史后 | 0 | 0 | 2,073,600 | 109.83 | 65.72 |
| 清空显示后 | 0 | 0 | 0 | 99.91 | 55.82 |

试测 Private Bytes 峰值增量为 115.23 MiB，正式采样增量为 114.91 MiB，门限为 160 MiB。正式验收逐轮检查每个保留轮次有 3 个图像缓冲区，第 18 轮后总数为 48，合计 99,532,800 字节。清空历史时仅显示帧持有 2,073,600 字节，清空显示后两处图像占用均为 0。

同一 Release 构建的预检试测用于选定门限，随后使用另一进程执行正式采样。这里的数值用于该 Windows 主机、指定夹具和 Qt offscreen 平台。无编译或回归测试与正式采样并行。50 ms 是运行面板合并刷新间隔，图像刷新的独立计时从点击 `Show Image` 开始。

完成 [S5.6 GUI Release 构建](development.md#329-s56-release-构建与验收) 后，从仓库根目录运行。

```powershell
$guiBuild = (Resolve-Path -LiteralPath 'out/build/s56-gui').Path
$openCvBin = (Resolve-Path -LiteralPath (Join-Path $env:OpenCV_DIR '../bin')).Path
$savedPath = $env:Path
$savedPlatformPath = $env:QT_QPA_PLATFORM_PLUGIN_PATH
$env:Path = "$qtPrefix/bin;$openCvBin;$env:Path"
$env:QT_QPA_PLATFORM_PLUGIN_PATH = "$qtPrefix/plugins/platforms"
try {
    & "$guiBuild/tests/ui/visionpro_acceptance_load.exe" -platform offscreen --output "$guiBuild/load-acceptance.json"
    if ($LASTEXITCODE -ne 0) { throw 'S5.6 load measurement failed.' }
} finally {
    $env:Path = $savedPath
    $env:QT_QPA_PLATFORM_PLUGIN_PATH = $savedPlatformPath
}
```

`out/build/s56-gui` 中的 `load-pilot.json`、`load-gates.json` 和 `load-acceptance.json` 分别记录试测、预先固定的门限和正式原始采样。`ctest.log`、`tests/ui/acceptance-parity.log`、`native-*.txt` 和 `launch.log` 记录 GUI 回归及部署启动，`out/build/s56-headless/ctest.log` 记录独立构建测试。

## 交接范围

S6 按现有契约继续完整 Group 执行与复用、循环、并发、触发及工业接入。托管资源导入仍按既有边界，扩大交付时补齐版本、摘要、清理与故障恢复的端到端证据。S4 的原始夹具和回归范围见 [S4 验收记录](s4-acceptance.md#release-回归范围)。
