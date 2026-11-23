# VisionPro

VisionPro 是一个采用 C++20、Qt、OpenCV 和 CMake 的桌面视觉算法软件项目。

目标是通过可视化流程编辑，将图像采集、视觉处理、结果判断和设备交互
组织为可配置、可运行、可保存的视觉应用方案。

功能规划参考 [VisionMaster 用户手册](docs/Manual/VisionMaster算法平台用户手册V3.0.2.pdf)，
调用与资源边界补充参考 [SDK 开发指南 V4.3.1（C++）](<docs/Manual/VisionMaster算法平台SDK开发指南V4.3.1（C++）.chm>)。
项目不承诺兼容其方案格式、插件或 SDK。

## 当前状态

S6.12 软件综合验收已通过。三套方案完成 GUI 编辑、保存重载与独立 CLI 对照，桌面基准与并发负载已复测。工业相机、实物 PLC 及整线联动待现场验证。[S6.12 验收记录与复现](docs/s6-acceptance.md)

S1 文档审查、S2 核心模型与绑定及 S3 无 GUI 执行与配置往返均已通过验收。S4 按本轮无 SHA-256 校验的范围完成验收和交接。固定方案覆盖真实视觉单链、五路汇合和旋转 ROI 继承。[S4 验收与样例复现](docs/s4-acceptance.md)

2026-11-16，S5.1 已完成。桌面入口使用 Qt 6.11.1 Widgets 和 QtNodes 3.0.16，内置流程示例可验证控制线分叉、汇合、命名出口、Group 导航和统一撤销重做。[S5.1 验收记录](docs/development.md#95-s51-接入与验收记录)

S5.2 已接通同会话草稿发布。成功受理时更新活动快照，已有运行及其后继继续使用本轮快照和资源提供方。桌面和 CLI 共用工具注册表。[S5.2 验收记录](docs/development.md#96-s52-应用服务验收记录)

S5.3 已完成桌面方案与流程编辑。画布、属性检查器和作用域诊断共用草稿与撤销历史，支持保存、另存、独立复制及重载。[S5.3 验收记录](docs/development.md#97-s53-方案与流程编辑验收记录)

S5.4 已完成 UInt8 灰度和 BGR 查看、像素检查及 ROI 编辑与预览。S4 固定样例验证了旋转、继承修正、排除后的成员掩膜，以及同轮 Blob 结果的包围框和质心叠加。[S5.4 验收记录](docs/development.md#98-s54-图像与-roi-验收记录)

S5.5 已接通桌面流程运行、取消、单节点隔离调试与有界结果历史。运行面板可查看本轮绑定来源，并在 Blob 实际输入图像上显示包围框和质心。[S5.5 验收记录](docs/development.md#99-s55-运行与调试验收记录)

S5.6 已完成第五阶段综合验收。三条 S4 流程在原始、语义修改和布局保存重载后通过 GUI/CLI 对照。桌面生命周期回归与 100 节点、300 条控制线、1920×1080 图像负载测量已通过。[S5 验收与交接](docs/s5-acceptance.md)

S6.1 已跑通双 Blob 循环。桌面可编辑循环边界、逐次查看取项结果，并把质心按迭代顺序导出为点集。保存重载后，独立 CLI 复现相同点集和面积汇总。[可运行方案](samples/s6-loop/solution.json) · [复现与验收](docs/development.md#101-s61-循环与点集)

S6.2 已将双 Blob 检测封装为可调用的 Group。可在桌面独立调试、以活动状态运行、导出带图像资源的子图包，并在另一份方案中作为新实例导入。[Group 样例](samples/s6-group/solution.json) · [桌面操作与 CLI 命令](docs/development.md#102-s62-group-调用与跨方案复用)

S6.3 已实现整数计算与累加、动态下标绑定、五路判断分流和格式化。样例可在桌面保存后用独立 CLI 复现，也可在关闭 Qt 和 OpenCV 的配置中运行。[计算与判断样例](samples/s6-calculation/solution.json) · [复现与验收](docs/development.md#103-s63-计算与逻辑)

S6.4 已接入会话全局值、显式队列收发和按工件汇合。固定方案让 A、B 的宽高交错到达，依次交付 B=(20,2)、A=(10,1) 各一次。桌面可只读预览全局版本、队列消息和汇合统计，另存的方案可由独立 CLI 复现。[共享数据样例](samples/s6-shared/solution.json) · [复现与验收](docs/development.md#104-s64-共享值与工件汇合)

S6.5 已支持多流程并行和同一流程的有界重入。每个流程可配置在途上限、FIFO 等待容量和满载响应。桌面可查看及取消选定轮次，旧方案继续使用单轮在途和 `Busy`。独占资源在工具实际退出后释放。[并行与重入样例](samples/s6-concurrency/solution.json) · [复现与验收](docs/development.md#105-s65-多流程并行与有界重入)

S6.6 已支持连续时钟和可控外部帧触发。桌面可为流程配置来源、帧号输入和间隔，并在停止时选择排空或取消。CLI 的 `trigger` 命令可重放同一方案，逐帧输出关联键、受理结果和运行摘要。[触发样例](samples/s6-trigger/solution.json) · [桌面与 CLI 复现](docs/development.md#106-s66-连续运行与外部触发)

S6.7 支持为工具节点设置每次尝试的超时、最大重试次数和错误出口。运行摘要保留每次尝试的状态，以及最终失败是否进入恢复分支。样例用整数溢出复现错误出口。[恢复样例](samples/s6-recovery/solution.json) · [桌面与 CLI 验收](docs/development.md#107-s67-超时错误出口和安全重试)

S6.8 支持圆、环、扇环和多边形 ROI，以及 N 点平面标定、像素点转毫米和两点测距。固定方案将 Blob 质心从 `(3,4) px` 转为 `(16,8) mm`，到参考点的距离为 `5 mm`。[测量样例](samples/s6-calibration/solution.json) · [桌面与 CLI 验收](docs/development.md#108-s68-roi-与标定测量)

S6.9 支持平移模板定位、随工件移动的 ROI 颜色测量、浮点闭区间判断和灰度分段类别图。固定方案从同一次定位取得五路测量，汇合后发送一次结果；类别图可在桌面查看标签、数量并按 ID 调色。[工件检测方案](samples/s6-inspection/solution.json) · [桌面与 CLI 验收](docs/development.md#109-s69-工件定位五路测量与类别图)

S6.10 已接入相机采集节点和相机帧触发入口，支持模拟设备与可选的海康 MVS 后端。一次触发向流程同时交付帧号和只读图像。模拟设备已验证缓冲复用、断连重连和停止后的迟到脉冲。实际 MVS SDK 已完成 GigE/USB 枚举，现场有工业相机后可继续真机验收。[相机样例](samples/s6-camera/solution.json) · [桌面与 CLI 验收](docs/development.md#1010-s610-相机采集与触发)

S6.11 已接入 TCP 二进制帧收发和 Modbus TCP 保持寄存器读写。样例可在本机发送含零字节的帧、按工件号发送 UTF-8 消息，以及写入并读回寄存器。写后丢应答时，运行记录保留操作键并报告效果未知，可以单独读回核对。PLC 设备侧本轮使用回环模拟器。[通信样例](samples/s6-communication/solution.json) · [桌面与 CLI 验收](docs/development.md#1011-s611-tcp-与-modbus-通信)

- 已建立初始目录、开发规则及基础设计文档。
- 已确定 C++20、Qt、OpenCV、CMake 技术栈。
- 已建立 `visionpro_core` 静态库，包含身份、配置/运行值、工具元数据、只读图像与裁剪、几何/ROI/掩膜、资源与状态定义及局部校验、结构化诊断和 CTest。
- 已建立 `visionpro_workflow`，包含方案/流程/节点、控制/绑定/包含关系、Group 公开数据与控制映射、配置快照、单次请求、结果记录及复制重映射。
- C01–C10 为 S1 审查通过的设计基线，具体能力按后续阶段实现和验收。
- 2026-10-27 的逐项结论见 [S1 文档审查记录](docs/development.md#81-s1-文档审查记录)。
- 2026-10-30，S2.1 在 Windows x64/MSVC 下完成 Release 构建，6 项 CTest 通过（含预期失败退出验证）；见 [S2.1 验收记录](docs/development.md#84-s21-实现与验证记录)。
- S2.2 已在同一环境完成 Release 构建，14 项 CTest 通过（含 S2.1 回归及预期失败检查）；见 [S2.2 验收记录](docs/development.md#85-s22-实现与验证记录)。
- S2.3 已完成 Release 构建，24 项 CTest 通过（含前两阶段回归及预期失败检查）；见 [S2.3 验收记录](docs/development.md#86-s23-实现与验证记录)。
- S2.4 已完成 Release 构建，34 项 CTest 通过（含核心回归及预期失败检查）；见 [S2.4 验收记录](docs/development.md#87-s24-实现与验证记录)。
- S2.5 新增 `validate_solution`，检查流程图、公开边界、类型化绑定及嵌套领域引用，返回独立的派生依赖索引；2026-11-09 完成 Release 构建，39/39 项 CTest 通过。见 [S2.5 验收记录](docs/development.md#88-s25-实现与验证记录)。
- S2.6 新增会话内 `ResultStore` 和 `BindingResolver`，按精确调用身份解析已有结果、作用域公开输入、常量和只读资源/全局快照，区分成功、等待、不可用和失败；2026-11-09 完成 Release 构建，44/44 项 CTest 通过。见 [S2.6 验收记录](docs/development.md#89-s26-实现与验证记录)。
- S2.7 已收敛 RunRequest/RunHandle、实例工厂及 InvocationOperation，补齐固定调用投影和组合验证；2026-11-09 完成 Release 全量重建，48/48 项 CTest 通过。14 项 S2 必需子范围全部通过，见 [S2 整体验收与 S3 交接清单](docs/development.md#810-s27-与-s2-整体验收记录)。
- 2026-11-11 完成 Manual 复核及 [S3.1–S3.9 实施计划](docs/development.md#6-第三阶段实施与验证)，覆盖内存执行、生命周期、配置往返及无 GUI 入口。
- S3.1 的执行计划固定工具配置和请求输入，并复用同一范围解析绑定。逐输出可用性、节点状态规格、外部诊断、耗时和异步通知已通过模拟验证。Windows x64 Release 的 55 项 CTest 全部通过。见 [S3.1 实现与验证](docs/development.md#812-s31-实现与验证记录)。
- S3.2 的 `WorkflowEngine` 支持按流程受理、同步顺序调用、FailFast 和本轮结果查询。不同流程独立受理，由同一工作线程按受理顺序执行。Windows x64 Release 的 59 项 CTest 全部通过。见 [S3.2 实现与验证](docs/development.md#813-s32-实现与验证记录)。
- S3.3 的执行器通过通知推进异步操作，在同一仲裁点处理取消和结果提交。逻辑终态后仍持有流程槽位，实际退出并清理后归还。Windows x64 Release 的 62 项 CTest 全部通过。见 [S3.3 实现与验证](docs/development.md#814-s33-实现与验证记录)。
- S3.4 的执行器按控制边到达与关闭调度无环流程，支持 AllActivated 汇合、命名出口零选与多选，并按本轮绑定就绪推进兄弟分路。见 [S3.4 实现与验证](docs/development.md#815-s34-实现与验证记录)。
- S3.5 的执行器支持每轮重置、跨轮累计、显式重置及单节点独立调用。调试默认使用隔离状态。见 [S3.5 实现与验证](docs/development.md#816-s35-实现与验证记录)。
- S3.6 支持精确查询已保留的运行结果、按容量读取观察通知，并在旧会话排空后启用校验通过的内存方案。见 [S3.6 实现与验证](docs/development.md#817-s36-实现与验证记录)。
- S3.7 支持版本化 `solution.json`、固定文件资源的 SHA-256 校验与字节保留，以及带诊断打开已知格式的草稿。见 [S3.7 实现与验证](docs/development.md#818-s37-实现与验证记录)。
- S3.8 在本地 NTFS 支持覆盖保存。提交前故障保留旧方案，协作保存者可检测修订冲突，运行持有的旧资源在退出后可重试清理。见 [S3.8 实现与验证](docs/development.md#819-s38-实现与验证记录)。
- S3.9 提供无 GUI 的 `visionpro_cli`。五路汇合与条件分支在磁盘方案保存前后的结果、数据来源和控制行为一致。见 [S3.9 验收记录](docs/development.md#820-s39-与-s3-整体验收记录)。

## 设计基线与当前重点

S1 已确定核心概念、数据结构和接口契约，包括：

- 方案、流程、工具定义、模块配置与运行实例的职责边界。
- 参数、端口、数据类型与数据引用。
- 执行编排与输入源、参数订阅的分离。
- 全局变量、数据队列和工具状态的独立契约。
- 图像、几何、ROI、坐标系与资源。
- 执行状态、检测判定、诊断和运行结果。
- 模块依赖、所有权与持久化边界。

第一阶段不以完成 GUI 或运行一个最小算法流程为验收条件。
上述内容已通过 S1 文档审查；目前已实现核心领域类型、方案/关系模型、快照、请求与结果记录。
完整图/绑定静态校验和运行绑定解析已通过 Release 验证。

S2 将这些契约落实为可构建、可测试的无 GUI 核心库，完成方案模型、配置校验和数据绑定解析。
它既包含基础类型，也包含实际的校验与取值逻辑；节点调度、取消和配置读写在 S3 接入。
具体交付和退出条件见 [S2 范围](docs/requirements.md#4-第二阶段核心模型与绑定验证)，
实施顺序和首批任务见 [S2 实施与验证](docs/development.md#5-第二阶段实施与验证)。

S3 的任务依赖、26 项验收子范围及退出条件见
[S3 实施与验证](docs/development.md#6-第三阶段实施与验证)。

## 流程与数据模型

画布主连线表示执行编排，不代表图像或结果沿相邻节点自动传递。
工具输入和可订阅参数分别选择数据来源，统一通过类型化绑定解析。
订阅本身不触发执行；同流程引用只能读取当前执行上下文中有效的结果。

跨流程共享当前值使用全局变量，按序消费数据使用数据队列；
累加器等跨调用状态由工具状态单独管理。
引擎分别管理工具状态、当前全局值和队列消息。方案只保存定义，重新打开后全局值从初值开始，队列为空。

运行计划基于不可变配置快照，结果按运行、作用域及逻辑调用隔离。
取消或过期的完成回调不能发布新结果或触发后继；设备连接由独立资源服务管理。
控制模型支持普通节点多前驱/多后继，分叉、汇合和基本条件分支纳入第三阶段无 GUI 验证。
该阶段可采用单工作线程、每流程一个在途运行、Busy 准入和无自动重试；
这些是阶段策略，不把模型限制为单链或整个方案全局互斥。
S6 已支持结构化循环、可取消的迭代间隔、多流程并行、可控事件和相机帧触发。通信节点可将本轮工件结果发往 TCP 对端，或通过 Modbus TCP 访问单个 UInt16 保持寄存器。

实例寿命与状态寿命分开，累积/首次初始化按声明的状态域管理。
ROI 包含旋转、继承、排除、栅格掩膜及坐标变换；资源同时支持托管、外部引用和动态来源。
SDK 参考补齐了一次性运行输入、复合 Group 公开边界、逐输出有效性、事件订阅与显式关闭规则。
GUI 和无 GUI 入口共用运行请求及结果接口，第三方 SDK 的名称、指针和异常限定在适配层。
详细契约见 [核心数据模型](docs/data-model.md)，
逐项手册依据与验收场景见 [C01–C10 依据映射](docs/requirements.md#2-c01c10-依据设计理由与验收映射)。

## 技术方案

| 项目 | 选择 | 状态 |
|---|---|---|
| 语言标准 | C++20 | 已确定 |
| GUI 框架 | Qt 6.11.1 / MSVC 2022 x64 | S5.1 Release 构建与部署启动通过 |
| 算法依赖 | OpenCV | 已确定 |
| 构建工具 | CMake | 已确定 |
| 桌面界面 | Qt Widgets | 已接通 `visionpro_gui` |
| 流程编辑器 | QtNodes 3.0.16 通用图层 | 已锁定源包，S5.1 画布交互通过 |

核心流程模型、运行调度和方案契约由 VisionPro 独立定义。
逻辑上划分应用层、核心层与能力层，具体模块映射见 [architecture.md](docs/architecture.md)。
首批工具按类型静态注册，动态插件后置。

S3.9 在 Windows x64、MSVC 19.51、CMake/CTest 4.4.0 环境完成 Release 验证，82/82 项 CTest 通过。
复现步骤见 [S3.9 构建与命令行验证](docs/development.md#316-s39-构建与命令行验证)。
当前构建与验收统一使用 Release。视觉目标由默认关闭的 `VISIONPRO_WITH_OPENCV` 启用，使用现有 OpenCV `4.14.0-pre` x64 Release 预编译包。见 [S4.7 构建与回归](docs/development.md#323-s47-release-构建与回归)。

## 开发阶段

1. 基础模型与契约设计：文档审查通过。
2. 核心模型与绑定验证。S2.1–S2.7 完成，Release 及全部 S2 必需子范围通过。
3. 无 GUI 执行与配置往返验证。S3.1–S3.9 完成，26 项必验子范围全部通过。
4. 无 GUI 视觉闭环。S4.1–S4.7 已完成，本轮 Release 回归 ON 97 项、OFF 81 项通过。见 [S4 验收范围](docs/s4-acceptance.md#release-回归范围)。
5. 基础可视化界面。S5.1–S5.6 已完成，GUI/CLI 一致性、生命周期及负载测量见 [S5 验收与交接](docs/s5-acceptance.md)。
6. S6.1–S6.9 已完成，S6.10、S6.11 已完成软件接入，S6.12 已完成软件综合验收。实机项及其验收条件见 [S6 交接记录](docs/s6-acceptance.md#待实机验证)。

各阶段范围与验收标准见 [requirements.md](docs/requirements.md)。
先用内存配置、模拟工具和测试图像验证核心，再完成保存加载及真实视觉处理。
配置格式的身份、版本和资源边界提前设计，完整读写不阻塞最早的模拟执行实验。
无 GUI 验收要求同一方案可加载执行，并能追溯数据来源、执行顺序和结果；
界面随后复用相同模型和执行服务。

## 目录职责

| 路径 | 用途 |
|---|---|
| source/ | 各模块实现 |
| include/visionpro/ | 必要的公共接口，按需建立 |
| plugins/ | 后续扩展预留 |
| tests/ | 自动化测试和固定测试数据 |
| samples/ | 可直接运行的固定样例方案和输入清单 |
| docs/ | 需求、架构、数据模型和开发说明 |
| third_party/ | 确需随仓库保存的第三方内容 |
| out/build/ | 本地构建产物，已被 Git 忽略 |

子目录随实际实现建立，不要求提前创建全部模块。

## 构建与运行

默认 [CMake 配置](CMakeLists.txt) 构建无 GUI 的 `visionpro_cli`、核心与存储库和测试程序，
不需要 Qt、OpenCV 或厂商 SDK。UUID 生成后端当前使用 Windows CNG，其他平台后端尚未实现。
`visionpro_cli run` 接受 `solution.json`、流程 UUID 与本次输入，实际退出后输出 JSON 追溯摘要。
`visionpro_cli save` 提交方案的新修订。见 [命令行用法与闭环复现](docs/development.md#316-s39-构建与命令行验证)。

`visionpro_cli replay` 按给定顺序在同一会话运行多条流程，适合复现共享队列和工件汇合。[输入序列与重放命令](docs/s6-acceptance.md#复现)

使用 `VISIONPRO_WITH_GUI=ON` 构建 `visionpro_gui`。左侧工具箱按海康手册分为 15 栏，每栏可独立展开，节点图标每行最多三个。点击栏目名浏览，或搜索节点，双击图标或拖入画布即可添加。流程首次以 100% 倍率打开，右下角可缩放、恢复 1:1 或查看全图。在顶部选择流程并单次运行。单击节点，右上显示输出图，右下列出模块历史；范围判断分别显示本次实测值、允许范围和判定。画布底栏显示同轮统计。双击普通节点可编辑参数与绑定，双击 Group 可进入内部。[桌面工作区](docs/architecture.md#42-参考-visionmaster-的桌面工作区改版方案)

同时启用 OpenCV 后，可从 **View** 打开图像文件或方案图像资源，选择视觉节点后使用 **Preview Selected ROI**。在 **Properties → Edit ROI…** 修改区域配置。[S5.4 构建与样例复现](docs/development.md#327-s54-release-构建与测试)

`visionpro_cli cameras` 列出相机，`trigger --camera-frames N` 从方案配置的设备取 N 帧并逐帧提交。默认构建包含模拟相机。安装海康 MVS 开发包后可启用 `VISIONPRO_WITH_MVS=ON` 构建 GigE/USB 适配器。[相机配置与复现命令](docs/development.md#1010-s610-相机采集与触发)

启用 OpenCV 的桌面版可直接运行 `visionpro_gui samples/s6-inspection`。选择 **Judge lane 5** 后点击 **单次运行**，即可看到第五路测量的 ROI、实测值和判断范围。点击历史行可查看旧轮，勾选 **跟随最新** 可回到最新轮次。单节点调试、重试记录和诊断仍可从 **运行详情** 查看。[五路案例与复现命令](docs/development.md#109-s69-工件定位五路测量与类别图)

## 文档导航

- [开发规则](AGENTS.md)
- [需求与阶段验收](docs/requirements.md)
- [架构设计](docs/architecture.md)
- [参考 VisionMaster 的桌面工作区改版方案](docs/architecture.md#42-参考-visionmaster-的桌面工作区改版方案)
- [ANS1 设计建议的采纳与调整记录](docs/architecture.md#11-ans1-设计建议采纳记录)
- [SDK 指南补充的设计取舍](docs/architecture.md#12-sdk-指南补充的设计取舍)
- [核心数据模型与 C01–C10 契约](docs/data-model.md)
- [S2 实施顺序、首批任务与验收覆盖](docs/development.md#5-第二阶段实施与验证)
- [S3 整体验收矩阵](docs/s3-acceptance.md#场景证据)
- [S4 实施顺序与验收标准](docs/development.md#7-第四阶段实施与验收计划)
- [S4 验收矩阵与固定样例](docs/s4-acceptance.md)
- [S5 实施顺序、验收与剩余实施细节](docs/development.md#9-第五阶段实施与验收计划)
- [开发与验证](docs/development.md)
