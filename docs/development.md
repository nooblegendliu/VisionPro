# 开发与验证

## 1. 当前阶段

C01–C10 已整理为当前设计基线，权威契约见 [data-model.md](data-model.md)。
S1 的 2026-10-27 文档审查结果见 [§8.1](#81-s1-文档审查记录)。
S2.1–S2.4 已于 2026-10-30 完成：已有 `visionpro_core`、`visionpro_workflow`、核心领域类型、
方案/关系模型、配置快照、类型化请求、结果记录及复制重映射。Debug/Release 结果见
[§8.4](#84-s21-实现与验证记录)、[§8.5](#85-s22-实现与验证记录)、[§8.6](#86-s23-实现与验证记录)、
[§8.7](#87-s24-实现与验证记录)。尚无执行器或可运行应用。
S2 核心模型与绑定验证的实施规划已于 2026-10-28 确定，按 [§5](#5-第二阶段实施与验证) 的七个子阶段推进；
S2.5 静态校验已完成并通过 Debug/Release 验证，记录见 [§8.8](#88-s25-实现与验证记录)；
S2.6 结果访问与绑定解析已完成并通过 Debug/Release 验证，记录见 [§8.9](#89-s26-实现与验证记录)；
S2.7 运行契约与组合验证已完成，Debug/Release 各 48/48 项 CTest 通过；S2 整体验收通过，证据与 S3 交接见 [§8.10](#810-s27-与-s2-整体验收记录)。
SDK 指南已用于补充契约与用例，未运行厂商 Demo、链接 SDK 或验证设备行为。

当前不要求建立完整应用；S2.4–S2.6 已验证命令保留在 §3.4–§3.6，S2.7 当前命令见 §3.7。

开发顺序以 [requirements.md](requirements.md) 为准：

| 阶段 | 主要交付与验证方式 |
|---|---|
| 1. 基础模型与契约设计 | 文档审查通过；历史审查记录见 §8.1–§8.3 |
| 2. 核心模型与绑定验证 | S2.1–S2.7 完成；14 项必需子范围和 Debug/Release 验证通过，整体记录见 §8.10 |
| 3. 无 GUI 执行与配置往返验证 | 模拟工具验证控制图、运行请求、结果快照、回调/退出及保存加载 |
| 4. 无 GUI 视觉闭环 | 使用少量真实 OpenCV 工具及固定图像验证结果 |
| 5. 基础可视化界面 | 复用核心服务，验证界面方案与无 GUI 执行一致 |
| 6. 后续能力 | 完整循环、多流程并行、触发、共享服务、设备及扩展工具 |

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
| 语言/运行库 | C++20；Debug `/MDd`，Release `/MD`；库与测试一致 |
| 测试依赖 | CTest + 仓库内轻量测试程序，无第三方测试框架 |

安装 Visual Studio 的 C++ 工具和 Windows SDK，并将 CMake/CTest 加入 PATH；
使用上述 VS 生成器时无须预先把 `cl.exe` 加入普通终端 PATH。
根 CMake 仍声明最低版本 3.25，此次未在该最低版本验证，也不承诺它支持本次使用的 VS 2026 生成器。
UUID 使用 Windows SDK 的 `bcrypt` 库和系统 `BCryptGenRandom`，API 依据见
[Microsoft Learn](https://learn.microsoft.com/en-us/windows/win32/api/bcrypt/nf-bcrypt-bcryptgenrandom)。
当前非 Windows 配置会明确失败；添加对应随机源后端后才能验证其他平台。

Qt、OpenCV 的具体版本和获取方式在相应模块接入前确定；流程编辑器选型在界面实现前完成。
首个核心构建不以 GUI 或 OpenCV 为前提；图像模型需要时可按 architecture §3 受控使用 OpenCV，
不将首批依赖选择扩展为核心永久禁止 OpenCV 的限制。

不得混用不兼容架构、编译器或运行库配置的依赖。

### 2.1 SDK 参考与未来适配环境

参考文件及版本差异见 [requirements.md](requirements.md) §2.1。
该版指南 Qt 控件章节的 Qt5.0–5.10/MSVC/QAxWidget 建议属于其二次开发环境，
不替本项目选择 Qt/CMake/编译器版本，也不使核心或无 GUI 测试依赖 COM 控件。

实际增加厂商适配器前，分别记录并验证 SDK 头文件/库版本、运行时/算子包、
架构/编译器/运行库、驱动和所需模块能力；Schema/方案版本与这些版本分开。
核实运行时版本、弃用接口替代、UTF-8/原生路径转换及接口线程/生命周期能力；
缺能力时明确诊断，不以 API 存在或文档文件名证明可运行。
原生核心测试不安装厂商运行时，适配集成与真实设备验证单独分组。
本次只读取静态文档，未执行其环境配置步骤或示例代码。

## 3. 构建约定

- 使用源码外构建目录。
- 使用 target 级编译要求、头文件路径和链接依赖。
- 要求 C++20，关闭非必要编译器扩展。
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

在仓库根目录的 PowerShell 中运行。Debug 与 Release 使用独立目录，
VS 为多配置生成器，选择配置使用 `--config` / `-C`，不依赖 `CMAKE_BUILD_TYPE`。
本节保留 S2.1 当时执行的命令和结果；当前源码请使用 §3.7，测试总数已扩充。

```powershell
cmake -S . -B out/build/s21-debug -G "Visual Studio 18 2026" -A x64 -DBUILD_TESTING=ON
cmake --build out/build/s21-debug --config Debug
ctest --test-dir out/build/s21-debug -C Debug --output-on-failure --no-tests=error

cmake -S . -B out/build/s21-release -G "Visual Studio 18 2026" -A x64 -DBUILD_TESTING=ON
cmake --build out/build/s21-release --config Release
ctest --test-dir out/build/s21-release -C Release --output-on-failure --no-tests=error
```

每条命令应返回 0；任一失败先处理再继续。S2.1 当时 CTest 结果为每种配置 6/6 通过，
其中 `core.failure_exit` 使用 `WILL_FAIL` 验证故意失败确实返回非零，不能删除该检查使测试变绿。
下列直接调用预期打印一次 `FAIL`，并分别得到退出码 **1**：

```powershell
& ./out/build/s21-debug/tests/core/Debug/visionpro_core_tests.exe --force-failure
$LASTEXITCODE
& ./out/build/s21-release/tests/core/Release/visionpro_core_tests.exe --force-failure
$LASTEXITCODE
```

测试输出保存在各构建目录的 `Testing/Temporary/LastTest.log`；
需要完整检查输出时可在 CTest 命令上增加 `-V`。

只构建核心库的入口也已验证：

```powershell
cmake -S . -B out/build/s21-core-only -G "Visual Studio 18 2026" -A x64 -DBUILD_TESTING=OFF
cmake --build out/build/s21-core-only --config Release
```

### 3.2 S2.2 构建与测试命令

本节为 S2.2 历史记录；当前源码请使用 §3.7。
同样在仓库根目录 PowerShell 执行，继续使用 §2 的工具链，两个独立目录覆盖 Debug/Release：

```powershell
cmake -S . -B out/build/s22-debug -G "Visual Studio 18 2026" -A x64 -DBUILD_TESTING=ON
cmake --build out/build/s22-debug --config Debug
ctest --test-dir out/build/s22-debug -C Debug --output-on-failure --no-tests=error

cmake -S . -B out/build/s22-release -G "Visual Studio 18 2026" -A x64 -DBUILD_TESTING=ON
cmake --build out/build/s22-release --config Release
ctest --test-dir out/build/s22-release -C Release --output-on-failure --no-tests=error
```

每条命令应返回 0，每种配置为 **14/14 通过**，包含 S2.1 的 6 项及 S2.2 新增 8 项。
每项失败均通过进程退出码传给 CTest；测试不依赖 Release 下禁用的 `assert`。
以下故意失败入口也在本次构建上重验，两种配置的 `$LASTEXITCODE` 均应为 **1**：

```powershell
& ./out/build/s22-debug/tests/core/Debug/visionpro_core_tests.exe --force-failure
$LASTEXITCODE
& ./out/build/s22-release/tests/core/Release/visionpro_core_tests.exe --force-failure
$LASTEXITCODE
```

测试日志分别保存在 `out/build/s22-debug/Testing/Temporary/LastTest.log` 和
`out/build/s22-release/Testing/Temporary/LastTest.log`，构建目录与日志均不提交。

### 3.3 S2.3 构建与测试命令

本节为 S2.3 历史记录；当前源码请使用 §3.7。
在仓库根目录的 PowerShell 中执行，沿用 §2 工具链和独立构建目录：

```powershell
cmake -S . -B out/build/s23-debug -G "Visual Studio 18 2026" -A x64 -DBUILD_TESTING=ON
cmake --build out/build/s23-debug --config Debug
ctest --test-dir out/build/s23-debug -C Debug --output-on-failure --no-tests=error

cmake -S . -B out/build/s23-release -G "Visual Studio 18 2026" -A x64 -DBUILD_TESTING=ON
cmake --build out/build/s23-release --config Release
ctest --test-dir out/build/s23-release -C Release --output-on-failure --no-tests=error
```

每条命令应返回 0，每种配置为 **24/24 通过**，包括原有 14 项与 S2.3 新增 10 项。
测试使用固定内存图像、结构化配置与运行记录，不访问设备或外部资源，也不启动 GUI。
预期失败入口在两种配置中均须实际返回 **1**：

```powershell
& ./out/build/s23-debug/tests/core/Debug/visionpro_core_tests.exe --force-failure
$LASTEXITCODE
& ./out/build/s23-release/tests/core/Release/visionpro_core_tests.exe --force-failure
$LASTEXITCODE
```

CTest 日志分别位于 `out/build/s23-debug/Testing/Temporary/LastTest.log` 与
`out/build/s23-release/Testing/Temporary/LastTest.log`；本次构建输出另保留在两个目录的 `build.log`。
这些本地产物均被 Git 忽略。验收内容和具体检查数见 §8.6。

### 3.4 S2.4 构建与测试命令

本节为 S2.4 历史记录；当前源码请使用 §3.7。
在仓库根目录 PowerShell 中运行；沿用 §2 工具链，两个源码外目录分别验证 Debug/Release：

```powershell
cmake -S . -B out/build/s24-debug -G "Visual Studio 18 2026" -A x64 -DBUILD_TESTING=ON
cmake --build out/build/s24-debug --config Debug
ctest --test-dir out/build/s24-debug -C Debug --output-on-failure --no-tests=error

cmake -S . -B out/build/s24-release -G "Visual Studio 18 2026" -A x64 -DBUILD_TESTING=ON
cmake --build out/build/s24-release --config Release
ctest --test-dir out/build/s24-release -C Release --output-on-failure --no-tests=error
```

上述命令均应返回 0，每种配置为 **34/34 通过**：已有 24 项核心测试与新增 10 项 workflow 测试。
预期失败入口单独调用也须返回 **1**，验证 Release 不因禁用 assert 而跳过断言：

```powershell
& ./out/build/s24-debug/tests/core/Debug/visionpro_core_tests.exe --force-failure
$LASTEXITCODE
& ./out/build/s24-release/tests/core/Release/visionpro_core_tests.exe --force-failure
$LASTEXITCODE
```

CTest 完整输出位于两个构建目录的 `Testing/Temporary/LastTest.log`，本次构建输出保存在对应 `build.log`。
构建目录与日志均被 Git 忽略；不需要 Qt、OpenCV、厂商 SDK 或外部设备。
实际结果、固定夹具与覆盖边界见 §8.7。

### 3.5 S2.5 构建与测试命令

在仓库根目录 PowerShell 中运行，沿用 §2 工具链与无 GUI 目标：

```powershell
cmake -S . -B out/build/s25-debug -G "Visual Studio 18 2026" -A x64 -DBUILD_TESTING=ON
cmake --build out/build/s25-debug --config Debug --parallel 1
ctest --test-dir out/build/s25-debug -C Debug --output-on-failure --no-tests=error

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
CTest 详细输出分别位于两个构建目录的 `Testing/Temporary/LastTest.log`，构建输出保留在对应 `build.log`；
这些日志和构建目录均被 Git 忽略。

### 3.6 S2.6 构建与测试命令

在仓库根目录 PowerShell 中运行，沿用 §2 工具链、无 GUI 目标和已验证的串行 MSBuild 方式：

```powershell
cmake -S . -B out/build/s26-debug -G "Visual Studio 18 2026" -A x64 -DBUILD_TESTING=ON
cmake --build out/build/s26-debug --config Debug --parallel 1
ctest --test-dir out/build/s26-debug -C Debug --output-on-failure --no-tests=error

cmake -S . -B out/build/s26-release -G "Visual Studio 18 2026" -A x64 -DBUILD_TESTING=ON
cmake --build out/build/s26-release --config Release --parallel 1
ctest --test-dir out/build/s26-release -C Release --output-on-failure --no-tests=error
```

以上配置、构建与 CTest 命令均已返回 0，Debug/Release 各 44/44 项通过，实际结果见 §8.9。
S2.6 注册 `workflow.result_store` 及 `workflow.resolver_values`、`workflow.resolver_availability`、
`workflow.resolver_isolation`、`workflow.resolver_domains`，保留此前 39 项 CTest，合计 44 项。
测试使用人工终态记录、显式作用域/公开输入、内存资源目录和只读全局快照；
不启动工具、读取外部文件、执行队列消费或连接设备。
CTest 详细输出位于对应构建目录的 `Testing/Temporary/LastTest.log`；构建目录及日志均被 Git 忽略。

### 3.7 S2.7 构建与测试命令

在仓库根目录 PowerShell 中运行，沿用 §2 工具链和串行 MSBuild。公共结构增加字段时使用完整重建，避免混用旧对象文件：

```powershell
cmake -S . -B out/build/s27-debug -G "Visual Studio 18 2026" -A x64 -DBUILD_TESTING=ON
cmake --build out/build/s27-debug --config Debug --parallel 1 --clean-first
ctest --test-dir out/build/s27-debug -C Debug --output-on-failure --no-tests=error

cmake -S . -B out/build/s27-release -G "Visual Studio 18 2026" -A x64 -DBUILD_TESTING=ON
cmake --build out/build/s27-release --config Release --parallel 1 --clean-first
ctest --test-dir out/build/s27-release -C Release --output-on-failure --no-tests=error
```

S2.7 保留全部既有测试，并补运行接口替身、所有权/错误边界和 S2 组合验收。
新增 `core.tool_execution`、`workflow.run_handles`、`workflow.composition` 和 `workflow.tool_execution`，
共 48 项 CTest；两种配置的实际结果记录在 §8.10。
CTest 证据保存在各构建目录的 `Testing/Temporary/LastTest.log`；构建日志一并保留，不能以编译成功代替行为验证。
模拟调用不得记为真实算法、调度器或设备验证。

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
S2.6 已完成并通过 Debug/Release 验证，结果见 §8.9；S2.7 及 S2 整体验收已通过，记录见 §8.10。

### 5.1 子阶段与依赖顺序

| 子阶段 | 主要交付 | 退出条件 |
|---|---|---|
| S2.1 工程底座与首个领域类型 | 实际 visionpro_core 目标、CTest 入口、最小强类型身份和诊断；确定首个验证环境 | 无 GUI 配置、Debug/Release 编译与首批真实行为测试通过，命令及环境可复现 |
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
5. 用 CTest 注册真实行为测试，覆盖合法/非法身份、文本往返及类型隔离；在独立构建目录验证 Debug、Release，并确认失败能够使测试返回失败。
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
- 首个验证环境中，core/workflow 及测试可独立于 GUI 构建，Debug/Release 的真实执行结果可复现。
- 固定内存方案贯通“构造模型 → 静态校验 → 人工结果查询与绑定解析 → 值或诊断”；不把人工夹具写成已完成节点执行或真实检测。
- 公共契约与实现一致，证据按 §8 记录代码版本、夹具、测试入口、期望/实际结果和命令；可编译接口不能代替所有权或绑定行为验证。
- 向 S3 交接可复用模型、校验/解析接口、夹具和已明确的后续事项。S3 的调度、提交/取消、回调及配置往返保持未验证状态，不能随 S2 一并标记完成。

## 6. 第三阶段验证

先以内存配置调用模拟工具，验证绑定到执行的完整过程，再加入配置往返。
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
| 同步完成与异步回调的生命周期 | 输入/图像/租约存活到实际退出，完成和清理各被追踪 |
| 已知格式缺工具、缺资源或包含暂不支持循环 | 可保留配置诊断编辑；所选执行范围明确报错，不删内容后执行 |
| 资源写入、清单提交或后续清理失败 | 提交前旧方案有效；提交后清理失败单独报告，不误报未保存 |
| 显式入口无前驱，或执行范围外存在连入边 | 入口接受一次启动许可；不完整执行范围在规划时诊断，不永久等待 |
| 两个保存者在修订检查和提交间交错 | 条件提交/互斥协议保留已提交版本，冲突方不无条件覆盖 |
| 两个请求先后注入不同图像，第二个被 Busy 拒绝 | 第一个运行输入不变，下一次无输入不会拾取被拒绝或旧请求的值 |
| 外部结果对象在下一次执行被覆盖 | 已发布且仍持有的上次结果保持原值；UI 不直接读外部“当前结果” |
| 输出失败但数量为零，或正计数却为空指针 | 逐输出诊断，不能变成合法空集合；非必需输出 Invalid 同样默认整次失败，不提交算法输出 |
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

## 7. 第四阶段及后续验证

第四阶段使用固定图像和真实 OpenCV 工具验证预期结果与数值容差，
包含加载失败、非法参数、共享图像与旋转/继承/修正 ROI、掩膜的代表场景，并保持无 GUI 可执行。
明确面积等结果的定义：像素计数与轮廓几何面积不能无说明地互相比较。
第五阶段验证界面生成的方案能够由相同无 GUI 入口运行，
两种入口使用相同配置与输入时得到一致检测结果。

执行器：
第三阶段已包含基本分支及汇合；增加完整循环、动态索引与并行时，
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
| 循环 | 同一节点多次调用不串值；零次迭代、提前退出及 First/Last/Collect/Reduce 导出符合规则 |
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
- 结论：**S2.1 完成，Debug/Release 构建与首批行为测试通过**；S2.2–S2.7 未开始，S2 整体验收未完成。
- 代码基线：`12f5d1f` 加本次 S2.1 工作区改动，本次未创建 Git 提交；实现与测试随包含本记录的提交一起追溯。
- 实现入口：[核心构建](../source/core/CMakeLists.txt)、[身份类型](../include/visionpro/core/id.hpp)、
  [身份创建](../include/visionpro/core/id_factory.hpp)、[诊断](../include/visionpro/core/diagnostic.hpp)、
  [错误返回](../include/visionpro/core/result.hpp)。
- 测试入口：[core_tests.cpp](../tests/core/core_tests.cpp)、[CTest 注册](../tests/core/CMakeLists.txt)。
  固定输入保存在该测试文件，包括 `00112233-4455-4677-8899-aabbccddeeff`、大小写/边界变体及非法文本；
  每种配置另从本机 Windows 系统随机源生成三类身份各 256 个样本。
- 工具/Schema、方案/快照、Session/Run/Scope/Invocation/Attempt、Frame/Correlation、控制图、绑定、ROI、设备：本子阶段不适用。
- 环境和复现命令见 §2、§3.1；构建不需要 Qt、OpenCV 或厂商运行库。

| 范围 / 测试入口 | 期望与验证内容 | Debug 实际结果 | Release 实际结果 |
|---|---|---|---|
| 工程底座 | 配置、编译实际静态库及测试程序，核心与测试使用一致运行库 | 成功，退出码 0 | 成功，退出码 0 |
| A03 / C02：编译期身份隔离 | 三类身份不可交叉构造、赋值、比较；不可默认/裸字节/字符串构造 | 静态断言通过 | 静态断言通过 |
| A03 / C02：`core.parse_valid` | 大小写归一、固定文本字节顺序、所有合法 variant、有效边界及非 NUL 结尾视图 | 111 检查，0 失败 | 111 检查，0 失败 |
| A03 / C02：`core.parse_invalid` | 长度、全部字符位置、分隔符、NUL/非 ASCII、nil、错误 version/variant，以及诊断优先级 | 1320 检查，0 失败 | 1320 检查，0 失败 |
| A03 / C02：`core.generate` | 768 个样本均为规范 UUIDv4，可往返且本次样本不重复 | 3841 检查，0 失败 | 3841 检查，0 失败 |
| A03 / C02：`core.value_semantics` | 身份比较、排序、复制、移动、独立持有输入值 | 42 检查，0 失败 | 42 检查，0 失败 |
| 最小诊断：`core.diagnostics` | 分类、名称、严重程度、结构化位置及文本按值持有；成功值 0 与错误分开；未知码明确回退 | 30 检查，0 失败 | 30 检查，0 失败 |
| 测试可靠性：`core.failure_exit` | 同一检查路径故意失败，进程必须返回非零；CTest 按 `WILL_FAIL` 判为通过 | 直接运行退出码 1；预期失败测试通过 | 直接运行退出码 1；预期失败测试通过 |

两种配置各 6/6 项 CTest 通过，其中 5 个正常行为组各执行合计 5344 次检查，
另有 1 次故意失败检查。Release 生成配置含 `NDEBUG`，运行检查没有使用 `assert`；
因此不会随 Release 优化配置被禁用。日志位于
`out/build/s21-debug/Testing/Temporary/LastTest.log` 和
`out/build/s21-release/Testing/Temporary/LastTest.log`，属于本机构建产物，不提交。
另以 `BUILD_TESTING=OFF` 完成独立 Release 核心库构建，确认没有生成测试目标；
该附加验证的临时构建目录在收尾时清理，正常 Debug/Release 目录保留以便查看结果和复用。

验证边界：样本无重复不证明全局无碰撞，方案级重复身份检查仍在 S2.5；
系统随机源失败已实现为 `IdGenerationFailed`，本次未通过故障注入执行该系统失败路径。
诊断对象的对应错误分类已测试，不等同于系统故障路径已覆盖。
本次未验证非 Windows 后端、CMake 最低声明版本、节点改名/复制、Schema/运行身份或任何节点执行行为。
这些后续事项不属于 S2.1 的退出条件；A03 仅强类型身份子范围通过，其余范围按原计划推进。

### 8.5 S2.2 实现与验证记录

本节为 S2.2 结束时的历史记录；后续 S2.3 的实现与验证见 §8.6。

- 日期：2026-10-30。
- 结论：**S2.2 完成，Debug/Release 各 14/14 项 CTest 通过**，包含 S2.1 回归及预期失败检查。
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

| 场景子范围 / 测试入口 | 期望与验证内容 | Debug 实际结果 | Release 实际结果 |
|---|---|---|---|
| A03/A04 身份：`core.identities` | UUID/Schema/运行类别不混用；27 类身份编译期隔离，token/关联键边界，计数下界/耗尽，组合键各维度不同 | 1111 检查，0 失败 | 1111 检查，0 失败 |
| A05 值：`core.values` | 配置值/运行值隔离；标量边界、空 List、None、缺失、错误分别表达；不可变持有和复制/移动有效 | 116 检查，0 失败 | 116 检查，0 失败 |
| A05/A32 类型：`core.schema` | Record/Enum 身份、版本、字段/成员及嵌套结构；未知/重复/缺失/错类型拒绝，诊断保留字段或索引 | 161 检查，0 失败 | 161 检查，0 失败 |
| A32 编码：`core.utf8` | UTF-8 标量边界、中文/emoji/NUL 与有界视图；过长编码、代理项、超范围、孤立延续、截断拒绝 | 270 检查，0 失败 | 270 检查，0 失败 |
| A06 元数据：`core.metadata` | 输入/参数/数据输出/控制出口分开；重复声明、未知枚举、订阅/结构参数与必填/默认规则可校验 | 24 检查，0 失败 | 24 检查，0 失败 |
| A06 约束：`core.constraints` | 数值范围、枚举集合、容量及非法约束声明；None/缺值/默认/来源错误的行为不同 | 40 检查，0 失败 | 40 检查，0 失败 |
| A06 转换：`core.conversions` | 精确匹配、Optional 包装、显式数值转换许可；单来源/集合/多来源区别 | 19 检查，0 失败 | 19 检查，0 失败 |
| A32 兼容：`core.compatibility` | Schema 版本区间、所需能力、完整性、同身份版本的嵌套定义一致性；失败不删除字段 | 20 检查，0 失败 | 20 检查，0 失败 |
| S2.1 回归与测试可靠性 | 原 5 个行为组与 `core.failure_exit`；正常检查不依赖 assert，故意失败进程退出码 1 | 6/6 通过，正常 5344 检查；故意失败退出 1 | 6/6 通过，正常 5344 检查；故意失败退出 1 |

每种配置的新增 8 组共 1761 次检查；加上 S2.1 回归，每种配置共 7105 次正常检查，
另有 1 次预期失败检查。结果可从 §3.2 的 `LastTest.log` 复核，Debug/Release 构建目录保留供后续使用。
初次编译的类型隔离断言发现 `FrameId` 聚合允许仅给来源并默认序号，已改为必须显式提供两个参数；
原断言未放宽，最终两种配置均通过。嵌套类型相等比较使用明确的递归调用，最终构建不再报告相应 MSVC 比较警告。

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
- 结论：**S2.3 完成，Debug/Release 各 24/24 项 CTest 通过**，包含 S2.1–S2.2 回归与预期失败检查。
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
  最终 Debug/Release 构建均返回 0，构建日志无编译警告。

| 场景子范围 / 测试入口 | 验证内容 | Debug 实际结果 | Release 实际结果 |
|---|---|---|---|
| A09：`core.image_layout` | 深度/通道、行步长、末行长度、短缓冲、零尺寸、乘加溢出及未知格式 | 136 检查，0 失败 | 136 检查，0 失败 |
| A09：`core.image_ownership` | 外部复制、只读共享、裁剪保活、根坐标累计、独立导出与克隆 | 53 检查，0 失败 | 53 检查，0 失败 |
| A10：`core.geometry` | 坐标轴/手性/单位、点线圆位姿、相似/仿射/投影、显式求逆、源有效域、数值边界和标定后端拒绝 | 70 检查，0 失败 | 70 检查，0 失败 |
| A10：`core.regions` | 多形状、自交/退化、继承/修正/排除、边界/空域、坐标目录和工具能力 | 81 检查，0 失败 | 81 检查，0 失败 |
| A26：`core.masks` | 配置引用/运行像素分离、单通道/阈值、尺寸/坐标匹配、显式映射/采样、所有权 | 40 检查，0 失败 | 40 检查，0 失败 |
| 资源模型：`core.resources` | 托管/外部/序列/设备/输出定义、路径基准/逃逸、目录身份/类别/版本引用 | 51 检查，0 失败 | 51 检查，0 失败 |
| A08：`core.shared_definitions` | 全局定义/当前值、完整队列消息、容量/收发策略、身份和精确类型 | 20 检查，0 失败 | 20 检查，0 失败 |
| A01：`core.state_models` | 三类状态域、节点/会话/运行隔离、代次溢出、初始化值/错误一致性和策略声明 | 23 检查，0 失败 | 23 检查，0 失败 |
| A05/A09/A26：`core.domain_values` | 两值域编译期隔离、嵌套 Image/Mask 保活、类型诊断、ROI/容器/Schema 保留可写别名回归 | 167 检查，0 失败 | 167 检查，0 失败 |
| A06/A32：`core.domain_metadata` | 工具状态/资源/图像/区域能力声明、重复/非法声明、状态嵌套 Schema 冲突 | 53 检查，0 失败 | 53 检查，0 失败 |
| S2.1–S2.2 回归与测试可靠性 | 原有行为与 `core.failure_exit`；故意失败入口直接调用也返回 1 | 14/14 通过，正常 7105 检查；故意失败退出 1 | 14/14 通过，正常 7105 检查；故意失败退出 1 |

每种配置新增 694 次检查，总计 **7799 次正常检查，0 失败**，另有 1 次预期失败检查。
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
- 结论：**S2.4 完成，Debug/Release 各 34/34 项 CTest 通过**；新增 workflow 模型、快照、复制和请求/结果测试。
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
- 环境沿用 §2，实际命令见 §3.4；Debug/Release 配置与构建均返回 0，构建日志无编译警告。
  未加载实际资源文件、连接设备、执行图像算法或调用厂商 SDK。

| 场景子范围 / 测试入口 | 验证内容 | Debug 实际结果 | Release 实际结果 |
|---|---|---|---|
| A01/A13/A24：`workflow.model_graph` | 同类型配置独立、五路分叉/多前驱汇合、嵌套 Group、直接/递归枚举、公开数据与控制映射、改名/布局不改引用 | 21 检查，0 失败 | 21 检查，0 失败 |
| A06：`workflow.model_bindings` | 输入/参数目标、五类来源、系统字段/固定选择器、源基数、转换及缺失策略的形状检查 | 40 检查，0 失败 | 40 检查，0 失败 |
| A03/A24：`workflow.model_structure` | 方案内身份唯一性、包含缺父/多父/环、入口层级、公开导出目标、布局和非法局部定义 | 89 检查，0 失败 | 89 检查，0 失败 |
| A01/A09：`workflow.snapshots` | 配置与工具元数据深复制、保留可写别名隔离、源释放保活、只读 API、快照身份和缺工具草稿边界 | 25 检查，0 失败 | 25 检查，0 失败 |
| A03/A24：`workflow.copy_subgraph` | Group 后代闭包、内部引用/包含/数据及控制导出重映射、外部来源报告、跨选区控制线略过 | 24 检查，0 失败 | 24 检查，0 失败 |
| A03：`workflow.copy_solution` | 新方案及各持久身份、两个 Workflow、资源/变量/队列/布局、复合值内 ROI/Mask/标定引用和公开默认值重映射 | 25 检查，0 失败 | 25 检查，0 失败 |
| A03：`workflow.copy_boundaries` | 明确 Preserve/Reject、外部父及外部或错误归属的引用、非法选择/结构、失败不修改输入 | 15 检查，0 失败 | 15 检查，0 失败 |
| A23/A24：`workflow.run_inputs` | Workflow/Node/Group 目标、禁用祖先、精确工具版本、必需 Optional、未知/重复/错类型、范围与默认值物化 | 56 检查，0 失败 | 56 检查，0 失败 |
| A01/A09/A23：`workflow.run_isolation` | 多请求输入隔离、无旧输入回退、保留别名隔离、快照固定和图像缓冲保活 | 15 检查，0 失败 | 15 检查，0 失败 |
| A04：`workflow.result_models` | 运行/快照/调用/尝试身份、输出地址、NG 成功、三类输出可用性、失败输出拒绝、终态记录隔离 | 67 检查，0 失败 | 67 检查，0 失败 |
| S2.1–S2.3 回归与测试可靠性 | 原 24 项；故意失败入口也直接调用，验证退出码传递 | 24/24 通过，正常 7799 检查；故意失败退出 1 | 24/24 通过，正常 7799 检查；故意失败退出 1 |

每种配置新增 **377 次检查**，总计 **8176 次正常检查，0 失败**，另有 1 次预期失败检查；
编译期断言验证只读接口及配置/运行类型边界，不计入上述运行检查数。
Debug/Release 日志及构建目录保留供复核；完整输出路径见 §3.4。
实现审查补齐了 Group/Workflow 的控制出口导出及复制/快照回归，测试标准未放宽；两种配置均一次构建和全量测试通过。

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
- 结论：**S2.5 完成，Debug/Release 各 39/39 项 CTest 通过**；新增 5 项静态校验测试，保留全部 34 项前阶段回归。
  S2.6–S2.7 尚未实现，整个 S2 尚未通过验收。
- 公共入口：[validation.hpp](../include/visionpro/workflow/validation.hpp) 的 `validate_solution` 与 `system_field_type`；
  实现位于 `source/workflow/validation.cpp`，内部领域值递归检查位于 `validation_values.cpp`。
- 测试：[validation_tests.cpp](../tests/workflow/validation_tests.cpp) 覆盖图、绑定、Group 与 Schema，
  [validation_domain_tests.cpp](../tests/workflow/validation_domain_tests.cpp) 覆盖 ROI 继承、嵌套资源与坐标/能力检查。
- 环境与 §2 相同：Windows x64、MSVC 19.51.36248.0、Windows SDK 10.0.26100.0、CMake/CTest 4.4.0。
  §3.5 两种配置的配置、最终构建和全量测试均返回 0，最终构建日志无编译警告。
  本地日志保留在 `out/build/s25-debug/`、`out/build/s25-release/` 下的 `build.log` 与 `Testing/Temporary/LastTest.log`，均被 Git 忽略。
- 本次在已有 S2.1–S2.4 未提交工作区基础上补充实现；没有覆盖前阶段成果或创建 Git 提交。
- 验证边界：静态类型、引用、控制可达性与依赖索引；没有结果解析、实际调度、资源加载、设备访问或算法运行。
  S2.6 负责运行结果/公开输入取值、实际 List 长度和可用性，S2.7 负责接口收敛，S3 负责运行与提交。

| 场景子范围 / 测试入口 | 实际验证内容 | Debug | Release |
|---|---|---|---|
| A03/A13：`workflow.validation_graph` | 五路分叉/汇合、非相邻/兄弟依赖、普通回边、自环、直接/间接逆序、纯数据/混合环、显式入口及范围外前驱/来源、诊断与索引 | 56 检查，0 失败 | 56 检查，0 失败 |
| A06/A07：`workflow.validation_bindings` | 字段/固定索引、目标/来源、基数、转换、Optional/缺失、默认值、静态长度约束、单位、公开输入/全局/资源及系统字段 | 112 检查，0 失败 | 112 检查，0 失败 |
| A24：`workflow.validation_groups` | 两层嵌套 Group、逐层数据/控制映射、映射完整性、作用域穿透与来源可达性 | 43 检查，0 失败 | 43 检查，0 失败 |
| A03/A32：`workflow.validation_schema` | 工具注册/版本、完整 Schema、显式能力支持、跨工具/公开接口/队列的类型冲突、同版本端口结构与改名 | 41 检查，0 失败 | 41 检查，0 失败 |
| A10/A26：`workflow.validation_domains` | ROI 来源/排除项的隐藏依赖、权限/禁用/结构参数、嵌套值路径、坐标/能力、掩膜/标定资源、资源槽、状态初值/设备配置 | 110 检查，0 失败 | 110 检查，0 失败 |
| 既有 34 项 CTest | S2.1–S2.4 回归，含 `core.failure_exit` 预期失败入口 | 全部通过 | 全部通过 |

每种配置新增 **362 次正常检查**，总计 **8538 次正常检查，0 失败**，另有 1 次预期失败检查。
测试使用固定 UUID/token、内存 Schema/图/ROI/资源目录，不读取真实资源、运行算法或连接设备。
最终 `git diff --check` 无空白错误；本次构建目录保留作验证依据，没有其他临时文件。

实现与验证中修复了默认几何值坐标检查、隐藏 ROI 的来源权限/Disabled 处理、队列 Schema 一致性、
状态/设备配置递归引用和确定超长的多来源集合漏检，并加入成对回归。
首轮并行 MSBuild 停在工程检查步骤后终止，改用 `--parallel 1` 完成构建；
编译中发现的局部标识重定义及测试坐标 ID 类别错误也已修复，未删除测试或放宽断言。

### 8.9 S2.6 实现与验证记录

- 日期：2026-11-09。
- 结论：**S2.6 完成，Debug/Release 各 44/44 项 CTest 通过**。
  新增 5 项结果访问/解析测试，全部前阶段回归通过；S2.7 尚未开始，整个 S2 尚未通过验收。
- 公共入口：[result_store.hpp](../include/visionpro/workflow/result_store.hpp) 的 `ResultStore`，
  [resolver.hpp](../include/visionpro/workflow/resolver.hpp) 的 `BindingResolver`、`ResolutionContext` 和四态解析结果。
  运行值物化/约束与领域检查由 workflow 内部 `runtime_values` 复用，RunRequest 保持原输入规则。
- 测试：[result_store_tests.cpp](../tests/workflow/result_store_tests.cpp) 与
  [resolver_tests.cpp](../tests/workflow/resolver_tests.cpp)；新增 5 项 CTest，合计 44 项。
- 环境与 §2 相同：Windows x64、MSVC 19.51.36248.0、Windows SDK 10.0.26100.0、CMake/CTest 4.4.0。
  §3.6 两种配置的配置、最终构建和全量测试均返回 0，最终构建日志没有编译警告。
  日志保留在 `out/build/s26-debug/`、`out/build/s26-release/` 的 `build.log` 与 `Testing/Temporary/LastTest.log`，均被 Git 忽略。
- 本次在已有 S2.1–S2.5 未提交工作区上继续实现，没有覆盖前阶段成果或创建 Git 提交。

| 场景子范围 / 测试入口 | 验证内容 | Debug | Release |
|---|---|---|---|
| A04/A09：`workflow.result_store` | 完整输出 Schema、精确会话/运行/快照/调用身份、重复调用拒绝、退役运行与持有值保活 | 81 检查，0 失败 | 81 检查，0 失败 |
| A05/A06：`workflow.resolver_values` | 常量/默认值、参数、本轮字段/固定索引、显式转换、多来源顺序和运行约束 | 35 检查，0 失败 | 35 检查，0 失败 |
| A04/A07：`workflow.resolver_availability` | Pending、正常缺失、失败/取消/超时、系统字段、Optional 不吞错、原始原因及四态优先级 | 52 检查，0 失败 | 52 检查，0 失败 |
| A04：`workflow.resolver_isolation` | 会话/运行/作用域/调用隔离、显式同宿主外层可见性、禁止兄弟域读取、退役与上下文检查 | 35 检查，0 失败 | 35 检查，0 失败 |
| A08/A09/A10/A23/A24/A26：`workflow.resolver_domains` | 公开输入及 Group 作用域隔离、资源/全局快照、图像保活与格式、嵌套 ROI 完整冻结、坐标与组合能力 | 48 检查，0 失败 | 48 检查，0 失败 |
| 既有 39 项 CTest | S2.1–S2.5 回归，含 `core.failure_exit` 预期失败入口 | 全部通过 | 全部通过 |

每种配置新增 **251 次正常检查**，总计 **8789 次正常检查，0 失败**，另有 1 次预期失败检查。
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
  所有权/错误边界与组合验证均在 Debug/Release 通过；14 项必需 S2 子范围证据见下表。
- 范围：收敛 RunHandle、实例工厂、InvocationOperation 及其与快照/固定输入的边界；
  使用可控测试替身验证契约，不建设调度器、真实设备适配或完整共享服务。
- 版本和环境：基准 HEAD `12f5d1f` 上的当前未提交工作区，已有 S2.1–S2.6 修改保留；工具链沿用 §2。
  C++20、Windows x64、MSVC 19.51.36248.0、CMake/CTest 4.4.0；Debug `/MDd`、Release `/MD`。
  新接口为 core/workflow 各自的 `tool_execution.hpp` 和 workflow `run_handle.hpp`；
  测试为 core `tool_execution_tests.cpp`、workflow `run_handle_tests.cpp`、`tool_execution_tests.cpp`、`composition_tests.cpp`。
  日志位于 `out/build/s27-debug`、`out/build/s27-release` 的 `build.log` 和 `Testing/Temporary/LastTest.log`。
- 新增 CTest：`core.tool_execution`、`workflow.run_handles`、`workflow.tool_execution`、`workflow.composition`；
  共 48 项，保留 S2.6 的全部 44 项。

| 本轮验证 | Debug | Release |
|---|---|---|
| §3.7 配置与全量重建退出码、编译警告 | 0；0 警告 | 0；0 警告 |
| 既有 44 项 CTest 回归 | 44/44 通过 | 44/44 通过 |
| `core.tool_execution` 实例与操作契约 | 91 检查，0 失败 | 91 检查，0 失败 |
| `workflow.run_handles` 已受理句柄契约 | 60 检查，0 失败 | 60 检查，0 失败 |
| `workflow.tool_execution` 固定调用与结果投影 | 38 检查，0 失败 | 38 检查，0 失败 |
| `workflow.composition` 固定内存组合验收 | 27 检查，0 失败 | 27 检查，0 失败 |
| 全量 CTest、正常检查及预期失败入口 | 48/48；9,005 正常检查，0 失败；1 项预期失败符合预期 | 48/48；9,005 正常检查，0 失败；1 项预期失败符合预期 |

本轮每配置新增 216 个正常检查。两次 CTest 均退出 0；编译使用 `/W4`，不依赖 `assert` 维持 Release 检查。
`git diff --check` 无空白错误；core 没有反向包含 workflow，两个库与测试均不依赖 Qt/OpenCV 或窗口初始化。
审查中补齐部分启动异常仍返回可等待句柄、清理后发布物理退出、工厂重复发行拒绝、逐处 ROI 冻结覆盖、
无目标工具的上下文检查及资源目录版本固定，均有对应回归。

#### 8.10.1 十四项 S2 子范围证据矩阵

下列 CTest 名称已与 `tests/core/CMakeLists.txt`、`tests/workflow/CMakeLists.txt` 核对。
“已有证据”引用 §8.4–§8.9 的 Debug/Release 实测结果；本轮全量回归已再次通过上述全部入口。
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
[composition_tests.cpp](../tests/workflow/composition_tests.cpp)，Debug/Release 各 27 个检查通过。
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
公共契约与代码已核对，全部必需项和两种配置验证通过，**S2 按既定模型与绑定范围通过整体验收**。
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
| 可复现无 GUI 运行入口 | core/workflow 库、固定内存夹具及 §3 构建步骤 | 无窗口初始化即可装配模拟工具并运行；输出追溯快照、运行、实际来源、顺序和诊断；Debug/Release 重复验证 |

S4 再接真实视觉算法与几何像素计算；完整循环、多流程协调、连续/外部触发、真实共享读写/队列服务、
设备及 SDK 按原阶段推进。S3 不以线程安全替代工件配对，也不提前建设动态插件或通用重试框架。
