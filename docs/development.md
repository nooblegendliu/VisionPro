# 开发与验证

## 1. 当前阶段

输出图像的专项测试在启用 OpenCV、Qt 和 Python 3 的 Windows 构建中运行。FTP 用例会启动临时回环服务器。以仓库根目录为当前目录，可执行下列命令。[逐项验收与运行约定](design/image-output-node.md#8-功能验收与版本核对)

```powershell
$buildDir = "out/build/s67-gui"
cmake --build $buildDir --config Release
ctest --test-dir $buildDir -C Release --output-on-failure -R 'image_output|image_archive|app.image_source_entries|visionpro_inspector_tests|visionpro_workspace_tests|infrastructure.solution_transfer'
```

使用构建产物中的 `visionpro_cli.exe archive-list <包.dat>` 可读取条目的名称、来源和字节数。`visionpro_cli.exe archive-extract <包.dat 或目录> --same-name-folder` 会校验图像内容，按包名建子目录解压。省略该选项时图像直接写入包所在目录。文件名相同时保留两份。

`app.image_output_local` 使用显式错误分支订阅失败节点的整数状态并归档现场图。`app.image_output_package` 分别按一张和两张每包归档原图与渲染图，使用各自输出路径及条目名称解压，逐字节对照原产物。`app.image_output_ftp` 覆盖无本地目录、目录不可写、离线回退和重新连接后的补传。

图像源的专项回归可在已经配置 Qt 与 OpenCV 的构建目录中执行。以下用 `$buildDir` 表示该目录。

```powershell
cmake --build $buildDir --config Release
ctest --test-dir $buildDir -C Release --output-on-failure -R 'tools.image_source|app.image_source_entries|app.cli_camera|app.vision_roundtrip|visionpro_workspace_tests'
```

无 GUI 宿主用 `TriggerControl::start_sdk` 绑定已发布快照、流程、公共图像输入和发送方，再向返回的订阅调用 `sdk_frame`。借用缓冲区重载会在返回前复制像素，传入只读 `ImageFrame` 时复用其所有权。投递号在同一订阅内递增，被拒的投递可修正后重试。目录事件由 `ImageDirectoryTrigger::open` 建立基线，宿主周期调用 `poll()`，两次稳定扫描后提交本批变更；停止时先撤销受理，再排空或取消在途调用。桌面共用这两个宿主层与正式执行器。

CLI 的 `run --input` 和 `replay` 支持把 Image 公共输入写为 JSON 文本，例如 `{"width":2,"height":1,"stride":2,"format":"Mono8","coordinates":"sensor","pixels":"00ff"}`。坐标系需在目标方案中声明。另可用 `trigger --advance-ms 100` 驱动本地图像连续取图，每次重复该选项表示推进一次时钟。

2026-11-26 的相机与 SDK 改版在启用 Qt、OpenCV、MVS 的 Windows x64 Release 上验证，最终 180 项 CTest 一次全量通过。运行全套测试的环境需允许本机 TCP/PLC 回环通信。模拟设备测试覆盖全局主动采集参数优先于旧节点参数，MVS 原生帧夹具核对 RGB24 的通道顺序与缓冲区复制。150% 缩放另在 1920×1080 虚拟屏上运行了工作台的 19 项检查。Qt 默认 800×800 虚拟屏在该缩放下只有约 533 个逻辑像素宽，小于主工作台的最小宽度，布局恢复用例需指定桌面尺寸。[相机与 SDK 桌面实测](design/image-source-camera-sdk-audit.md)

2026-11-26 在 Windows 桌面版打开图像源，用 `samples/s4/图像/双区域 输入.png` 连续执行两次，得到 12 × 10、Mono8，SN 从 1 到 2。第二次执行前通过结果表将宽度绑定至方案全局变量「图宽」，将文件名绑定至流程局部变量「图名」。执行后在方案资源中读到 12，在运行详情中读到「双区域 输入.png」（修订 1）、作用域 0、调用 1、尝试次数 1。Qt、OpenCV 和 MVS 启用的 Windows x64 Release 全量 CTest 通过 180/180 项。[参数窗口和模块结果实测截图](design/image-origin-node.md#2-参数图像和结果分别在哪里)

2026-11-25 的 Windows x64 Release 验证启用 Qt、OpenCV 和 MVS，全量 CTest 通过 180/180 项。`app.image_source_entries` 用同一个节点和下游绑定依次核对本地 PNG、模拟相机帧、SDK 本轮图像，并覆盖目录变更、序列停止和 SDK 缓冲区复用。Qt 工作台另以 150% 缩放执行图像源交互用例。关闭 MVS 时，相机、宿主和属性窗口的四项专项通过。关闭 Qt、OpenCV 和 MVS 后，节点、相机 CLI 与相机流生命周期三项专项通过。实机枚举时 MVS 4.7.1.4 返回零台设备，物理触发、链路统计与分时频闪仍待目标相机验证。

桌面工作台的两组固定样例、原生 Windows 布局检查及 Release 负载比较见[工作台验收记录](ui-workbench-acceptance.md)。改动运行入口或结果浏览时，请同时运行 `visionpro_workspace_tests`、`visionpro_editor_run_tests` 和 `visionpro_run_lifecycle_tests`。

方案级编排的磁盘样例在 `samples/f3-solution-graph`。将下列命令的索引依次设为 0、1、2，可分别复现 6、9 和上游越界。最后一种以非零状态退出，JSON 中只有 A 的运行记录。单流程调试仍用 `run --workflow`。

```powershell
visionpro_cli run-solution --solution samples/f3-solution-graph/solution.json --entry-input 5ca4f87a-ea1e-4d62-89b0-000000006310/index=0
```

C01–C10 已整理为当前设计基线，权威契约见 [data-model.md](data-model.md)。
S1 的 2026-10-27 文档审查结果见 [§8.1](#81-s1-文档审查记录)。
S2.1–S2.4 已于 2026-10-30 完成：已有 `visionpro_core`、`visionpro_workflow`、核心领域类型、
方案/关系模型、配置快照、类型化请求、结果记录及复制重映射。Release 结果见
[§8.4](#84-s21-实现与验证记录)、[§8.5](#85-s22-实现与验证记录)、[§8.6](#86-s23-实现与验证记录)、
[§8.7](#87-s24-实现与验证记录)。
S2 核心模型与绑定验证的实施规划已于 2026-10-28 确定，按 [§5](#5-第二阶段实施与验证) 的七个子阶段推进；
S2.5 静态校验已完成并通过 Release 验证，记录见 [§8.8](#88-s25-实现与验证记录)；
S2.6 结果访问与绑定解析已完成并通过 Release 验证，记录见 [§8.9](#89-s26-实现与验证记录)；
S2.7 运行契约与组合验证已完成，Release 48/48 项 CTest 通过；S2 整体验收通过，证据与 S3 交接见 [§8.10](#810-s27-与-s2-整体验收记录)。
S2 使用 VisionMaster SDK 指南补充契约与用例。S6.10 的海康 MVS 相机 SDK 构建、枚举结果和设备验收条件见 [§10.10](#1010-s610-相机采集与触发)。

2026-11-11 完成 S3.1 执行范围、计划与工具装配，以及 S3.2 最小内存执行闭环。
Release 验证及交接见 [§8.12](#812-s31-实现与验证记录) 和 [§8.13](#813-s32-实现与验证记录)。
2026-11-12 完成 S3.3 异步、取消与实际退出。验证记录见 [§8.14](#814-s33-实现与验证记录)。
2026-11-12 完成 S3.4 分叉、汇合与基本分支。验证记录见 [§8.15](#815-s34-实现与验证记录)。
2026-11-12 完成 S3.5 状态服务与单节点调用。验证记录见 [§8.16](#816-s35-实现与验证记录)。
2026-11-12 完成 S3.6 结果交付、通知与会话生命周期。验证记录见 [§8.17](#817-s36-实现与验证记录)。
2026-11-12 完成 S3.7 配置格式、资源解析与诊断加载。验证记录见 [§8.18](#818-s37-实现与验证记录)。
2026-11-12 完成 S3.8 保存事务与故障保护。验证记录见 [§8.19](#819-s38-实现与验证记录)。
2026-11-12 完成 S3.9 命令行闭环及 S3 整体验收。验证记录见 [§8.20](#820-s39-与-s3-整体验收记录)。
参考资料复核记录见 §8.11。[§6](#6-第三阶段实施与验证) 保留 S3.1–S3.9 的任务、依赖和退出条件。
S4.1 已接入现有 OpenCV Release 安装包并验证四个视觉工具的 Schema 与适配契约。
S4.2 接入图像桥接与 `visionpro.image.read_file`。S4.3 接入 ROI、冻结继承和掩膜求值。
S4.4 接入 `visionpro.image.threshold`，验证正相、反相、独立输出、实际掩膜及继承诊断。
S4.5 接入 `visionpro.image.blob` 和 `visionpro.judge.integer_range`，验证实际连通域、类型化绑定以及 OK/NG 后继。
S4.6 验证真实单链、五路汇合、ROI 继承与独立调用，补齐 CLI 摘要和外部图像方案保存重载。
S4.7 完成当前范围的整体验收，交付固定方案、输入清单和下一阶段运行入口。见 [S4 验收与交接](s4-acceptance.md)。
当前 Release 命令与实测结果见 [§3.23](#323-s47-release-构建与回归) 和 [§8.27](#827-s47-整体验收与交接记录)。

2026-11-16，S5.1 完成 Qt 6.11.1 Widgets 和 QtNodes 3.0.16 接入，已验证桌面启动、控制画布及独立无 GUI 构建。
S5.2 接通同会话草稿发布与资源隔离，已验证切换和关闭协调。
S5.3 完成桌面草稿编辑、作用域定位和方案文件往返，已验证独立 CLI 可运行编辑器保存的方案。
S5.4 完成 UInt8 灰度和 BGR 查看、ROI 编辑及有效成员预览，已用 S4 固定样例验证同轮结果叠加。
S5.5 完成桌面运行、取消、单节点隔离调试、结果历史和 Blob 查看。
S5.6 通过 GUI/CLI 对照、交互生命周期回归和负载验收。[S5 验收与交接](s5-acceptance.md)

开发顺序以 [requirements.md](requirements.md) 为准：

| 阶段 | 主要交付与验证方式 |
|---|---|
| 1. 基础模型与契约设计 | 文档审查通过；历史审查记录见 §8.1–§8.3 |
| 2. 核心模型与绑定验证 | S2.1–S2.7 完成；14 项必需子范围和 Release 验证通过，整体记录见 §8.10 |
| 3. 无 GUI 执行与配置往返验证 | S3.1–S3.9 完成。Release 82/82 项通过，26 项 S3 必验范围全部通过 |
| 4. 无 GUI 视觉闭环 | 使用少量真实 OpenCV 工具及固定图像验证结果 |
| 5. 基础可视化界面 | S5.1–S5.6 已验收。整体验收见 §9.10 |
| 6. 运行与工业接入 | [S6.1–S6.12 实施与验收计划](s6-plan.md)给出框架与节点的实施顺序、固定输入、验收入口和设备验证条件 |

S6.12 软件综合验收的分配置结果、三套固定输入、负载原始采样及实机待办见 [S6 验收与交接](s6-acceptance.md)。

绑定校验与解析先于或伴随执行器实现。
第三阶段可以单线程调度，但不能只验收单链，把分叉/汇合整体留到后续。
格式版本、稳定标识与资源边界提前确定；完整序列化不阻塞最早的内存执行验证。
第一阶段明确首批必需契约及后续扩展边界，不要求提前设计完整并行或插件框架。

## 2. 技术环境

已确定：

- C++20。
- Qt。
- OpenCV。
- CMake。

S2 首个验证组合采用 Windows x64、MSVC 和 CMake，首批使用轻量测试可执行程序配合 CTest，
不先引入第三方测试框架。测试应能报告失败并返回非零退出码，在 Release 下也实际执行，
不能只依赖会被禁用的 assert；类型隔离可同时用编译期断言验证。
后续如需引入测试框架，按实际测试需求选择并锁定版本和获取方式，不扩展自制通用测试框架。

2026-10-30 的 S2.1–S2.4 实测环境：

| 项目 | 已验证配置 |
|---|---|
| 操作系统/架构 | Windows 10.0.26300，x64 |
| CMake / CTest | 4.4.0 / 4.4.0 |
| Visual Studio / MSBuild | Visual Studio Professional 2026 18.7.3 / MSBuild 18.7.8 |
| 生成器 | `Visual Studio 18 2026`，`-A x64` |
| MSVC | 编译器 19.51.36248.0，工具集目录 14.51.36231 |
| Windows SDK | 10.0.26100.0 |
| 语言/运行库 | C++20；Release `/MD`；库与测试一致 |
| 测试依赖 | CTest + 仓库内轻量测试程序，无第三方测试框架 |

安装 Visual Studio 的 C++ 工具和 Windows SDK，并将 CMake/CTest 加入 PATH；
使用上述 VS 生成器时无须预先把 `cl.exe` 加入普通终端 PATH。
根 CMake 仍声明最低版本 3.25，此次未在该最低版本验证，也不承诺它支持本次使用的 VS 2026 生成器。
UUID 使用 Windows SDK 的 `bcrypt` 库和系统 `BCryptGenRandom`，API 依据见
[Microsoft Learn](https://learn.microsoft.com/en-us/windows/win32/api/bcrypt/nf-bcrypt-bcryptgenrandom)。
当前非 Windows 配置会明确失败；添加对应随机源后端后才能验证其他平台。

当前构建、测试和阶段验收统一使用 **Release**，MSVC 运行库使用 `/MD`，库、应用和测试保持一致。
OpenCV 使用已有的 `4.14.0-pre` x64 Release 预编译包，来源、摘要和运行依赖见 [§8.21](#821-s41-实现与验证记录)。
S5 使用 Qt **6.11.1 / MSVC 2022 x64**，验收的安装前缀为 `D:\Env\QT\6.11.1\msvc2022_64`。
`VISIONPRO_WITH_GUI=ON` 时查找 Qt Widgets 和 Qt OpenGL，测试另需 Qt Test。用本机 `CMAKE_PREFIX_PATH` 或 `Qt6_DIR` 指定安装位置。
CMake 使用 FetchContent 获取并静态构建 QtNodes **3.0.16**，归档固定到提交 `7c6341a66a8e46b8988140b9e60d892b6a3560b3`，
SHA-256 为 `e57e7092d6b0d115568eb852dcdbf09f975677821c0822e8976f112d046fe61f`。
离线配置可用 `VISIONPRO_QTNODES_ARCHIVE` 指向同一归档，CMake 在解包前核对摘要。上游示例、测试及文档目标关闭。[`QtNodes.cmake`](../cmake/QtNodes.cmake) [官方固定源码](https://github.com/paceholder/nodeeditor/tree/7c6341a66a8e46b8988140b9e60d892b6a3560b3)

GUI 和 OpenCV 构建使用 Windows x64、Release `/MD`。默认关闭 GUI，关闭时跳过 Qt 查找和 QtNodes 获取。
首个核心构建不以 GUI 或 OpenCV 为前提；图像模型需要时可按 architecture §3 受控使用 OpenCV，
不将首批依赖选择扩展为核心永久禁止 OpenCV 的限制。

不得混用不兼容架构、编译器或运行库配置的依赖。

### 2.1 SDK 参考与未来适配环境

参考文件及版本差异见 [requirements.md](requirements.md) §2.1。
该版指南 Qt 控件章节的 Qt5.0–5.10/MSVC/QAxWidget 建议属于其二次开发环境，
不替本项目选择 Qt/CMake/编译器版本，也不使核心或无 GUI 测试依赖 COM 控件。

实际增加厂商适配器前，分别记录并验证 SDK 头文件/库版本、运行时/算子包、
架构/编译器/运行库、驱动、授权及所需模块能力；Schema/方案版本与这些版本分开。
核实运行时版本、弃用接口替代、UTF-8/原生路径转换及接口线程/生命周期能力；
缺能力时明确诊断，不以 API 存在或文档文件名证明可运行。
原生核心测试不安装厂商运行时，适配集成与真实设备验证单独分组。
本次只读取静态文档，未执行其环境配置步骤或示例代码。

[安装指南](Manual/安装指南.pdf) 作为独立环境参考：第 11 页允许不安装主界面、仅通过二次开发使用；
第 10–13 页说明安装包版本匹配、授权选项和运行环境。其路径示例包含较后期版本，
不能与 V3.0.2 用户手册及 V4.3.x SDK 指南合并成同一兼容保证。
这些约束用于未来厂商适配验证，不将 .NET、厂商授权或控件安装引入原生 core/workflow 的构建要求。

### 2.2 S3.7 JSON 依赖

`visionpro_infrastructure` 私有使用 nlohmann/json 3.12.0。仓库保留上游标签的单头文件及 [MIT 许可证](../third_party/nlohmann_json/LICENSE.MIT)。
构建直接使用仓库内文件。单头文件 SHA-256 为
`aaf127c04cb31c406e5b04a63f1ae89369fccde6d8fa7cdda1ed4f32dfc5de63`，与[上游发布记录](https://github.com/nlohmann/json/releases/tag/v3.12.0)一致。
资源摘要使用 Windows CNG 的 SHA-256，发布新清单使用 [MoveFileExW](https://learn.microsoft.com/en-us/windows/win32/api/winbase/nf-winbase-movefileexw) 的同卷无覆盖移动。

## 3. 构建约定

- 使用源码外构建目录。
- 使用 target 级编译要求、头文件路径和链接依赖。
- 要求 C++20，关闭非必要编译器扩展。
- 统一使用 Release 构建和测试；阶段退出条件只要求该配置，历史记录仅保留对应结果。
- 支持不构建 GUI 的核心验证。
- 不提交本机绝对路径、生成文件和构建缓存。
- 环境稳定后再建立共享 CMake presets。

现有 [CMakeLists.txt](../CMakeLists.txt) 保留 `visionpro_build_options` 和 `visionpro_project_files`，
提供 `visionpro_core` 与公开依赖它的 `visionpro_workflow` 静态库，别名分别为 `visionpro::core`、`visionpro::workflow`。
八个核心测试程序覆盖基础、身份、值系统、元数据、图像、几何、资源/状态和领域值集成；
八个 workflow 测试程序覆盖模型、快照、复制、请求/结果、S2.5 静态校验，以及 S2.6 结果存储和运行绑定解析。
`BUILD_TESTING` 默认开启；关闭时不创建测试程序。当前没有 GUI 或应用目标，无需 Qt/OpenCV。
共享配置没有本机绝对路径；本地构建使用已被 Git 忽略的 `out/build/`。

### 3.1 S2.1 构建与测试命令

在仓库根目录的 PowerShell 中运行。使用独立的 Release 构建目录，
VS 为多配置生成器，选择配置使用 `--config` / `-C`，不依赖 `CMAKE_BUILD_TYPE`。
本节保留 S2.1 当时执行的命令和结果；当前源码请使用 §3.7，测试总数已扩充。

```powershell
cmake -S . -B out/build/s21-release -G "Visual Studio 18 2026" -A x64 -DBUILD_TESTING=ON
cmake --build out/build/s21-release --config Release
ctest --test-dir out/build/s21-release -C Release --output-on-failure --no-tests=error
```

每条命令应返回 0；任一失败先处理再继续。S2.1 当时 CTest 结果为 Release 6/6 通过，
其中 `core.failure_exit` 使用 `WILL_FAIL` 验证故意失败确实返回非零，不能删除该检查使测试变绿。
下列直接调用预期打印一次 `FAIL`，并得到退出码 **1**：

```powershell
& ./out/build/s21-release/tests/core/Release/visionpro_core_tests.exe --force-failure
$LASTEXITCODE
```

测试输出保存在 Release 构建目录的 `Testing/Temporary/LastTest.log`；
需要完整检查输出时可在 CTest 命令上增加 `-V`。

只构建核心库的入口也已验证：

```powershell
cmake -S . -B out/build/s21-core-only -G "Visual Studio 18 2026" -A x64 -DBUILD_TESTING=OFF
cmake --build out/build/s21-core-only --config Release
```

### 3.2 S2.2 构建与测试命令

本节为 S2.2 历史记录；当前源码请使用 §3.7。
同样在仓库根目录 PowerShell 执行，继续使用 §2 的工具链，使用独立目录验证 Release：

```powershell
cmake -S . -B out/build/s22-release -G "Visual Studio 18 2026" -A x64 -DBUILD_TESTING=ON
cmake --build out/build/s22-release --config Release
ctest --test-dir out/build/s22-release -C Release --output-on-failure --no-tests=error
```

每条命令应返回 0，Release 为 **14/14 通过**，包含 S2.1 的 6 项及 S2.2 新增 8 项。
每项失败均通过进程退出码传给 CTest；测试不依赖 Release 下禁用的 `assert`。
以下故意失败入口也在本次构建上重验，`$LASTEXITCODE` 应为 **1**：

```powershell
& ./out/build/s22-release/tests/core/Release/visionpro_core_tests.exe --force-failure
$LASTEXITCODE
```

测试日志保存在 `out/build/s22-release/Testing/Temporary/LastTest.log`，构建目录与日志均不提交。

### 3.3 S2.3 构建与测试命令

本节为 S2.3 历史记录；当前源码请使用 §3.7。
在仓库根目录的 PowerShell 中执行，沿用 §2 工具链和独立构建目录：

```powershell
cmake -S . -B out/build/s23-release -G "Visual Studio 18 2026" -A x64 -DBUILD_TESTING=ON
cmake --build out/build/s23-release --config Release
ctest --test-dir out/build/s23-release -C Release --output-on-failure --no-tests=error
```

每条命令应返回 0，Release 为 **24/24 通过**，包括原有 14 项与 S2.3 新增 10 项。
测试使用固定内存图像、结构化配置与运行记录，不访问设备或外部资源，也不启动 GUI。
预期失败入口在 Release 配置中须实际返回 **1**：

```powershell
& ./out/build/s23-release/tests/core/Release/visionpro_core_tests.exe --force-failure
$LASTEXITCODE
```

CTest 日志位于 `out/build/s23-release/Testing/Temporary/LastTest.log`；本次构建输出另保留在该目录的 `build.log`。
这些本地产物均被 Git 忽略。验收内容和具体检查数见 §8.6。

### 3.4 S2.4 构建与测试命令

本节为 S2.4 历史记录；当前源码请使用 §3.7。
在仓库根目录 PowerShell 中运行；沿用 §2 工具链，使用源码外目录验证 Release：

```powershell
cmake -S . -B out/build/s24-release -G "Visual Studio 18 2026" -A x64 -DBUILD_TESTING=ON
cmake --build out/build/s24-release --config Release
ctest --test-dir out/build/s24-release -C Release --output-on-failure --no-tests=error
```

上述命令均应返回 0，Release 为 **34/34 通过**：已有 24 项核心测试与新增 10 项 workflow 测试。
预期失败入口单独调用也须返回 **1**，验证 Release 不因禁用 assert 而跳过断言：

```powershell
& ./out/build/s24-release/tests/core/Release/visionpro_core_tests.exe --force-failure
$LASTEXITCODE
```

CTest 完整输出位于 Release 构建目录的 `Testing/Temporary/LastTest.log`，本次构建输出保存在对应 `build.log`。
构建目录与日志均被 Git 忽略；不需要 Qt、OpenCV、厂商 SDK 或外部设备。
实际结果、固定夹具与覆盖边界见 §8.7。

### 3.5 S2.5 构建与测试命令

在仓库根目录 PowerShell 中运行，沿用 §2 工具链与无 GUI 目标：

```powershell
cmake -S . -B out/build/s25-release -G "Visual Studio 18 2026" -A x64 -DBUILD_TESTING=ON
cmake --build out/build/s25-release --config Release --parallel 1
ctest --test-dir out/build/s25-release -C Release --output-on-failure --no-tests=error
```

每条命令应返回 0；实际构建和测试结果集中记录在 §8.8，不能仅凭命令列表认定验证通过。
S2.5 新增 `workflow.validation_graph`、`workflow.validation_bindings`、`workflow.validation_groups`、
`workflow.validation_schema` 和 `workflow.validation_domains`，并保留全部前阶段回归及 `core.failure_exit`。
这些测试使用内存模型，不启动 GUI、不访问设备、不执行算法或读取外部资源文件。

本机首轮 `--parallel 4` 构建停在 MSBuild 工程检查步骤，已终止；改为 `--parallel 1` 后开始正常编译。
这只是本次构建执行方式，不修改共享编译器、依赖或运行库配置。
CTest 详细输出位于 Release 构建目录的 `Testing/Temporary/LastTest.log`，构建输出保留在对应 `build.log`；
这些日志和构建目录均被 Git 忽略。

### 3.6 S2.6 构建与测试命令

在仓库根目录 PowerShell 中运行，沿用 §2 工具链、无 GUI 目标和已验证的串行 MSBuild 方式：

```powershell
cmake -S . -B out/build/s26-release -G "Visual Studio 18 2026" -A x64 -DBUILD_TESTING=ON
cmake --build out/build/s26-release --config Release --parallel 1
ctest --test-dir out/build/s26-release -C Release --output-on-failure --no-tests=error
```

以上配置、构建与 CTest 命令均已返回 0，Release 44/44 项通过，实际结果见 §8.9。
S2.6 注册 `workflow.result_store` 及 `workflow.resolver_values`、`workflow.resolver_availability`、
`workflow.resolver_isolation`、`workflow.resolver_domains`，保留此前 39 项 CTest，合计 44 项。
测试使用人工终态记录、显式作用域/公开输入、内存资源目录和只读全局快照；
不启动工具、读取外部文件、执行队列消费或连接设备。
CTest 详细输出位于对应构建目录的 `Testing/Temporary/LastTest.log`；构建目录及日志均被 Git 忽略。

### 3.7 S2.7 构建与测试命令

在仓库根目录 PowerShell 中运行，沿用 §2 工具链和串行 MSBuild。公共结构增加字段时使用完整重建，避免混用旧对象文件：

```powershell
cmake -S . -B out/build/s27-release -G "Visual Studio 18 2026" -A x64 -DBUILD_TESTING=ON
cmake --build out/build/s27-release --config Release --parallel 1 --clean-first
ctest --test-dir out/build/s27-release -C Release --output-on-failure --no-tests=error
```

S2.7 保留全部既有测试，并补运行接口替身、所有权/错误边界和 S2 组合验收。
新增 `core.tool_execution`、`workflow.run_handles`、`workflow.composition` 和 `workflow.tool_execution`，
共 48 项 CTest；Release 配置的实际结果记录在 §8.10。
CTest 证据保存在 Release 构建目录的 `Testing/Temporary/LastTest.log`；构建日志一并保留，不能以编译成功代替行为验证。
模拟调用不得记为真实算法、调度器或设备验证。

### 3.8 S3.1 Release 构建与测试命令

在仓库根目录 PowerShell 中运行以下命令。

```powershell
cmake -S . -B out/build/s31-release -G "Visual Studio 18 2026" -A x64 -DBUILD_TESTING=ON
cmake --build out/build/s31-release --config Release --parallel 1
ctest --test-dir out/build/s31-release -C Release --output-on-failure --no-tests=error --output-log out/build/s31-release/ctest.log
```

CTest 运行全部 55 项测试，包含 S2 的 48 项回归和 7 项 S3.1 测试。逐项输出保存在构建目录的
`Testing/Temporary/LastTest.log`。范围与验收证据见 [§8.12](#812-s31-实现与验证记录)。

### 3.9 S3.2 Release 构建与测试命令

在仓库根目录 PowerShell 中运行以下命令。

```powershell
cmake -S . -B out/build/s32-release -G "Visual Studio 18 2026" -A x64 -DBUILD_TESTING=ON
cmake --build out/build/s32-release --config Release --parallel 1
ctest --test-dir out/build/s32-release -C Release --output-on-failure --no-tests=error --output-log out/build/s32-release/ctest.log
```

验收覆盖和运行记录见 [§8.13](#813-s32-实现与验证记录)。

### 3.10 S3.3 Release 构建与测试命令

在仓库根目录 PowerShell 中运行以下命令。

```powershell
cmake -S . -B out/build/s33-release -G "Visual Studio 18 2026" -A x64 -DBUILD_TESTING=ON
cmake --build out/build/s33-release --config Release --parallel 1
ctest --test-dir out/build/s33-release -C Release --output-on-failure --no-tests=error --output-log out/build/s33-release/ctest.log
```

验收覆盖和运行记录见 [§8.14](#814-s33-实现与验证记录)。

### 3.11 S3.4 Release 构建与测试命令

在仓库根目录 PowerShell 中运行以下命令。

```powershell
cmake -S . -B out/build/s34-release -G "Visual Studio 18 2026" -A x64 -DBUILD_TESTING=ON
cmake --build out/build/s34-release --config Release --parallel 1
ctest --test-dir out/build/s34-release -C Release --output-on-failure --no-tests=error --output-log out/build/s34-release/ctest.log
```

验收覆盖和运行记录见 [§8.15](#815-s34-实现与验证记录)。

### 3.12 S3.5 Release 构建与测试命令

在仓库根目录 PowerShell 中运行以下命令。

```powershell
cmake -S . -B out/build/s35-release -G "Visual Studio 18 2026" -A x64 -DBUILD_TESTING=ON
cmake --build out/build/s35-release --config Release --parallel 1
ctest --test-dir out/build/s35-release -C Release --output-on-failure --no-tests=error --output-log out/build/s35-release/ctest.log
```

验收覆盖和运行记录见 [§8.16](#816-s35-实现与验证记录)。

### 3.13 S3.6 Release 构建与测试命令

在仓库根目录 PowerShell 中运行以下命令。

```powershell
cmake -S . -B out/build/s36-release -G "Visual Studio 18 2026" -A x64 -DBUILD_TESTING=ON
cmake --build out/build/s36-release --config Release --parallel 1
ctest --test-dir out/build/s36-release -C Release --output-on-failure --no-tests=error --output-log out/build/s36-release/ctest.log
```

验收覆盖和运行记录见 [§8.17](#817-s36-实现与验证记录)。

### 3.14 S3.7 Release 构建与测试命令

在仓库根目录 PowerShell 中运行以下命令。

```powershell
cmake -S . -B out/build/s37-release -G "Visual Studio 18 2026" -A x64 -DBUILD_TESTING=ON
cmake --build out/build/s37-release --config Release --parallel 1
ctest --test-dir out/build/s37-release -C Release --output-on-failure --no-tests=error --output-log out/build/s37-release/ctest.log
```

验收覆盖和运行记录见 [§8.18](#818-s37-实现与验证记录)。

### 3.15 S3.8 Release 构建与测试命令

在仓库根目录 PowerShell 中运行以下命令。

```powershell
cmake -S . -B out/build/s38-release -G "Visual Studio 18 2026" -A x64 -DBUILD_TESTING=ON
cmake --build out/build/s38-release --config Release --parallel 1
ctest --test-dir out/build/s38-release -C Release --output-on-failure --no-tests=error --output-log out/build/s38-release/ctest.log
```

验收覆盖和运行记录见 [§8.19](#819-s38-实现与验证记录)。

### 3.16 S3.9 构建与命令行验证

在仓库根目录 PowerShell 中构建并运行 Release 测试。

```powershell
cmake -S . -B out/build/s39 -G "Visual Studio 18 2026" -A x64 -DBUILD_TESTING=ON
cmake --build out/build/s39 --config Release --parallel 1
ctest --test-dir out/build/s39 -C Release --output-on-failure --no-tests=error --output-log out/build/s39/ctest-release.log
```

单独复现保存前后的命令行闭环和故障路径。

```powershell
ctest --test-dir out/build/s39 -C Release -R '^app\.cli_' --output-on-failure --no-tests=error
& .\out\build\s39\source\app\Release\visionpro_cli.exe --help
```

`run` 通过方案目录下的 `solution.json` 和流程 UUID 选中执行目标。重复 `--input <端口 ID>=<值>` 提供本次公开输入，
按 Schema 读取 Bool、Int64、Float64 和 String，省略的输入由请求按方案定义处理。`run` 等待实际退出，输出本轮 JSON 摘要。
`save` 使用 S3.8 事务提交原方案配置的新修订，并返回清理报告。下一次 `run` 重新读取磁盘配置。
两种命令都接受 `--root <命名根 ID>=<目录>`，支持相对路径和带空格的路径，路径带空格时用引号包住整个参数值。

```powershell
$cli = '.\out\build\s39\source\app\Release\visionpro_cli.exe'
& $cli run --solution $manifest --workflow $workflowId --input 'offset=7'
& $cli save --solution $manifest
& $cli run --solution $manifest --workflow $workflowId --input 'offset=7'
```

`$manifest` 和 `$workflowId` 使用实际方案的文件路径和流程 UUID。`offset=7` 对应验收夹具的公开 Int64 输入。
成功退出码为 `0`，参数错误为 `2`，其他失败为 `1`。诊断 JSON 写入 stderr，已受理的失败运行也在 stdout 保留终态摘要。
`run`、`save` 和 `--help` 在返回前显式刷新并检查 stdout。输出写入或刷新失败时返回 `1`，
并在 stderr 报告 `stage=report`、`ExecutionBoundaryFailure`；已执行的流程或已提交的保存不会因此回滚。
摘要字段见 [data-model.md §4.11](data-model.md#411-s39-命令行运行摘要)，实测记录见 [§8.20](#820-s39-与-s3-整体验收记录)。

命令行静态注册下列模拟工具，配置版本均为 1。

| 工具类型 ID | 输入与结果 |
|---|---|
| `visionpro.sim.file_byte` | Binary 资源槽 `payload`，输出文件首字节 `value`（Int64） |
| `visionpro.sim.add` | Int64 输入 `left`、`right`，输出 `value`（Int64） |
| `visionpro.sim.select` | Int64 输入 `mask`（0–3），按位 1、2 选择 `left`、`right` 控制出口 |
| `visionpro.sim.collect` | 从多个 Int64 节点输出收集 `values`，按绑定顺序输出 `List<Int64>` |
| `visionpro.sim.deliver` | 接收并输出单个 `List<Int64>` 值 `values`，模拟交付 |

### 3.17 S4.1 Release 构建与验证

在仓库根目录用 PowerShell 分别配置 ON 和 OFF。将 `OpenCV_DIR` 设为现有安装包中含 `OpenCVConfig.cmake` 的目录。
已核验包的安装根目录为库目录下的 `OpenCV-4.12/build-vs/install`，配置文件位于该安装根目录的 `x64/vc18/lib`。

```powershell
$env:OpenCV_DIR = Join-Path $openCvInstall 'x64/vc18/lib'
cmake -S . -B out/build/s41-on -G "Visual Studio 18 2026" -A x64 -DCMAKE_CONFIGURATION_TYPES=Release -DBUILD_TESTING=ON -DVISIONPRO_WITH_OPENCV=ON "-DOpenCV_DIR=$env:OpenCV_DIR"
cmake --build out/build/s41-on --config Release --parallel 1
ctest --test-dir out/build/s41-on -C Release --output-on-failure --no-tests=error --output-log out/build/s41-on/ctest.log

cmake -S . -B out/build/s41-off -G "Visual Studio 18 2026" -A x64 -DCMAKE_CONFIGURATION_TYPES=Release -DBUILD_TESTING=ON -DVISIONPRO_WITH_OPENCV=OFF
cmake --build out/build/s41-off --config Release --parallel 1
ctest --test-dir out/build/s41-off -C Release --output-on-failure --no-tests=error --output-log out/build/s41-off/ctest.log
```

`$openCvInstall` 使用本机安装根目录的实际路径。ON 要求 Windows x64/MSVC、仅 Release 和 `/MD`（`MultiThreadedDLL`）。
单配置生成器通过 `CMAKE_BUILD_TYPE=Release` 选择配置。CMake 精确查找 `4.14.0`，并核对 `-pre` 后缀和三个组件的 Release 导入库及 DLL。
CTest 为 `tools.opencv_release_smoke` 单独把导入目标的 `bin` 路径加入 PATH。直接运行此程序时，可将
`x64/vc18/bin` 加到当前进程的 PATH，或把 `opencv_world4140.dll` 放到该程序旁，并使用 x64 MSVC 运行库。

### 3.18 S4.2 Release 构建与验证

沿用 [§3.17](#317-s41-release-构建与验证) 中的 `OpenCV_DIR`。在仓库根目录的 PowerShell 中运行以下命令。

```powershell
cmake -S . -B out/build/s42-on -G "Visual Studio 18 2026" -A x64 -DCMAKE_CONFIGURATION_TYPES=Release -DBUILD_TESTING=ON -DVISIONPRO_WITH_OPENCV=ON "-DOpenCV_DIR=$env:OpenCV_DIR"
cmake --build out/build/s42-on --config Release --parallel 1
ctest --test-dir out/build/s42-on -C Release -R '^(core\.|workflow\.|tools\.|app\.vision_image_summary$)' --output-on-failure --no-tests=error --output-log out/build/s42-on/ctest.log

cmake -S . -B out/build/s42-off -G "Visual Studio 18 2026" -A x64 -DCMAKE_CONFIGURATION_TYPES=Release -DBUILD_TESTING=ON -DVISIONPRO_WITH_OPENCV=OFF
cmake --build out/build/s42-off --config Release --parallel 1
ctest --test-dir out/build/s42-off -C Release -R '^(core\.|workflow\.)' --output-on-failure --no-tests=error --output-log out/build/s42-off/ctest.log
```

ON 的 CTest 为使用 OpenCV 的程序引入安装包的 `bin` 路径。直接运行 ON 的 `visionpro_cli` 时，在同一 PowerShell 中设置该路径。

```powershell
$openCvBin = (Resolve-Path -LiteralPath (Join-Path $env:OpenCV_DIR '../bin')).Path
$env:Path = "$openCvBin;$env:Path"
& .\out\build\s42-on\source\app\Release\visionpro_cli.exe --help
```

本次读图与摘要用例使用未配置摘要的 `PerInvocation` 外部图像，经 `FileResourceProvider` 和 `WorkflowEngine` 验证。
执行范围为上述 CTest 分组，涉及 SHA-256 的既有方案存储与 CLI 独立进程回归本次跳过。

### 3.19 S4.3 Release 构建与验证

沿用 [§3.17](#317-s41-release-构建与验证) 的 `OpenCV_DIR` 和现有安装包。在仓库根目录的 PowerShell 中执行。

```powershell
cmake -S . -B out/build/s43-on -G "Visual Studio 18 2026" -A x64 -DCMAKE_CONFIGURATION_TYPES=Release -DBUILD_TESTING=ON -DVISIONPRO_WITH_OPENCV=ON "-DOpenCV_DIR=$env:OpenCV_DIR"
cmake --build out/build/s43-on --config Release --parallel 1
ctest --test-dir out/build/s43-on -C Release -R '^(core\.|workflow\.|tools\.|app\.vision_image_summary$)' --output-on-failure --no-tests=error --output-log out/build/s43-on/ctest.log

cmake -S . -B out/build/s43-off -G "Visual Studio 18 2026" -A x64 -DCMAKE_CONFIGURATION_TYPES=Release -DBUILD_TESTING=ON -DVISIONPRO_WITH_OPENCV=OFF
cmake --build out/build/s43-off --config Release --parallel 1
ctest --test-dir out/build/s43-off -C Release -R '^(core\.|workflow\.)' --output-on-failure --no-tests=error --output-log out/build/s43-off/ctest.log
```

掩膜夹具使用未配置摘要的 `PerInvocation` 外部资源，验证按实际成员像素、方向、调用身份和诊断执行。
本轮执行上述 CTest 分组，跳过涉及 SHA-256 的既有存储与独立进程 CLI 用例。

### 3.20 S4.4 Release 构建与验证

沿用 [§3.17](#317-s41-release-构建与验证) 的 `OpenCV_DIR` 和现有安装包。在仓库根目录的 PowerShell 中执行。

```powershell
cmake -S . -B out/build/s44-on -G "Visual Studio 18 2026" -A x64 -DCMAKE_CONFIGURATION_TYPES=Release -DBUILD_TESTING=ON -DVISIONPRO_WITH_OPENCV=ON "-DOpenCV_DIR=$env:OpenCV_DIR"
cmake --build out/build/s44-on --config Release --parallel 1
ctest --test-dir out/build/s44-on -C Release -R '^(core\.|workflow\.|tools\.|app\.vision_image_summary$)' --output-on-failure --no-tests=error --output-log out/build/s44-on/ctest.log

cmake -S . -B out/build/s44-off -G "Visual Studio 18 2026" -A x64 -DCMAKE_CONFIGURATION_TYPES=Release -DBUILD_TESTING=ON -DVISIONPRO_WITH_OPENCV=OFF
cmake --build out/build/s44-off --config Release --parallel 1
ctest --test-dir out/build/s44-off -C Release -R '^(core\.|workflow\.)' --output-on-failure --no-tests=error --output-log out/build/s44-off/ctest.log
```

本轮按逐像素、坐标、输出所有权和调用记录验证。PNG 掩膜与 BMP 读图使用未配置摘要的 `PerInvocation` 外部资源。
涉及 SHA-256 的方案存储与独立进程 CLI 往返回归留在后续闭环验收。

### 3.21 S4.5 Release 构建与验证

沿用 [§3.17](#317-s41-release-构建与验证) 的 `OpenCV_DIR` 和现有安装包，在仓库根目录的 PowerShell 中执行。
本轮复用 S4.4 的 ON/OFF 构建目录，各自写入 S4.5 日志。

```powershell
cmake -S . -B out/build/s44-on -G "Visual Studio 18 2026" -A x64 -DCMAKE_CONFIGURATION_TYPES=Release -DBUILD_TESTING=ON -DVISIONPRO_WITH_OPENCV=ON "-DOpenCV_DIR=$env:OpenCV_DIR"
cmake --build out/build/s44-on --config Release --parallel 1 *> out/build/s44-on/build-s45.log
ctest --test-dir out/build/s44-on -C Release -R '^(core\.|workflow\.|tools\.|app\.vision_image_summary$)' --output-on-failure --no-tests=error --output-log out/build/s44-on/ctest-s45.log

cmake -S . -B out/build/s44-off -G "Visual Studio 18 2026" -A x64 -DCMAKE_CONFIGURATION_TYPES=Release -DBUILD_TESTING=ON -DVISIONPRO_WITH_OPENCV=OFF
cmake --build out/build/s44-off --config Release --parallel 1 *> out/build/s44-off/build-s45.log
ctest --test-dir out/build/s44-off -C Release -R '^(core\.|workflow\.)' --output-on-failure --no-tests=error --output-log out/build/s44-off/ctest-s45.log
```

按固定像素、几何数值、选择器和控制行为验证，文件掩膜使用未配置摘要的 `PerInvocation` 外部资源。
本轮未执行 SHA-256 校验，涉及摘要的方案存储与独立进程 CLI 往返回归留在后续闭环验收。

### 3.22 S4.6 Release 构建与验证

沿用 [§3.17](#317-s41-release-构建与验证) 中的 `OpenCV_DIR`。在仓库根目录的 PowerShell 中复用现有 ON/OFF 构建目录，单独记录 S4.6 日志。

```powershell
cmake -S . -B out/build/s44-on -G "Visual Studio 18 2026" -A x64 -DCMAKE_CONFIGURATION_TYPES=Release -DBUILD_TESTING=ON -DVISIONPRO_WITH_OPENCV=ON "-DOpenCV_DIR=$env:OpenCV_DIR"
cmake --build out/build/s44-on --config Release --parallel 1 *> out/build/s44-on/build-s46.log
ctest --test-dir out/build/s44-on -C Release -R '^(core\.|workflow\.|tools\.|app\.vision_(image_summary|roundtrip|diagnostics)$)' --output-on-failure --no-tests=error --output-log out/build/s44-on/ctest-s46.log

cmake -S . -B out/build/s44-off -G "Visual Studio 18 2026" -A x64 -DCMAKE_CONFIGURATION_TYPES=Release -DBUILD_TESTING=ON -DVISIONPRO_WITH_OPENCV=OFF
cmake --build out/build/s44-off --config Release --parallel 1 *> out/build/s44-off/build-s46.log
ctest --test-dir out/build/s44-off -C Release -R '^(core\.|workflow\.)' --output-on-failure --no-tests=error --output-log out/build/s44-off/ctest-s46.log
```

视觉 CLI 测试使用未配置摘要的 `PerInvocation` 外部 PNG、BMP 文件，逐像素核对重载前后的结果。
本轮未执行 SHA-256 校验，执行范围为上述 CTest 分组，含摘要校验的既有存储和模拟 CLI 回归未纳入。

### 3.23 S4.7 Release 构建与回归

沿用 [§3.17](#317-s41-release-构建与验证) 的 `OpenCV_DIR`。从仓库根目录运行，ON 和 OFF 使用独立构建目录。排除范围见 [S4 回归范围](s4-acceptance.md#release-回归范围)。

```powershell
$excluded = '^(app\.cli_(roundtrip|diagnostics)|infrastructure\.(file_resource_execution|solution_diagnostic_loading|solution_save_(failures|conflict|cleanup)))$'

cmake -S . -B out/build/s47-on -G "Visual Studio 18 2026" -A x64 -DCMAKE_CONFIGURATION_TYPES=Release -DBUILD_TESTING=ON -DVISIONPRO_WITH_OPENCV=ON "-DOpenCV_DIR=$env:OpenCV_DIR"
cmake --build out/build/s47-on --config Release --parallel 1 *> out/build/s47-on/build.log
ctest --test-dir out/build/s47-on -C Release -E $excluded --output-on-failure --no-tests=error --output-log out/build/s47-on/ctest.log

cmake -S . -B out/build/s47-off -G "Visual Studio 18 2026" -A x64 -DCMAKE_CONFIGURATION_TYPES=Release -DBUILD_TESTING=ON -DVISIONPRO_WITH_OPENCV=OFF
cmake --build out/build/s47-off --config Release --parallel 1 *> out/build/s47-off/build.log
ctest --test-dir out/build/s47-off -C Release -E $excluded --output-on-failure --no-tests=error --output-log out/build/s47-off/ctest.log
```

在已加载 MSVC 命令行工具的 PowerShell 中复核可执行文件导入。

```powershell
dumpbin /NOLOGO /DEPENDENTS out/build/s47-on/source/app/Release/visionpro_cli.exe

$binaries = @(Get-ChildItem -File -Recurse -Filter '*.exe' -LiteralPath out/build/s47-off/source,out/build/s47-off/tests |
    Where-Object { $_.Directory.Name -eq 'Release' })
if (!$binaries.Count) { throw 'No OFF Release binaries found.' }
foreach ($binary in $binaries) {
    $imports = dumpbin /NOLOGO /DEPENDENTS $binary.FullName
    if ($LASTEXITCODE -ne 0 -or ($imports -match '(?i)^\s*(opencv|qt)\S*\.dll\s*$')) {
        throw "Unexpected dependency check result for $($binary.FullName)"
    }
}
```

固定方案的运行、保存和重载命令见 [样例复现](s4-acceptance.md#样例复现)。

### 3.24 S5.1 Release 构建测试与启动

从仓库根目录运行。将 `$qtPrefix` 指向本机 Qt 6.11.1 的 x64 套件，沿用 [§3.17](#317-s41-release-构建与验证) 的 `OpenCV_DIR`。
使用本地固定归档时，在 GUI 配置命令中追加 `-DVISIONPRO_QTNODES_ARCHIVE=<归档的绝对路径>`。

```powershell
$qtPrefix = 'D:/Env/QT/6.11.1/msvc2022_64'
$guiBuild = 'out/build/s51-gui'
$headlessBuild = 'out/build/s51-headless'

cmake -S . -B $guiBuild -G "Visual Studio 18 2026" -A x64 -DCMAKE_CONFIGURATION_TYPES=Release -DBUILD_TESTING=ON -DVISIONPRO_WITH_GUI=ON -DVISIONPRO_WITH_OPENCV=ON "-DCMAKE_PREFIX_PATH=$qtPrefix" "-DOpenCV_DIR=$env:OpenCV_DIR"
cmake --build $guiBuild --config Release --parallel 6
ctest --test-dir $guiBuild -C Release --parallel 2 --output-on-failure --no-tests=error --output-log "$guiBuild/ctest.log"

cmake -S . -B $headlessBuild -G "Visual Studio 18 2026" -A x64 -DCMAKE_CONFIGURATION_TYPES=Release -DBUILD_TESTING=ON -DVISIONPRO_WITH_GUI=OFF -DVISIONPRO_WITH_OPENCV=OFF
cmake --build $headlessBuild --config Release --parallel 6
ctest --test-dir $headlessBuild -C Release --parallel 2 --output-on-failure --no-tests=error --output-log "$headlessBuild/ctest.log"

$installPrefix = Join-Path (Get-Location) 'out/install/s51-gui'
cmake --install $guiBuild --config Release --prefix $installPrefix --component VisionProGui
& (Join-Path $installPrefix 'bin/visionpro_gui.exe')
```

`VisionProGui` 组件部署 Qt DLL 和平台插件，使用系统安装的 Visual C++ 运行库。
可从 `Route` 输出口拉线到同层节点的输入口，拖动节点调整布局，选中控制线后使用 `Remove Link` 删除。
双击 `Review (Group)` 进入子层，点 `Parent Group` 返回，通过 `Undo` 和 `Redo` 验证两层共用编辑历史。

单独复测 Qt Test 的 Windows 平台后端时，在本次 PowerShell 会话中添加 Qt 的运行库和平台插件路径。

```powershell
$env:Path = "$qtPrefix/bin;$env:Path"
$env:QT_QPA_PLATFORM_PLUGIN_PATH = "$qtPrefix/plugins/platforms"
& "$guiBuild/tests/ui/Release/visionpro_canvas_tests.exe" -platform windows
```

### 3.25 S5.2 Release 构建与测试

沿用 [§3.24](#324-s51-release-构建测试与启动) 的 `$qtPrefix` 和 `$env:OpenCV_DIR`，从仓库根目录构建两套独立的 Release 配置。
若使用本地锁定归档，在 GUI 配置命令中追加 `-DVISIONPRO_QTNODES_ARCHIVE=<归档的绝对路径>`。

```powershell
$guiBuild = 'out/build/s52-gui'
$headlessBuild = 'out/build/s52-headless'

cmake -S . -B $guiBuild -G "Visual Studio 18 2026" -A x64 -DCMAKE_CONFIGURATION_TYPES=Release -DBUILD_TESTING=ON -DVISIONPRO_WITH_GUI=ON -DVISIONPRO_WITH_OPENCV=ON "-DCMAKE_PREFIX_PATH=$qtPrefix" "-DOpenCV_DIR=$env:OpenCV_DIR"
cmake --build $guiBuild --config Release --parallel 4
ctest --test-dir $guiBuild -C Release --parallel 2 --output-on-failure --no-tests=error --output-log "$guiBuild/ctest.log"

cmake -S . -B $headlessBuild -G "Visual Studio 18 2026" -A x64 -DCMAKE_CONFIGURATION_TYPES=Release -DBUILD_TESTING=ON -DVISIONPRO_WITH_GUI=OFF -DVISIONPRO_WITH_OPENCV=OFF
cmake --build $headlessBuild --config Release --parallel 4
ctest --test-dir $headlessBuild -C Release --parallel 2 --output-on-failure --no-tests=error --output-log "$headlessBuild/ctest.log"

$installPrefix = Join-Path (Get-Location) 'out/install/s52-gui'
cmake --install $guiBuild --config Release --prefix $installPrefix --component VisionProGui
& (Join-Path $installPrefix 'bin/visionpro_gui.exe')
```

从 S5.2 起，启用 OpenCV 的桌面包同时部署 OpenCV DLL。定向复测可给任一构建目录的 CTest 增加 `-R '^(workflow\.(session_lifecycle|session_publication|state_session_publication)|infrastructure\.file_draft_publication)$'`。

### 3.26 S5.3 Release 构建与测试

沿用 [§3.24](#324-s51-release-构建测试与启动) 的 `$qtPrefix`、`$env:OpenCV_DIR` 和本地锁定归档配置方式，从仓库根目录执行。

```powershell
$guiBuild = 'out/build/s53-gui'
$headlessBuild = 'out/build/s53-headless'

cmake -S . -B $guiBuild -G "Visual Studio 18 2026" -A x64 -DCMAKE_CONFIGURATION_TYPES=Release -DBUILD_TESTING=ON -DVISIONPRO_WITH_GUI=ON -DVISIONPRO_WITH_OPENCV=ON "-DCMAKE_PREFIX_PATH=$qtPrefix" "-DOpenCV_DIR=$env:OpenCV_DIR"
cmake --build $guiBuild --config Release --parallel 6
ctest --test-dir $guiBuild -C Release --parallel 2 --output-on-failure --no-tests=error --output-log "$guiBuild/ctest.log"

cmake -S . -B $headlessBuild -G "Visual Studio 18 2026" -A x64 -DCMAKE_CONFIGURATION_TYPES=Release -DBUILD_TESTING=ON -DVISIONPRO_WITH_GUI=OFF -DVISIONPRO_WITH_OPENCV=OFF
cmake --build $headlessBuild --config Release --parallel 4
ctest --test-dir $headlessBuild -C Release --parallel 2 --output-on-failure --no-tests=error --output-log "$headlessBuild/ctest.log"

$installPrefix = Join-Path (Get-Location) 'out/install/s53-gui'
cmake --install $guiBuild --config Release --prefix $installPrefix --component VisionProGui
& (Join-Path $installPrefix 'bin/visionpro_gui.exe')
```

在桌面程序中双击工具箱条目添加节点，拖动输出口连接控制线。双击普通节点，或选中节点后执行 **模块属性…**，可在独立窗口编辑入口、父层级和绑定。
数据来源选择器接受以 `/` 分隔的字段 ID 和从 0 开始的索引，例如 `0/area_pixels`。
双击 **Diagnostics** 的作用域错误定位节点。可通过 **View** 的 **Remove Diagnosed Control Link** 删除当前诊断指向的控制边，再用撤销恢复。
从 **File** 保存到方案目录，另存和独立复制选择尚无 `solution.json` 的目标目录。外部引用和待配置的命名根显示在诊断面板。

定向复测使用 `-R '^(visionpro_(canvas|editor|inspector)_tests|infrastructure\.solution_transfer)$'`。

### 3.27 S5.4 Release 构建与测试

沿用 [§3.24](#324-s51-release-构建测试与启动) 的 `$qtPrefix`、`$env:OpenCV_DIR` 和 QtNodes 固定归档配置。从仓库根目录执行。将 `$vsRoot` 指向 Visual Studio 安装目录，使用其 x64 开发环境及 Ninja。

```powershell
$vsRoot = 'C:/Program Files/Microsoft Visual Studio/18/Professional'
& "$vsRoot/Common7/Tools/Launch-VsDevShell.ps1" -VsInstallationPath $vsRoot -Arch amd64 -HostArch amd64 -SkipAutomaticLocation
$ninja = "$vsRoot/Common7/IDE/CommonExtensions/Microsoft/CMake/Ninja/ninja.exe"
$archive = (Resolve-Path -LiteralPath 'out/deps/qtnodes-3.0.16-7c6341a.tar.gz').Path
$guiBuild = 'out/build/s54-ninja'
$headlessBuild = 'out/build/s54-headless'

cmake -S . -B $guiBuild -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_TRY_COMPILE_CONFIGURATION=Release -DCMAKE_MSVC_DEBUG_INFORMATION_FORMAT=Embedded -DBUILD_TESTING=ON -DVISIONPRO_WITH_GUI=ON -DVISIONPRO_WITH_OPENCV=ON "-DCMAKE_MAKE_PROGRAM=$ninja" "-DCMAKE_PREFIX_PATH=$qtPrefix" "-DOpenCV_DIR=$env:OpenCV_DIR" "-DVISIONPRO_QTNODES_ARCHIVE=$archive"
cmake --build $guiBuild --parallel 8
ctest --test-dir $guiBuild -C Release --parallel 4 --output-on-failure --no-tests=error --output-log "$guiBuild/ctest.log"

cmake -S . -B $headlessBuild -G "Visual Studio 18 2026" -A x64 -DCMAKE_CONFIGURATION_TYPES=Release -DBUILD_TESTING=ON -DVISIONPRO_WITH_GUI=OFF -DVISIONPRO_WITH_OPENCV=OFF
cmake --build $headlessBuild --config Release --parallel 4
ctest --test-dir $headlessBuild -C Release --parallel 4 --output-on-failure --no-tests=error --output-log "$headlessBuild/ctest.log"

$installPrefix = Join-Path (Get-Location) 'out/install/s54-gui'
cmake --install $guiBuild --config Release --prefix $installPrefix --component VisionProGui
& (Join-Path $installPrefix 'bin/visionpro_gui.exe')
```

从 **File → Open Solution…** 打开 `samples/s4`，选择 **旋转 ROI 继承** 流程。
从 **View → Open Image Resource…** 选择 **纯白 BGR BMP**，选择 `s4.sample.rotated` 和 **Grayscale UInt8**。
选中旋转阈值节点 `…1303` 后从 **View → Preview Selected ROI** 查看 7 个成员像素。选中 **测量连通域** 节点 `…1304` 后预览其继承和修正后的 6 个成员像素。
在 **Properties → Edit ROI…** 修改区域。坐标匹配时可在图像上拖绘矩形，旋转角使用度输入。掩膜通过方案中的 Mask 资源选择。完成编辑后重新预览查看成员变化。

定向复测使用 `-R '^visionpro_(image_view|visual)_tests$'`。

### 3.28 S5.5 Release 构建与测试

按 [§3.27](#327-s54-release-构建与测试) 初始化 MSVC x64 开发环境及 `$ninja`、`$qtPrefix`、`$env:OpenCV_DIR`、`$archive`。从仓库根目录执行。

```powershell
$guiBuild = 'out/build/s55-gui'
$headlessBuild = 'out/build/s55-headless'

cmake -S . -B $guiBuild -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_TRY_COMPILE_CONFIGURATION=Release -DCMAKE_MSVC_DEBUG_INFORMATION_FORMAT=Embedded -DBUILD_TESTING=ON -DVISIONPRO_WITH_GUI=ON -DVISIONPRO_WITH_OPENCV=ON "-DCMAKE_MAKE_PROGRAM=$ninja" "-DCMAKE_PREFIX_PATH=$qtPrefix" "-DOpenCV_DIR=$env:OpenCV_DIR" "-DVISIONPRO_QTNODES_ARCHIVE=$archive"
cmake --build $guiBuild --parallel 8
ctest --test-dir $guiBuild -C Release --parallel 4 --output-on-failure --no-tests=error --output-log "$guiBuild/ctest.log"

cmake -S . -B $headlessBuild -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_TRY_COMPILE_CONFIGURATION=Release -DCMAKE_MSVC_DEBUG_INFORMATION_FORMAT=Embedded -DBUILD_TESTING=ON -DVISIONPRO_WITH_GUI=OFF -DVISIONPRO_WITH_OPENCV=OFF "-DCMAKE_MAKE_PROGRAM=$ninja"
cmake --build $headlessBuild --parallel 6
ctest --test-dir $headlessBuild -C Release --parallel 4 --output-on-failure --no-tests=error --output-log "$headlessBuild/ctest.log"

$installPrefix = Join-Path (Get-Location) 'out/install/s55-gui'
cmake --install $guiBuild --config Release --prefix $installPrefix --component VisionProGui
& (Join-Path $installPrefix 'bin/visionpro_gui.exe')
```

1. 打开 `samples/s4/solution.json`，选择 **旋转 ROI 继承** 流程，从 **Run → Run Workflow** 运行。
2. 在 **Run and Results** 选中 **测量连通域**，点 **Show Image**。图像面板显示该调用实际的二值输入、Blob 包围框与质心，以及运行和快照编号。
3. 选中旋转阈值节点的 `binary` 输出查看轮廓及成员掩膜。选中判断节点可分别查看技术状态和判定。

在画布上选中普通工具节点后，通过 **Run → Debug Selected Node…** 提供本次输入。桌面输入表单支持 Bool、Int64、Float64、String 和 OpenCV 构建中的 UInt8 文件图像。仍需满足选中工具的参数、资源和输入约束。
**Run → History and Notifications…** 可配置历史轮数及观察容量。在历史下拉框中选择具体运行会固定该轮，选择 **Latest run** 恢复随最新轮更新。
定向复测使用 `-R '^visionpro_(editor_run|run_lifecycle|visual)_tests$'`。

### 3.29 S5.6 Release 构建与验收

按 [§3.28](#328-s55-release-构建与测试) 的两套 Ninja Release 命令配置和构建，将 `$guiBuild`、`$headlessBuild` 和 `$installPrefix` 分别设为 `out/build/s56-gui`、`out/build/s56-headless` 和 `out/install/s56-gui`。
GUI 配置同时启用 OpenCV。独立 CLI 配置关闭 GUI 和 OpenCV。两套均启用 `BUILD_TESTING` 并运行全量 CTest。

从仓库根目录复测对照、生命周期和图像用例。CTest 使用 offscreen 平台。Windows 平台复测用以下命令生成独立日志。

```powershell
$guiBuild = (Resolve-Path -LiteralPath 'out/build/s56-gui').Path
ctest --test-dir $guiBuild -C Release --output-on-failure -R '^visionpro_(acceptance_parity|run_lifecycle|visual)_tests$'
if ($LASTEXITCODE -ne 0) { throw 'Desktop regression failed.' }

$openCvBin = (Resolve-Path -LiteralPath (Join-Path $env:OpenCV_DIR '../bin')).Path
$savedPath = $env:Path
$savedPlatformPath = $env:QT_QPA_PLATFORM_PLUGIN_PATH
$env:Path = "$qtPrefix/bin;$openCvBin;$env:Path"
$env:QT_QPA_PLATFORM_PLUGIN_PATH = "$qtPrefix/plugins/platforms"
try {
    foreach ($suite in 'acceptance_parity', 'run_lifecycle', 'visual') {
        $exe = Join-Path $guiBuild "tests/ui/visionpro_${suite}_tests.exe"
        $log = Join-Path $guiBuild "native-$suite.txt"
        $test = Start-Process -FilePath $exe -ArgumentList '-platform', 'windows', '-o', "`"$log,txt`"" -Wait -PassThru
        if ($test.ExitCode -ne 0) { throw "Native Qt test failed ($suite). See $log" }
    }
} finally {
    $env:Path = $savedPath
    $env:QT_QPA_PLATFORM_PLUGIN_PATH = $savedPlatformPath
}
```

双入口用例会创建临时方案，调用独立 CLI 并在进程退出后读取完整报告。CTest 的 Qt 日志在 `tests/ui/acceptance-parity.log`。
完整构建和回归结束后单独运行负载测量。[负载命令与门限](s5-acceptance.md#负载测量)

## 4. 第一阶段验证

2026-10-27 已按以下检查项完成文档审查，结果见 [§8.1](#81-s1-文档审查记录) 和 [§8.2](#82-s1-场景定义逐项审查)。

以设计审查为主：

- 检查术语和对象职责是否一致。
- 检查字段、标识与引用规则。
- 检查所有权、可变性和生命周期。
- 检查配置与运行状态是否分离。
- 检查 Node 配置、ToolInstance、InvocationOperation、NodeResult 与 NodeState 的边界。
- 检查实例寿命独立于状态寿命，Init、重置和配置兼容代次不会破坏在途状态。
- 检查创建、配置快照、释放和失败路径，避免实例依赖可变编辑配置。
- 检查应用、核心和能力三层的依赖方向，以及无需 GUI 初始化的装配入口。
- 检查控制连线不隐含数据传递，输入和参数绑定不自动执行节点。
- 检查多前驱/多后继、AllActivated、未选关闭和至少一次有效到达条件；失败不能冒充未选。
- 检查 ROI 浮点形状、局部方向、继承/修正/排除及坐标变换，不以整数包围框代替目标。
- 检查外部/托管/序列/设备资源和编辑/执行/无损保存的分层。
- 检查 RunRequest 一次性输入、公开端口、Group/布局分组/循环的区别，以及独立调用的宿主边界。
- 检查输出级有效性、借用结果物化、事件通道与注销静默；保留 SDK 未明确部分作为适配验证项。
- 检查结果引用的字段选择、缺失处理和调用作用域。
- 检查逻辑调用与重试尝试身份一致；发布资格校验、终态接受及控制推进不能互相脱节。
- 检查数据输入、资源租约和触发的区别；逻辑取消后仍在执行的对象不能提前销毁。
- 区分共享当前值、FIFO 消息、工具状态与本轮输出。
- 用工件错配、部分数据齐备等反例检查跨流程契约。
- 使用 [data-model.md](data-model.md) 中的示例与反例检查表达能力。
- 记录未决问题及对应结论。

外部设计稿只作为待评估材料，结论落入现有职责文档。
ANS1.md 的采纳记录见 [architecture.md](architecture.md) §11；
其中的“建议冻结”和示例接口不视为实现。
已核对的手册来源集中记录于 requirements §2；不可由手册功能推断线程或内部存储实现。

进入实现工作后，如某项 C++ 表达方式存在不确定性，可用小型编译实验验证。
实验结果不能替代整个模型的审查。

## 5. 第二阶段实施与验证

S2 将设计基线落实为能够无 GUI 构建和测试的 core/workflow 库：用代码构造方案，
检查模型和绑定，再从人工结果中解析当前调用所需的数据。
阶段范围与通过标准以 [requirements.md §4](requirements.md#4-第二阶段核心模型与绑定验证) 为准；
与调度、算法及数据服务的分工见 [architecture.md §2.1](architecture.md#21-s2-在整体架构中的位置)。
以下计划保持原范围；S2.1–S2.5 已通过，证据见 §8.4–§8.8。
S2.6 已完成并通过 Release 验证，结果见 §8.9；S2.7 及 S2 整体验收已通过，记录见 §8.10。

### 5.1 子阶段与依赖顺序

| 子阶段 | 主要交付 | 退出条件 |
|---|---|---|
| S2.1 工程底座与首个领域类型 | 实际 visionpro_core 目标、CTest 入口、最小强类型身份和诊断；确定首个验证环境 | 无 GUI 配置、Release 编译与首批真实行为测试通过，命令及环境可复现 |
| S2.2 基础类型、值系统与工具元数据 | 补齐持久/Schema/运行身份、错误返回、ConfigValue/DataValue、Record/List/Optional，以及参数、端口和 ToolDescriptor | 身份类别不混用；空集合、None、缺失和错误可区分；Schema、必填性、默认值、范围/枚举及转换约束可校验 |
| S2.3 图像、几何、资源与状态模型 | 图像只读所有权、裁剪视图、坐标和 ROI/掩膜；资源、全局变量、队列及节点状态的定义和校验 | 共享输入与独立输出不互相污染，视图保留父缓冲；区域、资源和状态域可表达并校验，运行对象不混入配置 |
| S2.4 方案、关系、快照与请求模型 | Solution/Workflow/Node、控制/包含关系、Binding/OutputReference、Group 公开映射、不可变快照、类型化请求与结果身份；复制重映射 | 五路分叉、多前驱汇合和公开边界可构造；改名保持引用，复制重映射；配置、快照、请求输入和结果分别持有 |
| S2.5 Schema、图与绑定静态校验 | 身份、引用、端口/字段、类型、选择器、转换、作用域校验及派生依赖索引 | 合法兄弟分路依赖通过；非法回边、直接/间接逆序和控制/数据联合依赖环被拒绝；诊断定位到具体节点及绑定 |
| S2.6 结果访问与绑定解析 | 会话内最小结果访问结构、BindingResolver；常量、公开输入、本轮结果、字段、固定索引、声明的转换及必要资源引用 | 不同运行/作用域不串值；缺本轮结果不读历史；Pending、无值、来源失败和越界分别返回；取值没有执行、消费或写入副作用 |
| S2.7 运行契约收敛与整体验收 | RunRequest/RunHandle、快照、实例工厂、InvocationOperation 的具体接口；组合验证和验收记录、S3 交接清单 | 接口可编译且所有权/错误边界可验证；全部 S2 必需范围通过，记录与实际能力一致 |

先完成 S2.1–S2.2；基础类型接口稳定后，S2.3 的几何/资源模型与 S2.4 的图结构可以分开推进。
相关模型须在对应 Validator/Resolver 完成前接入，S2.3 是必需项，不能绕过后直接验收。
随后依次完成 S2.5、S2.6 和 S2.7。
运行接口从 S2.4 开始形成草案，随绑定校验和解析调整，在 S2.7 收敛，不能等到 S3 才决定输入语义。

每个子任务随实现补充测试，以独立可编译、可测试、可审阅的单元提交；
不强求一个子阶段恰好对应一个 PR，也不将所有测试推迟到 S2.7。
公开头文件放在 include/visionpro/core 或 include/visionpro/workflow，内部头文件靠近 source 中的实现；
测试与固定夹具分别按 tests/core、tests/workflow、tests/fixtures 组织，目录随实际内容建立。

### 5.2 首批实施范围

第一批完成 S2.1，并建立 S2.2 所需的身份与诊断基础：

1. 验证 §2 的 Windows x64/MSVC/CMake 组合，确认实际可用的生成器与编译环境；依赖和运行库配置保持一致。
2. 建立 include/visionpro/core、source/core、tests/core 的最小实际结构，创建 visionpro_core 并复用 visionpro_build_options。workflow 目标随其首批实现加入，不先建立空壳库。
3. 至少实现 SolutionId、WorkflowId、NodeId 的强类型区分、UUIDv4 生成/解析及规范文本表示；Schema 标识和运行计数身份随后按各自契约补齐，不统一套用 UUID。
4. 实现最小 Diagnostic/ErrorCode 和错误返回约定，包含稳定错误分类、严重程度、对象位置与说明，不扩展异常或日志框架。
5. 用 CTest 注册真实行为测试，覆盖合法/非法身份、文本往返及类型隔离；在独立构建目录验证 Release，并确认失败能够使测试返回失败。
6. 将实际成功的版本、构建/测试命令及结果记录到本文。未经执行的命令不能作为已验证步骤；节点改名与复制的测试在 S2.4 随配置模型加入。

首批不创建调度器、GUI、设备适配或完整算法接口。其交付是可构建的领域代码与测试，
不是整个 S2 的完成记录。

### 5.3 结果访问与绑定验证方法

使用固定内存方案、人工 NodeResult 和最小调用上下文，复用 [data-model.md](data-model.md) §5 的
“A 图像来源 → B 定位 → C 测量 → D 判断”关系示例。
控制图只描述顺序，C 的图像仍绑定 A，位置变换绑定 B，D 的数值绑定 C.radius。
测试先校验模型，再人工填入某次运行的图像、变换和半径，调用 Resolver 检查值及实际来源；
不为制造这些结果提前实现算法或节点调度。

正常与反例须成对验证：本轮 C.radius 为 12.4 时 D 取得该值；新一轮 C 仍 Pending 时返回未就绪，
C 已失败时返回来源错误，均不能读取旧轮的 12.4。空 List 是有效值，但访问其 [0] 应报越界；
Optional 允许的 None 不得吞掉类型错误或来源失败。
静态图用例既包含合法兄弟分路依赖，也包含直接逆序、经多个节点形成的控制/数据联合环，
以及非法普通回边，不能只验证相邻节点和单链。

最小结果访问结构按会话隔离，使用 InvocationKey 区分运行、节点、作用域和逻辑调用。
如果提供人工结果插入接口，须有明确的重复键处理，不能静默覆盖已有结果；
此项单线程测试不等同于结果提交与取消竞争的运行验证。
必要资源引用使用内存目录或测试替身验证身份、类别、存在性和受控返回类型，
不要求真实文件读取或设备连接。Resolver 不阻塞等待生产者，不触发来源或隐式消费队列。

配置与运行数据的隔离也通过代码验证：不同请求持有各自输入，修改编辑配置不改变已生成快照，
共享图像只读，独立修改输出不影响另一读取者，视图在有效持有期间保持父缓冲存活。
RunHandle、实例工厂和操作句柄使用具体可编译接口及必要测试替身验证所有权和错误返回；
实际 Busy 受理、执行/取消、回调、状态 Init 和资源收尾继续归 S3。

### 5.4 验收覆盖与退出条件

下表将 [requirements.md §9](requirements.md#9-验收场景与阶段矩阵c10) 中带 S2 范围的 14 个场景
映射到实施子阶段，只列本阶段的验证内容。表中是完整测试计划，已通过的子范围以 §8 的验收记录为准；
完整场景及后续执行范围仍以 requirements 的矩阵为准。

| 场景 | 主要测试落点 | S2 必须验证的内容 |
|---|---|---|
| A01 | S2.3、S2.4 | 同类型节点配置独立；实例、状态、状态域与代次分别建模，配置不包含可变运行状态 |
| A03 | S2.1、S2.4、S2.5 | 强类型稳定身份、改名保持引用、复制新身份并重映射；重复身份和删除后悬空引用可定位 |
| A04 | S2.2、S2.4、S2.6 | 会话/运行/作用域/调用/尝试及帧/工件身份分开；只读取当前运行中合法可见作用域的结果，允许声明可见的外层结果，不串用其他迭代结果 |
| A05 | S2.2、S2.6 | Record/List/Optional 类型保持；空集合、None、零、未产生和失败可区分；非法字段或任意载荷拒绝 |
| A06 | S2.2、S2.5、S2.6 | 非相邻引用、字段和固定索引；参数/端口允许来源、必填性、默认值及声明的转换可校验；Int64 到 Float64 不默认无损 |
| A07 | S2.5、S2.6 | 普通越界、类型错误和来源失败分别诊断，默认值或 Optional 不吞错；动态索引和格式化容错的执行留待 S6 |
| A08 | S2.3、S2.6 | 共享值、消息及其定义分离；普通取值和预览没有写入或消费副作用，真实全局/队列服务留待 S6 |
| A09 | S2.3、S2.6 | 图像只读共享、独立输出和裁剪视图所有权，运行缓冲不得进入配置；取消中的操作寿命由 S3 验证 |
| A10 | S2.3、S2.5 | ROI 多形状、继承/修正/排除、坐标/单位/方向及边界策略可表达、可校验，实际算法适配留待 S4/S6 |
| A13 | S2.4、S2.5 | 合法兄弟分路依赖通过，显式逆序及控制/数据联合环拒绝；实际调度顺序由 S3 验证 |
| A23 | S2.4、S2.6 | 类型化 RunRequest 的输入必需性、类型和只读持有；单次输入与配置分离，不从旧请求隐式补值；Busy/原子受理由 S3 验证 |
| A24 | S2.4、S2.5 | Workflow/Group 公开端口、稳定映射、层级和作用域可表达与校验，不穿透公开边界读取内部结果 |
| A26 | S2.3、S2.5 | 掩膜所有权、格式/尺寸/坐标及几何组合能力可表达、可校验；像素成员判定和实际转换留待 S4 |
| A32 | S2.2、S2.5 | Schema 明确字段、类型、编码和版本/能力约束；不允许以枚举不完整为由静默删字段，厂商转换与运行库验证留待后续 |

阶段正文另外要求五路分叉、多前驱汇合、基本条件出口及非法回边的模型校验，在 S2.4–S2.5 覆盖；
不能因为 A11/A12 的执行验证归 S3 就省略这些结构。资源引用的成功与失败解析在 S2.6 覆盖，
运行接口的可编译性和所有权测试在 S2.7 汇总，不以场景编号未单列为由遗漏。

S2 结束前须同时满足：

- 上述范围及 requirements §4 的必需项全部通过。未覆盖或失败的项记录原因并保持未完成，不通过删减场景或放宽标准收尾。
- 首个验证环境中，core/workflow 及测试可独立于 GUI 构建，Release 的真实执行结果可复现。
- 固定内存方案贯通“构造模型 → 静态校验 → 人工结果查询与绑定解析 → 值或诊断”；不把人工夹具写成已完成节点执行或真实检测。
- 公共契约与实现一致，证据按 §8 记录代码版本、夹具、测试入口、期望/实际结果和命令；可编译接口不能代替所有权或绑定行为验证。
- 向 S3 交接可复用模型、校验/解析接口、夹具和已明确的后续事项。S3 的调度、提交/取消、回调及配置往返保持未验证状态，不能随 S2 一并标记完成。

## 6. 第三阶段实施与验证

S3 交付可通过无 GUI 入口加载、执行、追溯并保存方案的运行服务，
范围以 [requirements.md §5](requirements.md#5-第三阶段无-gui-执行与配置往返验证) 为准。
先以内存配置调用模拟工具，证明绑定、调度、结果和生命周期一致，再加入配置往返。
以下为 2026-11-11 确定的实施计划。S3.1–S3.9 的验证记录见 §8.12–§8.20。

采用单工作线程就绪调度、每流程 maxInFlight=1、重复启动返回 Busy、默认 FailFast，
无自动重试或隐式排队；检测 NG 不等于技术失败，槽位保留至实际退出和清理完成。
不同流程的准入槽位独立，不因单工作线程引入全方案 Busy 锁；完整并行与业务汇合仍归 S6。
S4 接真实算法，S5 接 GUI；完整 Group/循环、连续与外部触发、共享写入/队列服务、
动态索引、恢复/重试、真实设备及 SDK 不因本次计划提前进入 S3。

### 6.1 当前资产与必须接通的边界

| 可复用资产 | S3 接入任务 |
|---|---|
| ConfigSnapshot、RunRequest、ExecutionPlan、ValidationIndex、BindingResolver | S3.1 实现方案级不变量、编辑诊断及范围规划。S3.2 的同步运行服务复用计划和绑定解析，使用请求固定的输入及只读全局值 |
| RunHandle、RunControl、RunSubmitter | S3.2 接通生产准入和同步运行控制。S3.3 按完整身份处理异步唤醒，逻辑终态后继续跟踪实际退出 |
| ToolRegistry、PreparedTool 与 InvocationOperation | S3.3 接入工厂、异步通知和注入式资源租约。启动和取消回调退出后才清理操作持有对象 |
| NodeTarget 与固定输入 | S3.1 按目标 Schema 从请求准备独立输入，无法满足的上游订阅报诊断。S3.5 已接通目标执行、独立结果和调试状态隔离 |
| NodeStateDefinition、状态域和代次 | S3.1 从类型化固定参数生成有效状态规格。S3.5 已接通类型化访问、Init、重置、代次和保活 |
| NodeResult、ResultStore 与输出校验 | S3.3 在同一运行控制锁内接受取消或结果并更新后继许可。S3.6 接通会话历史查询和结果持有 |
| 控制边、就绪节点与派生数据依赖 | S3.4 接通 AllActivated、命名出口选择和关闭传播。待数据节点在来源结果提交或确定缺失时重新进入就绪队列 |
| Diagnostic、elapsed 和 timings | S3.1 验证类型化外部错误域/原码和分范围耗时表示。S3.6 验证模拟适配及交付 |
| WorkflowSession 与 RunObservation | S3.6 接通内存候选校验、会话切换、排空关闭及独立观察队列 |
| Solution、资源/变量/队列定义及布局 | S3.7 接通版本化清单、诊断加载、显式路径和固定文件资源锁定。S3.8 接通覆盖保存、修订冲突保护及退役托管文件清理 |

具体契约由 [data-model.md](data-model.md) §2.1、§5.5、§8.1.1、§9、§10、§13 维护。
S2 的历史验收范围见 §8.10。

### 6.2 子阶段与依赖顺序

| 子阶段 | 主要交付 | 退出条件 |
|---|---|---|
| S3.1 执行范围、计划与工具装配 | 静态工具注册和工厂装配；从快照和派生依赖生成 ExecutionPlan；全局不变量/编辑诊断/执行范围分层；明确状态投影、输出可用性、诊断与耗时的接口扩展；可控同步/异步工具夹具 | 范围外前驱、控制/数据冲突、缺工具及不支持能力在副作用前拒绝；无关流程局部问题不阻断合法目标；注册工具无需引擎识别具体算法；后续接口有可审查的类型和测试入口 |
| S3.2 最小内存执行闭环 | 生产 RunSubmitter/RunControl、按流程原子准入、运行/作用域/调用身份；顺序调用、固定输入、逐输出校验、结果提交和流程终态 | 内存方案真实调用模拟工具；Busy 不污染输入，编辑不改本轮快照；NG 继续，技术失败停止后继且不读历史；正常无匹配、允许的 NotProduced 与 Invalid 分别处理；不同流程无全局 Busy 互斥 |
| S3.3 异步、取消与实际退出 | 操作到引擎的事件唤醒；同一仲裁点处理提交资格、终态及控制事件；重复/迟到拦截；输入、实例、状态/资源访问保活和槽位释放 | 确定性覆盖取消先/后于提交、重复完成和旧身份通知；结果及控制最多接受一次；后继启动前再查取消；完成未退出仍 Busy；等待超时和客户端丢弃句柄不提前释放 |
| S3.4 分叉、汇合与基本分支 | 逐边 Pending/Arrived/Closed、显式入口许可、就绪队列、AllActivated、零/多选与嵌套关闭；绑定四态驱动数据就绪 | 五路各执行，逻辑及模拟发送各一次；乱序/重复不多调用；未选/零选/嵌套不永久等待；失败不冒充正常关闭；合法兄弟依赖只细化顺序，不补跑来源 |
| S3.5 状态服务与单节点调用 | 类型化状态访问和状态仓库；节点参数投影有效初值/策略，Init/重置/兼容代次和修改时机；单节点专用准备路径与默认隔离调试 | 同类型两节点不同初值，一个每轮重置、一个跨轮累计；实例重建不丢会话状态；旧代次保留至退出，改名/布局不重置；目标单独调用，缺输入失败，不启动外部后继或污染活动状态 |
| S3.6 结果交付、通知与会话生命周期 | 结果物化/查询/保留；必要完成与可选观察通道分离、有界容量及注销；内存候选会话切换和关闭；模拟外部错误域/原码及分范围耗时 | 后端下轮覆盖不改已持有快照；历史淘汰不使活动图像悬空；慢观察者不阻塞完成；关闭显示不丢必要通知；旧会话不错误投递，关闭拒绝新请求；0/1 各状态域、未知原码/耗时可追溯 |
| S3.7 配置格式、资源解析与诊断加载 | infrastructure 编解码与资源解析；目录/JSON 版本化格式、显式值编码和路径基准；解析/编辑诊断/执行检查分层；候选加载和未知内容保存限制 | 稳定 ID、三类关系、节点状态选择、资源/变量/队列定义与布局往返；Int64 边界、空 List、None、Binary 和中文路径通过；缺工具/资源可诊断编辑但不能误执行；损坏不替换有效配置，未知内容不被有损覆盖 |
| S3.8 保存事务与故障保护 | 不可变托管资源新版本、临时清单、原子提交；修订比较和提交处于同一受控协议；提交与清理结果分开；保护仍被运行持有的旧资源 | 资源写、临时清单、提交与清理逐点故障注入；提交前旧方案有效，提交后清理失败不误报未保存；两个协作保存者交错不丢更新；外部资源不被改写或自动托管 |
| S3.9 无 GUI 入口与整体验收 | app 装配，可指定方案文件、流程及输入的命令行入口和追溯摘要；加载/执行/保存/重载集成夹具；验收矩阵、复现命令及实际记录 | 无窗口初始化完成文件方案执行；往返前后业务结果、来源和控制行为一致；§6.5 全部 S3 子范围与 S2 回归通过，Release 证据可复现 |

运行主线按 S3.1 → S3.2 → S3.3 → S3.4 推进，再完成 S3.5 和 S3.6。
格式/兼容性设计可在内存执行期间准备，S3.7 的完整读写在内存主线通过后接入，
并依赖 S3.5 的状态配置和 S3.6 的会话边界完成验收；随后 S3.8，最终由 S3.9 汇总。
每个子阶段随实现补充并运行受影响测试，不把验证集中推迟到最后；一个子阶段可拆为多个可审阅改动。

core 提供类型契约及静态注册接口；workflow 承担计划、调度、状态及结果服务；tools 提供具体工具及注册项；
infrastructure 承担 JSON、文件、资源及存储后端；app 承担装配和无 GUI 命令。
固定测试数据与故障夹具放入 tests 下相应目录，构建目标随实际实现建立，不预先创建空模块。

### 6.3 首批任务与技术决策

S3.1 按以下顺序落实接口与夹具。实际测试和后续接入记录见 §8.12。

1. 保留全方案严格校验入口，复用内部检查建立编辑诊断和所选范围规划入口；整体检查身份唯一性，执行范围保留依赖闭包、入口与公开边界约束。
2. 明确状态能力与节点选择的投影：由已校验类型化节点参数生成有效状态规格，配置选择可保存；运行累计值、锁和代次访问句柄不进入普通方案。
3. 补输出必需性和允许未产生原因、完成物化与投影。原生无匹配示例用成功的 Optional None；另外覆盖合法 NotProduced，非必需输出 Invalid 仍默认整次失败，不能只测试全部必需输出。
4. 明确外部诊断来源/原码及分范围耗时记录，保留已有错误分类和稳定码；未知耗时不记为零，不把 SDK 不同状态域统一按非零错误映射。
5. 装配静态注册表、执行计划和小型模拟工具；确定独立 NodeTarget 输入准备及异步通知接入接口。当前操作实现继续复用，状态服务和生产调度由后续子阶段接通。
6. 建立对应 CTest 入口及固定夹具，验证非法计划在副作用前拒绝，并回归既有校验/绑定/运行契约；同步更新实际接口说明，不将夹具通过写成 S3 整体完成。

S3.7 的 JSON 依赖与获取方式见 §2.2，清单字段和 Binary 编码见 data-model §10.2.1。
Int64 使用带类型的十进制字符串。含未知字段的清单保留已知视图，写回入口返回兼容性诊断。
S3.8 首个后端以 Windows 本地文件系统验证提交保证，不提前承诺网络共享或断电恢复；
对非协作第三方写入说明实际检测边界，不能将“先检查修订再无条件替换”当作防冲突协议。
这些技术决策不引入 Qt、OpenCV、厂商运行时或通用迁移框架作为 S3 的前提。

### 6.4 验证方法与重点反例

使用固定输入，成功路径与失败路径均需有可复现的结果。

重点检查：

- 静态注册表按工具类型创建实例，不修改引擎来识别具体算法。
- 同类型节点的配置、可变实例及持续状态互不污染。
- 运行使用配置快照，编辑配置不会改变本次调用。
- 创建失败、调用失败和释放资源均不破坏节点配置。
- 控制顺序与数据依赖冲突时明确报错，不自动重排或补跑来源。
- 顺序、分叉、汇合、基本分支及关闭传播符合契约；无条件五分路不会被当成互斥分支。
- 失败、取消、重复启动和本轮结果隔离符合契约。
- 实例重建不丢会话状态，首次初始化与显式重置互不混淆；旧快照状态不被编辑清空。
- 同步返回与异步操作均拥有所需输入/资源，实际退出前不销毁。
- 单次请求输入与快照一起受理，拒绝/Busy 无输入串扰；调试调用不补跑来源、不污染活动状态。
- 外部结果一次物化后逐输出验证，后续调用不会改写已持有快照。
- 关闭可选显示通知不阻止内部完成；模拟订阅注销、旧后端代次和关闭期间的新请求。
- 方案保存并加载后恢复稳定标识、绑定和相同输入下的行为。
- 损坏/不可解码输入不替换有效模型；已知格式缺资源/工具可带诊断编辑，执行校验明确拒绝。
- 未知字段/节点不能无损保留时禁止覆盖保存；外部资源引用不被强制转成托管资源。
- 保存故障不破坏旧清单/资源；提交后清理失败与保存失败分开，外部修改冲突不被覆盖。
- 交错两个遵守协议的保存者，确认修订检查与提交间无丢失更新；非协作修改按后端实际能力记录。
- 全局变量与队列定义可保存加载，当前值、积压消息和工具累积状态默认不持久化。
- 无 GUI 入口可以加载并执行同一方案，初始化不需要窗口或控件。
- 调用记录可追溯配置快照、运行身份、实际数据来源、执行顺序及结果诊断。

首版重点反例及可观测通过条件：

| 场景 | 通过条件 |
|---|---|
| 上轮成功，本轮图像来源失败（默认 FailFast） | 本轮下游不调用，不取得上轮图像；流程失败且可定位来源 |
| B 控制先于 A，但 B 读取 A 的本轮输出 | 执行前拒绝，即使 B 的目标为 Optional；诊断包含绑定位置，A/B 均未产生副作用 |
| 来源返回空集合，下游读取 [0] | 报索引错误，不能因目标可选而返回 None 或零 |
| 模拟工具忽略取消，稍后返回 | 不发布迟到输出，不触发模拟发送；实际返回前保留工具及其资源 |
| 同一次调用重复报告完成 | 结果与后继控制只接受一次，模拟发送计数不增加 |
| 同一流程运行中或仍在收尾时再次启动（S3 准入策略） | 返回 Busy，旧运行上下文、实例和独占资源未被替换或复用；不推导出方案全局互斥 |
| 同类型两个节点配置不同，期间编辑方案 | 本轮各自使用原快照且互不污染；后续运行使用重新校验的配置 |
| 五路测量/条件检测汇合后发送 | 五路均执行，逻辑及模拟发送各一次；改变完成次序和重复到达不增加次数 |
| 部分分路未选、全部未选或嵌套分支 | 传播关闭，AllActivated 不等不存在的到达；无有效到达不伪造调用 |
| 选中分路失败，其他分路成功 | 按 FailFast 结束等待；失败不得伪装成正常未选，不能调用成功发送 |
| 兄弟分路 B 读取已激活的 A | 在没有相反显式控制时 A 完成后调用 B，不重写控制线或补跑来源 |
| 每轮实例重新创建，会话累加器继续运行 | 计数跨轮保留；显式重置恢复初值，另一节点计数不变 |
| 同一工具类型的两个节点选不同初值和初始化方式 | 一个每轮重置、另一个跨轮累计；编辑只影响新快照，保存加载保持选择，不用不同 ToolType 替代节点配置 |
| 同步完成与异步回调的生命周期 | 输入/图像/租约存活到实际退出，完成和清理各被追踪 |
| 已知格式缺工具、缺资源或包含暂不支持循环 | 可保留配置诊断编辑；所选执行范围明确报错，不删内容后执行 |
| 资源写入、清单提交或后续清理失败 | 提交前旧方案有效；提交后清理失败单独报告，不误报未保存 |
| 显式入口无前驱，或执行范围外存在连入边 | 入口接受一次启动许可；不完整执行范围在规划时诊断，不永久等待 |
| 两个保存者在修订检查和提交间交错 | 条件提交/互斥协议保留已提交版本，冲突方不无条件覆盖 |
| 两个请求先后注入不同图像，第二个被 Busy 拒绝 | 第一个运行输入不变，下一次无输入不会拾取被拒绝或旧请求的值 |
| 外部结果对象在下一次执行被覆盖 | 已发布且仍持有的上次结果保持原值；UI 不直接读外部“当前结果” |
| 输出失败但数量为零，或正计数却为空指针 | 逐输出诊断，不能变成合法空集合；非必需输出 Invalid 同样默认整次失败，不提交算法输出 |
| 无匹配、合法未产生和失效输出 | 无匹配示例成功输出 Optional None；另按元数据接受合法 NotProduced；未声明原因、缺必需输出或任何 Invalid 均拒绝，不误取零或历史值 |
| 单节点原绑定指向上游，但请求显式提供本次输入 | 专用准备路径只使用目标调用的合法输入；无上游执行、无外部后继；订阅参数不足明确失败，不伪造上游结果 |
| 不同成功码约定、未知错误和未测得耗时 | 调用错误、输出有效性与业务判定分别映射；原始域/码可查询；模块与算法耗时不重复相加，未知耗时与真实零值分开 |
| 关闭显示订阅，必要完成回调仍在进行 | 控制与结果正常完成，观察者不再访问；必要完成路径保留到操作实际退出 |
| 关闭过程中注销回调并切换会话 | 已进入回调先安全退出，旧代次不映射新对象，用户对象最后释放 |
| 加载结束通知携带失败或运行等待超时 | 失败不切换有效配置；超时不假装已退出，也不释放仍占用资源 |

用可控制完成时机的假工具验证取消/提交竞争，两种先后顺序都要覆盖，
不要依赖偶然线程时序或长时间 sleep。模拟发送只记录调用，不连接真实服务。
模拟外部结果覆盖、输入消费、回调注册/注销和错误域使用可控制的假后端，
既验证正常交付，也验证等待中的取消、旧句柄代次、通道超容与实际退出先后关系。
空指针/错误长度使用安全夹具，不以真实悬空指针解引用测试错误处理。
尚未引入超时和重试时，不为通过测试而提前建立相应通用框架。

配置往返比较不要求运行标识、时间戳与耗时相同。
核心验收通过后再加入真实算法，不以模拟工具的通过代替算法验证。

### 6.5 验收覆盖与退出条件

下表覆盖 [requirements.md §9](requirements.md#9-验收场景与阶段矩阵c10) 中全部包含 S3 的 26 项场景。
仅验收各场景的 S3 子范围；A14 等纯后续场景不提前计入。表中落点为计划，不是现有测试名称或通过证据。
实测结果及对应测试见 [S3 整体验收矩阵](s3-acceptance.md#场景证据)。

| 场景 | 主要落点 | S3 必须验证的内容 |
|---|---|---|
| A01 | S3.5、S3.7 | 同类型节点不同初值/重置方式，配置与状态隔离；实例重建保留会话累计，往返保持节点选择 |
| A02 | S3.5 | Init 按状态域一次，显式重置、配置兼容代次与在途旧状态协调；改名/布局不重置 |
| A03 | S3.1、S3.7 | 身份、复制重映射和悬空引用回归；保存加载不改变稳定身份 |
| A04 | S3.2、S3.3 | 运行/作用域/调用/尝试及帧/关联身份正确追溯，不依赖最新值或时间戳；循环及真实重试留 S6 |
| A05 | S3.1、S3.2、S3.6 | 零、空 List、Optional None、允许的 NotProduced 与 Invalid 区分；非法类型或无声明缺失拒绝 |
| A06 | S3.1、S3.2、S3.4 | 非相邻来源、字段/固定索引和显式转换实际进入调用，错误定位；动态索引留 S6 |
| A07 | S3.2、S3.4 | 普通索引越界失败，来源失败不被 Optional/默认值吞掉；格式化特殊容错留 S6 |
| A08 | S3.2、S3.7 | 只读全局快照带版本，普通解析/预览不写全局或消费队列；只保存定义/初值，服务留 S6 |
| A09 | S3.3、S3.4、S3.6 | 分路共享只读输入，独立模拟输出不污染另一分路；裁剪视图和取消中的调用保活，真实算法留 S4 |
| A11 | S3.4 | 五路测量/条件均执行，逻辑和模拟发送各一次，乱序/重复到达不增调用次数 |
| A12 | S3.4 | 按位多选、部分/零选和嵌套关闭无永久等待；零到达不调用，选中失败不冒充正常缺项 |
| A13 | S3.1、S3.4 | 合法兄弟依赖细化就绪顺序；逆序和联合等待环在副作用前拒绝，不自动补跑 |
| A16 | S3.3 | 同步/模拟异步同一结果规则；迟到不发布，实际退出前输入/实例/租约仍有效 |
| A17 | S3.2、S3.4 | NG 继续，技术失败无普通算法输出，默认 FailFast 明确终态并解除等待；恢复留 S6 |
| A18 | S3.3 | 取消先/后于提交、重复完成、旧 Attempt/代次通知只接受一次；不假定副作用回滚，不提前实现重试 |
| A19 | S3.7、S3.8 | 托管/外部资源往返、缺工具/资源诊断和执行拒绝、未知内容无损限制；用小型文件夹具验证存储 |
| A20 | S3.8 | 保存各步骤故障不破坏旧有效方案；提交/清理结果分开；交错协作保存者无丢失更新，外部修改边界明确 |
| A22 | S3.6、S3.9 | 历史释放不使活动结果/图像悬空，保留范围明确；设计、构建、模拟与硬件证据分开 |
| A23 | S3.2、S3.3 | 两请求输入隔离，Busy 不保留隐式待执行输入；下一轮缺输入拒绝，原输入持有到退出；真实重试留 S6 |
| A25 | S3.2、S3.6 | 后端下一轮覆盖不改已持有快照；有效空值、正常未产生、Invalid、坏长度/空指针分别处理，含非必需输出失效 |
| A27 | S3.3、S3.6 | 重复/迟到/注销交错安全；必要完成与观察通知分开，慢观察者和通道超容不丢唯一终态 |
| A28 | S3.6、S3.7 | 候选加载失败不切换有效配置；模拟管理操作与后端安全边界协调；关闭拒绝新请求并等待回调/调用实际退出 |
| A29 | S3.2、S3.3 | 单次同步等待与异步受理结果一致，等待超时不伪装成清理完成；连续/多流程服务留 S6 |
| A30 | S3.5 | 单节点显式输入独立准备，不要求原流程先执行；不补跑来源、不激活外部后继，缺输入/参数失败，隔离调试不污染活动状态 |
| A31 | S3.1、S3.6 | 分别映射调用/输出/业务状态的 0/1 约定；未知域/原码保留；模块/算法耗时及来源明确，未知值不写为零 |
| A32 | S3.1、S3.6、S3.7 | 模拟名称/字符串/Binary 按 Schema 转换，UTF-8 路径往返；不完整枚举和版本/能力不匹配明确诊断，不静默删除配置 |

S3 结束前须同时满足：

- 九个子阶段的退出条件和上述 26 项 S3 子范围全部通过；未覆盖或失败的项保持未完成，不用人工结果插入代替调度、提交或竞争测试。
- 无 GUI 入口接受磁盘方案及指定流程/输入，完成加载、执行、保存和重载；固定五路及条件分支方案有业务结果、来源、控制顺序和诊断证据，不只运行硬编码内存夹具。
- 核心/运行/存储测试独立于 Qt、OpenCV 和厂商运行时，在既有 Windows x64 工具链验证 Release；保留 S2 回归，新接口变化应更新对应行为验证，不删减标准。
- 验收记录保存实际代码版本、工具链、夹具、CTest 名称、复现命令和结果；S3 新命令随实际入口完成后记录，现阶段不提供未经运行的“已验证命令”。
- 文档与实现能力一致，未实现的合法结构有明确诊断和配置保留策略；为 S4 交接可复用运行/存储服务，不将模拟通过记为真实算法、SDK 或设备通过。

## 7. 第四阶段实施与验收计划

本计划制定于 2026-11-15，以提交 `cee6190` 的实际代码为起点。制定时复核 S3 Release 构建，CTest 82/82 通过。
S4.1–S4.7 的完成记录见 [§8.21](#821-s41-实现与验证记录) 至 [§8.27](#827-s47-整体验收与交接记录)。
S4 目标是让同一磁盘方案通过现有无 GUI 入口完成“本地图像 → 二值化 → Blob 分析 → 结果判断”，
并以像素、数值、来源和控制行为证明真实算法与 S3 的运行、存储契约一致。

### 7.1 当前代码基础与必须补齐的接缝

下表记录制定计划时的代码基础。

| 当前代码 | 可复用能力 | S4 必须完成的工作 |
|---|---|---|
| `source/workflow/workflow_engine.cpp`、`execution_plan.cpp` | 静态装配、快照、数据绑定、分叉汇合、取消与结果保留 | 直接注册真实工具；不另建执行器或算法专用调度入口 |
| `include/visionpro/core/image.hpp`、`source/core/image.cpp` | 只读图像、深度/通道/步长、共享裁剪及根坐标关系 | 集中实现 OpenCV 桥接；补充保持空间元数据的新像素发布入口，避免 `copy` 重置裁剪图的 root/origin |
| `geometry.hpp`、`InvocationContextData::regions`、`resolver.cpp` | ROI 形状、变换、排除、掩膜规则和本轮继承来源冻结 | 实际计算像素成员、边界、空域和修正后的方向；不能把模型校验视为算法适配 |
| `FileResourceLease::bytes()`、`PreparedTool::prepare` | 固定文件字节、摘要检查、显式资源槽及调用租约 | 直接从租约字节解码；嵌套 `MaskConfig` 的资源检查尚不等于获得像素，须显式取得匹配租约 |
| `source/tools/simulation.cpp`、`ToolRegistry::create` | 模拟工具和一次性 registration 集合装配 | 新增视觉 registration，由 app 合并创建注册表；保留模拟工具及旧方案 |
| `source/app/main.cpp`、`run_summary.cpp` | 独立 CLI 进程、模拟工具装配、Int64/List 与来源摘要 | 补齐真实工具装配和 S4 输出类型 |
| 现有 core/workflow/infrastructure/app 测试 | S3 的 82 个回归入口 | 增加真实像素与算法证据；保留既有测试，不以测试数量代替验收 |

S4 不包含 GUI、相机、PLC/TCP、动态插件、完整 Group/循环、自动重试或通用表达式系统。
这些边界不缩减既有模型；未支持内容仍须保留配置并明确诊断。

### 7.2 先固定的依赖与算法契约

**依赖与模块。** S4.1 接入已有的 OpenCV `4.14.0-pre` Windows x64/MSVC Release 预编译包，使用 `core`、`imgproc`、`imgcodecs` 和 `/MD`。
`VISIONPRO_WITH_OPENCV` 默认 OFF，核心、模拟 CLI 与既有测试可独立构建。ON 精确查找已验证版本，缺少依赖会导致配置失败。
安装包通过 `OpenCV_DIR` 从本机提供，构建命令见 [§3.17](#317-s41-release-构建与验证)，依赖证据见 [§8.21](#821-s41-实现与验证记录)。

S4.1 在 `source/tools/vision/` 建立 `visionpro_vision_tools`，提供工具元数据、符号适配校验和静态注册入口。
S4.2 注册 `visionpro.image.read_file` 的实例工厂。`opencv_image` 集中处理像素复制和 OpenCV 异常转换，
`read_file` 从 `FileResourceLease` 固定的字节解码。S4.3 的 `roi`、`roi_mask` 复用相同的解码和图像所有权边界。
S4.4 注册 `visionpro.image.threshold`，用 `cv::threshold` 处理 UInt8 Mono，再用 `cv::bitwise_and` 应用求值器生成的 0/255 成员掩膜。
两种模式均将 ROI 外像素清零。core/workflow 公共接口使用项目图像类型。

**首批工具与 Schema。** 工具配置版本和新 Record Schema 从 1 开始；以下字段为实施基线。

| 工具 | 输入与配置 | 输出与明确行为 |
|---|---|---|
| `visionpro.image.read_file` | `source` 槽为 Image 资源；`decode_mode` 为 Gray8/Native8；`frame_origin: Point2D` 为固定参数 | `image: Image`。`frame_origin` 必须为方案已声明 Pixel 坐标系的 `(0,0)`，复用现有类型化坐标校验；不按节点显示名生成坐标身份 |
| `visionpro.image.threshold` | `image: Image(UInt8/Mono)`；必需 `roi: Region2D`，全图使用显式 FullImage 配置；`threshold: Int64 [0,255]`；`mode` 为 Binary/BinaryInverse；可选 `mask` 资源槽 | `binary: Image(UInt8/Mono)`；`effective_roi: Region2D`；`roi_mask: RasterMask`。Binary 为 `src > threshold` 时 255，否则 0；Inverse 只在有效 ROI 内反转；ROI 外恒为 0，输出尺寸与坐标不变 |
| `visionpro.image.blob` | `binary: Image(UInt8/Mono)`；`connectivity` 为 4/8；`roi: Optional<Region2D>` 默认 None 表示全图；可选 `mask` 槽 | `count: Int64`、`components: List<BlobRecord>`、`total_area_pixels: Int64`、`largest_area_pixels: Optional<Int64>`；非零像素为前景，不输出背景 label |
| `visionpro.judge.integer_range` | `value: Int64`；固定闭区间 `minimum`、`maximum` | `passed: Bool`，节点技术状态为 Succeeded，业务判定为 OK/NG；`minimum > maximum` 在工具开始计算前报配置错误；NG 不阻止正常后继 |

`BlobRecord` 至少包括 `area_pixels: Int64`、`centroid: Point2D`、
`bounds: Record{origin: Point2D, width: Int64, height: Int64}`。
面积是前景像素数量，不能用 `contourArea` 的几何面积替代，也不把像素数量标记成长度单位。
质心与包围框 origin 使用输入图像当前坐标系和 Pixel 单位；包围框覆盖半开像素索引区间。
组件按其最先出现的前景像素 `(y,x)` 升序排序，不依赖 OpenCV 内部 label 编号。
正常无 Blob 返回 `count=0`、空 List、总面积 0、最大面积 None；它是成功结果。
S4.5 用 `cv::connectedComponentsWithStats` 计算统计，再按标签图中首次出现的位置排列组件。
OpenCV 的标签与统计使用有符号 32 位整数，输入像素数超过 `INT_MAX - 1` 时在 `binary` 输入报告 `SizeOverflow`。
整数范围工具的工厂在节点派发前校验上下界，反序区间定位到 `maximum`。
算法依据见 [OpenCV 连通域与面积接口](https://docs.opencv.org/4.13.0/d3/dc0/group__imgproc__shape.html)；
阈值边界依据见 [OpenCV threshold 说明](https://docs.opencv.org/4.13.0/d7/d4d/tutorial_py_thresholding.html)。

**图像读取与所有权。** 首批验收 PNG/BMP 的 UInt8 Mono/BGR；Gray8 对 BGR 显式灰度转换，Native8 保留支持的原格式。
threshold/Blob 的 `image_formats` 仅声明 UInt8/Mono；Native8 的 BGR 输出必须明确拒绝，不隐式灰度或逐通道阈值。
先按原深度解码并检查，再执行所选转换；UInt16、浮点或未支持通道明确拒绝，不自动截断成 UInt8。
禁止工具重新按 `FileResourceLease::path()` 打开文件；通过 `bytes()` 和 `imdecode` 使用已固定内容，
路径、资源身份、修订和摘要继续由 infrastructure 管理。解码忽略 EXIF 自动旋转，不因文件元数据隐式改变坐标。
格式和解码行为依据见 [OpenCV 图像编解码接口](https://docs.opencv.org/4.13.0/d4/da8/group__imgcodecs.html)。

初版桥接按有效像素逐行复制，支持带 padding 的行及非连续裁剪视图；不暴露可写 `cv::Mat` 指向只读共享缓冲。
新增最小 ImageHandle 工厂：从已有图像复制空间元数据并发布同尺寸、可不同像素格式的新缓冲；
保留 current/root frame 和 origin，校验尺寸、步长、长度及溢出。不得通过重新贴坐标标签隐藏重采样。
派生 `ImageFrame` 保留输入的可选 FrameId/captured_at；文件读取不伪造采集时间。
阈值和 Blob 首版同步运行，不承诺抢占正在执行的 OpenCV 调用；取消后仍由引擎拦截迟到结果，并持有资源到实际退出。
`cv::Exception` 映射为带 provider、operation、原始码及版本的 Diagnostic，未知异常保留现有边界保护。

### 7.3 ROI 与掩膜的首批实际能力

S4.3 的 `evaluate_roi` 已实现本节的像素求值、租约匹配和分层追溯。输出包括 0/255 成员掩膜、像素数、修正方向和可发布的 `Region2D`。
终态采用输入图像当前的 Pixel 坐标，`FullImage` 以本次目标图像尺寸展开。掩膜的局部目标尺寸在没有 ROI 修正时必须匹配目标图像，
有修正时先回到掩膜声明的局部目标域再取样。边界按半开域判断，位于远端上界且包含该边的几何会触发 `Reject`。

- 支持 FullImage、Empty、AxisAlignedRect、RotatedRect、RasterMask；支持 Reject/Intersect 和 Error/ValidEmptyOutput。圆、环、扇环、多边形、Fill 等未适配能力明确报告，不降级为包围框。
- 像素中心为整数 `(x,y)`。轴对齐矩形使用 `[x,x+width) × [y,y+height)`；旋转矩形在自身局部坐标中使用 `[-width/2,width/2) × [-height/2,height/2)`，通过逆变换判定像素中心。角度为弧度，图像正向为顺时针。
- 支持无校准、无 valid_domain 限制的可逆 Similarity/Affine 修正；不支持的 Projective/Calibrated 或有界变换明确诊断。矩阵按现有列向量约定组合；不对顶点取整后用填充结果替代成员规则。
- 继承仅使用 `InvocationContextData::regions` 冻结的本轮来源。先展开来源自身的几何与排除，再对整个继承集合施加外层修正，最后减去当前层排除集合。首版支持继承主来源和本地 RegionPrimitive 排除，保留主来源已经包含的排除项；排除项本身为 InheritedRegion 的复杂集合暂不支持，明确诊断。每层方向先按该层声明确定，再随该层变换映射；方向与区域一同变换，保留各层来源和方向记录。
- 几何组合在符号空间展开，最终在本次目标图像求成员；不隐式继承上游调用时的图像裁剪结果。需要继承确切像素集合时，使用显式 RasterMask。每层边界/空域策略在该层映射到当前目标后检查，不绕过来源的 Error/Reject。
- 首版边界判断以完整包含几何是否超出目标采样域 `[0,width) × [0,height)` 为准，排除区不得掩盖包含区域越界；Intersect 再求交集。排除区可与目标求交但不能扩大有效区域。空域 Error 报 EmptyRegion；ValidEmptyOutput 得全零的同尺寸图和成功的空 Blob 结果，不创建零尺寸 ImageHandle。
- 掩膜支持 UInt8 Mono、NonZero/AtLeastThreshold、同尺寸同坐标直接映射和显式 Nearest 映射；Nearest 使用目标像素中心逆映射到源，取 `floor(v+0.5)`，源外为无效成员。不同尺寸/坐标无显式映射时报错；Linear 暂不支持。几何包含与掩膜排除按 Combine 能力执行，不自动把普通 Image 当作掩膜。
- MaskConfig 必须匹配节点声明的可选 `mask` 资源槽，核对固定后的资源身份、种类、版本、解码格式与尺寸。嵌套引用未指定 revision 时采用已固定槽位的版本，显式指定时必须一致；不直接用未固定引用与租约作相等比较。S4 每次调用支持一个不同的掩膜文件，可重复引用；缺槽、引用不符或额外掩膜资源明确拒绝。这样复用现有租约，不新增按任意路径查资源的接口。

ROI 求值结果至少保留有效掩膜、像素成员数、当前坐标、修正后方向以及冻结来源。
threshold 发布无未解析 InheritedRegion 的 `effective_roi`，保持该 ROI 的配置身份；
全图同样由显式配置提供 RegionId，`roi_mask` 表示实际成员。该输出必须是 Region2D 而非 Optional，符合当前继承引用的类型校验。
成员掩膜由内部求值器生成，Region2D 输出保留展开后的几何、变换与排除；当前表达式无法无损展开时明确拒绝，不能只丢掉排除项后输出。
如果一层 `Error` 约束和外层 `ValidEmptyOutput` 加排除无法合并，像素求值可供只消费成员的工具使用，需要发布 `Region2D` 的调用取得 `UnsupportedRegion`。
S4.4 用两个真实 threshold 节点验证冻结流转及平移和排除。输出坐标系随输入图像确定，绑定解析在修正前核对实际坐标和单位。
S4.5 按 V08 验证 Blob 分别绑定全前景图像与另一路阈值的有效 ROI。Blob 直接消费 ROI 成员掩膜。
`RunBindingTrace::regions` 保留冻结定义、绑定路径和生产者调用身份，CLI 摘要记录绑定路径及生产者身份。
求值结果记录各层修正、方向、成员数和实际掩膜租约引用。

### 7.4 子阶段、依赖与交付门槛

| 子阶段 | 具体实施及代码落点 | 完成门槛 |
|---|---|---|
| S4.1 依赖、Schema 与适配契约 | 根及 tools CMake 增加可选视觉目标；核验并锁定现有 OpenCV Release 包；建立四个工具的版本化元数据与 registration 组合；落实 §7.2–§7.3 的类型和不支持边界 | OFF 配置不要求 OpenCV 且旧测试通过；ON 在 Release 可链接并运行最小解码/处理程序；元数据校验通过，未实现工具不注册成可用 |
| S4.2 图像桥接与真实文件读取 | `source/tools/vision/` 增加集中桥接/解码和 read_file；core 增加保留空间元数据的独立输出工厂；`tests/tools/` 增加图像夹具 | 实际 PNG/BMP 与中文路径通过；裁剪、padding、通道、独立所有权及非法图像诊断通过；固定资源改动不改变已固定字节 |
| S4.3 ROI、继承与掩膜求值 | 内部 ROI 求值器消费完整冻结来源；实现变换、排除、边界、空域、mask 槽解码与映射；对不足的类型化追溯做最小扩展 | §7.5 的旋转7像素、继承6像素、边界和掩膜黄金集合通过；声明能力与实际支持一致，支持外内容有定位诊断 |
| S4.4 二值化工具 | threshold 调用 OpenCV，接入 ROI、独立图像输出、有效区域与 mask 结果 | 阈值边界和反相逐像素一致；真实工具不修改输入；空域和不支持图像路径符合契约 |
| S4.5 Blob 与结果判断 | 连通域统计、确定性排序、类型化 Record/Optional 输出、integer_range 及组合参数检查 | 两组件、单像素、孔洞、4/8连通、裁剪坐标、空结果及 OK/NG 数值全部通过；实际通过字段/列表选择器绑定判断输入 |
| S4.6 运行、CLI 与配置往返 | 完成真实工具装配与摘要；新增真实单链、五路、ROI继承及独立调用夹具；保存重载用独立 CLI 进程 | 真实算法经现有 WorkflowEngine 运行；NG继续、FailFast/取消/历史隔离通过；磁盘往返的结果、来源和控制一致，模拟方案仍可执行 |
| S4.7 整体验收与交接 | 固定样例方案和输入清单，完成 ON/OFF 构建与无 SHA-256 回归，记录复现命令和交接入口 | §7.5–§7.7 的本轮场景有固定输入、数值断言、实际结果与日志 |

主线顺序为 S4.1 → S4.2 → S4.3 → S4.4 → S4.5 → S4.6 → S4.7。
Schema 和桥接固定后，Blob 数学测试、CLI 类型输出可独立准备，但只在真实集成验证后算完成。
每一子阶段提交实现、直接相关测试及对应接口说明；不等最后一次统一补测试。

### 7.5 固定夹具与可复现验收数值

验收图像使用小型无损文件和明确像素数组。固定输入清单记录格式、尺寸、文件字节数、有效像素规则和资源标识。
期望值手算或由独立数学参考产生，不能用被测 OpenCV/ROI 函数重新生成“期望”。
下表坐标均指像素中心，未另写时为输入当前图像坐标系。

| 编号 | 固定输入或操作 | 必须得到的结果 |
|---|---|---|
| V01 阈值边界 | Mono8 `[0,127,128,129,255]`，阈值128 | Binary `[0,0,0,255,255]`；Inverse `[255,255,255,0,0]`；阈值0/255另测等值边界 |
| V02 Blob 主样例 | 12×10，背景10；`x=1..3,y=1..2` 和 `x=7..9,y=5..7` 为200；阈值128 | count=2，面积6/9，总面积15，最大面积Some(9)；质心 `(2,1.5)`/`(8,6)`；bounds `(1,1,3,2)`/`(7,5,3,3)`，顺序固定 |
| V03 连通性 | 3×3 仅 `(0,0)`、`(1,1)` 非零 | 4连通两组件，各面积1；8连通一组件，面积2、质心 `(0.5,0.5)` |
| V04 面积与空结果 | 单像素；3×3前景删除中心；全零图 | 面积分别1、8；全零图成功且 count=0、components为空、total=0、largest=None，不能用轮廓面积替代 |
| V05 判断与绑定 | V02 的 count 接范围 `[2,2]`/`[3,3]`；另一判断绑定 `components[0].area_pixels` | 前者OK/后者NG，均为技术成功且后继照常；字段结果为6；对空列表读取 `[0]` 报 IndexOutOfRange，不能当0 |
| V06 步长与裁剪 | 4×3 Mono8、stride=6，有效像素 `[1,2,3,4;11,12,13,14;21,22,23,24]`，裁剪 `(1,1,3,2)` 后阈值13 | binary `[0,0,255;255,255,255]`，面积4；质心当前帧 `(1.25,0.75)`，用保留 origin 显式换算根帧 `(2.25,1.75)`；原图/padding不变，派生图 root/origin 保留 |
| V07 旋转 ROI | 9×9 前景图，中心 `(4,4)`、宽4、高2、角π/4 | 成员恰为 `{(3,3),(4,3),(3,4),(4,4),(5,4),(4,5),(5,5)}`，共7，不是包围框 |
| V08 继承与修正 | 同一全255原图分叉：FullImage threshold 提供全前景 binary，V07 threshold 提供 effective_roi；Blob 读前者图像、继承后者区域，再平移 `(+1,0)` 并排除 `(5,4,1,1)` | ROI成员及Blob面积均为6，即V07集合平移后去掉 `(5,4)`；不能改读V07已清零的binary，否则交集只有3；source本身带排除时仍保留；本轮来源调用可追溯；另测方向0经π/2修正为向量 `(0,1)` |
| V09 边界和空域 | 4×3图，ROI `(-1,0,3,2)`；再测全部排除 | Reject定位越界；Intersect为4像素；全排除 Error报EmptyRegion，ValidEmptyOutput全零/空结果；另测支持域边界和贴边旋转矩形 |
| V10 掩膜规则 | 3×2掩膜 `[0,1,127;128,255,0]` | NonZero为4成员，AtLeastThreshold(128)为2；等于阈值包含；全零掩膜按空域策略处理 |
| V11 掩膜显式映射 | 2×2 `[1,0;0,1]`，Nearest平移 `(+1,0)` 到3×2 | 仅 `(1,0),(2,1)` 有效；无变换却尺寸/坐标不匹配时报错；补测半像素取整和源外采样 |
| V12 真实五路 | V02 同一原图扇出，阈值依次100/128/180/210/250，各接Blob和 `[2,2]` 判断，再AllActivated汇合 | Blob count依次 `[2,2,2,0,0]`，判定 `[OK,OK,OK,NG,NG]`；各路一次，汇合和模拟交付各一次；原图及兄弟输出不变 |
| V13 真实资源与往返 | 托管/外部PNG/BMP、中文和空格路径；从不同工作目录执行→保存→新进程重载→执行 | 输入/输出有效像素、Blob值/排序、判定、绑定与ROI来源、控制行为一致；保持资源类别和引用，运行缓冲不进入清单 |
| V14 失败与本轮隔离 | 上轮成功，下轮损坏编码/缺资源/摘要不符；非法阈值、min>max、未支持深度/ROI、掩膜错配 | 按发生层报告资源/参数/图像/区域诊断；受理前错误无工具调用，受理后失败FailFast；无旧图、旧Blob或伪成功后继 |
| V15 结果与取消保活 | 持有A的真实图像结果后运行B并淘汰历史；受控门闩在真实计算和结果接受间插入取消 | A像素不变，释放引擎后已持有结果仍可读；取消先赢不发布输出/不调用后继，实际退出前不复用槽位；不用偶然sleep证明竞争 |
| V16 单节点与输入快照 | 以 RunRequest 注入真实Image执行threshold/Blob单节点；Busy竞争提供另一图；编辑ROI/阈值 | 仅目标执行；受理图像/参数不变，Busy输入不泄漏到下一轮；后续新快照采用编辑值；运行后仍可读旧结果 |

整数像素、成员集合、面积、数量、包围框与稳定排序要求精确相等。
这些小型确定性夹具的质心/变换坐标绝对误差 ≤ `1e-9` pixel，方向单位向量分量误差 ≤ `1e-9`。
灰度转换另用固定色块核对 BGR/RGB 顺序及固定整数期望；不能以放宽全局误差容忍通道错误。
图像比较只取有效行像素，并连同尺寸、格式和坐标比较；忽略 padding 字节布局差异，不用编码文件字节或 ImageHandle 身份相等代替像素比较。
往返排除新 session/run/invocation/frame 分配值、耗时和时间戳，但要检查引用确实落在各自本轮，不能删掉整个来源字段后比较。

### 7.6 CLI、测试组织与实施后的复现方式

S4.2 的 CLI 已支持 Image 元数据输出，包括宽高、深度、通道、current/root frame、origin、可选帧身份和采集时间。
S4.4 已支持实际 `Region2D` 的形状、变换、排除和冻结来源摘要，以及 `RasterMask` 的映射、成员规则和直接映射掩膜的成员数。
S4.5 支持 JSON 布尔值、带坐标系与单位的 Point2D，以及按字段 ID 递归序列化的 Record。Optional 无值时为 `null`，有值时递归序列化其内容。
List 继续使用数组，Int64 保留十进制字符串。S4.6 增加 Float64 数字、UTF-8 字符串与包含 `schema`、`version`、`member` 的枚举对象。
UInt8 图像和掩膜像素增加有效行样本的最小值、最大值和非零数。CLI 使用这些统计及已有布局和来源字段，持久化验收另外直接核对有效像素。
NG 维持成功退出码0；技术错误为非零，并同时保留已受理运行的终态摘要。
本阶段文件图像由方案资源输入；真实 Image 请求注入由 C++ RunRequest 验证，不扩展通用 CLI 图像参数语言。

测试分组包括 `tools.image_bridge`、`tools.image_read`、`tools.roi`、`tools.mask`、`tools.threshold`、
`tools.blob`、`tools.judgement`、`workflow.vision_single_chain`、`workflow.vision_integration`、`workflow.vision_lifecycle`、
`app.vision_roundtrip` 和 `app.vision_diagnostics`。
各组包含有意义的成功/失败断言；实现时可按夹具规模拆分，不预先要求固定新增测试数量。
`tests/tools` 验证算法本身，workflow/app 组必须走真实注册表和引擎；CLI 往返继续采用独立进程。
V12 允许复用现有模拟 collect/deliver 检查交付次数，但读图、五路阈值、Blob和判断必须是真实工具。
collect 的数据来源为各路 Blob.count（Int64），控制前驱为各路 judge；不能把 Bool 的 passed 绑定给现有整数集合输入。

当前 ON/OFF 命令见 [§3.23](#323-s47-release-构建与回归)。S4.7 在独立构建中运行适用的无 SHA-256 回归，核对 OFF 可执行文件的 OpenCV/Qt 导入。
固定样例提供资源、流程 UUID 和输入规则。独立 CLI 在不同工作目录运行并重载，分别核对本轮生产者身份，再比较结果、来源和控制边。[样例与证据](s4-acceptance.md)

### 7.7 阶段退出条件与证据

S4 完成条件如下。S4.7 的验证范围按本轮无 SHA-256 要求执行，具体覆盖与排除项见 [验收矩阵](s4-acceptance.md)。

1. 四个真实工具及 ROI/掩膜支持范围按元数据实现，V01–V16 的本轮纳入场景全部通过，未实现能力显式诊断。
2. 单链、真实五路、继承ROI、单节点调用、失败与取消均使用现有运行服务；运行和保存不依赖窗口初始化。
3. Release 的 ON/OFF 构建和适用回归通过，断言在 Release 有效。OpenCV 预编译包版本、组件、工具链兼容性和运行依赖可复现。
4. 结果证据包含配置和 Schema 版本、输入清单、预期像素或数值、实际结果、误差判定和诊断。绑定、ROI、资源及调用身份可追溯。
5. 保存重载保持真实算法行为；不持久化运行图像、临时结果、cv::Mat或设备句柄。只新增工具/Record Schema时不机械升级方案格式；新增持久化字段时单独检查兼容性。
6. S4记录与代码一致，已实现接口同步到data-model/architecture，构建与复现方式同步到本文件；README只在门槛通过后改为完成。

对应需求矩阵：A09→V06/V12/V15，A10→V07–V09，A11的本阶段真实算法部分→V12，
A19→V13/V14，A21的无GUI算法部分→V01–V13，A25的图像结果部分→V04/V14/V15，A26→V09–V11。
A05/A06/A07/A17/A18/A23分别以V04/V05/V14–V16补充真实数据集成；A21的GUI一致性仍归S5，设备/SDK部分仍归后续阶段。
不将构建成功、只返回Succeeded、仅模拟测试通过或性能测量代替上述门槛。
本阶段可记录固定夹具耗时和峰值资源供后续优化，但不在没有基准的情况下承诺实时性或吞吐率。

### 7.8 后续阶段验证边界

第五阶段验证界面生成的方案能够由相同无 GUI 入口运行，
两种入口使用相同配置与输入时得到一致检测结果。已确定的 S5 交付与验证安排见 [§9](#9-第五阶段实施与验收计划)。

执行器：
第三阶段范围包含基本分支及汇合；增加完整循环、动态索引与并行时，
继续验证未执行分支、调用作用域、零次迭代、区域导出与越界处理；
不将尚未支持的结构当作普通顺序流程执行。

编辑器：
模型与显示一致、控制连接校验、非相邻来源绑定、参数修改和布局恢复；
预览参数不会消费消息或修改共享状态。

共享服务与工具状态：
全局读写及版本、并发快照、FIFO 顺序、空满队列、等待取消、
完整记录与关联标识汇合、组合消费原子性、累积状态的作用域及重置。

脚本与逻辑工具：
声明类型和实际输出一致；分支只激活选定出口；
共享副作用通过显式接口执行，失败处理不假定外部操作可自动回滚。

算法：
固定输入、明确期望值与数值容差。

设备：
模拟验证和真实硬件验证分别记录。

后续能力验收补充以下反例，随对应功能实现，不计入当前已通过测试：

| 能力 | 必须覆盖的反例 |
|---|---|
| 分支/汇合扩展 | 在第三阶段基础上增加更复杂嵌套/并行完成次序；控制通过仍不自动填充缺失数据 |
| 循环 | 绑定起止值在进入循环时解析并固定；非负毫秒间隔与数值步长分开，等待可取消；同一节点多次调用不串值，零次迭代、提前退出及 First/Last/Collect/Reduce 导出符合规则 |
| 动态索引/格式化 | 普通动态越界失败；格式化仅按声明忽略索引/截取范围，不吞掉来源失败 |
| 重试/超时 | 旧 AttemptId 迟到不能提交或激活后继；响应丢失不默认重发非幂等命令 |
| 多流程汇合 | 工件 A/B 结果交错、重复、缺项及超时后迟到，不能拼成一个结果 |
| 相机/共享连接 | 节点释放不误关共享连接；SDK 复用与裁剪视图持有期间的缓冲生命周期正确 |
| 内存保留 | 历史淘汰后活动视图仍有效；队列、共享值和未齐备汇合的容量达到上限时按策略处理 |
| Group/独立调试 | 层级和公开边界正确，两实例状态隔离；只执行目标范围，导入不误连同名对象 |
| 连续与停止 | 停止不再创建新轮，排空/取消不同；未实际退出不释放资源 |
| 厂商适配 | 枚举缺项、字符串/二进制转换、不同错误域和缺能力可定位；回调及指针寿命有实际证据 |
| 标签图 | 类别栅格与类别表一致，显示调色板变化不修改业务结果 |

## 8. 验收记录与状态（C10）

验收场景编号统一使用 [requirements.md](requirements.md) §9 的 A01–A33。
每条记录至少保存下列信息；不适用项明确标记，不用空字段暗示已验证。

| 信息 | 目的 |
|---|---|
| 场景编号、所验收 C 契约、阶段 | 说明验证范围，防止单链测试代替分叉汇合验收 |
| 方案/快照、工具/Schema 版本、输入夹具及固定资源版本/摘要 | 能复现具体配置；动态输入记录实际来源，不仅记录路径 |
| Session/Run/Scope/Invocation/Attempt、Frame/Correlation（适用时） | 区分执行身份、图像与工件，不跨轮混用 |
| 控制出口选择、入边到达/关闭及实际调用次数 | 判断等待、跳过、重复激活和后继副作用 |
| 绑定实际来源、全局读取版本、转换与采用的 ROI | 追溯取值和几何处理，不能只记录“运行成功” |
| 期望输出、判定、数值定义/容差及实际结果 | 验证业务正确性和技术状态 |
| 错误/取消时序、完成与实际退出、资源释放记录 | 验证迟到、竞争和生命周期 |
| 工具链/依赖/设备环境、运行入口及证据位置 | 区分可复现模拟与真实硬件结论 |
| SDK/后端版本、错误域/原码、回调和借用寿命验证依据（适用时） | 不把文档描述等同于当前运行库行为；明确尚未验证项 |

设计审查、实现、编译、自动化测试、人工验证和真实设备验证分别记录。
可写“已设计、未实现”或“模拟已通过、真实硬件未验证”，不得用其中一种替代另一种。
阶段验收引用实际结果与失败原因；未实现项保留待验证状态，不能通过删除场景放宽标准。

修改文档时检查跨文档契约、阶段范围、链接/锚点、表格和残留旧限制。
仅文档变更不要求编造构建结果；没有可运行工程时明确说明未运行代码测试。

### 8.1 S1 文档审查记录

- 审查日期：2026-10-27。
- 阶段结论：**文档审查通过，相关能力尚未实现**。
- 审查基线：收尾前提交 `226776698007ce75d85a329a02a4d76da1d1e0e2` 中的 README、requirements、architecture、data-model、development 及 CMakeLists.txt；本次补录结论并修正状态，不改变领域契约和阶段范围。
- 审查依据：[requirements.md](requirements.md) §3 的九条通过条件、§9 的 A01–A33，以及本文 §4 的检查项。
- 方法与证据范围：静态文档交叉核对、示例与反例推演、CMake 文件检查；手册来源沿用 requirements §2 的依据映射，本次未重新逐页复核原始 PDF/CHM。
- 验证边界：未执行构建、代码测试、模拟工具、厂商 Demo 或真实设备验证。运行身份、输入夹具、实际调用次数、数值结果和设备环境不适用（本次为文档审查）；后续执行验证必须另行记录。

核心对象的职责、身份、寿命、所有权和持久化边界在 [data-model.md](data-model.md) §2–§4、§6、§8、§10–§13 中可分别定位。
模块依赖和无需 GUI 初始化的装配边界由 [architecture.md](architecture.md) §2–§3、§5、§9 定义，
控制、数据、包含关系及结果、状态、共享值、消息未混用。该项交付完整性审查通过。

下表按 requirements §3 的原顺序逐项记录；“通过”均指文档审查通过。

| 条件 | 审查结果与依据 | 结论 |
|---|---|---|
| 1. 五路测量、条件检测和单次汇合 | [data-model](data-model.md) §5.2 与 [architecture](architecture.md) §6 允许多前驱/多后继；AllActivated 在至少一次有效到达且全部入边确定后只调用一次，未要求虚拟业务节点规避边数限制；对应 A11、A12 | 通过 |
| 2. 非相邻及兄弟分路依赖、逆序诊断 | [data-model](data-model.md) §3、§5–§5.1 明确结构化来源与就绪约束；兄弟分路依赖可细化顺序，B → A 且 B 读取 A 在执行前拒绝，绑定不补跑来源；对应 A06、A13 | 通过 |
| 3. 实例与状态寿命、初始化和代次 | [data-model](data-model.md) §2.1、§13 分离实例和状态寿命；Init、显式重置、兼容配置及新旧状态代次有规则，在途操作保持旧代次；对应 A01、A02 | 通过 |
| 4. ROI、坐标与边界策略 | [data-model](data-model.md) §6–§7 明确浮点形状、继承、修正、排除、掩膜、坐标变换、越界与空区域；不能隐式替换成整数包围框；对应 A09、A10、A26 | 通过 |
| 5. 本轮结果、无值、错误、NG 与取消 | [data-model](data-model.md) §4、§5.1、§8–§9 区分合法无值、未产生、无效输出、来源失败、选择器错误和执行终态；NG 可技术成功，失败默认不提供算法输出；对应 A04、A05、A07、A17、A25、A31 | 通过 |
| 6. 同步/异步、迟到、清理与共享连接 | [data-model](data-model.md) §2.3–§2.4、§8.3–§8.5 及 [architecture](architecture.md) §5.2、§9 明确提交资格、物理退出、回调注销和连接所有权；取消不提前释放活动资源；对应 A16、A18、A27–A29 | 通过 |
| 7. 资源引用/嵌入及分层校验 | [data-model](data-model.md) §10 允许托管与外部引用，区分编辑、执行和无损保存；加载失败保护、原子提交及并发保存的保证边界明确；对应 A19、A20、A28 | 通过 |
| 8. C01–C10 来源、理由与场景归属 | [requirements](requirements.md) §2、§2.1、§9 为十项契约及 SDK 补充提供依据映射、设计理由和正常/边界场景；A01–A33 的定义审查结果见 §8.2，执行验证阶段保持原矩阵 | 通过 |
| 9. 实现选择与后续细化落点 | [data-model](data-model.md) §15.2 明确 C++ 接口、调度、存储、算法和后续服务的细化时机；目标契约与阶段实现区分，未以首版单线程或 Busy 策略缩减模型 | 通过 |

### 8.2 S1 场景定义逐项审查

本表记录 [requirements.md](requirements.md) §9 中全部 A01–A33 的定义审查结果，覆盖 C01–C10。
检查的是模型可表达性、正常/边界条件、可观察结果和阶段归属；**截至此次 S1 审查，全部场景的执行验证均未进行**。
执行验证阶段及完整通过条件以 requirements §9 为准，本表不改变或替代该矩阵。

| 场景 / 契约 | 文档审查结果及依据 | 结论 |
|---|---|---|
| A01 / C01 | [data-model](data-model.md) §2.1、§13 明确同类型节点隔离、实例重建与状态保留，正常累计及重置边界可判定 | 定义审查通过 |
| A02 / C01 | [data-model](data-model.md) §2.1、§13 明确状态域 Init、显式重置及配置代次；编辑、改名与在途状态的反例有规则 | 定义审查通过 |
| A03 / C02 | [data-model](data-model.md) §3 明确身份稳定、复制重映射及删除后的悬空诊断，不依赖显示名称 | 定义审查通过 |
| A04 / C02 | [data-model](data-model.md) §3、§8.2 分离运行、作用域、调用、尝试、帧及工件身份，跨轮/迭代串值的禁止条件明确 | 定义审查通过 |
| A05 / C03 | [data-model](data-model.md) §4、§8.1.1 区分类型化空集合、None、零、未产生与失败，并约束 Schema 和载荷 | 定义审查通过 |
| A06 / C04 | [data-model](data-model.md) §3、§5 明确字段、固定/动态索引及显式转换；动态能力后置但目标结构完整 | 定义审查通过 |
| A07 / C04 | [data-model](data-model.md) §5.1 区分普通越界失败和工具声明的局部容错，Optional/默认值不得吞掉来源错误 | 定义审查通过 |
| A08 / C04 | [data-model](data-model.md) §5、§11–§12 区分版本化读取、预览和显式消费/写入，副作用边界可检查 | 定义审查通过 |
| A09 / C05 | [data-model](data-model.md) §6、§8.4 明确共享输入、独立输出、视图所有权及取消期间保留，污染和提前释放反例有规则 | 定义审查通过 |
| A10 / C05 | [data-model](data-model.md) §7 明确旋转、继承、修正、排除与坐标方向，越界和空区域按工具声明处理 | 定义审查通过 |
| A11 / C06 | [data-model](data-model.md) §5.2 与 [architecture](architecture.md) §6 明确五路汇合、单次逻辑/发送及重复到达去重，不依赖完成次序 | 定义审查通过 |
| A12 / C06 | [data-model](data-model.md) §5.2 明确多选、零选、嵌套关闭和至少一次有效到达，选中失败不能冒充正常未参与 | 定义审查通过 |
| A13 / C06 | [data-model](data-model.md) §5.1 区分合法兄弟依赖与显式逆序，规划拒绝冲突且不补跑来源 | 定义审查通过 |
| A14 / C06 | [data-model](data-model.md) §5.3、§8.2 明确迭代作用域、显式导出及零次/失败规则，完整执行验证归 S6 | 定义审查通过 |
| A15 / C06 | [data-model](data-model.md) §5.4、§8.2、§12 明确独立流程和关联范围；错序、重复、缺项及迟到的配对边界明确 | 定义审查通过 |
| A16 / C07 | [data-model](data-model.md) §2.3、§8.3 明确同步/异步共用提交规则及取消后持有至物理退出，模拟与设备验证分开 | 定义审查通过 |
| A17 / C08 | [data-model](data-model.md) §8.1、§8.3 区分技术成功/NG 与失败，恢复保留原失败且不得制造算法输出 | 定义审查通过 |
| A18 / C08 | [data-model](data-model.md) §8.3 规定取消/提交竞争的一次裁决、迟到拒绝和外部副作用边界，后续重试单独验证 | 定义审查通过 |
| A19 / C09 | [data-model](data-model.md) §10.1–§10.2 明确托管/外部资源及编辑、执行、无损保存分层，缺依赖和未知内容均有处理规则 | 定义审查通过 |
| A20 / C09 | [data-model](data-model.md) §10.4 明确提交成功边界、清理失败及协作保存者的冲突保护，非协作写入不超出后端保证 | 定义审查通过 |
| A21 / C10 | [architecture](architecture.md) §3.1 与本文 §7 明确双入口复用相同服务及固定图像/误差验证，算法与 GUI 证据分阶段补齐 | 定义审查通过 |
| A22 / C08/C10 | [data-model](data-model.md) §8.4、§12 及本文 §8 区分活动所有权、显示淘汰和业务容量策略，设计与执行证据分别记录 | 定义审查通过 |
| A23 / C03/C04/C07 | [data-model](data-model.md) §4.1 明确输入与快照原子受理、Busy 不串扰、新轮必需输入和重试固定输入 | 定义审查通过 |
| A24 / C01/C04/C06/C09 | [data-model](data-model.md) §2.1、§4.1、§5.5、§10.3 明确 Group 公开边界、实例隔离、宿主依赖及导入重映射 | 定义审查通过 |
| A25 / C02/C03/C05/C08 | [data-model](data-model.md) §6、§8.1.1、§8.4–§8.5 明确结果物化和持有；空集合、无效输出、坏长度/空指针及非必需 Invalid 的反例可判定 | 定义审查通过 |
| A26 / C05 | [data-model](data-model.md) §7 明确掩膜所有权、尺寸/坐标、有效像素及组合策略，空区域和不匹配不隐式修正 | 定义审查通过 |
| A27 / C07/C08/C10 | [data-model](data-model.md) §8.5 明确完成与观察通道、身份校验、注销静默和超限策略，关闭显示不丢业务完成 | 定义审查通过 |
| A28 / C01/C07/C09 | [data-model](data-model.md) §2.4 与 [architecture](architecture.md) §5.2 明确候选加载、管理操作协调及退出顺序，失败保留有效配置 | 定义审查通过 |
| A29 / C06/C07 | [data-model](data-model.md) §4.1 及 [architecture](architecture.md) §5.1 明确单次句柄、连续停止、排空/取消与实际退出，等待超时不等于释放 | 定义审查通过 |
| A30 / C04/C06 | [data-model](data-model.md) §5.5 明确调试范围、完整输入、独立运行身份和默认隔离状态，不补跑外部来源或后继 | 定义审查通过 |
| A31 / C08/C10 | [data-model](data-model.md) §8.1.1、§9 明确输出状态域、原始错误码及耗时口径，未知错误不归为 NG | 定义审查通过 |
| A32 / C03/C04/C09 | [data-model](data-model.md) §4、§9–§10 与 [architecture](architecture.md) §3.1 明确 Schema 补足、编码转换及版本能力边界，不静默删字段 | 定义审查通过 |
| A33 / C03/C05/C10 | [data-model](data-model.md) §4 的标签栅格/类别表和 Schema 约束区分业务值与显示调色板，完整领域类型/算法验证归 S6 | 定义审查通过 |

### 8.3 S1 收尾与后续事项

本轮未发现阻断 S1 的模型或契约矛盾。审查中发现的两项收尾问题已处理：

- 阶段审查结果未落地：由 §8.1–§8.2 补录九条通过条件和全部场景定义的逐项结论。
- 项目状态误写“尚无构建配置”：README 和本文已改为“已有初始 CMake 骨架”，并明确业务目标与测试尚未建立；相关文档统一指向本次 S1 结论。

本次仅更新审查记录和状态描述，未修改模型契约、验收条件或后续阶段归属。
S1 无遗留阻断项；后续细化继续以 [data-model.md](data-model.md) §15.2 为准，不记作已实现。
S2 开始时确定适用的工具链、依赖与测试框架，将具体 C++ 类型/接口及绑定校验转为可构建、可测试的核心库。
调度和配置往返、真实算法、GUI、完整循环/共享服务及设备接入仍分别按 S3–S6 推进；
已有 CMake 骨架及本次文档审查均不构成这些阶段的执行验收证据。

### 8.4 S2.1 实现与验证记录

本节为 S2.1 结束时的历史记录；后续 S2.2 的实现与验证见 §8.5。

- 日期：2026-10-30。
- 结论：**S2.1 完成，Release 构建与首批行为测试通过**；S2.2–S2.7 未开始，S2 整体验收未完成。
- 代码基线：`12f5d1f` 加本次 S2.1 工作区改动，本次未创建 Git 提交；实现与测试随包含本记录的提交一起追溯。
- 实现入口：[核心构建](../source/core/CMakeLists.txt)、[身份类型](../include/visionpro/core/id.hpp)、
  [身份创建](../include/visionpro/core/id_factory.hpp)、[诊断](../include/visionpro/core/diagnostic.hpp)、
  [错误返回](../include/visionpro/core/result.hpp)。
- 测试入口：[core_tests.cpp](../tests/core/core_tests.cpp)、[CTest 注册](../tests/core/CMakeLists.txt)。
  固定输入保存在该测试文件，包括 `00112233-4455-4677-8899-aabbccddeeff`、大小写/边界变体及非法文本；
  Release 另从本机 Windows 系统随机源生成三类身份各 256 个样本。
- 工具/Schema、方案/快照、Session/Run/Scope/Invocation/Attempt、Frame/Correlation、控制图、绑定、ROI、设备：本子阶段不适用。
- 环境和复现命令见 §2、§3.1；构建不需要 Qt、OpenCV 或厂商运行库。

| 范围 / 测试入口 | 期望与验证内容 | Release 实际结果 |
|---|---|---|
| 工程底座 | 配置、编译实际静态库及测试程序，核心与测试使用一致运行库 | 成功，退出码 0 |
| A03 / C02：编译期身份隔离 | 三类身份不可交叉构造、赋值、比较；不可默认/裸字节/字符串构造 | 静态断言通过 |
| A03 / C02：`core.parse_valid` | 大小写归一、固定文本字节顺序、所有合法 variant、有效边界及非 NUL 结尾视图 | 111 检查，0 失败 |
| A03 / C02：`core.parse_invalid` | 长度、全部字符位置、分隔符、NUL/非 ASCII、nil、错误 version/variant，以及诊断优先级 | 1320 检查，0 失败 |
| A03 / C02：`core.generate` | 768 个样本均为规范 UUIDv4，可往返且本次样本不重复 | 3841 检查，0 失败 |
| A03 / C02：`core.value_semantics` | 身份比较、排序、复制、移动、独立持有输入值 | 42 检查，0 失败 |
| 最小诊断：`core.diagnostics` | 分类、名称、严重程度、结构化位置及文本按值持有；成功值 0 与错误分开；未知码明确回退 | 30 检查，0 失败 |
| 测试可靠性：`core.failure_exit` | 同一检查路径故意失败，进程必须返回非零；CTest 按 `WILL_FAIL` 判为通过 | 直接运行退出码 1；预期失败测试通过 |

Release 配置 6/6 项 CTest 通过，其中 5 个正常行为组各执行合计 5344 次检查，
另有 1 次故意失败检查。Release 生成配置含 `NDEBUG`，运行检查没有使用 `assert`；
因此不会随 Release 优化配置被禁用。日志位于
`out/build/s21-release/Testing/Temporary/LastTest.log`，属于本机构建产物，不提交。
另以 `BUILD_TESTING=OFF` 完成独立 Release 核心库构建，确认没有生成测试目标；
该附加验证的临时构建目录在收尾时清理，正常 Release 目录保留以便查看结果和复用。

验证边界：样本无重复不证明全局无碰撞，方案级重复身份检查仍在 S2.5；
系统随机源失败已实现为 `IdGenerationFailed`，本次未通过故障注入执行该系统失败路径。
诊断对象的对应错误分类已测试，不等同于系统故障路径已覆盖。
本次未验证非 Windows 后端、CMake 最低声明版本、节点改名/复制、Schema/运行身份或任何节点执行行为。
这些后续事项不属于 S2.1 的退出条件；A03 仅强类型身份子范围通过，其余范围按原计划推进。

### 8.5 S2.2 实现与验证记录

本节为 S2.2 结束时的历史记录；后续 S2.3 的实现与验证见 §8.6。

- 日期：2026-10-30。
- 结论：**S2.2 完成，Release 14/14 项 CTest 通过**，包含 S2.1 回归及预期失败检查。
  S2.3–S2.7 尚未开始，整个 S2 尚未通过验收。
- 代码基线：`12f5d1f` 加本会话 S2.1 和 S2.2 工作区改动，未创建 Git 提交，保留前一阶段成果。
- 公共接口：[UUID 身份](../include/visionpro/core/id.hpp)、[Schema 身份](../include/visionpro/core/schema_id.hpp)、
  [运行身份](../include/visionpro/core/runtime_id.hpp)、[计数工厂](../include/visionpro/core/runtime_id_factory.hpp)、
  [值与类型](../include/visionpro/core/value.hpp)、[工具元数据](../include/visionpro/core/tool_descriptor.hpp)、
  [结构化诊断](../include/visionpro/core/diagnostic.hpp)。
- 测试与固定夹具：[identity_tests.cpp](../tests/core/identity_tests.cpp)、
  [value_tests.cpp](../tests/core/value_tests.cpp)、[metadata_tests.cpp](../tests/core/metadata_tests.cpp)。
  包括固定 UUID/token、0 与 `uint64_t` 最大值、UTF-8 正反例、整数/浮点边界、嵌套 Schema、
  工具配置版本 7 / Schema 版本 2 的元数据夹具，无外部文件或设备。
- 环境沿用 §2，实际命令见 §3.2；无新增第三方依赖，不要求 Qt、OpenCV 或厂商运行库。
- 会话/运行/作用域等仅作固定内存身份夹具，不代表实际运行；方案/快照、工具执行、图像、ROI、
  队列、设备及厂商后端版本在本子阶段不适用。

| 场景子范围 / 测试入口 | 期望与验证内容 | Release 实际结果 |
|---|---|---|
| A03/A04 身份：`core.identities` | UUID/Schema/运行类别不混用；27 类身份编译期隔离，token/关联键边界，计数下界/耗尽，组合键各维度不同 | 1111 检查，0 失败 |
| A05 值：`core.values` | 配置值/运行值隔离；标量边界、空 List、None、缺失、错误分别表达；不可变持有和复制/移动有效 | 116 检查，0 失败 |
| A05/A32 类型：`core.schema` | Record/Enum 身份、版本、字段/成员及嵌套结构；未知/重复/缺失/错类型拒绝，诊断保留字段或索引 | 161 检查，0 失败 |
| A32 编码：`core.utf8` | UTF-8 标量边界、中文/emoji/NUL 与有界视图；过长编码、代理项、超范围、孤立延续、截断拒绝 | 270 检查，0 失败 |
| A06 元数据：`core.metadata` | 输入/参数/数据输出/控制出口分开；重复声明、未知枚举、订阅/结构参数与必填/默认规则可校验 | 24 检查，0 失败 |
| A06 约束：`core.constraints` | 数值范围、枚举集合、容量及非法约束声明；None/缺值/默认/来源错误的行为不同 | 40 检查，0 失败 |
| A06 转换：`core.conversions` | 精确匹配、Optional 包装、显式数值转换许可；单来源/集合/多来源区别 | 19 检查，0 失败 |
| A32 兼容：`core.compatibility` | Schema 版本区间、所需能力、完整性、同身份版本的嵌套定义一致性；失败不删除字段 | 20 检查，0 失败 |
| S2.1 回归与测试可靠性 | 原 5 个行为组与 `core.failure_exit`；正常检查不依赖 assert，故意失败进程退出码 1 | 6/6 通过，正常 5344 检查；故意失败退出 1 |

Release 的新增 8 组共 1761 次检查；加上 S2.1 回归，Release 共 7105 次正常检查，
另有 1 次预期失败检查。结果可从 §3.2 的 `LastTest.log` 复核，Release 构建目录保留供后续使用。
初次编译的类型隔离断言发现 `FrameId` 聚合允许仅给来源并默认序号，已改为必须显式提供两个参数；
原断言未放宽，最终 Release 配置通过。嵌套类型相等比较使用明确的递归调用，最终构建不再报告相应 MSVC 比较警告。

验证边界与后续衔接：

- A04 此次只验证身份表达和键区分，当前无结果存储或执行上下文，不能据此声称跨轮/迭代取值已验证。
- 当前值系统支持标量、二进制和复合结构；图像、几何、资源引用与状态模型在 S2.3 加入。
- `validate_binding` 只校验一个目标的来源种类和类型/转换许可；图结构、作用域、引用和依赖环检查在 S2.5，
  实际转换、聚合和运行取值在 S2.6。`resolve_*_value` 的空输入只表示未配置，不能代表 Pending/本轮无输出。
- 工具元数据仍是可编辑定义；快照和运行请求分离在 S2.4，完整执行接口在 S2.7 收敛。
- `FrameId` 的唯一域、关联键有效期、序列的所属域和并发分配仍由后续运行/适配层负责；无厂商 API 或真实设备验证。
- 当前只支持 Windows UUID 后端；系统随机源失败路径仍未故障注入。此次不改变其他阶段的验收范围。

### 8.6 S2.3 实现与验证记录

本节为 S2.3 结束时的历史记录；后续 S2.4 的实现与验证见 §8.7。

- 日期：2026-10-30。
- 结论：**S2.3 完成，Release 24/24 项 CTest 通过**，包含 S2.1–S2.2 回归与预期失败检查。
  下一项为 S2.4；S2.4–S2.7 尚未开始，整个 S2 尚未通过验收。
- 代码基线：`12f5d1f` 加本会话 S2.1–S2.3 工作区改动；保留已有改动，未创建 Git 提交。
- 公共接口：[image.hpp](../include/visionpro/core/image.hpp)、[coordinate.hpp](../include/visionpro/core/coordinate.hpp)、
  [geometry.hpp](../include/visionpro/core/geometry.hpp)、[resource_reference.hpp](../include/visionpro/core/resource_reference.hpp)、
  [resource.hpp](../include/visionpro/core/resource.hpp)、[state.hpp](../include/visionpro/core/state.hpp)，
  同步扩充值、工具元数据、运行期 `MessageId` 和诊断接口。
- 新增测试：[image_tests.cpp](../tests/core/image_tests.cpp)、[geometry_tests.cpp](../tests/core/geometry_tests.cpp)、
  [resource_state_tests.cpp](../tests/core/resource_state_tests.cpp)、[domain_value_tests.cpp](../tests/core/domain_value_tests.cpp)。
  夹具包括带填充行的小图、嵌套裁剪、固定坐标/矩阵/区域、掩膜、资源目录及会话/运行/调用状态记录，
  不读取实际图像文件、不连接设备、不使用厂商运行库。
- 环境沿用 §2，实际命令见 §3.3；核心和测试仍无 Qt/OpenCV 或新第三方依赖。
  最终 Release 构建返回 0，构建日志无编译警告。

| 场景子范围 / 测试入口 | 验证内容 | Release 实际结果 |
|---|---|---|
| A09：`core.image_layout` | 深度/通道、行步长、末行长度、短缓冲、零尺寸、乘加溢出及未知格式 | 136 检查，0 失败 |
| A09：`core.image_ownership` | 外部复制、只读共享、裁剪保活、根坐标累计、独立导出与克隆 | 53 检查，0 失败 |
| A10：`core.geometry` | 坐标轴/手性/单位、点线圆位姿、相似/仿射/投影、显式求逆、源有效域、数值边界和标定后端拒绝 | 70 检查，0 失败 |
| A10：`core.regions` | 多形状、自交/退化、继承/修正/排除、边界/空域、坐标目录和工具能力 | 81 检查，0 失败 |
| A26：`core.masks` | 配置引用/运行像素分离、单通道/阈值、尺寸/坐标匹配、显式映射/采样、所有权 | 40 检查，0 失败 |
| 资源模型：`core.resources` | 托管/外部/序列/设备/输出定义、路径基准/逃逸、目录身份/类别/版本引用 | 51 检查，0 失败 |
| A08：`core.shared_definitions` | 全局定义/当前值、完整队列消息、容量/收发策略、身份和精确类型 | 20 检查，0 失败 |
| A01：`core.state_models` | 三类状态域、节点/会话/运行隔离、代次溢出、初始化值/错误一致性和策略声明 | 23 检查，0 失败 |
| A05/A09/A26：`core.domain_values` | 两值域编译期隔离、嵌套 Image/Mask 保活、类型诊断、ROI/容器/Schema 保留可写别名回归 | 167 检查，0 失败 |
| A06/A32：`core.domain_metadata` | 工具状态/资源/图像/区域能力声明、重复/非法声明、状态嵌套 Schema 冲突 | 53 检查，0 失败 |
| S2.1–S2.2 回归与测试可靠性 | 原有行为与 `core.failure_exit`；故意失败入口直接调用也返回 1 | 14/14 通过，正常 7105 检查；故意失败退出 1 |

Release 新增 694 次检查，总计 **7799 次正常检查，0 失败**，另有 1 次预期失败检查。
编译期断言同时验证配置域不能持运行图像/掩膜及跨值域容器；检查数不包括这些编译期断言。
完整行为输出保存在 §3.3 的 `LastTest.log`，构建目录与日志保留供复核。

本次检查与修复：

- 拒绝数值溢出后的坐标轴，Similarity 使用相对尺度和归一化正交检查，防止微小尺度下的剪切/不等尺度误通过。
- Windows 绝对路径不接受 `/foo`、`\foo`、`C:foo` 或伪装为 UNC 的设备命名空间。
- 发现并修复 move 容器发布后仍可被原元素指针修改的问题：Schema、Binary、List、Record 和 ROI 工厂
  从独立容器发布不可变数据，已封装子值及只读像素继续共享；两值域和运行图像嵌套均有回归。
- 清理几何 visitor 的不可达返回结构；保留所有测试断言，未通过抑制警告或放宽校验取得通过。

验证边界与后续衔接：

- A01/A08 只覆盖定义和运行记录；未执行 Init、状态迁移/重置、共享写入、FIFO 消费或并发汇合。
- A09 验证内存所有权，不代表已验证设备租约、取消中的后台操作或 OpenCV 转换。
- A10/A26 覆盖静态形状/坐标/能力与矩阵映射；未计算真实交集、排除后空域、像素成员、栅格化或重采样，
  受控标定也无执行后端。对应算法/设备验收继续归 S4/S6。
- 资源路径和目录检查不访问文件或校验实际摘要；命名根解析、文件加载、内容锁定、设备与输出服务尚未实现。
- S2.4 接入方案/节点/关系/快照/请求及复制重映射，S2.5 校验图结构与绑定，S2.6 解析运行值，S2.7 收敛接口。
  当前结果不能标记整项 A01/A08/A09/A10/A26 或整个 S2 验收完成。

### 8.7 S2.4 实现与验证记录

本节为 S2.4 结束时的历史记录；S2.5 的后续实现与验证见 §8.8。

- 日期：2026-10-30。
- 结论：**S2.4 完成，Release 34/34 项 CTest 通过**；新增 workflow 模型、快照、复制和请求/结果测试。
  下一项为 S2.5；S2.5–S2.7 尚未开始，整个 S2 尚未通过验收。
- 代码基线：`12f5d1f` 加本会话 S2.1–S2.4 工作区改动；未覆盖前阶段成果，未创建 Git 提交。
- 新增 `visionpro_workflow` 静态库，公开依赖 `visionpro_core`，继续使用标准 C++20，不增加 GUI 或第三方依赖。
- 公共接口：[binding.hpp](../include/visionpro/workflow/binding.hpp)、[model.hpp](../include/visionpro/workflow/model.hpp)、
  [snapshot.hpp](../include/visionpro/workflow/snapshot.hpp)、[copy.hpp](../include/visionpro/workflow/copy.hpp)、
  [run.hpp](../include/visionpro/workflow/run.hpp)。core 复用校验入口补充公开输入/输出描述校验，以及 1701–1708 错误码和控制线诊断位置。
- 测试与夹具：[model_tests.cpp](../tests/workflow/model_tests.cpp)、[snapshot_tests.cpp](../tests/workflow/snapshot_tests.cpp)、
  [copy_tests.cpp](../tests/workflow/copy_tests.cpp)、[run_tests.cpp](../tests/workflow/run_tests.cpp)。
  夹具包括五路测量/条件/汇合、嵌套 Group、两个流程的方案、嵌套资源/ROI 引用、固定内存图像、独立请求和人工终态结果。
  快照夹具使用方案格式版本 2/修订 12、工具配置版本 7/Schema 版本 2；固定 UUID/token、Session/Snapshot/Run/Scope/Invocation/Attempt
  及关联身份的具体取值见对应测试工厂。身份只是内存夹具，未发生真实节点调用。
- 环境沿用 §2，实际命令见 §3.4；Release 配置与构建均返回 0，构建日志无编译警告。
  未加载实际资源文件、连接设备、执行图像算法或调用厂商 SDK。

| 场景子范围 / 测试入口 | 验证内容 | Release 实际结果 |
|---|---|---|
| A01/A13/A24：`workflow.model_graph` | 同类型配置独立、五路分叉/多前驱汇合、嵌套 Group、直接/递归枚举、公开数据与控制映射、改名/布局不改引用 | 21 检查，0 失败 |
| A06：`workflow.model_bindings` | 输入/参数目标、五类来源、系统字段/固定选择器、源基数、转换及缺失策略的形状检查 | 40 检查，0 失败 |
| A03/A24：`workflow.model_structure` | 方案内身份唯一性、包含缺父/多父/环、入口层级、公开导出目标、布局和非法局部定义 | 89 检查，0 失败 |
| A01/A09：`workflow.snapshots` | 配置与工具元数据深复制、保留可写别名隔离、源释放保活、只读 API、快照身份和缺工具草稿边界 | 25 检查，0 失败 |
| A03/A24：`workflow.copy_subgraph` | Group 后代闭包、内部引用/包含/数据及控制导出重映射、外部来源报告、跨选区控制线略过 | 24 检查，0 失败 |
| A03：`workflow.copy_solution` | 新方案及各持久身份、两个 Workflow、资源/变量/队列/布局、复合值内 ROI/Mask/标定引用和公开默认值重映射 | 25 检查，0 失败 |
| A03：`workflow.copy_boundaries` | 明确 Preserve/Reject、外部父及外部或错误归属的引用、非法选择/结构、失败不修改输入 | 15 检查，0 失败 |
| A23/A24：`workflow.run_inputs` | Workflow/Node/Group 目标、禁用祖先、精确工具版本、必需 Optional、未知/重复/错类型、范围与默认值物化 | 56 检查，0 失败 |
| A01/A09/A23：`workflow.run_isolation` | 多请求输入隔离、无旧输入回退、保留别名隔离、快照固定和图像缓冲保活 | 15 检查，0 失败 |
| A04：`workflow.result_models` | 运行/快照/调用/尝试身份、输出地址、NG 成功、三类输出可用性、失败输出拒绝、终态记录隔离 | 67 检查，0 失败 |
| S2.1–S2.3 回归与测试可靠性 | 原 24 项；故意失败入口也直接调用，验证退出码传递 | 24/24 通过，正常 7799 检查；故意失败退出 1 |

Release 新增 **377 次检查**，总计 **8176 次正常检查，0 失败**，另有 1 次预期失败检查；
编译期断言验证只读接口及配置/运行类型边界，不计入上述运行检查数。
Release 日志及构建目录保留供复核；完整输出路径见 §3.4。
实现审查补齐了 Group/Workflow 的控制出口导出及复制/快照回归，测试标准未放宽；Release 配置一次构建和全量测试通过。

验证边界与后续衔接：

- 本阶段的五路图、汇合、条件出口和 Group 是模型夹具，没有控制到达/关闭传播或实际调度。
  绑定来源/选择器类型、公开边界、控制/数据联合依赖环及执行范围检查归 S2.5。
- ConfigSnapshot 只固定配置及工具元数据，不锁定外部文件内容，也不授予资源/状态访问权；
  缺工具或未完成映射的局部合法草稿可捕获，不能因此认定可执行。
- RunRequest 是独立的不可变准备结果，必需输入每次重新提供；当前无 RunHandle、Busy、原子受理或执行身份分配。
  独立目标状态策略只是声明，互斥与隔离实例的运行验证归 S3。
- NodeResult 是人工终态记录；未实现结果存储、工具完整输出核对、当前上下文解析、一次提交或取消竞争。
  S2.6 接入当前运行取值，S2.7 收敛运行接口，S3 验证提交和生命周期。
- 复制只产生内存配置，完整方案复制不会自动复制资源文件；Preserve 报告的外部依赖仍须后续显式解析和校验。
  不据此声称方案 JSON 往返、设备复制或完整 Group 执行已验收。

### 8.8 S2.5 实现与验证记录

- 日期：2026-11-09。
- 结论：**S2.5 完成，Release 39/39 项 CTest 通过**；新增 5 项静态校验测试，保留全部 34 项前阶段回归。
  S2.6–S2.7 尚未实现，整个 S2 尚未通过验收。
- 公共入口：[validation.hpp](../include/visionpro/workflow/validation.hpp) 的 `validate_solution` 与 `system_field_type`；
  实现位于 `source/workflow/validation.cpp`，内部领域值递归检查位于 `validation_values.cpp`。
- 测试：[validation_tests.cpp](../tests/workflow/validation_tests.cpp) 覆盖图、绑定、Group 与 Schema，
  [validation_domain_tests.cpp](../tests/workflow/validation_domain_tests.cpp) 覆盖 ROI 继承、嵌套资源与坐标/能力检查。
- 环境与 §2 相同：Windows x64、MSVC 19.51.36248.0、Windows SDK 10.0.26100.0、CMake/CTest 4.4.0。
  §3.5 Release 的配置、最终构建和全量测试均返回 0，最终构建日志无编译警告。
  本地日志保留在 `out/build/s25-release/` 下的 `build.log` 与 `Testing/Temporary/LastTest.log`，均被 Git 忽略。
- 本次在已有 S2.1–S2.4 未提交工作区基础上补充实现；没有覆盖前阶段成果或创建 Git 提交。
- 验证边界：静态类型、引用、控制可达性与依赖索引；没有结果解析、实际调度、资源加载、设备访问或算法运行。
  S2.6 负责运行结果/公开输入取值、实际 List 长度和可用性，S2.7 负责接口收敛，S3 负责运行与提交。

| 场景子范围 / 测试入口 | 实际验证内容 | Release |
|---|---|---|
| A03/A13：`workflow.validation_graph` | 五路分叉/汇合、非相邻/兄弟依赖、普通回边、自环、直接/间接逆序、纯数据/混合环、显式入口及范围外前驱/来源、诊断与索引 | 56 检查，0 失败 |
| A06/A07：`workflow.validation_bindings` | 字段/固定索引、目标/来源、基数、转换、Optional/缺失、默认值、静态长度约束、单位、公开输入/全局/资源及系统字段 | 112 检查，0 失败 |
| A24：`workflow.validation_groups` | 两层嵌套 Group、逐层数据/控制映射、映射完整性、作用域穿透与来源可达性 | 43 检查，0 失败 |
| A03/A32：`workflow.validation_schema` | 工具注册/版本、完整 Schema、显式能力支持、跨工具/公开接口/队列的类型冲突、同版本端口结构与改名 | 41 检查，0 失败 |
| A10/A26：`workflow.validation_domains` | ROI 来源/排除项的隐藏依赖、权限/禁用/结构参数、嵌套值路径、坐标/能力、掩膜/标定资源、资源槽、状态初值/设备配置 | 110 检查，0 失败 |
| 既有 34 项 CTest | S2.1–S2.4 回归，含 `core.failure_exit` 预期失败入口 | 全部通过 |

Release 新增 **362 次正常检查**，总计 **8538 次正常检查，0 失败**，另有 1 次预期失败检查。
测试使用固定 UUID/token、内存 Schema/图/ROI/资源目录，不读取真实资源、运行算法或连接设备。
最终 `git diff --check` 无空白错误；本次构建目录保留作验证依据，没有其他临时文件。

实现与验证中修复了默认几何值坐标检查、隐藏 ROI 的来源权限/Disabled 处理、队列 Schema 一致性、
状态/设备配置递归引用和确定超长的多来源集合漏检，并加入成对回归。
首轮并行 MSBuild 停在工程检查步骤后终止，改用 `--parallel 1` 完成构建；
编译中发现的局部标识重定义及测试坐标 ID 类别错误也已修复，未删除测试或放宽断言。

### 8.9 S2.6 实现与验证记录

- 日期：2026-11-09。
- 结论：**S2.6 完成，Release 44/44 项 CTest 通过**。
  新增 5 项结果访问/解析测试，全部前阶段回归通过；S2.7 尚未开始，整个 S2 尚未通过验收。
- 公共入口：[result_store.hpp](../include/visionpro/workflow/result_store.hpp) 的 `ResultStore`，
  [resolver.hpp](../include/visionpro/workflow/resolver.hpp) 的 `BindingResolver`、`ResolutionContext` 和四态解析结果。
  运行值物化/约束与领域检查由 workflow 内部 `runtime_values` 复用，RunRequest 保持原输入规则。
- 测试：[result_store_tests.cpp](../tests/workflow/result_store_tests.cpp) 与
  [resolver_tests.cpp](../tests/workflow/resolver_tests.cpp)；新增 5 项 CTest，合计 44 项。
- 环境与 §2 相同：Windows x64、MSVC 19.51.36248.0、Windows SDK 10.0.26100.0、CMake/CTest 4.4.0。
  §3.6 Release 的配置、最终构建和全量测试均返回 0，最终构建日志没有编译警告。
  日志保留在 `out/build/s26-release/` 的 `build.log` 与 `Testing/Temporary/LastTest.log`，均被 Git 忽略。
- 本次在已有 S2.1–S2.5 未提交工作区上继续实现，没有覆盖前阶段成果或创建 Git 提交。

| 场景子范围 / 测试入口 | 验证内容 | Release |
|---|---|---|
| A04/A09：`workflow.result_store` | 完整输出 Schema、精确会话/运行/快照/调用身份、重复调用拒绝、退役运行与持有值保活 | 81 检查，0 失败 |
| A05/A06：`workflow.resolver_values` | 常量/默认值、参数、本轮字段/固定索引、显式转换、多来源顺序和运行约束 | 35 检查，0 失败 |
| A04/A07：`workflow.resolver_availability` | Pending、正常缺失、失败/取消/超时、系统字段、Optional 不吞错、原始原因及四态优先级 | 52 检查，0 失败 |
| A04：`workflow.resolver_isolation` | 会话/运行/作用域/调用隔离、显式同宿主外层可见性、禁止兄弟域读取、退役与上下文检查 | 35 检查，0 失败 |
| A08/A09/A10/A23/A24/A26：`workflow.resolver_domains` | 公开输入及 Group 作用域隔离、资源/全局快照、图像保活与格式、嵌套 ROI 完整冻结、坐标与组合能力 | 48 检查，0 失败 |
| 既有 39 项 CTest | S2.1–S2.5 回归，含 `core.failure_exit` 预期失败入口 | 全部通过 |

Release 新增 **251 次正常检查**，总计 **8789 次正常检查，0 失败**，另有 1 次预期失败检查。
夹具使用固定 UUID/token、人工终态、运行作用域、内存图像/ROI 和资源/全局快照；没有真实工具调用或设备访问。
审查补齐了所有绑定的运行身份/退役前置校验、继承 ROI 跨边界组合能力，以及空图像格式列表拒绝规则，
对应正反例和既有 RunRequest 回归均通过。最终 `git diff --check` 无空白错误；构建日志保留作验收依据。

本阶段明确区分人工记录、运行取值和实际执行：

- ResultStore 为单线程人工终态记录容器；不验证真实调用资格，不承担 S3 的提交/取消竞争或迟到回调处理。
  重复 InvocationKey 即使换 Attempt 仍拒绝；精确查询不回退历史，退役后拒绝新记录，已持有的值和图像继续有效。
- 当前没有允许缺失的输出元数据，Succeeded 必须提供全部声明输出的 Available 值；Optional None 和空 List 均为合法值。
  运行来源和公开输入中的 ROI 必须完整，不可携带尚未解析的继承引用。
- Resolver 不阻塞、启动或补跑来源，不读取最新结果。多来源结果按 Failure > Unavailable > Pending > Resolved 判定，
  正常缺失仅按显式策略转为 None 或省略集合项，其余值保持配置顺序和来源记录。
- 公开输入属于具体运行作用域；外层结果仅允许明确声明的同静态宿主祖先，不允许跨 Group 或其他迭代隐式读取。
  这些用例不代表完整循环或 Group 调度已经实现。
- 全局数据来自显式只读快照，不以定义初值代替缺失当前值；资源只返回目录中可核对的类型化引用，不进行 I/O。
- ROI 保留原符号表达式，并按来源索引/嵌套路径冻结完整源 Region、调用身份及其 exclusions/transform。
  本阶段不求几何变换、布尔区域、栅格化、真实像素成员或标定结果。
- 没有共享写入、队列消费、状态 Init、设备调度、真实算法、GUI 或方案文件读写；这些继续由后续阶段接入。

### 8.10 S2.7 与 S2 整体验收记录

- 日期：2026-11-09。
- 当前状态：**S2.7 完成，S2 整体验收通过**。既有 S2.1–S2.6 全部回归、新增运行接口、
  所有权/错误边界与组合验证均在 Release 通过；14 项必需 S2 子范围证据见下表。
- 范围：收敛 RunHandle、实例工厂、InvocationOperation 及其与快照/固定输入的边界；
  使用可控测试替身验证契约，不建设调度器、真实设备适配或完整共享服务。
- 版本和环境：基准 HEAD `12f5d1f` 上的当前未提交工作区，已有 S2.1–S2.6 修改保留；工具链沿用 §2。
  C++20、Windows x64、MSVC 19.51.36248.0、CMake/CTest 4.4.0；Release `/MD`。
  新接口为 core/workflow 各自的 `tool_execution.hpp` 和 workflow `run_handle.hpp`；
  测试为 core `tool_execution_tests.cpp`、workflow `run_handle_tests.cpp`、`tool_execution_tests.cpp`、`composition_tests.cpp`。
  日志位于 `out/build/s27-release` 的 `build.log` 和 `Testing/Temporary/LastTest.log`。
- 新增 CTest：`core.tool_execution`、`workflow.run_handles`、`workflow.tool_execution`、`workflow.composition`；
  共 48 项，保留 S2.6 的全部 44 项。

| 本轮验证 | Release |
|---|---|
| §3.7 配置与全量重建退出码、编译警告 | 0；0 警告 |
| 既有 44 项 CTest 回归 | 44/44 通过 |
| `core.tool_execution` 实例与操作契约 | 91 检查，0 失败 |
| `workflow.run_handles` 已受理句柄契约 | 60 检查，0 失败 |
| `workflow.tool_execution` 固定调用与结果投影 | 38 检查，0 失败 |
| `workflow.composition` 固定内存组合验收 | 27 检查，0 失败 |
| 全量 CTest、正常检查及预期失败入口 | 48/48；9,005 正常检查，0 失败；1 项预期失败符合预期 |

本轮每配置新增 216 个正常检查。两次 CTest 均退出 0；编译使用 `/W4`，不依赖 `assert` 维持 Release 检查。
`git diff --check` 无空白错误；core 没有反向包含 workflow，两个库与测试均不依赖 Qt/OpenCV 或窗口初始化。
审查中补齐部分启动异常仍返回可等待句柄、清理后发布物理退出、工厂重复发行拒绝、逐处 ROI 冻结覆盖、
无目标工具的上下文检查及资源目录版本固定，均有对应回归。

#### 8.10.1 十四项 S2 子范围证据矩阵

下列 CTest 名称已与 `tests/core/CMakeLists.txt`、`tests/workflow/CMakeLists.txt` 核对。
“已有证据”引用 §8.4–§8.9 的 Release 实测结果；本轮全量回归已再次通过上述全部入口。
这里只验收 development §5.4/requirements §4 的 S2 子范围，不将各场景的 S3–S6 运行义务提前记为完成。

| 场景 | S2 子范围与现有精确 CTest 入口 | 已有证据与后续边界 |
|---|---|---|
| A01 | `core.state_models`、`workflow.model_graph`、`workflow.snapshots`：同类型配置独立，状态域/代次与快照分离 | §8.6–§8.7；S2.7 补实例/操作所有权替身，真实状态 Init/重置归 S3 |
| A03 | `core.identities`、`workflow.copy_subgraph`、`workflow.copy_solution`、`workflow.copy_boundaries`、`workflow.validation_graph`、`workflow.validation_bindings`、`workflow.validation_schema`：身份、改名、复制及悬空引用 | §8.5、§8.7–§8.8；保存加载身份往返归 S3 |
| A04 | `core.identities`、`workflow.result_models`、`workflow.result_store`、`workflow.resolver_isolation`：Session/Run/Scope/Invocation/Attempt、Frame/Correlation 及精确结果读取 | §8.5、§8.7、§8.9；循环、重试和真实业务关联协调仍属后续阶段 |
| A05 | `core.values`、`core.schema`、`core.domain_values`、`workflow.resolver_values`、`workflow.resolver_availability`：Record/List/Optional、合法空值、等待、未产生与失败 | §8.5–§8.6、§8.9；不引入任意类型载荷 |
| A06 | `core.metadata`、`core.constraints`、`core.conversions`、`workflow.validation_bindings`、`workflow.resolver_values`：非相邻引用、字段/固定索引、来源/必填/默认/转换 | §8.5、§8.8–§8.9；动态索引和通用表达式不属于 S2 |
| A07 | `workflow.validation_bindings`、`workflow.resolver_values`、`workflow.resolver_availability`：越界/类型错/来源失败分离，默认与 Optional 不吞错 | §8.8–§8.9；格式化工具的局部容错归 S6 |
| A08 | `core.shared_definitions`、`workflow.resolver_domains`：定义/运行值/消息分离，全局只读版本追溯，预览不消费或写入 | §8.6、§8.9；真实全局/队列服务归 S6 |
| A09 | `core.image_ownership`、`core.domain_values`、`workflow.run_isolation`、`workflow.result_store`、`workflow.resolver_domains`：共享只读、独立图像、裁剪视图及退役后的持有值保活 | §8.6–§8.7、§8.9；取消中的后台操作寿命由 S3 验证 |
| A10 | `core.geometry`、`core.regions`、`workflow.validation_domains`、`workflow.resolver_domains`：多形状、坐标/方向、继承/修正/排除与符号来源冻结 | §8.6、§8.8–§8.9；实际几何/像素计算归 S4/S6 |
| A13 | `workflow.model_graph`、`workflow.validation_graph`：合法兄弟依赖、逆序及控制/数据联合环 | §8.7–§8.8；就绪调度和执行次序归 S3 |
| A23 | `workflow.run_inputs`、`workflow.run_isolation`、`workflow.resolver_domains`：必需类型化输入、请求/作用域隔离、图像只读持有 | §8.7、§8.9；真实 Busy/原子受理和重试重新注入仍未验收 |
| A24 | `workflow.model_structure`、`workflow.copy_subgraph`、`workflow.validation_groups`、`workflow.resolver_domains`：层级、稳定公开映射和禁止 Group 穿透 | §8.7–§8.9；完整 Group 执行、独立宿主服务和导入复用归后续阶段 |
| A26 | `core.masks`、`core.domain_values`、`workflow.validation_domains`、`workflow.resolver_domains`：掩膜所有权、格式/尺寸/坐标、映射和组合能力 | §8.6、§8.8–§8.9；实际重采样、像素成员判定归 S4 |
| A32 | `core.schema`、`core.utf8`、`core.compatibility`、`workflow.validation_schema`：字段/编码、版本/能力及不完整 Schema 拒绝，失败不删声明 | §8.5、§8.8；厂商枚举、字符串/二进制适配及运行库验证归 S3/S6 |

阶段正文另要求的五路分叉、多前驱汇合、基本条件出口及非法回边，由
`workflow.model_graph`、`workflow.validation_graph` 覆盖；资源目录与引用的成功/失败由
`core.resources`、`workflow.validation_domains`、`workflow.resolver_domains` 覆盖。
`core.failure_exit` 验证测试失败能向 CTest 传递，不能把预期失败算入正常检查通过数。

#### 8.10.2 固定组合夹具与证据边界

现有 `resolver_tests.cpp` 的 Harness 已串联配置构造、ConfigSnapshot 捕获、
BindingResolver 工厂内静态校验、人工结果插入与解析，正反例按 §8.9 记录。
S2.7 的 `workflow.composition` 已注册，夹具代码为
[composition_tests.cpp](../tests/workflow/composition_tests.cpp)，Release 27 个检查通过。
它使用固定 UUID、Session/Snapshot/Run 标识，根 Scope 为 0，节点 A–D 分别使用 Invocation 1–4、Attempt 0；
`composition.measurement` v1 Record 含 Float64 `radius` 字段。
公开 `seed` 输入为 2×2 UInt8 Mono 图像，像素为 `[3, 17, 29, 251]`；
人工变换保留 `camera` 到 `measurement` 坐标系、Pixel 单位和平移 `(4.5, -2.0)`。
组合验收按 §5.3 核对以下关系，操作/工厂替身另由运行接口测试负责：

| 固定场景 | 要核对的证据 |
|---|---|
| A 图像→B 位置变换→C 测量记录→D 判断输入 | 控制顺序与独立数据绑定并存；C 可非相邻读取 A 图像，D 从 C 的 Record 字段取得半径，追溯具体调用 |
| 本轮人工半径 12.4 / 新一轮 Pending / 本轮失败 | 值、Pending 和来源失败分开；新轮不读取旧轮 12.4，原始失败诊断保留；默认值/Optional 不吞错由既有 `workflow.resolver_availability` 补充 |
| 编辑原方案和工具默认值、释放人工结果索引 | 已捕获 C 非相邻绑定及 D 默认阈值 13.0 不随编辑变为其他来源或 1.0；已持有的图像在仓库/请求/快照退出后仍保持原像素 |
| `workflow.run_handles`、`core.tool_execution` 与 `workflow.tool_execution` | 固定请求、Busy/拒绝与接受分开；同类型实例隔离、空/错误/重复发行实例拒绝；部分启动错误仍可等待清理；完成、取消请求和物理退出分开；全就绪才固定租约和调用，完成载荷经校验后回到精确结果查询 |

这些是内存模型、人工终态和可控接口替身证据；即使替身方法被调用，也不代表 WorkflowEngine、
控制到达/关闭传播、真实异步回调竞争、OpenCV 检测或设备通信已经实现。
核心接口不反向依赖 workflow，工具输入使用类型化、固定值；运行对象不回写 Node 或普通方案配置。
公共契约与代码已核对，全部必需项和 Release 配置验证通过，**S2 按既定模型与绑定范围通过整体验收**。
实际状态服务暂未接入，含状态的 core 调用返回 MissingCapability；实例寿命/重试能力是给 S3 的声明，
没有实例池、自动重试、生产 Busy 准入或执行器。S3 后续验收不得用上述人工结果和受控替身代替。

#### 8.10.3 S3 交接清单

以下均为后续待实现/验证项。复用入口说明 S2 已提供什么，验收方式说明 S3 还必须证明什么，
不因已有接口或替身测试而提前记为完成。

| S3 工作 | 复用的 S2 接口/资产 | 待实现与验收方式 |
|---|---|---|
| 运行装配、准入和单次句柄 | ConfigSnapshot、RunRequest、校验/解析接口；S2.7 收敛后的 RunHandle/工厂契约 | 每流程一个在途运行与 Busy；两个不同输入请求交错时拒绝方不污染已受理运行；实际退出前不复用实例/独占资源 |
| 顺序、分叉、汇合与基本分支 | Workflow/Group 控制模型、ValidationIndex、五路图夹具 | 模拟五路均执行且逻辑/发送各一次；改变完成次序、重复到达、部分/零选及嵌套关闭；AllActivated 不永久等待未选路径 |
| 数据就绪与错误策略 | BindingResolver 四态、SourceTrace、精确作用域/调用选择 | 合法兄弟依赖只细化就绪顺序；Pending 不忙等或补跑；来源失败阻止下游读取历史，NG 不误判为技术失败，默认 FailFast 有明确终态 |
| 提交、取消及回调竞争 | NodeResult、输出完整性校验、ResultStore 寻址/重复拒绝规则；操作接口 | 用可控假后端覆盖取消先/后于提交、重复完成、旧 Attempt/旧代次迟到；只接受一次结果和控制推进，不能只依赖单线程仓库去保证竞争原子性 |
| 物理退出、保活与关闭 | 只读 ImageHandle/DataValue、固定输入和操作所有权契约 | 完成与退出分离，取消后仍持有输入/实例/租约；关闭停止受理，回调注销确认静默后才释放；等待超时不得伪装为已清理 |
| 实例与状态域 | NodeStateDefinition/NodeState、状态键/代次、同类型隔离夹具 | 实例重建不丢会话累计；Init 按域一次、显式重置、配置换代与在途旧代次共存；创建/调用失败不改配置 |
| 单节点隔离调用 | NodeTarget、固定公开输入、DebugStatePolicy、公开边界校验 | 仅调用目标，不补跑来源或激活外部后继；输入缺失明确失败，隔离调试不污染活动状态；暂不支持的 Group 能力明确拒绝 |
| 结果获取与显示通知边界 | 不可变 NodeResult、精确身份、系统字段投影和保留规则 | 假后端下轮覆盖不改已持有结果；关闭可选显示通知不阻断必要完成；慢观察者不阻塞运行，完整诊断与原码保留 |
| 方案往返与失败保护 | Solution/Workflow、稳定 ID、类型化资源/变量/队列定义、复制重映射 | 无 GUI 加载/执行/保存同一方案；损坏/缺工具/缺资源及未知内容可诊断；加载失败保留有效配置，禁止有损覆盖 |
| 保存提交与资源清理 | 格式/工具版本、路径基准、资源修订声明 | 临时资源/清单写入故障不破坏旧方案；清理失败与提交失败分离；交错保存者验证修订比较与提交的完整协议 |
| 可复现无 GUI 运行入口 | core/workflow 库、固定内存夹具及 §3 构建步骤 | 无窗口初始化即可装配模拟工具并运行；输出追溯快照、运行、实际来源、顺序和诊断；Release 重复验证 |

S4 再接真实视觉算法与几何像素计算；完整循环、多流程协调、连续/外部触发、真实共享读写/队列服务、
设备及 SDK 按原阶段推进。S3 不以线程安全替代工件配对，也不提前建设动态插件或通用重试框架。

### 8.11 Manual 复核与 S3 计划修订记录

- 2026-11-11 基于提交 `b32d6e5` 的文档与代码进行静态核对，形成 S3 实施计划。实现与测试记录从 §8.12 开始。
- 原始资料：[用户手册 V3.0.2](Manual/VisionMaster算法平台用户手册V3.0.2.pdf)、[SDK 开发指南](<Manual/VisionMaster算法平台SDK开发指南V4.3.1（C++）.chm>)、[安装指南](Manual/安装指南.pdf)。用户手册按印刷页定位，PDF 页序加 6；CHM 按章节/接口定位，版本差异沿用 requirements §2.1。
- 结论：保留现有模块边界、控制/绑定分离、稳定身份、快照、不可变结果和受控取消；参考资料不证明 AllActivated 内部协议、保存原子性或注销后静默等自有保证已经实现。

| 复核事实与设计修订 | 文档落点与实施阶段 |
|---|---|
| 用户手册 §5.11.7，116–117 页：每个变量计算节点可选初值及每轮初始化；补工具能力、节点选择、快照有效状态规格的连接 | data-model §2.1/§13、architecture §5；S3.1 明确契约，S3.5 状态服务，S3.7 验证配置往返 |
| 用户手册 §5.11.8，117 页：无匹配不输出；SDK IntResultInfo/ImageResultInfo 区分逐输出有效性；补正常无值、允许 NotProduced、Invalid 的元数据与完成投影 | data-model §8.1.1、requirements A05/A25；S3.1/S3.2/S3.6 验证，不将现有全部输出必需的接口当作已覆盖 |
| SDK“模块相关操作”：独立模块只执行自身，输入需预先满足；现有 NodeTarget 固定输入尚未接入独立调用准备 | data-model §5.5、architecture §5；S3.5 接入，不补跑上游、不伪造结果 |
| SDK 各状态域成功约定不同，回调分别提供模块/算法耗时；补类型化外部诊断和分范围、来源、未知值表示 | data-model §9、requirements A31；S3.1 明确接口，S3.6 模拟验证；不猜参考字段未注明的单位 |
| 用户手册 §5.11.10，124 页图 5-118：循环间隔为毫秒，结束值可绑定匹配个数；补可取消时间间隔和进入循环时固定边界的自有规则 | data-model §5.3、requirements A14、development §7；配置及执行仍归 S6 |
| 安装指南第 11 页支持不安装主界面；版本、授权和运行库匹配属于厂商环境约束 | requirements §2.2、development §2.1；不新增原生核心依赖，真实适配仍归后续阶段 |

S3 实施顺序和验收覆盖统一维护在 §6，场景编号及跨阶段归属以 requirements §9 为准。
该轮复核的验证范围为文档差异、契约与阶段一致性、验收覆盖及本地链接。S3.1 的实际构建和测试见下节。

### 8.12 S3.1 实现与验证记录

2026-11-11 在基于 `b32d6e5` 的 S3.1 工作区完成 Windows x64 Release 构建。
环境为 Visual Studio Professional 2026、MSBuild 18.7.8、MSVC 19.51.36248.0、Windows SDK 10.0.26100.0、
CMake/CTest 4.4.0，构建与运行命令见 [§3.8](#38-s31-release-构建与测试命令)。
CTest 通过 **55/55** 项，包括 48 项 S2 回归及预期失败退出检查。证据位于
`out/build/s31-release/build.log`、`ctest.log` 与 `Testing/Temporary/LastTest.log`。

| S3.1 验证 | 夹具及观察结果 |
|---|---|
| `workflow.execution_scope` | 保留含局部错误的完整快照，并生成合法入口的闭包。范围外前驱和绑定、公开导出越界、逆序及联合等待环、全局重复 ID、缺工具与不支持能力均有诊断，工厂与工具计数为零 |
| `workflow.planned_bindings` | 输入 5 固定到请求，第一节点同步调用输出 7。第二节点先返回等待，再从明确的本轮调用准备值 7，验证期间只启动第一节点 |
| `workflow.independent_plan` | 从独立请求直接准备目标输入 41，保留其来源。Group 内目标绕开外部控制和输入绑定，无法取得的订阅参数定位到参数，工厂与工具计数为零 |
| `core.tool_registry` | 静态注册项拥有描述、版本能力和工厂，工厂按固定配置创建独立实例。类型、版本和能力不匹配在调用工厂前失败 |
| `core.invocation_notifications` | 同步与可控异步夹具分别验证逻辑完成和实际退出，覆盖已完成时登记、一次通知、弱持有及锁外查询 |
| `workflow.state_projection` | 同一工具类型两个节点分别固定初值 −5 和 17，选择每轮与会话状态域。编辑影响新快照，非法映射或状态规格被拒绝 |
| `workflow.completion_contract` | 校验成功 Optional None、空 List、允许的 NotProduced 和消费者缺失策略，拒绝缺槽及非必需 Invalid 的成功完成。外部状态域与原码、实测零及未知耗时保留到结果 |

S3.2 的 `WorkflowEngine` 复用 `ExecutionPlan` 的固定请求、控制入口、依赖索引和工具配置。
计划默认接受同步、按调用创建实例的线性流程；其他能力由计划使用方声明。
各后续能力的实际验证记录从 §8.13 起维护。
本次验证使用内存方案及模拟工具。

### 8.13 S3.2 实现与验证记录

2026-11-11 在基于 `b32d6e5` 且包含 S3.1 的工作区完成 S3.2 Windows x64 Release 构建。
使用 Visual Studio Professional 2026、MSBuild 18.7.8、MSVC 19.51.36248.0、Windows SDK 10.0.26100.0
及 CMake/CTest 4.4.0。复现命令见 [§3.9](#39-s32-release-构建与测试命令)。
CTest **59/59** 项通过，包括 4 项新增测试及 55 项既有回归。构建和测试证据位于
`out/build/s32-release/build.log`、`ctest.log` 和 `Testing/Temporary/LastTest.log`。

| Release 测试 | 固定夹具及实际结果 | 检查数 |
|---|---|---|
| `workflow.execution_chain`（A04、A05、A06、A08、A17） | 三个模拟节点按控制顺序调用。首节点输出含 7 和 11 的 List，检测 NG 后继续。末节点跨节点读取字段及索引，将 11 显式转换为 Float64，再加全局版本 7 对应的值 5，发布结果 16。结果与追溯分别保留 Optional None、合法 NotProduced、调用键和来源版本 | 33 通过 |
| `workflow.execution_fail_fast`（A05、A06、A07、A17、A25） | 先完成一轮成功调用，再验证显式失败和非必需输出 Invalid。失败轮只调用首节点，后继记录 Skipped/FailFast。空 List 越界、未接受的不产出和全局值缺失或版本不符均定位到绑定，既有结果仍按原运行身份可查 | 58 通过 |
| `workflow.atomic_admission`（A23、A29） | 两个不同快照和输入竞争同一流程，只有一个受理，另一请求返回该轮的 Busy。第二流程独立受理为 Pending。首流程实例清理期间仍 Busy，清理后运行编辑后的快照，输出 131 | 33 通过 |
| `workflow.sync_run_lifecycle`（A23、A29） | 条件变量控制同步调用与清理。等待期限到达不改变运行终态，取消先于提交时不发布算法输出，后继无调用键。丢弃句柄后运行仍结束，引擎销毁后保留句柄结果。禁用入口关闭线性链，独立节点执行明确拒绝 | 40 通过 |
| `workflow.run_isolation`（A08、A23，扩充既有用例） | 固定调用方提供的全局值、版本和更新来源，原容器变化不影响请求。验证重复、未知、类型不匹配及跨会话条目 | 28 通过 |

`WorkflowEngine` 在 S3.2 贯通受理、按控制顺序调用、结果提交和查询。单工作线程按受理顺序执行各流程。
异步通知、迟到结果仲裁和实际退出的验证见 §8.14。

### 8.14 S3.3 实现与验证记录

2026-11-12 在基于 `b32d6e5` 且包含 S3.1–S3.2 的工作区完成 S3.3 Windows x64 Release 构建。
使用 Visual Studio Professional 2026、MSBuild 18.7.8、MSVC 19.51.36248.0、Windows SDK 10.0.26100.0
及 CMake/CTest 4.4.0。复现命令见 [§3.10](#310-s33-release-构建与测试命令)。
CTest **62/62** 项通过，包括 3 项新增集成测试和 59 项既有回归。构建和测试证据位于
`out/build/s33-release/build.log`、`ctest.log` 和 `Testing/Temporary/LastTest.log`。

| Release 测试 | 固定夹具及实际结果 | 检查数 |
|---|---|---|
| `workflow.async_completion`（A04、A16、A18、A27、A29） | 模拟异步首节点等待时，独立同步流程可以完成。拒绝各身份分量不符、错误输出类型、重复完成和旧轮完成/退出。已接受的 NG 结果 11 在清理后推进一次后继，得到 12。新一轮得到 24。登记前已完成并退出的操作仍只推进一次 | 57 通过 |
| `workflow.async_cancellation`（A16、A18、A23） | 取消先于提交时立即得到逻辑 Cancelled，协作请求发送一次，后台迟到结果不覆盖取消。取消后于首节点提交时保留已发布值 15，跳过后继。两种情况都等待实际退出才达到 Quiescence | 40 通过 |
| `workflow.async_lifetime`（A09、A16、A23、A29） | 用裁剪图像和修订 7 的模拟租约验证保活。逻辑成功后等待 Quiescence 仍可超时，丢弃句柄和实例析构期间同流程仍 Busy。析构返回后释放上下文、租约与实例并归还槽位，下一轮正常返回 25 | 28 通过 |
| `core.tool_execution`（扩充既有用例） | 后端退出报告与在途取消交错时保持实际退出未达，覆盖取消回调重入完成/退出并抛出异常，以及启动中提前报告退出时的实例占用 | 113 通过 |
| `workflow.tool_execution`（扩充既有用例） | 等待或不可用绑定不获取租约。固定槽全部就绪后核对资源身份和版本，获取失败或异常时归还已获取的租约 | 49 通过 |

析构门闩用例复现了旧清理顺序在工具实例析构前释放上下文的问题。调整持有顺序后重新构建并通过全量测试。
S3.3 在线性流程的前一操作实际退出并清理后启动后继，异步等待期间其他流程可推进。
分叉、汇合及分支关闭传播的验证见 §8.15。

### 8.15 S3.4 实现与验证记录

2026-11-12 在基于 `b32d6e5` 且包含 S3.1–S3.3 的工作区完成 S3.4 Windows x64 Release 构建。
使用 Visual Studio Professional 2026、MSBuild 18.7.8、MSVC 19.51.36248.0、Windows SDK 10.0.26100.0
及 CMake/CTest 4.4.0。复现命令见 [§3.11](#311-s34-release-构建与测试命令)。
CTest **66/66** 项通过，包含 4 项新增集成测试和 62 项回归。构建和测试证据位于
`out/build/s34-release/build.log`、`ctest.log` 和 `Testing/Temporary/LastTest.log`。

| Release 测试 | 固定夹具及实际结果 | 检查数 |
|---|---|---|
| `workflow.fork_join`（A09、A11、A17） | 五路共享只读图像，各自产生独立图像。测量按 5、1、4、3、2 路完成，最后一路退出后汇合与模拟发送各调用一次。拒绝重复完成和退出，NG 照常到达，16 条控制边最终到达。发送值按绑定顺序为 `[51, 21, 61, 31, 41]` | 80 通过 |
| `workflow.sibling_dependencies`（A06、A13） | 显式入口等待实际入边，已获控制许可的兄弟节点等来源提交值 40 后输出 41，并记录实际调用身份。来源仍在等待退出。逆序引用与控制、数据联合等待环在工具调用前拒绝 | 24 通过 |
| `workflow.branch_selection`（A11、A12） | 外层多选及内层部分选择送出 `[33, 11]`，内层零选时送出 `[11]`。外层零选关闭子路径，跳过汇合与发送，无条件分路仍执行。缺失来源保留 NotSelected 追溯，未声明接受的缺失在汇合调用前报告 InputUnavailable | 80 通过 |
| `workflow.branch_fail_fast`（A12、A17） | 已选异步分路技术失败时保留原诊断和已有的 NotSelected 关闭，以 Failed 关闭剩余待定边，跳过汇合和发送。向在途兄弟发出一次取消请求，拒绝其迟到输出，并在实际退出后完成清理 | 36 通过 |

`WorkflowEngine` 在根作用域的无环控制图上维护逐边状态和就绪队列。控制前驱清理后交付到达或正常关闭，
汇合在所有实际入边确定且有实际或虚拟到达时取得一次调度许可。已获控制许可的兄弟节点可在来源结果提交时恢复绑定准备。
本次验证使用内存方案和模拟工具。S3.5 接续状态服务与单节点调用。

### 8.16 S3.5 实现与验证记录

2026-11-12 在基于 `413af3c` 的工作区完成 S3.5 Windows x64 Release 构建。
使用 Visual Studio Professional 2026、MSBuild 18.7.8、MSVC 19.51.36248.0、Windows SDK 10.0.26100.0
及 CMake/CTest 4.4.0。复现命令见 [§3.12](#312-s35-release-构建与测试命令)。
CTest **70/70** 项通过，包含 4 项新增集成测试和 66 项回归。构建与测试证据位于
`out/build/s35-release/` 下的 `build-libraries.log`、`build.log`、`ctest.log` 和 `Testing/Temporary/LastTest.log`。

| Release 测试 | 固定夹具及实际结果 | 检查数 |
|---|---|---|
| `workflow.state_lifetimes`（A01、A02） | 同类型计数器分别使用初值 −5、17、31。前两轮每轮域分别输出 −2、−1，会话域分别累计到 20、24。显式重置后目标从 17 累计到 19，另一会话节点累计到 41。实例按调用创建，Init 按域执行。改名和布局保留代次，改变初值换代，两种兼容策略对绑定编辑的处理均通过 | 117 通过 |
| `workflow.debug_state_isolation`（A01、A30） | 两次默认隔离调用从调试初值 80 得到 85、81。原会话值从 20 继续累计到 22。显式 Active 单节点调用复用活动代次，累计到 29，下一轮到 30 | 46 通过 |
| `workflow.state_acceptance`（A02、A16、A18） | 异步成功发布暂存值 12。无效完成、技术失败和取消后的迟到成功均未改变该值，下一次调用从 12 累计到 13。旧代次保留到实际退出，忙时重置和使用活动状态的调试受阻，编辑后的新代次从 30 累计到 32。DuringComputation 已写入的 13 在取消后仍可读取 | 51 通过 |
| `workflow.independent_execution`（A30） | 目标单独运行并追溯 Provided 和 Default，输出 51、4，各自按本轮身份查询。缺输入和不能满足的上游参数订阅在工具调用前报告诊断。启用 Group 内的普通工具可作为目标，Group 目标明确报缺少能力 | 42 通过 |
| `core.tool_execution`（扩充既有用例） | 验证 Init 的类型及异常诊断、状态访问的调用身份与一次性绑定、完成封写及实际退出撤销访问 | 133 通过 |
| `workflow.sync_run_lifecycle`（扩充既有用例） | 独立调用只创建目标实例，清理期间同流程提交返回 Busy | 47 通过 |

状态访问和代次契约见 [data-model.md §13.1](data-model.md#131-s35-状态访问与代次)，
单节点准入和重置入口见 [data-model.md §4.9](data-model.md#49-s35-状态服务与单节点调用)。
本次验证使用内存方案和模拟工具。S3.6 接续结果交付、通知和会话生命周期。

### 8.17 S3.6 实现与验证记录

2026-11-12 在基于 `3cca2cc` 的工作区完成 S3.6 Windows x64 Release 构建。
使用 Visual Studio Professional 2026、MSBuild 18.7.8、MSVC 19.51.36248.0、Windows SDK 10.0.26100.0
及 CMake/CTest 4.4.0。复现命令见 [§3.13](#313-s36-release-构建与测试命令)。
CTest **74/74** 项通过，包含 4 项新增集成测试和 70 项回归。
`out/build/s36-release/` 保留库、全量和定向构建日志，以及 `ctest.log` 和 `Testing/Temporary/LastTest.log`。

| Release 测试 | 固定夹具及实际结果 | 检查数 |
|---|---|---|
| `workflow.result_retention`（A09、A22、A25） | 后端复用图像、UTF-8 文本和 Binary 缓冲。容量 1 淘汰旧索引后，在途消费者仍读到原图像像素 49。历史查询返回 ResultRetired，已持有结果在会话关闭和引擎销毁后仍可读取。容量 0、在途待提交查询和身份不符也通过 | 38 通过 |
| `workflow.external_result_delivery`（A05、A25、A31、A32） | 调用成功码 0、输出成功码 1 和业务判定分别映射。保留未知原码、未知耗时单位、缺测和实测零。逐槽区分 0、空 List、Optional None 和合法 NotProduced。截短图像、负长度、正计数空指针、非法 UTF-8、非必需 Invalid 和输出枚举缺项均报诊断 | 71 通过 |
| `workflow.run_observations`（A27） | 双节点异步链交付两次结果、逻辑终态和实际清理。容量 2 的慢订阅保留最后两条并记录淘汰 2 条。等待中和有积压的订阅均可注销，其他订阅与运行结果照常完成 | 77 通过 |
| `workflow.session_lifecycle`（A22、A27、A28） | 缺工具或版本能力不符的候选保留原活动快照及观察队列。切换等待旧后端实际清理，旧结果 41 仍可查询。新会话结果 72 使用独立会话身份，拒绝旧完成通知。关闭清空观察队列，拒绝新提交，等在途调用退出后进入 Closed | 50 通过 |

关闭用例用独立观察订阅确认关闭生效后再检查原队列已清空。该用例在 Release 下连续通过 15 次，记录见 `ctest-session-repeat.log`。
结果、观察和会话接口见 [data-model.md §4.10](data-model.md#410-s36-结果交付与会话生命周期)。
本次验证使用内存候选和模拟后端。S3.7 接续文件格式、资源解析和诊断加载。

### 8.18 S3.7 实现与验证记录

2026-11-12 在基于 `93a3fcd` 的工作区完成 S3.7 Windows x64 Release 构建。
使用 Visual Studio Professional 2026、MSBuild 18.7.8、MSVC 19.51.36248.0、Windows SDK 10.0.26100.0
及 CMake/CTest 4.4.0。复现命令见 [§3.14](#314-s37-release-构建与测试命令)。
CTest **77/77** 项通过，包含 3 项新增集成测试和 74 项回归。
`out/build/s37-release/` 保留库、定向和全量构建日志，以及 `ctest-s37-tests.log`、`ctest.log` 和 `Testing/Temporary/LastTest.log`。

| Release 测试 | 固定夹具及实际结果 | 检查数 |
|---|---|---|
| `infrastructure.solution_roundtrip`（A01、A03、A08、A19、A24、A32） | 中文目录中的方案往返保留稳定 ID、Group 公开边界、控制边、包含、绑定、坐标与布局。Int64 上下限、空 List、嵌套 Optional、Binary 和含掩膜排除的 ROI 恢复原值。两节点分别恢复每轮状态和会话累计策略。资源、变量和队列定义均保留，再写清单的字节一致 | 21 通过 |
| `infrastructure.file_resource_execution`（A19、A28） | 中文托管路径、命名根和绝对路径全部解析。托管文件删除及锁定外部文件覆盖后，已加载方案仍读到原始字节。动态文件先后读到 `x` 和 `n`，删除后在创建实例前拒绝。重新加载识别摘要不符，失败候选保留活动快照 | 16 通过 |
| `infrastructure.solution_diagnostic_loading`（A03、A19、A28） | 缺工具和缺资源定义的已知方案可诊断打开。选中缺失依赖的流程被拒绝，独立完好的流程成功运行。损坏 JSON、重复键、未知版本有明确诊断，未知嵌套字段定位到 JSON Pointer 并阻止保存和执行。发布冲突保留原始清单，失败加载保留活动会话 | 22 通过 |

新清单使用同卷无覆盖发布。已有方案的修订比较、覆盖提交和资源清理由 S3.8 接续。
清单字段及加载入口见 [data-model.md §10.2.1](data-model.md#1021-s37-清单与加载接口)。

### 8.19 S3.8 实现与验证记录

2026-11-12 在基于 `48aa494` 的工作区完成 S3.8 Windows x64 Release 构建。
使用本地固定 NTFS 卷、Visual Studio Professional 2026、MSBuild 18.7.8、MSVC 19.51.36248.0、
Windows SDK 10.0.26100.0 和 CMake/CTest 4.4.0。复现命令见 [§3.15](#315-s38-release-构建与测试命令)。
CTest **80/80** 项通过，包含 3 项保存事务测试和 77 项回归。
`out/build/s38-release/` 保留构建日志、`ctest-save.log`、全量 `ctest.log` 和 `Testing/Temporary/LastTest.log`。

| Release 测试 | 固定夹具及实际结果 | 检查数 |
|---|---|---|
| `infrastructure.solution_save_failures`（A19、A20） | 在第二个资源写入、临时清单写入和提交前注入故障，另用 Windows 句柄阻止真实替换。每次均保留原清单和资源字节，重新加载后运行得到 `a`，并回收已写入的暂存文件。外部文件写入请求和含未知字段的覆盖保存均被拒绝 | 43 通过 |
| `infrastructure.solution_save_conflict`（A20） | 第一个保存者暂停在提交前，第二个在进入暂存前等待。首个提交后，第二个收到 `SolutionSaveConflict`，重载保留首个修改并运行得到 `x`。同修订的外部字节修改和暂存期间的外部修改均报告冲突。重新加载后可继续保存配置并复用当前托管文件 | 22 通过 |
| `infrastructure.solution_save_cleanup`（A19、A20） | 活动运行继续持有旧文件并读到 `a`。实际退出后重试清理删除旧文件，重载运行得到 `x`。提交后的注入清理故障单独报告。摘要不符和当前外部别名均保留对应文件 | 34 通过 |

S3.8 验证时，保存用 `LockFileEx` 独占包目录锁文件的字节范围，在同一锁内复核摘要，再用 `MoveFileExW` 同卷替换清单。S4.6 在该锁内改为逐字节比较清单原文。
对非协作写入的检测截至替换前最后一次比较。断电恢复和网络共享属于后续后端验证范围。 [LockFileEx](https://learn.microsoft.com/en-us/windows/win32/api/fileapi/nf-fileapi-lockfileex)、[MoveFileExW](https://learn.microsoft.com/en-us/windows/win32/api/winbase/nf-winbase-movefileexw)

接口、版本分配和清理重试见 [data-model.md §10.4.1](data-model.md#1041-s38-保存与清理接口)。

### 8.20 S3.9 与 S3 整体验收记录

2026-11-12 在基于 `0c7fab5` 的工作区验证 S3.9。环境为 Windows x64、本地固定 NTFS、
Visual Studio Professional 2026、MSBuild 18.7.8、MSVC 19.51.36248.0、Windows SDK 10.0.26100.0 和 CMake/CTest 4.4.0。
复现命令见 [§3.16](#316-s39-构建与命令行验证)。
Release 完成全量构建，CTest **82/82** 项通过，包含 2 项新增命令行用例及 80 项回归。
`out/build/s39/` 保存 `build-release.log`、`ctest-release.log` 及 Release 配置的 `LastTest` 日志。

| CTest 入口 | 固定夹具及结果 | Release 的检查数 |
|---|---|---|
| `app.cli_roundtrip` | 独立 CLI 进程加载含中文和空格路径的方案。托管字节 `abc` 提供 97，公开输入 `offset=7` 与保存的常量共同产生五路交付 `[104, 99, 100, 101, 102]`。11 条控制边全部到达，汇合与模拟交付各调用一次。条件掩码 3、1、0 分别交付 `[11, 22]`、`[11]` 和跳过汇合与交付。`save` 将修订 12 提交为 13；新进程重新加载后，同一输入的输出、来源关系、派发顺序、控制边和关闭原因全部一致 | 269 通过 |
| `app.cli_diagnostics` | 核对帮助、参数错误、缺必填输入与数字越界。已受理的加法溢出运行报告 `Failed` 和 `FailFast`，保留终态摘要。未知字段拒绝覆盖保存并保留原清单及资源。损坏清单与缺失资源返回可定位诊断 | 21 通过 |

回归用例 `workflow.sibling_dependencies` 验证控制就绪的消费者等待实际生产者，并在追溯中排在生产者之后，25 项检查通过。
用 `dumpbin /DEPENDENTS` 检查 Release 命令行程序，直接依赖为 Windows 系统库和 MSVC 运行库。

Release 通过日志覆盖验收矩阵引用的 41 个测试入口和 S2 的 48 个回归入口。**S3 的 26 项必验子范围全部通过**。
场景、行为和测试入口见 [S3 整体验收矩阵](s3-acceptance.md#场景证据)。

S4 复用静态注册、无 GUI 命令行装配、运行快照和文件方案存储，接续本地图像、二值化、Blob 分析及结果判断的真实算法验证。

### 8.21 S4.1 实现与验证记录

2026-11-15 在 Windows x64、Visual Studio Professional 2026、MSBuild 18.7.8、MSVC 19.51.36248.0、
Windows SDK 10.0.26100.0、CMake/CTest 4.4.0 验证。复现命令见 [§3.17](#317-s41-release-构建与验证)。

两个独立 Release 构建均通过。ON 通过 **84/84** 项 CTest，包含全部既有回归和两个 S4.1 用例。
OFF 通过 **82/82** 项 CTest，CMake 缓存无 `OpenCV_DIR`，39 个 Release 可执行文件均无 OpenCV 或 Qt 的 DLL 导入。
构建与测试日志分别保存在 `out/build/s41-on/` 和 `out/build/s41-off/` 的 `build.log` 与 `ctest.log`。

沿用本机已有的 `OpenCV-4.12/build-vs/install` 安装产物。`opencv_version.exe --verbose` 报告其实际版本为 `4.14.0-pre`，
源码修订为 OpenCV `5d88781f74b49ec6c2f609b273ce05241d8803f1` 和 opencv_contrib `115e941b7a2a95a2855a7cfa7332ea64707a8f3b`。
现有工程和安装包均使用 VS 2026、MSVC 19.51.36248.0、x64 和 Release `/MD`。
包的 CMake 配置从 `opencv_world4140` 提供 `core`、`imgproc`、`imgcodecs` 三个组件。
`dumpbin /HEADERS` 确认 DLL 的机器类型为 `8664 (x64)`，`dumpbin /DEPENDENTS` 确认其依赖 Windows 系统库（含 Media Foundation）、UCRT 和
`MSVCP140.dll`、`CONCRT140.dll`、`VCRUNTIME140.dll`、`VCRUNTIME140_1.dll`。PNG 使用随包构建的 libpng 1.6.53。

| 相对安装根目录的文件 | SHA-256 |
|---|---|
| `x64/vc18/lib/OpenCVConfig.cmake` | `b141a6fdca3add5be1105b4bfde155a098a7029cf370fe612b1b89eecaf23f8e` |
| `x64/vc18/lib/OpenCVConfig-version.cmake` | `343f3d3fef18aed6c6996ee48d53827a158e2e7d53e90855329c1b9cf4571bd7` |
| `x64/vc18/lib/OpenCVModules.cmake` | `5f86aed1d0943768c689b82752c3cc35bd8fed4aa0ada1a3cdff591923cb66b9` |
| `x64/vc18/lib/OpenCVModules-release.cmake` | `376f2a097e3a77f126368feee3431b96d51dc6e4ec6035042908bc43b8ec3a1b` |
| `x64/vc18/lib/opencv_world4140.lib` | `00afe3d9fa621c399ed289019849b96eb0932b456525075dbda721ddb4cbcbe1` |
| `x64/vc18/bin/opencv_world4140.dll` | `0bdee6e91162a95d7daf03c254c2ebeb30b2b85359d0de28aad6de1d6ebbfe2d` |

`tools.opencv_release_smoke` 使用两张独立构造的 2×2 图，检查原始解码、重新编码后的文件签名与像素、DLL/头文件版本一致性。

| 输入 | SHA-256 | 实测结果 |
|---|---|---|
| 71 字节 Mono8 PNG | `7d9c8022352ed64bc7f0873007256b8374015fd54ff1887bafbc0845e6756cbd` | 像素 `[0,128;129,255]`，阈值 128 得 `[0,0;255,255]`，非零计数 2 |
| 70 字节 24 位 BMP | `1f1280b485e1c861fe6523402e0d03f24bde1b1d0a033f60d54672332358de51` | BGR 蓝、绿、红、白，灰度转换得 `[29,150;76,255]` |

`tools.vision_contracts` 通过 core 校验四个版本 1 工具描述，并验证参数边界、类型化输出与绑定、可选资源和适配诊断。
支持的几何、掩膜和继承配置通过检查，Projective、有界修正、继承排除、Linear 掩膜采样等越界能力按区域定位返回 `UnsupportedRegion`。

ON 配置在 OpenCV 查找被禁用、指定 4.12.0、选择 Debug 或显式指定 `/MT` 时均按预期失败。
分别给 ON 和 OFF 的 CLI 加载使用四种视觉类型的单节点方案，都在受理前返回带节点和工具定位的 `UnknownTool`，退出码为 1。

### 8.22 S4.2 实现与验证记录

2026-11-15 在 Windows x64、MSVC 19.51.36248.0、Release `/MD` 验证，沿用 S4.1 的 OpenCV `4.14.0-pre` 安装包。
复现命令与本轮测试范围见 [§3.18](#318-s42-release-构建与验证)。两套 CLI 的 `--help` 均成功，ON 的目录列出 `visionpro.image.read_file`。

两个独立 Release 构建均通过。ON 的所选 CTest **79/79** 项通过，覆盖核心、流程、视觉工具与图像摘要。OFF 的核心和流程 CTest **74/74** 项通过。
构建与测试日志保存在 `out/build/s42-on/` 和 `out/build/s42-off/` 各自的 `build.log` 与 `ctest.log`。

| 用例 | 实测结果 |
|---|---|
| `core.image_ownership`、`tools.image_bridge` | 带填充的 4×3 Mono8 裁剪窗口复制为 `[12,13,14;22,23,24]`。分别修改 Mat 和发布图像的源缓冲，另一侧保持原像素。裁剪派生图保留当前和根坐标、偏移、帧身份及采集时间。尺寸、步长、长度、溢出与不支持的像素格式可诊断。 |
| `tools.image_read` 的实际文件 | 中文和空格路径下的 2×2 PNG，两模式均得到 Mono8 `[0,128;129,255]`。BMP 的 Native8 保留蓝、绿、红、白的 BGR 像素，Gray8 得 `[29,150;76,255]`。记录指向本次 `source` 租约。 |
| `tools.image_read` 的固定租约 | 真实 provider 取得租约后覆盖源文件。本轮仍得到 `[0,128;129,255]`，下一轮得到 `[9,17;33,65]`。替换图的 EXIF 方向为 6，像素顺序保持原样。 |
| `tools.image_read` 的诊断 | 空与截断图像返回 `InvalidImage`，UInt16 和带 Alpha 的 PNG 返回 `UnsupportedPixelFormat`。非零原点定位到 `frame_origin`，未声明的坐标系在受理前失败。失败的解码没有有效图像输出。 |
| `app.vision_image_summary` | 同一摘要入口读取真实 BMP 结果，输出 2×2 UInt8 BGR、当前/根坐标 `camera`、原点 `(0,0)` 和空采集元数据。 |

### 8.23 S4.3 实现与验证记录

2026-11-15 在 Windows x64、MSVC 19.51.36248.0、Release `/MD` 验证，沿用现有 OpenCV `4.14.0-pre`。
复现命令和本轮测试范围见 [§3.19](#319-s43-release-构建与验证)。

两个独立 Release 构建均通过。ON 执行的 CTest **82/82** 项通过，覆盖核心、流程、视觉工具和图像摘要。OFF 的核心和流程用例 **74/74** 项通过。
构建和测试日志分别位于 `out/build/s43-on/` 和 `out/build/s43-off/` 下的 `build.log`、`ctest.log`。

| 用例 | 实测结果 |
|---|---|
| `tools.roi` V07–V08 | 9×9 旋转矩形精确得到计划中的 7 个成员。继承后平移并扣除外层排除得到 6 个成员，加入来源自身的排除后为 5。分层方向、仿射修正、跨单位修正及展平后再求值均符合预期。 |
| `tools.roi` V09 | `(-1,0,3,2)` 在 `Reject` 下报 `ImageBounds`，`Intersect` 下为 4 个成员。明确区分贴边、包含远端上界和微超界，验证内层的越界与空域限制、全排除的零掩膜，以及无法无损发布的分层策略。 |
| `tools.mask` V10–V11 | 固定 3×2 掩膜的 `NonZero`、`AtLeastThreshold(128)` 分别为 4、2 个成员。2×2 掩膜平移 `(+1,0)` 后成员为 `(1,0)`、`(2,1)`，半像素和源外取样符合约定。附加 ROI 修正后局部映射仍生效。 |
| `tools.mask` 资源与诊断 | 中文路径的实际文件通过固定租约解码。替换磁盘文件后仍可按两种有效规则读取同一租约内容。验证掩膜排除、空域、缺槽、额外槽、身份、种类、显式版本、坏编码、尺寸和像素格式诊断。 |
| `tools.roi_runtime` | 两个节点均调用实际求值器，通过 `WorkflowEngine` 发布和继承 `Region2D`。第一轮上游裁剪后为 5 个像素，下游依据完整冻结定义重新展开后为 6 个。第二轮加入来源排除后，上游为 6 个、下游为 5 个。两轮逐一核对 `RunBindingTrace::regions` 中的来源定义、修正、路径和 `ResultIdentity`。 |

### 8.24 S4.4 实现与验证记录

2026-11-15 在 Windows x64、MSVC 19.51.36248.0、Release `/MD` 验证，沿用现有 OpenCV `4.14.0-pre`。
两个 Release 构建均通过。ON 的核心、流程、视觉工具与摘要 CTest **83/83** 项通过，OFF 的核心与流程 CTest **74/74** 项通过。
ON 的 CLI `--help` 列出读图和二值化工具，OFF 列出模拟工具。
复现命令见 [§3.20](#320-s44-release-构建与验证)。日志位于 `out/build/s44-on/` 和 `out/build/s44-off/` 下的 `build.log`、`ctest.log`。

| 用例 | 实测结果 |
|---|---|
| `tools.threshold` V01 阈值边界 | `[0,127,128,129,255]` 在阈值 128 得正相 `[0,0,0,255,255]` 与反相 `[255,255,255,0,0]`。阈值 0、255 的等值边界及局部 ROI 外清零均通过。局部 ROI 的阈值 255 反相结果为 `[0,255,255,255,0]`。 |
| `tools.threshold` V06 二值化 | 4×3、步长 6 的输入裁剪到 3×2，在阈值 13 得 `[0,0,255;255,255,255]`。二值图与掩膜存储独立于输入。原图和裁剪视图像素保留，二值图保留坐标、原点、帧身份及采集时间。 |
| `tools.threshold` 继承与坐标 | 两个真实节点发布与消费 `effective_roi`。旋转 ROI 的 7 个成员经平移和排除得到 6 个，核对上游冻结定义和调用身份。实际来源坐标错配时在消费节点的 `roi.source` 报 `CoordinateMismatch`。 |
| `tools.threshold` 文件掩膜与失败路径 | 中文路径的实际 PNG 通过 `mask` 租约产生成员掩膜 `[0,0,255,255]`，反相结果为 `[0,0,255,0]`。越界阈值、BGR、UInt16 定位到对应参数或输入。全排除在 `Error` 下失败且无有效输出，`ValidEmptyOutput` 发布全零图与掩膜。 |
| `app.vision_image_summary` | 保留真实 BMP 的 BGR 摘要验证，并用 Gray8 读图和两个真实阈值节点核对图像元数据、展开区域和冻结生产者身份。掩膜成员数由 3 变为 1。 |

### 8.25 S4.5 实现与验证记录

2026-11-15 使用 Windows x64、MSVC 19.51.36248.0、Release `/MD` 构建，沿用 OpenCV `4.14.0-pre`。
ON 的核心、流程、视觉工具和摘要 CTest **85/85** 项通过，OFF 的核心与流程 CTest **74/74** 项通过。
启用 OpenCV 时 CLI `--help` 列出四个视觉工具。复现命令与日志路径见 [§3.21](#321-s45-release-构建与验证)。

| 用例 | 实测结果 |
|---|---|
| `app.vision_image_summary` V02 | 12×10 图像经真实二值化和 Blob 后得到 2 个组件，面积依次为 6、9。总面积为 15，最大面积为 Some(9)。质心为 `(2,1.5)`、`(8,6)`，包围框为 `(1,1,3,2)`、`(7,5,3,3)`。 |
| `tools.blob` V03–V04 | 对角像素在 4 连通下分成两个面积为 1 的组件，8 连通下为一个面积为 2 的组件，质心为 `(0.5,0.5)`。单像素和有孔图形的面积为 1、8。全零图像成功产生空 List 与 None。首像素排序用例中组件面积为 2、8，后者包围框原点更靠左。 |
| `tools.blob` V06、V08 | 裁剪图的面积为 4，质心在当前帧为 `(1.25,0.75)`，按原点偏移换算到根帧为 `(2.25,1.75)`。两条真实阈值输出分别供给图像与继承 ROI，平移并排除后总面积为 6，追加来源排除后为 5。核对冻结来源身份及原图像素。 |
| `tools.blob` 边界、空域及掩膜 | 验证 Reject、Intersect、Error 和 ValidEmptyOutput。经中文路径的 PNG 租约读取掩膜，阈值 129 的成员规则产生一个面积为 2 的组件，并核对实际资源引用。 |
| `tools.judgement` 闭区间 | 覆盖区间内外、等值和 Int64 两端。OK 与 NG 均发布 Bool 并成功完成。反序区间在派发前报告 `ConstraintViolation`，定位到 `maximum`。 |
| `app.vision_image_summary` V05 | `count=2` 对 `[2,2]` 判 OK、对 `[3,3]` 判 NG。NG 后继继续执行，以 `components[0].area_pixels` 绑定 6 并判 OK。第二轮空结果的最大面积摘要为 `null`，访问 `[0]` 得到定位到选择器的 `IndexOutOfRange`。 |

### 8.26 S4.6 实现与验证记录

2026-11-15 在 Windows x64、MSVC 19.51.36248.0、Release `/MD` 构建，沿用 OpenCV `4.14.0-pre`。
ON 的核心、流程、视觉工具与 CLI CTest **90/90** 项通过，OFF 的核心与流程 CTest **74/74** 项通过。
复现命令、日志路径及无 SHA-256 的验证范围见 [§3.22](#322-s46-release-构建与验证)。

| 用例 | 实测结果 |
|---|---|
| `workflow.vision_single_chain` | 真实 BMP 读图、二值化和 Blob 得到面积 6、9。检测判定为 NG，技术运行成功。 |
| `workflow.vision_integration` V12 | 按阈值 100、128、180、210、250 得计数 `[2,2,2,0,0]` 和判定 `[OK,OK,OK,NG,NG]`。五路各运行一次，AllActivated 汇合和模拟交付各一次。核对精确来源、结果存储和原图像素。 |
| `workflow.vision_lifecycle` V15–V16 | 独立注入图像只执行目标阈值或 Blob。Busy 请求的图像未进入后续运行，编辑后的快照采用新阈值和 ROI，已持有的旧图像跨历史淘汰和引擎销毁仍可读。门闩暂停真实阈值返回，取消拦截输出与后继，实际退出后空图下一轮得到 0 个组件。 |
| `app.vision_roundtrip` V08、V13 外部资源 | 独立 CLI 进程从不同工作目录执行、保存并重载含中文和空格路径的 PNG/BMP。修订 12→13，模拟条件分支也通过。逐像素检查读图、二值图和旋转掩膜。两路阈值分别提供整幅前景和冻结 ROI，平移并排除后 Blob 总面积为 6，核对当轮生产者与输出端口。清单同修订的空白变化触发 `SolutionSaveConflict`。 |
| `app.vision_diagnostics` | 上轮有效图像被损坏后，本轮报告 `InvalidImage` 并给出终态摘要，后继按 FailFast 跳过。删除图像后在运行受理前报告 `ResourceNotFound`。 |
| `app.vision_image_summary` | 验证 Float64、UTF-8 字符串和嵌套枚举，维持已有 Blob 和 ROI 摘要。带填充的图像按有效行统计，直接掩膜按成员规则计数。 |

### 8.27 S4.7 整体验收与交接记录

2026-11-15 使用 Windows x64、MSVC 19.51.36248.0、Windows SDK 10.0.26100.0 和 CMake/CTest 4.4.0，从两个独立目录完成 Release `/MD` 构建。ON 沿用 OpenCV `4.14.0-pre`，CLI 导入 `opencv_world4140.dll`。OFF 的 39 个 Release 可执行文件均无 OpenCV 或 Qt DLL 导入。

ON 的 **97/97** 项和 OFF 的 **81/81** 项适用回归均通过。本轮两套矩阵各排除 7 项会实际核对摘要的既有用例，新增 6 项无摘要外部资源和 CLI 回归。
固定方案的三条流程在修订 1 与保存后的修订 2 均成功运行，稳定输出、控制边和本轮来源关系一致。
[构建命令与日志路径](#323-s47-release-构建与回归) [验收证据与交接](s4-acceptance.md)

## 9. 第五阶段实施与验收计划

2026-11-16 完成 **S5.1–S5.6** 及第五阶段整体验收。
功能范围由 [requirements §7](requirements.md#7-第五阶段基础可视化界面) 定义，
界面适配职责见 [architecture §4](architecture.md#4-配置模型与编辑器)，运行与持久化契约见 [data-model](data-model.md)。
本节记录实施顺序、验收基准和各阶段结果。综合验收使用 S4 固定输入和 S5 桌面入口，证据见 [§9.10](#910-s56-综合验收与交接)。

### 9.1 已确定的实施基线

- 使用 §2 的现有 Qt 与 Qt Widgets。主窗口组织工具箱/流程树、流程画布、参数与绑定、图像与结果面板。
- 使用 QtNodes 通用图层适配 VisionPro 领域模型。主连线表达执行编排，输入/参数绑定与 Group 包含关系独立编辑；QtNodes 的数据传播机制不承担业务执行，其场景 JSON 不替代方案格式。
- 同会话运行使用 `WorkflowSession::publish_and_submit`，文件草稿使用 `publish_solution_run`。编辑先进入草稿，受理时更新活动快照，保存由独立操作完成。已受理运行保留本轮快照与资源提供方。
- 节点、连线、绑定及布局使用统一编辑命令和撤销历史。Group 包含与基本公开边界可编辑，完整 Group 执行和复用继续归 S6。
- 图像/ROI 交互覆盖现有 S4 能力。掩膜自由涂抹、更多 ROI 形状的高级交互后置。
- 文件默认外部引用，复制资源到方案目录为显式操作。另存为保留稳定身份，独立复制方案生成新身份并重映射引用。完整托管导入及实际摘要校验不新增为 S5 交付范围，既有托管资源契约和能力保持不变。
- 结果显示采用有界历史和合并刷新，初版沿用 16 轮历史、32 条观察通知，保留配置入口。通知溢出后重新查询，历史查看不被新结果自动覆盖。

### 9.2 子阶段与完成条件

验收记录见 [S5.1](#95-s51-接入与验收记录)、[S5.2](#96-s52-应用服务验收记录)、[S5.3](#97-s53-方案与流程编辑验收记录)、[S5.4](#98-s54-图像与-roi-验收记录)、[S5.5](#99-s55-运行与调试验收记录) 和 [S5.6](#910-s56-综合验收与交接)。

| 子阶段 | 交付 | 可观察完成条件 |
|---|---|---|
| S5.1 Qt 与 QtNodes 接入（已完成） | 现有 Qt 的 GUI 构建与启动、QtNodes 版本及依赖锁定、领域模型适配验证 | 多前驱汇合、多后继分叉、命名出口、统一撤销及 Group 层级切换可表达；记录适配问题并解决；无 GUI 构建仍可独立运行 |
| S5.2 应用服务衔接（已完成） | WorkflowSession 同会话配置发布、对应资源提供方固定、方案切换与关闭协调 | 旧运行不受编辑影响，下一次启动使用新配置；改名/布局保留兼容状态；资源修改不串到旧运行；发布失败不静默执行旧稿 |
| S5.3 方案与流程编辑（已完成） | 节点/流程、控制、绑定、基本 Group、复制粘贴、撤销重做、布局及文件操作 | 非相邻绑定和分支/汇合可操作；作用域错误可定位；新建、保存、另存、独立复制及重载保持约定的身份、引用和资源目标 |
| S5.4 图像与 ROI（已完成） | UInt8 灰度/BGR 查看、像素检查、Blob/ROI/掩膜叠加及首批 ROI 编辑 | 几何轮廓与有效像素对应，缩放/平移保持业务坐标。旋转、继承、修正、排除与边界行为可对照固定夹具 |
| S5.5 运行与调试（已完成） | 流程启动与取消、单节点隔离调试、一次性输入、历史与来源、桌面 Blob 叠加 | 单节点仅调用目标并保持会话状态。技术状态、判定、逻辑终态和实际退出分别可查。慢显示与关闭面板期间仍能完成业务运行 |
| S5.6 综合验收（已完成） | GUI/CLI 对照、交互生命周期回归、负载测量及证据交接 | 覆盖 §9.3 全部必验范围，记录 Qt/QtNodes 及工具版本、输入、结果和限制；必需项未通过时保持 S5 未完成 |

耗时校验、资源准备、加载保存及关闭等待不得阻塞 GUI 线程；后台处理固定配置与结果，界面显示在 GUI 线程完成。

### 9.3 验收基准与证据

复用 [S4 固定方案与输入清单](s4-acceptance.md#固定方案与输入)，覆盖真实单链、五路汇合和旋转 ROI 继承。
不仅加载展示现有样例，还要在 GUI 修改参数、绑定、ROI 和布局后保存，并由独立 CLI 进程重载运行。
相同配置与输入应得到一致输出、业务判定、控制行为和本轮来源关系；各进程运行身份分别验证，不要求随机身份、时间戳或耗时相等。

| 验证范围 | 必需证据 |
|---|---|
| A21：GUI/CLI 双入口 | 固定输入、版本、GUI 保存方案、CLI 输出及数值/来源对照；布局往返不改变执行语义 |
| 编辑与持久化 | 增删、复制、撤销、非相邻绑定、Group 公开边界和作用域诊断；另存后路径仍指向约定资源，保存失败保留原方案；未知内容不被静默丢弃 |
| 同会话发布与资源 | 运行中修改参数和资源，旧运行及后续节点仍使用原上下文，新运行使用新配置；发布失败、会话切换、关闭与状态兼容行为有可复现验证 |
| 图像与 ROI | 显示颜色、步长、裁剪原点、坐标变换和所有权正确；轮廓及成员掩膜与实际像素处理一致 |
| A27：观察与界面生命周期 | 慢显示、通道超容、历史淘汰、关闭/重绑面板及旧会话通知；业务完成独立推进，已销毁对象不被访问 |
| A30：单节点调试 | 一次性输入只作用于本次目标，不改持久绑定、不补跑来源或触发后继；缺输入明确失败，隔离状态不污染活动运行 |

初始测量基线为 **100 个节点、300 条控制线、1920×1080 图像**。测量记录画布操作、显示刷新、加载/保存、执行至界面可再次运行，以及历史图像的内存占用。
合并刷新间隔和验收门限见 [§9.4](#94-合并刷新与性能门限)。
正确性测试、Qt 集成测试、人工交互与显示检查分别记录；编译或截图不能替代双入口和生命周期验证。

S4 的无 SHA-256 验收范围及其七项排除保持原记录，见 [S4 Release 回归范围](s4-acceptance.md#release-回归范围)。
S5 不据此宣称完整托管导入已验证；后续扩大到该交付时，须补齐资源版本、摘要、清理与故障恢复验收。

### 9.4 合并刷新与性能门限

运行面板按 **50 ms** 间隔合并刷新，观察队列超容时根据运行身份重新查询。S5.6 负载门限在试测后固定，以单独启动的 Release 进程执行验收。[负载实测、门限和复现](s5-acceptance.md#负载测量)

### 9.5 S5.1 接入与验收记录

2026-11-16 使用 Visual Studio Professional 2026、MSBuild 18.7.8、MSVC 19.51.36248.0、Windows SDK 10.0.26100.0 和 CMake/CTest 4.4.0 完成 Windows x64 Release `/MD` 验证。[复现命令](#324-s51-release-构建测试与启动)

| 范围 | 实测结果 |
|---|---|
| GUI 与 OpenCV 开启 | 全量构建通过，CTest **105/105** 通过，包含原有文件 SHA-256 用例 |
| GUI 与 OpenCV 关闭 | 独立构建通过，CTest **88/88** 通过。`visionpro_cli --help` 返回 0，39 个 Release 应用和测试程序均无 Qt/OpenCV DLL 导入 |
| 画布交互 | `visionpro_canvas_tests` 在 offscreen 和 Windows 后端均通过 4 个功能用例。覆盖从同一命名出口分叉、拖线到已有前驱的汇合入口、节点移动、Group 命名出口、嵌套层级和跨层撤销重做。核对控制线身份、布局恢复、绑定及包含关系 |
| 依赖与启动 | 锁定归档构建成功，摘要不匹配的归档被拒绝。部署包在清除 Qt 查找路径后，使用 Windows 平台插件创建流程窗口，正常关闭并返回 0 |

QtNodes 的默认几何计算通过 `NodeRole::Size` 回写尺寸，适配器将它保留在界面缓存。尺寸变化只影响图元绘制和端口命中。[QtNodes 几何实现](https://github.com/paceholder/nodeeditor/blob/7c6341a66a8e46b8988140b9e60d892b6a3560b3/src/DefaultHorizontalNodeGeometry.cpp)

Qt 6 鼠标事件携带视口控件，QtNodes 3.0.16 默认连接图元的移动和释放处理把该控件转换为视图。适配场景接管连接图元的这两类事件，沿用 QtNodes 的临时连线图元，按落点提交带稳定 ID 的领域命令。[Qt 6.11.1 事件实现](https://github.com/qt/qtbase/blob/v6.11.1/src/widgets/graphicsview/qgraphicsview.cpp) [QtNodes 连接图元](https://github.com/paceholder/nodeeditor/blob/7c6341a66a8e46b8988140b9e60d892b6a3560b3/src/ConnectionGraphicsObject.cpp)

两套构建日志分别位于 `out/build/s51-gui` 和 `out/build/s51-headless` 的 `build.log` 与 `ctest.log`。Windows 画布测试明细和部署启动记录位于 GUI 构建目录的 `canvas-tests-windows.txt` 与 `launch.log`。

### 9.6 S5.2 应用服务验收记录

2026-11-16 使用与 S5.1 相同的 Windows x64 Release `/MD` 工具链完成构建与全量 CTest。[复现命令](#325-s52-release-构建与测试)

| 验证范围 | 实测结果 |
|---|---|
| 同会话发布、切换与关闭 | `workflow.session_publication` 验证旧运行物理退出前保留提供方，旧后继使用原绑定和资源。新运行和后续提交采用已发布配置。忙、校验失败、预检失败及过期提交保留活动配置。`workflow.session_lifecycle` 验证切换和关闭排空，以及同方案重新激活后拒绝旧捕获 |
| 工具状态 | `workflow.state_session_publication` 验证改名和布局调整保留累计状态，不兼容绑定创建新代次 |
| 文件草稿 | `infrastructure.file_draft_publication` 使用同一资源 ID 的两条文件路径，验证旧、新后继在实际调用时分别读取各自路径。未选范围缺文件时所选目标仍可运行，选中资源缺失返回诊断并保留清单与活动快照 |
| Release 回归 | GUI 与 OpenCV 开启 **108/108** 通过，关闭两者的独立构建 **91/91** 通过。无 GUI CLI 的帮助命令正常退出，二进制未导入 Qt 或 OpenCV DLL |
| Windows 部署启动 | `VisionProGui` 安装随包的 Qt 和 OpenCV DLL。在清除开发套件查找路径后，程序创建流程画布窗口并正常退出 |

S5.2 验收使用应用服务和文件方案入口发起运行。桌面接入见 §9.9。
构建、回归和启动日志分别见 `out/build/s52-gui` 与 `out/build/s52-headless` 的 `build.log`、`ctest.log` 和 `launch.log`。

### 9.7 S5.3 方案与流程编辑验收记录

2026-11-16 使用与 S5.2 相同的 Windows x64 Release `/MD` 工具链完成构建、回归和部署启动。[复现命令](#326-s53-release-构建与测试)

| 范围 | 实测结果 |
|---|---|
| 控制画布和编辑历史 | `visionpro_canvas_tests` 和 `visionpro_editor_tests` 验证分支、汇合、节点拖动、多流程增删、复制粘贴新身份、跨流程撤销、作用域诊断定位及已失效诊断的修复按钮状态 |
| 实际检查器操作 | `visionpro_inspector_tests` 通过 Qt 按钮与嵌套对话框编辑非相邻输出绑定，保存 List 索引和 Record 字段，拒绝未知字段，编辑 Group 命名出口，并验证两项编辑共用撤销和重做 |
| 保存与重载 | 编辑器测试覆盖新建、首次保存、原路径修订递增、布局、另存稳定身份、独立复制重映射、损坏清单保留当前方案及未知字段只读。保存的可执行模拟方案由独立 CLI 进程重载并运行成功 |
| 文件与资源 | `infrastructure.solution_transfer` 的 31 项断言覆盖 Unicode 路径、包内外资源、显式复制、布局和资源槽重映射、重新加载后读取实际文件、目标冲突、并发发布、回滚及精确刷新保存基线 |
| Release 构建 | 启用 GUI 和 OpenCV 的 CTest **111/111** 通过，独立无 GUI 配置 **92/92** 通过。无 GUI CLI 启动成功，DLL 导入检查无 Qt 或 OpenCV |
| Windows 部署 | 使用部署目录内的 Qt 和 OpenCV DLL 创建桌面编辑器窗口，正常关闭并返回 0 |

剪贴板在当前编辑器的原流程及 Group 层内使用。普通保存保留编辑历史，另存和独立复制打开目标文档并开始新的历史。
ROI 专用编辑见 §9.8，GUI/CLI 结果对照见 §9.10。
构建与回归日志见 `out/build/s53-gui` 和 `out/build/s53-headless` 的 `build.log`、`ctest.log`，部署启动见 GUI 构建目录的 `launch.log`。

### 9.8 S5.4 图像与 ROI 验收记录

2026-11-16 使用 Windows x64 Release、MSVC 19.51、Qt 6.11.1、QtNodes 3.0.16 和 OpenCV 4.14.0-pre 验证。见[复现命令](#327-s54-release-构建与测试)。

| 范围 | 实测结果 |
|---|---|
| 图像查看 | `visionpro_image_view_tests` 核对带行填充的裁剪 BGR、灰度和原图所有权。验证缩放、平移、适应窗口和拖绘保持局部 Pixel 坐标，像素检查同时报告裁剪根坐标 |
| 真实几何和输出 | `visionpro_visual_tests` 对照 S4 固定方案，逐像素核对旋转区域的 7 个成员及继承、修正、排除后的 6 个成员。由同一轮实际输出验证 Blob 面积 6、质心 (5, 4)、3 × 3 包围框及结果叠加，拒绝混合不同运行的输出 |
| 交互、掩膜和策略 | Qt 集成测试覆盖打开文件与方案图像、选中节点预览、保留符号 ROI 的修正编辑、直接 Mask 资源槽的联动及统一撤销。求值测试覆盖 Reject、Intersect、有效空域、掩膜阈值及目标尺寸校验 |
| Release 回归 | GUI 与 OpenCV 配置 **113/113** 项 CTest 通过。无 GUI 和无 OpenCV 配置 **92/92** 项通过，CLI 可启动，DLL 导入检查无 Qt 或 OpenCV |
| Windows 部署 | `VisionProGui` 安装组件使用部署目录内的 Qt 和 OpenCV DLL 创建原生窗口，正常退出并返回 0 |

草稿预览使用当前图像和可静态解析的同作用域阈值继承链。运行态结果和历史的桌面查看见 §9.9。
构建和测试日志见 `out/build/s54-ninja` 与 `out/build/s54-headless` 的 `build.log`、`ctest.log`。部署启动见 GUI 构建目录的 `launch.log`。

### 9.9 S5.5 运行与调试验收记录

2026-11-16 使用 Windows x64 Release、MSVC 19.51、Ninja、Qt 6.11.1、QtNodes 3.0.16 和 OpenCV 4.14.0-pre 验证。[复现命令](#328-s55-release-构建与测试)

| 范围 | 实测结果 |
|---|---|
| 同会话运行和隔离调试 | `visionpro_editor_run_tests` 通过桌面动作运行默认输入 2、一次性调试输入 9，再以修改后的草稿输入 3 得到正常会话累计值 5。调试仅调用目标，缺输入在受理前报告，原绑定与撤销历史保持。无效草稿保留之前的运行记录。 |
| 文件资源和历史 | 相同用例验证未保存草稿可运行绝对路径资源，缺方案目录的相对引用报错。配置保留一轮历史后，面板继续显示已固定的旧轮，回到最新轮可查看后续结果。 |
| A27 界面生命周期 | `visionpro_run_lifecycle_tests` 验证取消后的逻辑终态及物理退出、容量 1 的通知溢出、隐藏期间运行完成和重开后按身份查询。换方案和关闭窗口等待旧会话排空，旧轮结果保留且旧通知不进入新面板。 |
| Blob 和真实图像输出 | `visionpro_visual_tests` 从桌面运行 S4 旋转继承样例。Blob 叠加使用该轮实际二值输入，包围框与质心对应组件。阈值输出可显示同调用 ROI 和掩膜，判断节点分别显示技术成功与 OK。一次性 PNG 的隔离调用只运行 Blob，实测面积为 4 像素。 |
| Release 回归和部署 | GUI 与 OpenCV 配置 **115/115** 项 CTest 通过，无 GUI 和无 OpenCV 配置 **92/92** 项通过。CLI 启动及 DLL 导入检查通过。部署包使用其 Qt 和 OpenCV DLL 创建原生窗口，正常退出并返回 0。 |

构建和回归日志见 `out/build/s55-gui` 与 `out/build/s55-headless` 的 `build.log`、`ctest.log`。部署启动见 GUI 构建目录的 `launch.log`。

### 9.10 S5.6 综合验收与交接

2026-11-16 完成 Windows x64 Release 回归。GUI 和 OpenCV 开启 **116/116** 项 CTest 通过，独立无 GUI 和无 OpenCV 构建 **92/92** 项通过。Windows 原生 Qt 平台复测通过双入口对照、生命周期和图像用例，部署窗口成功创建并正常关闭。[复现命令](#329-s56-release-构建与验收)

三条 S4 固定流程各完成原始、语义修改、仅布局修改并重载的对照，共 **9 对**。新增回归验证运行中编辑与历史图像所有权，以及窗口销毁后迟到的完成和实际退出。负载在默认保留 16 轮、观察容量 32 的配置下通过验收门限。[验收矩阵、数值与交接](s5-acceptance.md)

## 10. S6 实施与验证

### 10.1 S6.1 循环与点集

2026-11-18 在 Windows x64 Release 上完成全量回归。Qt 与 OpenCV 配置 **120/120** 项 CTest 通过，无 GUI、无 OpenCV 配置 **95/95** 项通过。桌面对照通过 Qt offscreen 平台操作编辑器、保存和重载，CLI 由独立进程运行。两套构建的回归日志分别位于 `out/build/s61-gui/ctest.log` 和 `out/build/s61-headless/ctest.log`。

[双 Blob 循环方案](../samples/s6-loop/solution.json)复用 S4 的「双区域 输入.png」。从桌面打开方案，运行「按 Blob 个数遍历组件」，在运行面板查看「遍历组件，汇集质心」的公开输出。在标有迭代 0 和 1 的两行取项调用间切换，可查看各次的调用身份和导出来源。用 **File → Save As** 保存到新目录，重新打开后运行。独立 CLI 读取另存目录的 `solution.json`，将下方 `$solution` 替换为它的绝对路径即可对照。

| 场景 | 验收依据与结果 |
|---|---|
| 两个 Blob | 桌面和独立 CLI 执行相同修订。索引 0、1 各派发一次，使用不同作用域和调用身份。`points` 按顺序导出像素质心 (2, 1.5) 和 (8, 6)，每项可追溯到对应调用的 `item.centroid`。`areas` 为 [6, 9]，首项、末项、求和分别为 6、9、15。 |
| 零次循环 | 在桌面把阈值改为 255，保存并重载。桌面和 CLI 均成功，取项调用 0 次，点集和面积列表为空，可选首／末项均无值，求和为初值 0。核心测试另验必需首项缺失会报错，以及非零归约初值会保留。 |
| 中途取消 | 桌面设置 30 秒迭代间隔，等待第一项已收集，再触发取消。3 秒内进入静止状态，只执行一次取项，循环状态为 `Cancelled`，未发布部分点集。无 GUI 测试还在间隔内运行另一条流程，验证等待期间调度器能处理其他工作。 |
| 核心边界 | 整数列表用例覆盖反向含终点、嵌套循环与提前退出、零步长、计数溢出及中途越界。两轮运行各自导出累计序列 [1, 2]，验证每轮状态的隔离。 |

在 Windows x64 的 PowerShell 中从仓库根目录执行。两套构建沿用 [§3.28 的 Release 配置](#328-s55-release-构建与测试)，将目录设为 `out/build/s61-gui` 与 `out/build/s61-headless`。前者开启 Qt 与 OpenCV，后者均关闭。

```powershell
$guiBuild = 'out/build/s61-gui'
$coreBuild = 'out/build/s61-headless'
$openCvBin = (Resolve-Path (Join-Path $env:OpenCV_DIR '../bin')).Path
$env:Path = "$openCvBin;$env:Path"
$solution = (Resolve-Path 'samples/s6-loop/solution.json').Path
& "$guiBuild/source/app/visionpro_cli.exe" run --solution $solution --workflow 5ca4f87a-ea1e-4d62-89b0-000000006100
ctest --test-dir $guiBuild -C Release -R 'visionpro_loop_parity_tests|app.structured_loops|workflow.loop_state_and_plan_capability' --output-on-failure
ctest --test-dir $coreBuild -C Release -R 'app.structured_loops|workflow.loop_state_and_plan_capability' --output-on-failure
```

### 10.2 S6.2 Group 调用与跨方案复用

2026-11-18 完成 Windows x64 Release 回归。Qt 与 OpenCV 配置 **125/125** 项 CTest 通过，无 GUI、无 OpenCV 配置 **99/99** 项通过。Qt offscreen 编辑器将 Group 导出后在另一个方案内导入两次、接线、保存并重载。移走源方案和导出包后，桌面和独立 CLI 仍从目标方案得到同一组像素质心 (2, 1.5)、(8, 6) 和总面积 15，导出来源可追溯至各自的子作用域。包测试覆盖必需宿主服务或资产缺失、摘要不匹配、同名异 ID、坐标定义冲突与失败回滚。完整日志见 `out/build/s62-gui/ctest.log` 和 `out/build/s62-headless/ctest.log`。

[双 Blob Group 方案](../samples/s6-group/solution.json)以 `threshold` 为公开输入，默认 128。Group 内的分叉分别启动图像处理和会话计数。公开数据端口包括点集、面积列表、首末面积、总面积和实例调用次数 `calls`。控制出口 `measured` 激活宿主中的面积读取节点。

1. 从桌面打开样例，运行「双 Blob Group 调用」。查看 Group 的 `points`、`total_area` 和 `calls`。选择 Group，在 **Run** 菜单中独立调试，将公开阈值改为 255，可见空点集和总面积 0。**Run Selected Group** 可单独推进该实例的活动计数。
2. 选中普通 Group，经 **File** 菜单导出到新目录。包内的 `subgraph.json` 和资产子目录须一起保留。在另一份已保存方案中选中目标流程，通过 **Import Subgraph Package** 导入两次。每次导入选中新实例。将 A 设为流程入口，将 A 的 `measured` 出口连到 B，再保存并重载。
3. 在新的桌面会话运行该流程，两个实例各得到两个质心和调用次数 1。单独以活动状态运行 A，其计数升至 2；再运行整条流程，A 和 B 分别为 3、2。CLI 进程从新会话启动，复现保存配置的首次运行结果。实例状态保留在所属会话中，子图包和方案文件只保存配置。

命令在仓库根目录的 PowerShell 执行。Release 配置沿用 [§3.28](#328-s55-release-构建与测试)，目录改为 `out/build/s62-gui` 和 `out/build/s62-headless`。原始样例具有固定 ID，导入的新实例使用目标方案中的新 ID。

```powershell
$guiBuild = 'out/build/s62-gui'
$coreBuild = 'out/build/s62-headless'
$env:Path = (Resolve-Path (Join-Path $env:OpenCV_DIR '../bin')).Path + ';' + $env:Path
$solution = (Resolve-Path 'samples/s6-group/solution.json').Path
$flow = '5ca4f87a-ea1e-4d62-89b0-000000006210'
$group = '5ca4f87a-ea1e-4d62-89b0-000000006211'
& "$guiBuild/source/app/visionpro_cli.exe" run --solution $solution --workflow $flow
& "$guiBuild/source/app/visionpro_cli.exe" run --solution $solution --workflow $flow --group $group --input threshold=255
ctest --test-dir $guiBuild -C Release -R '^visionpro_group_parity_tests$' --output-on-failure
ctest --test-dir $coreBuild -C Release -R 'infrastructure.subgraph_package|workflow.group_state_domains|workflow.group_exports_and_debug|workflow.enabled_workflow_requests' --output-on-failure
```

### 10.3 S6.3 计算与逻辑

2026-11-19 在 Windows x64 完成两组 Release 构建和全量 CTest。启用 Qt 与 OpenCV 的配置通过 **130/130** 项，关闭二者的配置通过 **103/103** 项。Qt offscreen 桌面测试将方案另存、重载，再让独立 CLI 在另一个工作目录运行三个流程，逐节点比较状态、输出、OK/NG 和所选控制出口。[固定验收方案](../samples/s6-calculation/solution.json)

| 样例流程 | 输入或操作 | 预期结果 |
|---|---|---|
| 面积取项、格式化与累加 | 使用公开输入默认值 `areas=[6,9]`、`index=1` | 计算节点读出 9。格式化在原列表上依次请求下标 `[0,2,1]`，允许跳过第 2 个请求，输出 `A=6\|A=9`，`used_indices=[0,1]`、`used_values=[6,9]`。 |
| 五路判断与条件分流 | 第五路输入默认 9，上限 8。再将输入改为 7 | 第一轮四个 OK、一个 NG，逻辑与为 false，实际执行「不合格后继」。第二轮五个 OK，执行「合格后继」。两轮流程均成功。 |
| 严格取项后格式化 | 将公开输入改为 `index=2` | 普通取项报 `IndexOutOfRange`，流程失败，格式化节点跳过且无输出。面积计算流程的动态绑定越界也报同一诊断。 |

`visionpro.calc.int64` 提供加、减、乘、整除。除零和超出 Int64 可表示范围的运算以技术失败结束。`visionpro.calc.int64_accumulate` 保存初值 `initial` 和模式 `mode`。在同一个桌面会话连续运行第一个流程两次，每轮累加器和会话累加器依次得到 `(11,21)`、`(11,22)`。改显示名后，会话累加器继续计数。空闲时选中会话累加器，使用 **Run → Reset Selected Node State**，下一次从该节点配置的初值加 1。每次独立启动 CLI 都建立新会话。

`visionpro.logic.and` 按配置顺序接收布尔来源，`visionpro.logic.condition` 根据结果激活 `true` 或 `false` 出口。`visionpro.format.int64_list` 先取 `list`，再按 `values` 的绑定顺序追加标量。参数 `indices` 给出读取顺序，留空时使用全部元素。`skip_out_of_range` 填写允许跳过的 **请求位置**，从 0 起算。`format` 中恰有一个 `{value}`，`separator` 分隔各实际采用项。桌面检查器可调整多源顺序、选择下标来源并修改这些参数。

以下命令在仓库根目录执行，使用无 Qt、无 OpenCV 的 Release 构建。配置方法见 [§3.28](#328-s55-release-构建与测试)，构建目录使用 `out/build/s63-gui` 和 `out/build/s63-headless`。两个指定越界的调用预期退出码为 1。全量日志分别位于 `out/build/s63-gui/ctest.log` 和 `out/build/s63-headless/ctest.log`。

```powershell
$cli = 'out/build/s63-headless/source/app/visionpro_cli.exe'
$solution = (Resolve-Path 'samples/s6-calculation/solution.json').Path
$area = '5ca4f87a-ea1e-4d62-89b0-000000006310'
$judge = '5ca4f87a-ea1e-4d62-89b0-000000006320'
$strict = '5ca4f87a-ea1e-4d62-89b0-000000006330'
& $cli run --solution $solution --workflow $area
& $cli run --solution $solution --workflow $area --input index=2
& $cli run --solution $solution --workflow $judge
& $cli run --solution $solution --workflow $judge --input fifth=7
& $cli run --solution $solution --workflow $strict --input index=2
ctest --test-dir out/build/s63-headless -C Release -R 'workflow.resolver_dynamic_indices|infrastructure.dynamic_file_resources|tools.logic|app.cli_calculations' --output-on-failure
ctest --test-dir out/build/s63-gui -C Release -R 'visionpro_(calculation_parity|inspector|visual)_tests' --output-on-failure
```

### 10.4 S6.4 共享值与工件汇合

2026-11-19 在 Windows x64 完成 Release 构建与全量 CTest。启用 Qt 和 OpenCV 的配置通过 **136/136** 项，关闭二者的配置通过 **108/108** 项。桌面测试使用 Qt offscreen，执行另存、重开和运行，再让独立 CLI 从另一工作目录运行保存的方案。[固定验收方案](../samples/s6-shared/solution.json)

| 样例流程 | 新会话首次运行的预期结果 |
|---|---|
| 乱序测量与工件交付 | 输入依次为 A 宽 10、B 宽 20、B 高 2、A 高 1。第一条队列按 FIFO 取出四个片段。B 先齐备，第二条队列依次收到序号 1 的 B=(20,2) 和序号 2 的 A=(10,1)。每个接收信封的消息 ID 与本次实际发送一致。 |
| 全局值写入与一致读取 | 初始宽度为 0、版本 0。写入 10 后版本为 1。显式批量读取依配置顺序返回高度 2（版本 0）、宽度 10（版本 1）。同轮普通绑定仍读到准备时的宽度 0、版本 0。 |

桌面使用 **Run → Preview Shared Data…** 查看每个变量的值与版本、队列的消息序号和关联键，以及各命名汇合的完成、重复、过期和容量统计。可在节点检查器按稳定 ID 选择或调整共享目标的顺序。样例固定使用批次 1，重开后可从空队列复跑。同一会话处理下一批工件时使用更大的批次号。跨进程复现时，消息 UUID 各自生成，校验按发送节点追踪到接收信封，并比较序号、工件键及内容。

专项回归还覆盖以下边界。

- 队列容量为 2、满时拒绝，第 3 条发送得到 `QueueFull`。精确批量或跨队列组合接收缺项时保留全部原消息，取消空队列接收或满队列发送也保留队列内容。
- 汇合保留首个有效来源。重复片段及并发到达的最后一个字段只交付一次。超时后可查询缺失字段，大批次号关闭旧批次，新批次可以复用原工件号。暂存名额、批次键和命名汇合的容量分别计数。
- 桌面连续刷新后，未收消息的序号仍为 1、接收计数仍为 0。在有待收消息、未齐工件和已写全局值时保存，重开后首条消息序号回到 1，全局读取回到初值和版本 0，同号工件可重新暂存。跨方案子图导入另验缺失或类型不匹配的共享目标，以及缺少共享服务时原宿主仍能运行。

内置节点覆盖 Int64 全局读写与多值读取、测量片段及完整宽高 Record 的队列收发，以及必需来源为 `width`、`height` 的工件汇合。服务接口按声明类型校验。多流程并行与有界重入安排在 S6.5。[数据契约](data-model.md#11-全局变量) · [后续计划](s6-plan.md#s65-多流程并行与重入)

以下命令在仓库根目录执行。两种 Release 配置沿用 [§3.28](#328-s55-release-构建与测试)，构建目录分别为 `out/build/s64-gui`、`out/build/s64-headless`。全量回归可移除 `-R` 及其表达式。

```powershell
$cli = 'out/build/s64-headless/source/app/visionpro_cli.exe'
$solution = (Resolve-Path 'samples/s6-shared/solution.json').Path
& $cli run --solution $solution --workflow '5ca4f87a-ea1e-4d62-89b0-000000006410'
& $cli run --solution $solution --workflow '5ca4f87a-ea1e-4d62-89b0-000000006420'
ctest --test-dir out/build/s64-headless -C Release -R 'workflow\.(shared_data|correlation_join)|tools\.shared_data|app\.(shared_package|cli_shared_workflows)' --output-on-failure
ctest --test-dir out/build/s64-gui -C Release -R 'visionpro_(inspector|shared_parity)_tests' --output-on-failure
```

### 10.5 S6.5 多流程并行与有界重入

2026-11-19 在 Windows x64 完成两套 Release 构建和全量 CTest。启用 Qt 和 OpenCV 的配置通过 **142/142** 项，关闭二者的配置通过 **113/113** 项。并行顺序和独占持有数由能控制同步调用和实际退出的测试工具测得。[并行验收测试](../tests/workflow/concurrency_tests.cpp)

| 场景 | 已验证结果 |
|---|---|
| 跨流程并行与资源独占 | A 的同步调用停在屏障时，B 独立完成。另将 A、B 绑定到同一独占资源，A 逻辑完成后继续持有租约，B 等到实际退出才取得租约，峰值为 1。无关流程 C 可在等待期间完成。 |
| 在途 1、等待 1 | 三个不同输入依次得到立即受理、排队和 `RunQueueFull`。排队轮次保持 Pending，可在第一轮仍被阻塞时单独取消，取消后可再次使用等待名额。容量为 2 的等待队列按进入顺序运行，满时也可配置返回 `RunBusy`。 |
| 在途 2 与状态隔离 | 两轮输入 10、20 的逐轮状态各自产生 10、20，并保有各自的运行及调用身份。会话累计节点从 0 连续加 1 得到 1、2。改变初值后的新状态代次从 100 独立累计到 102，旧轮仍能在原代次完成。 |
| 排队后的文件与配置 | 首轮和等待轮共用原快照。修改文件并发布另一目录的提供方后，原等待轮仍使用原参数和目录。SnapshotLocked 读到原字节，PerInvocation 读到原目录下更新后的字节，后续新轮读取新目录。 |
| 桌面与既有流程 | Qt offscreen 验证桌面并发提交、定点取消、清理后晋升和换方案。另存、重开样例后，独立 CLI 复现两条流程的节点输出和循环作用域。S6.4 的宽高乱序样例仍依次交付 B=(20,2)、A=(10,1)。 |

打开[并行与重入样例](../samples/s6-concurrency/solution.json)，选择「有界重入与每轮累计」。在流程画布的空白处取消节点选择，可在 **Properties → Workflow → Admission** 查看或修改在途上限、等待容量和满载响应。样例为 2 个在途和 1 个等待名额，两次循环之间间隔 2 秒。每次提交可以输入不同的 `delta`，流程结束时「读取本轮加数」输出相应输入。切换到「独立流程与会话累计」运行两次，可在同一桌面会话中看到 1、2。运行面板选中待取消的轮次后使用 **Run → Cancel Selected Run**。

下列命令在仓库根目录执行，Release 配置参数沿用 [§3.28](#328-s55-release-构建与测试)。CLI 每次启动时建立一个新会话。保持同一会话的并发、排队与累计由桌面或下列 CTest 复现。

```powershell
$cli = 'out/build/s65-headless/source/app/visionpro_cli.exe'
$solution = (Resolve-Path 'samples/s6-concurrency/solution.json').Path
& $cli run --solution $solution --workflow '5ca4f87a-ea1e-4d62-89b0-000000006510' --input delta=7
& $cli run --solution $solution --workflow '5ca4f87a-ea1e-4d62-89b0-000000006520'
ctest --test-dir out/build/s65-headless -C Release -R 'workflow\.(parallel_dispatch|exclusive_resources|bounded_admission|reentrant_state|queued_file_snapshots|state_acceptance)|app\.cli_shared_workflows' --output-on-failure
ctest --test-dir out/build/s65-gui -C Release -R 'visionpro_(run_lifecycle|inspector|concurrency_parity)_tests' --output-on-failure
```

### 10.6 S6.6 连续运行与外部触发

2026-11-19 通过 Windows x64 Release 全量 CTest。启用 Qt 和 OpenCV 的构建通过 **147/147** 项，关闭二者的构建通过 **118/118** 项。样例的两个流程均将 Int64 帧号送入逐轮累加节点，输出即本轮帧号。外部事件由宿主提供，连续时钟可在 CLI 中手动推进。真实相机信号的验证安排在 S6.10。[样例方案](../samples/s6-trigger/solution.json) · [触发与并发测试](../tests/workflow/concurrency_tests.cpp)

| 验收场景 | 实测结果 |
|---|---|
| 三帧与满载 | 固定阻住首帧，配置 1 个在途和 2 个等待名额。10、20、30 分别立即受理、排队、排队。第 4 帧按配置返回占用者身份或 `RunQueueFull`。排空后前三轮逐帧输出，第四帧没有运行号。 |
| 连续时钟与快照 | 首个间隔前没有轮次，同一时刻重复推进只触发一次。晚到多个间隔时只产生一帧。同会话发布新的默认加数 100 后，下一帧 2 输出 102。停止后再推进一小时也没有新轮。 |
| 停止与物理退出 | 停止期间仍在资源预检的事件无法受理。取消跳过排队帧，运行中的同步调用在退出后记为已取消。模拟异步调用已提交成功但尚未退出时，等待返回 `DeadlineExpired`，独占租约持有数为 1。开放退出屏障后再等待成功，持有数为 0。 |
| 桌面保存与重开 | 编辑并另存外部源，再重开方案。桌面和独立 CLI 均得到 11、22、4000000000，核对了关联键与每轮输出。连续模式从帧 1、2 开始，重开后旧订阅的迟到事件无法启动新方案。 |

桌面打开样例后，在流程画布空白处取消节点选择，进入 **Properties → Workflow → Trigger source** 编辑来源。选择「外部帧号」并执行 **Run → Arm Workflow Trigger**，再用 **Send External Frame…** 输入帧号。「连续帧号」启用后按 100 ms 间隔触发。**Stop Trigger and Drain** 等待已受理轮次完成，**Stop Trigger and Cancel Runs** 请求取消并等待实际退出。在 **Run and Results** 选择某轮即可核对来源、帧号输入、关联键和初始受理方式。[桌面与 CLI 对照测试](../tests/ui/editor_run_tests.cpp)

从仓库根目录运行以下命令。第一条回放三个外部帧，第二条依次推进受控时钟，预期输出帧号 1、2、3。CLI 将每次事件的受理结果写入 `events`，受理的轮次附带固定输入和节点输出。某帧被拒绝时仍保留其他帧的结果，进程返回 1 并向 stderr 写入诊断。

```powershell
$cli = 'out/build/s66-headless/source/app/visionpro_cli.exe'
$solution = (Resolve-Path 'samples/s6-trigger/solution.json').Path
& $cli trigger --solution $solution --workflow '5ca4f87a-ea1e-4d62-89b0-000000006610' --frame 101 --frame 203 --frame 307 --stop drain
& $cli trigger --solution $solution --workflow '5ca4f87a-ea1e-4d62-89b0-000000006620' --advance-ms 99 --advance-ms 1 --advance-ms 250 --advance-ms 100
ctest --test-dir out/build/s66-headless -C Release -R 'workflow\.trigger_|app\.cli_trigger_replay' --output-on-failure
ctest --test-dir out/build/s66-gui -C Release -R 'visionpro_(editor_run|run_lifecycle)_tests' --output-on-failure
```

### 10.7 S6.7 超时、错误出口和安全重试

2026-11-19 完成 Windows x64 Release 构建。启用 Qt 和 OpenCV 的配置通过 **152/152** 项 CTest，关闭二者的配置通过 **123/123** 项。设备行为由可以分别控制发送、响应和退出的模拟端点验证。[故障注入测试](../tests/workflow/attempt_tests.cpp)

| 场景 | 验收结果 |
|---|---|
| 首次超时后重试 | 第一次超时后仍持有独占租约。发布输入 900 的新配置，再让旧尝试退出，第二次仍使用原输入 73 和同一逻辑调用。两次尝试的编号为 0、1，租约同时持有数最高为 1。迟到通知后，最终结果发布一次，正常后继执行一次。 |
| 超时与取消 | 同步启动阻塞时，逻辑结果先成为 `TimedOut`，退出后才释放资源。两个异步节点并行时，一个节点的取消回调阻塞，另一个仍能按期超时。两次均超时保留末次 `TimedOut`。在两次尝试之间取消则提交 `Cancelled`，停止重试与恢复调度。 |
| 最终失败与 NG | 重试耗尽后，接通的错误出口只激活一次，原节点保留失败诊断。错误出口未连接或只连到禁用节点时，流程失败。检测 NG 保留成功的技术状态，首次执行即走正常路径。 |
| 发送成功、响应丢失 | 普通端点只发送一次，未知效果诊断可查到操作键。支持协议去重的端点两次接收同一键，只产生一次效果。新一轮调用获得新键。 |
| 桌面保存与 CLI | Qt 验收通过属性面板设置 5000 ms、1 次重试和 `fault` 出口，连接恢复节点后另存并重开。桌面和独立 CLI 的源节点均记录两次失败，标记 `error_handled=true`，恢复节点均输出 42。[桌面对照测试](../tests/ui/inspector_tests.cpp) |

打开[恢复样例](../samples/s6-recovery/solution.json)，选中加法节点后，在 **Properties → Execution policy** 可编辑超时、重试上限和错误出口。样例默认输入 `delta=1`，计算 `INT64_MAX + 1` 两次失败，恢复节点输出 `-1`。将输入改为 `0`，正常后继输出 `10`。两条路径的流程最终状态均为 `Succeeded`，可用源节点的 `status`、`attempts` 及 `error_handled` 区分。[CLI 回归](../tests/app/cli_tests.cpp)

从仓库根目录执行以下命令，Release 配置方法见 [§3.28](#328-s55-release-构建与测试)。

```powershell
$cli = 'out/build/s67-headless/source/app/visionpro_cli.exe'
$solution = (Resolve-Path 'samples/s6-recovery/solution.json').Path
$workflow = '5ca4f87a-ea1e-4d62-89b0-000000006710'
& $cli run --solution $solution --workflow $workflow
& $cli run --solution $solution --workflow $workflow --input delta=0
ctest --test-dir out/build/s67-headless -C Release -R 'workflow\.(timeout_retry|logical_timeout_and_cancel|recovery_and_judgement|external_deduplication)|app\.cli_error_recovery' --output-on-failure
ctest --test-dir out/build/s67-gui -C Release -R '^visionpro_inspector_tests$' --output-on-failure
```

### 10.8 S6.8 ROI 与标定测量

2026-11-19，Windows x64 Release 全量回归通过，启用 GUI 与 OpenCV 时为 **154/154** 项，关闭两者时为 **123/123** 项。Qt 交互与像素对照使用 offscreen，测量精度使用合成点对和固定图像验收。

使用启用 OpenCV 的 Windows x64 Release 构建。桌面操作另需启用 GUI。打开[测量方案](../samples/s6-calibration/solution.json)，从 **Run → Run Workflow** 运行，在运行面板依次查看阈值、Blob、平面标定、两次坐标转换和测距结果。

| 固定输入或操作 | 预期与实测 |
|---|---|
| 9×9 白图，阈值节点使用圆心 (3,4)、半径 1 px 的圆 | 掩膜包含中心及上下左右共 5 个像素。Blob 面积为 5，质心为 (3,4) px |
| 四角标定点，仿射关系 `X=2x+10, Y=3y-4` | 独立质心映射为 (16,8) mm，参考点 (5,5) px 映射为 (20,11) mm，距离 5 mm。样例拟合 RMS 为 0 mm，回归容差为 1e-6 mm |
| 在桌面将 **Map reference point → Properties → point** 的常量改为 (3,6) px，另存并重开 | 桌面和独立 CLI 均得到参考点 (16,14) mm、距离 6 mm；两边都读到同一 Calibration UUID 的修订 1 |
| 像素点移到源域 [0,8]×[0,8] 外，或修改已锁定文件的内容 | 分别报告 `InvalidTransform` 和 `ResourceVersionMismatch` |

**编辑 ROI。** 在阈值或 Blob 的属性面板打开 **Edit ROI…**，选择 Circle、Ring、Annular sector 或 Polygon。角度以度输入，正方向从图像 +X 朝 +Y。预览显示实际成员掩膜。四种形状的测试分别核对手列像素、阈值结果和 Blob 面积；桌面测试另存、重开后逐像素读取显示的掩膜。斜边、跨零、内孔、排除和继承修正另有边界用例。[ROI 求值测试](../tests/tools/roi_tests.cpp) · [阈值与 Blob 对照](../tests/tools/blob_tests.cpp) · [桌面测试](../tests/ui/visual_tests.cpp)

**使用自己的标定。** 按[版本 1 文件](../samples/s6-calibration/resources/affine.json)填写模型、源/目标坐标系、源域 `[xmin,ymin,xmax,ymax]` 及 `[[x_px,y_px],[X_mm,Y_mm]]` 点对。模型名为 `Similarity`、`Affine` 或 `Projective`，退化点组和穿越投影无穷线的有效域会产生诊断。桌面通过 **Edit → Add Coordinate Frame…** 声明文件中使用的坐标系，在 **N-point planar calibration → Resources → File…** 选择文件。文件选择器记录 SHA-256 并锁定该资源修订。将标定节点的 `transform` 输出接到坐标转换工具的同名输入，将转换出的两点绑定到距离工具的 `first` 与 `second`。[模型与误差测试](../tests/tools/calibration_tests.cpp)

本次复用 `out/build/s67-gui` 和 `out/build/s67-headless` 两个 Release 目录；新环境可按 §3.28 配置。运行独立 CLI 时，将 OpenCV 的 Release DLL 目录加入当前进程的 `PATH`。下列命令从仓库根目录执行。

```powershell
$guiBuild = 'out/build/s67-gui'
& "$guiBuild/source/app/visionpro_cli.exe" run --solution samples/s6-calibration/solution.json --workflow 5ca4f87a-ea1e-4d62-89b0-000000006810
ctest --test-dir $guiBuild -C Release -R '^tools\.(planar_calibration|vision_contracts|roi|blob)$|^visionpro_(image_view|visual|calibration_parity)_tests$' --output-on-failure --no-tests=error
```

### 10.9 S6.9 工件定位、五路测量与类别图

**六区桌面工作区复现。** 从仓库根目录启动 `visionpro_gui samples/s6-inspection`（也接受 `solution.json`）。在方案副本中选中 **Judge lane 5**，点击 **单次运行**。右上来源为 **Measure lane 5**，显示从 (25,9) 开始的 2×2 ROI；右下记录实测 B=50，范围 [10,40]，判定 NG；底栏显示 4 OK / 1 NG。双击判断节点，把 `maximum` 改为 50，再运行一次。第二轮为 5 OK / 0 NG。点第一轮记录可同时回看原图、阈值和底栏统计。勾选 **跟随最新** 可返回第二轮。

Qt 自动验收在临时目录复制样例，从画布选择节点、在属性中修改上限并保存。每轮另外启动一次 CLI 核对工件结果。用例在 1280×720、1440×800 检查六区位置，并核对历史淘汰后仍能回看固定轮次、跳过节点时清除旧图、重开方案后清空历史。对于 **Format OK / Format NG**，用例从五个测量来源逐一选图，核对各自 ROI。另用循环样例在同一轮切换迭代 0、1、0，核对各自的调用与 Blob 面积 6、9、6。启用 Qt 和 OpenCV 后运行：

```powershell
ctest --test-dir out/build/s67-gui -R '^visionpro_workspace_tests$' --output-on-failure --no-tests=error
```

复查字号和缩放时运行 `visionpro_canvas_tests`、`visionpro_editor_tests` 和 `visionpro_workspace_tests`。三组分别核对长中文标题与端口位置、视点恢复与缩放条命中、两轮历史值与表格尺寸。使用 Windows Qt 后端检查系统字体；可通过 `QT_SCALE_FACTOR=1.25` 或 `1.5` 在同一显示器上模拟额外缩放。运行 `visionpro_workspace_tests -platform windows five_lane_selection_and_history`，将 `VISIONPRO_WORKSPACE_COMPACT_SCREENSHOT` 和 `VISIONPRO_WORKSPACE_SCREENSHOT` 设为 PNG 输出路径，可分别保存 1280×720 编辑视图和 1440×800 全图视图。这两组窗口尺寸使用逻辑像素。

要核对多来源图像，可运行 `visionpro_workspace_tests -platform windows format_image_sources`。将 `VISIONPRO_IMAGE_SOURCES_SCREENSHOT` 设为 PNG 输出路径，可保存实际选中第五路的缩略条和主图。

在上述 Qt 构建的 `tests/ui` 目录运行以下用例，可复查桌面键盘操作与主题渲染。主题用例采样启动即深色和浅转深后的标题、分类及焦点框。并发用例让两个流程同时保持运行，再检查每次取消后的实际状态。

```powershell
.\visionpro_workspace_tests -platform windows appearance_preferences toolbox_category_grid_and_search
.\visionpro_editor_tests -platform windows keyboard_connects_steps_and_undoes
.\visionpro_inspector_tests -platform windows locates_diagnostic_field
.\visionpro_run_lifecycle_tests -platform windows workspace_cancel_shortcut_keeps_its_flow
```

在 1280×720 工作区单击 **Judge lane 5**，确认右上显示第五路 ROI，右下显示该节点的历史记录。放大 ROI 后双击同一节点，在独立的“模块属性”窗口把 `maximum` 从 40 改为 50 并按 Enter。保持参数窗口打开，从主窗口再次运行，随后在右下切换两条历史记录。核对 ROI 的观察位置和倍率一致，两轮的 B 均为 50，第一轮仍显示 [10,40]、NG，第二轮显示 [10,50]、OK。另选含 Group 的方案，双击 Group 应进入内部画布，返回上层后从 Group 的右键“属性…”打开其配置。

以下原生 Qt 用例还覆盖阈值 128 改为 150 后单步撤销、改绑上游并定位来源、切换不同裁剪范围后自动适配，以及用方向键读当前帧像素。将 `VISIONPRO_PROPERTIES_DIALOG_SCREENSHOT` 设为 PNG 输出路径，`five_lane_selection_and_history` 会单独保存第五路判断节点的属性窗口。

```powershell
.\visionpro_inspector_tests -platform windows edits_threshold_inline_then_binds_output preserves_full_width_values_and_defaults locates_diagnostic_field
.\visionpro_image_view_tests -platform windows keyboard_pixel_inspection_and_compatible_frames
.\visionpro_workspace_tests -platform windows five_lane_selection_and_history properties_dialog_reuses_selection_and_clears_on_replacement format_image_sources recovers_image_view_after_failed_measurement panels_float_and_redock
```

2026-11-23，Windows x64 Release 的 23 项 Qt 界面与集成 CTest 全部通过，其中 TCP、PLC 和五路 TCP 投递用例使用本机回环。Windows 原生 Qt 在默认倍率及额外 125%、150% 缩放下通过五路历史回看、停靠和像素读数验收。三轮测量另验证了中间轮 ROI 越界时清空图像及读数，第三轮显示自己的图像并恢复首轮的观察位置。

2026-11-20，Windows x64 Release 启用 Qt、OpenCV 与 MVS 的 176 项 CTest 全部通过，其中 6 项通信测试使用本机回环。上述工作区还在 Windows 原生 Qt 平台上运行了两轮历史回看、OK/NG 两条分支的来源切换与循环调用回看，各项均通过。部署的桌面版已验证可通过清单路径启动五路方案。

2026-11-19，Windows x64 Release 全量 CTest 通过。启用 Qt 与 OpenCV 时为 **158/158** 项，关闭两者时为 **123/123** 项。桌面验收使用 Qt Widgets 的 offscreen 平台，CLI 验收使用独立进程和固定图像。在临时方案中换入无法匹配的非恒定模板时，只有读图和定位两个节点执行；定位返回 NG，其余测量和投递均未启动。

使用启用 OpenCV 的 Release 构建。桌面打开[工件检测方案](../samples/s6-inspection/solution.json)，另存至新目录并重开。选择 **Locate and inspect five regions → Run Workflow**。在运行结果中查看 **Locate workpiece** 的模板资源、平移矩阵及各 **Measure lane** 的输入来源，选择测量调用可在现场图上查看实际 ROI 掩膜。

| 固定输入或桌面编辑 | 验收值 |
|---|---|
| 模板参考左上角 (1,1) px，现场图中在 (4,3) px | 定位输出从 `workpiece` 到 `camera` 的平移 (+3,+2) px，五个 `placement` 来自本轮同一次定位，模板修订 1 |
| 五个 2×2 ROI，闭区间 [10,40] | B 为 10、20、30、40、50，G 均为 73，R 均为 151。前四路 OK，第五路 NG，逻辑与为 false，队列对 `part-001` 只收到一条 `NG B=10,20,30,40,50` |
| 将 **Measure lane 5 → Edit ROI…** 的 Y 改为 10，保存重开 | 第五路包含两个 40 和两个 41，均值 40.5，判断 NG。再将 **Judge lane 5** 的 `maximum` 改为 40.5 后，五路全部 OK，队列收到一条 `OK B=10,20,30,40,40.5` |
| 切换 **Classify a 2 by 2 image → Run Workflow** | 原始标签为 `[[0,1],[2,1]]`，ID 0、1、2 的数量为 1、2、1，对类别 2 数量的判断为 OK |

选择 **Classify intensity → Properties → Display → Edit class color…** 可按类别 ID 改色。保存重载后，桌面按新色显示，独立 CLI 与桌面仍给出相同的原始标签、类别计数和判断。类别表遗漏实际出现的 ID 2 会产生 `MissingField`。CLI 在标签数不超过 256 时还输出逐行的 `pixels.labels`，可直接核对像素位置。

在仓库根目录执行下列命令，运行前将 OpenCV 的 Release DLL 目录加入 `PATH`。将 `$solution` 替换为桌面另存的清单路径即可复现编辑后的结果。

```powershell
$guiBuild = 'out/build/s67-gui'
$solution = 'samples/s6-inspection/solution.json'
& "$guiBuild/source/app/visionpro_cli.exe" run --solution $solution --workflow 5ca4f87a-ea1e-4d62-89b0-000000006910
& "$guiBuild/source/app/visionpro_cli.exe" run --solution $solution --workflow 5ca4f87a-ea1e-4d62-89b0-000000006950
ctest --test-dir $guiBuild -C Release -R '^tools\.(template_and_color|class_map|judgement)$|^app\.cli_inspection$|^visionpro_(visual|inspection_parity)_tests$' --output-on-failure --no-tests=error
```

### 10.10 S6.10 相机采集与触发

2026-11-19，Windows x64 Release 全量 CTest 通过。启用 Qt、OpenCV 和 MVS 时为 **166/166** 项，关闭三者时为 **127/127** 项。桌面使用 Qt Widgets offscreen 打开、编辑、另存并重开[相机方案](../samples/s6-camera/solution.json)，独立 CLI 从保存后的文件复现单次采集和逐帧触发。两个流程共用设备资源“检测相机”，默认序列号为 `SIM-001`。

| 场景 | 实测结果 |
|---|---|
| 单次采集和缓冲复用 | 模拟设备向同一块缓冲交替写入 7、31。第二帧发布并断连后，第一轮保留的 2×2 Mono8 图像四个像素仍均为 7。MVS 帧结构夹具也独立检查了缓冲复用、空指针、格式、载荷长度与丢包标记。 |
| 模拟软触发和硬触发 | 桌面把单次采集节点改为 Software、超时改为 1200 ms，保存后的方案在两个入口均取得相同图像和帧号。Hardware 模式下两个脉冲触发两轮，每轮只执行一个后继。CLI 记录 `received=2, accepted=2, rejected=0, missing=0, duplicates=0, closed=true`。 |
| 停止和重连 | 等待硬触发时断连唤醒取帧，旧流停止。重新连接并布防后可取得新帧。等待中取消以及停止后到达的脉冲均未产生额外运行。设备别名在原租约释放前无法占用同一设备。 |
| 跳号、重复与回绕 | 向模拟触发入口交付 8 个指定的 32 位原始帧号，包含跳号、重复、回绕前的迟到帧和半圈歧义值。5 帧启动流程，3 帧被拒，缺帧计数为 2；另用超过 32 位的原始设备帧号核对顺序。 |
| 诊断和计时 | 模拟设备的断连、超时、取消和未知错误保留各自的诊断，注入关闭失败时保留原始操作和错误码。一次单次采集的主机调用耗时为 56,900 ns，模拟设备报告 100 µs。另一次硬触发样例的两次主机等待和采集分别为 23,999 µs 和 15,069 µs，设备 tick 为 1、2，频率 100 Hz。主机数值随调度变化。MVS 的设备耗时保留未知。 |
| 实际 SDK | 本机 x64 MVS SDK 自报 4.7.1.4，GigE/USB 枚举成功，发现 0 台。把方案副本改用 MVS 和不存在的序列号，单次采集及触发 CLI 均返回退出码 1 和 `ResourceNotFound`；触发报告收到 0 帧且成功关闭。 |

在桌面打开样例后，选择 **单帧采集 → 采集一帧 → Properties → Resources**，可用 **New…** 新建相机，或用 **Edit device…** 选择适配器和序列号。MVS 适配器可用 **Scan devices** 枚举。通过 **Run → Run Workflow** 验证采集节点。选择 **相机触发 → Properties → Trigger source → Camera frames** 可编辑设备、Image 输入、坐标系、模式与超时。用 **Run → Arm Workflow Trigger** 开始，用 **Stop Trigger and Drain** 排空，或用 **Stop Trigger and Cancel Runs** 取消。运行面板显示受理、拒绝、缺帧、重复及停止状态。

从仓库根目录在 MSVC x64 开发环境中执行。以下命令复用已按 [§3.28](#328-s55-release-构建与测试) 配好的 Ninja 构建目录。MVS 构建需要开发包，`MVCAM_COMMON_RUNENV` 指向含 `Includes` 与 `Libraries/win64` 的目录。运行前将匹配的 MVS x64、OpenCV Release DLL 目录加入 `PATH`。无 SDK 时使用 `-DVISIONPRO_WITH_MVS=OFF` 可运行模拟相机和 CLI。

```powershell
$guiBuild = 'out/build/s67-gui'
$headlessBuild = 'out/build/s67-headless'
cmake -S . -B $guiBuild -DVISIONPRO_WITH_GUI=ON -DVISIONPRO_WITH_OPENCV=ON -DVISIONPRO_WITH_MVS=ON
cmake --build $guiBuild --config Release --parallel 6
cmake -S . -B $headlessBuild -DVISIONPRO_WITH_GUI=OFF -DVISIONPRO_WITH_OPENCV=OFF -DVISIONPRO_WITH_MVS=OFF
cmake --build $headlessBuild --config Release --parallel 6
$cli = "$guiBuild/source/app/visionpro_cli.exe"
$solution = 'samples/s6-camera/solution.json'
& $cli cameras
& $cli run --solution $solution --workflow 5ca4f87a-ea1e-4d62-89b0-000000061010
& $cli trigger --solution $solution --workflow 5ca4f87a-ea1e-4d62-89b0-000000061020 --camera-frames 2 --stop drain
ctest --test-dir $guiBuild -C Release -R 'camera|mvs_' --output-on-failure --no-tests=error
ctest --test-dir $headlessBuild -C Release -R 'camera' --output-on-failure --no-tests=error
```

现场目前没有工业相机。实机验收需记录型号、序列号、驱动与固件版本，选用设备支持的原生 Mono8、RGB8Packed 或 BGR8Packed 格式，逐项验证 Single、Continuous、Software 和 Line0 硬触发。记录每种模式的帧号、时间来源、收帧与受理计数、缺帧与拒绝、停止状态，并在拔插重连后核对首帧保留及新订阅的触发次数。

### 10.11 S6.11 TCP 与 Modbus 通信

2026-11-19，Windows x64 Release 全量 CTest 通过。启用 Qt、OpenCV 和 MVS 的构建为 **171/171** 项，关闭三者的无 GUI 构建为 **130/130** 项。TCP 对端和 Modbus TCP 模拟器独立于生产客户端。桌面使用 Qt Widgets offscreen，从属性面板创建或编辑端点、另存并重开，独立 CLI 加载保存后的方案。现场没有实物 PLC。[固定方案](../samples/s6-communication/solution.json) · [测试对端](../tests/app/communication_peer.py) · [独立 CLI 验收](../tests/app/communication_cli_tests.py)

| 场景 | 观测结果 |
|---|---|
| TCP 拆包、粘包 | 对端拆开发送第一帧的头和载荷，再把第一帧尾部与完整第二帧一次发送。两次接收分别取到 `41 00 42`、`4e 45 58 54`。桌面还核对了输入 `43 00 ff` 和含零常量。空帧和 65536 字节边界均有覆盖。 |
| TCP 中途断连 | 对端声明 5 字节，只交付前三个字节就关闭。第一次接收未发布载荷，错误出口上的第二次接收建立新连接并取得 `fresh`。对端只收到一次原始发送请求。 |
| 按工件发送 | 外部触发工件 41、42。格式化节点和 UTF-8 编码节点将两条带工件号的消息交给 TCP 发送。对端依次只收到一条“工件=41”和一条“工件=42”，CLI 保留每轮关联键。 |
| 正常 PLC 写读 | 单元 1、零基地址 16，FC06 写入 `0x1234`。请求后六字节为 `01 06 00 10 12 34`，应答逐字节回显。随后 FC03 读取一个保持寄存器，响应 PDU 为 `03 02 12 34`，返回 4660。 |
| 已写入、应答丢失 | 模拟器先改变寄存器，再关闭连接。写调用只尝试一次，`confirmed` 未产生，`ExternalEffectUnknown` 带逻辑操作键和 Winsock `recv` 原码 0。桌面在运行详情中核对诊断后，切换到只读流程读回 4660。CLI 通过显式错误出口重连并读回 4660，随后独立只读流程得到同一值，模拟器对该场景记录的总写入次数为 1。 |
| 异常应答与重试 | 异常码 `02` 保留为已知地址错误，未知的 `42` 保留原码并标为未映射。错事务号或错回显不会确认写入。单独丢弃一次读应答时，配置重试的 FC03 再读成功，FC06 次数仍为 1。写节点配置自动重试时，预检报告 `MissingCapability`，对端收到 0 个请求。 |

一次独立 CLI 验收记录到 FC06 请求与应答均为 `000100000006010600101234`，主机调用耗时 24,221,900 ns。FC03 请求为 `000200000006010300100001`，应答为 `0002000000050103021234`。这两种应答均不携带设备处理耗时，报告为 `value=null, unit=unknown`。主机耗时随本机调度和连接状态变化。

按 [§3.28](#328-s55-release-构建与测试) 配置 Release 构建目录。客户端使用 Windows 自带的 Winsock，无 GUI 构建也包含四个通信节点。回环 CLI 和桌面验收使用 Python 3 标准库。CTest 夹具自动选择本机临时端口。

```powershell
$guiBuild = 'out/build/s67-gui'
$headlessBuild = 'out/build/s67-headless'
cmake --build $guiBuild --config Release
cmake --build $headlessBuild --config Release
ctest --test-dir $guiBuild -C Release -R 'communication|visionpro_(tcp|plc)_parity' --output-on-failure --no-tests=error
ctest --test-dir $headlessBuild -C Release -R 'communication' -V --no-tests=error
```

手工复现时，在两个终端分别运行以下两条命令，保留两个对端进程。它们仅监听本机回环地址。

```powershell
python -B tests/app/communication_peer.py --protocol tcp --port 16111
python -B tests/app/communication_peer.py --protocol modbus --port 16112
```

桌面打开通信样例，在对应流程选择 **Run → Run Workflow**。节点的 **Properties → Resources → Edit device…** 可编辑地址、端口及 Modbus 单元号。TCP 输入可填十六进制，在运行面板中选择接收节点并用 **Show Bytes** 核对原始字节。在第三个终端执行下面的 CLI 命令即可对照。复现丢应答时，可在启动 Modbus 模拟器时附加 `--fault lost-ack`，先运行写读流程，再执行最后一条只读命令。结束后在两个对端终端按 Ctrl+C。

```powershell
$cli = 'out/build/s67-headless/source/app/visionpro_cli.exe'
$solution = 'samples/s6-communication/solution.json'
& $cli run --solution $solution --workflow 5ca4f87a-ea1e-4d62-89b0-000000061110 --input packet=410042
& $cli trigger --solution $solution --workflow 5ca4f87a-ea1e-4d62-89b0-000000061130 --frame 41 --frame 42
& $cli run --solution $solution --workflow 5ca4f87a-ea1e-4d62-89b0-000000061120
& $cli run --solution $solution --workflow 5ca4f87a-ea1e-4d62-89b0-000000061140
```

接入实物 PLC 后，需按现场约定的单元号和可写测试寄存器，另行核对设备型号、寄存器映射、应答、拔插恢复和设备端写入次数。

### 10.12 S6.12 软件综合验收

2026-11-19，Windows x64 Release 在开启 Qt、OpenCV、MVS 和关闭三者的配置中分别通过全量回归。新增 CLI `replay` 在同一会话内按序执行多个工作流，可复现宽高交错到达及重复工件。三套双入口测试、两种负载测量、输入格式及命令见 [S6.12 验收记录](s6-acceptance.md)。实物相机、PLC 与整线追溯待现场验证。
