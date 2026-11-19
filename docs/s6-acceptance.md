# S6.12 软件综合验收与设备交接

2026-11-19，S6.12 软件综合验收通过。Windows x64 Release 全量回归在 Qt、OpenCV、MVS 开启配置中通过 175/175 项，关闭三者的无界面配置通过 131/131 项。Qt 使用 offscreen，网络用例连接本机临时端口。两套负载测量单独运行。现场相机、PLC 及两者联动均标为待验证。

## GUI 与独立 CLI 对照

固定输入均使用版本 1 方案。Qt Widgets 通过 offscreen 平台打开方案、编辑实际业务参数、保存并重载。独立 CLI 从磁盘加载同一份修订。每次比较都核对调用次数、命名出口、诊断、输出以及本轮实际的绑定来源。进程分别生成会话和消息身份，在各自报告内验证其引用关系再作对照。[桌面验收用例](../tests/ui/acceptance_parity_tests.cpp)

| 方案及固定输入 | GUI 编辑 | 两个入口的预期与核对项 |
|---|---|---|
| [Group 内的 Blob 循环](../samples/s6-group/solution.json)，双区域 PNG | 将 Group 内部的阈值从公开输入的默认 128 改为常量 255 | 原方案执行 10 次节点调用，两个迭代分别导出质心 (2, 1.5)、(8, 6) px，面积合计 15。修改后执行 8 次，零次迭代，点集为空、面积为 0。两种情况下 Group 均执行一次并从 `measured` 出口返回。逐项对照完整摘要，保留 Group、迭代、作用域、调用和尝试间的引用。 |
| [两个工位的宽高汇合](../samples/s6-cross-workflow/solution.json)，[五步输入](../samples/s6-cross-workflow/runs.json) | 宽度校正量从 0 改为 +1 | 输入顺序为 A 宽 10、B 高 2、B 宽 20、A 高 1、再交付 A 高 1。每种修订的五轮调用数均为 2、2、4、4、2，出口依次为 waiting、waiting、ready、ready、ignored。原方案依次收到 B=(20,2)、A=(10,1)，修改后收到 B=(21,2)、A=(11,1)。两次入队、两次实际读取，重复输入没有第三条消息。[固定预期](../samples/s6-cross-workflow/expected.json) |
| [五路测量接本机 TCP](../samples/s6-acceptance/prepare_inspection.py)，固定工件与模板 BMP | 将第五路 B 均值的判定上限从 40 改为 50 | 实测平移 (+3,+2) px，B 均值依次为 10、20、30、40、50。工件 101 走 NG，修改后工件 102 走 OK。每轮执行 20 次节点调用，队列保留工件键与五个实测值。独立 TCP 对端按连接核对实际收到的单帧 UTF-8 载荷，分别为“工件=101 NG”和“工件=102 OK”。 |

前两套方案对应 A03、A04、A08、A15、A18、A24、A30，五路检测与发送对应 A09、A11、A17、A21、A31。分配置回归同时覆盖 S5 三条原始流程在原样、业务修改和布局重载后的九组 GUI/CLI 对照，以及窗口关闭、运行取消和历史图像寿命。[S5 验收基线](s5-acceptance.md)

摘要中的 `groups`、`iterations` 与导出来源记录子调用关系，`nodes[].attempts` 保留尝试，队列输出保留工件键、消息 ID 和 FIFO 序号。模拟相机的 `trigger` 报告另可用每轮输入图像的 `frame.source`、`frame.sequence` 核对触发事件。模拟 PLC 写后丢应答时，诊断中的 `operation_key` 与原始 Winsock 状态定位这次写入，读回流程核对寄存器的实际值。[运行摘要](../source/app/run_summary.cpp) · [设备与通信验证记录](development.md#1010-s610-相机采集与触发)

## 桌面负载复测

沿用 S5 的 100 节点、300 条控制线、1920×1080 Mono8 PNG、1440×900 offscreen 窗口。18 轮逐轮核对全部控制线以及面积 230,400 像素的单个 Blob。计时和内存使用同一套 S5 夹具和门限，采样期间未并行构建或跑回归。[夹具](../tests/ui/acceptance_load.cpp) · [原始采样](acceptance/s6-12/desktop-load.json)

| 项目 | 样本数 | 本次 p95（ms） | S5 门限（ms） |
|---|---:|---:|---:|
| 画布拖动到重绘 | 20 | 104.19 | 180 |
| 打开方案到可用 | 5 | 50.97 | 120 |
| 保存到修订提交 | 5 | 78.58 | 160 |
| 启动到退出且界面可再运行 | 18 | 157.12 | 250 |
| 阈值图显示到重绘 | 18 | 28.06 | 50 |
| Blob 图显示到重绘 | 18 | 16.37 | 40 |

历史上限 16 轮，观察容量 32。第 18 轮保留 48 个不同图像缓冲，共 99,532,800 字节。清空历史后仅显示面板持有 2,073,600 字节，清空显示后归零。各采样点的 Private Bytes 相对首轮前最大增量为 116.84 MiB，原门限 160 MiB。观察通道累计报告丢弃 1,258 条通知，每轮结果仍可查询。

## 并发、溢出与释放

无界面 Release 进程使用生产 `WorkflowEngine`、实际队列节点与结果历史。20 批各提交 7 个带唯一工件键的生产请求。每批用门闩等到两个调用同时进入节点，再放行发送。每条载荷为 64 KiB，每批结束由另一个流程按 FIFO 一次读回五条。逐条核对工件键、消息 ID、来源轮次、序号和完整载荷。[夹具](../tests/workflow/acceptance_concurrent_load.cpp) · [原始采样](acceptance/s6-12/concurrent-load.json)

| 检查项 | 实测 |
|---|---|
| 流程准入 | 在途上限 2、等待上限 4，实测峰值分别为 2、4。140 次生产提交中受理 120 次，其中 80 次排队，另有 20 次明确返回 Busy。 |
| 业务队列 | 容量和峰值均为 5，文本载荷峰值 327,680 字节。120 次生产运行中，100 次成功发送，20 次得到 QueueFull。20 次消费运行共读回 100 条，丢弃旧消息 0 条，最后剩余 0 条。 |
| 结果与通知 | 140 个已完成运行中，历史最多可按原身份查到 4 轮，关闭后为 0。慢观察者容量 3，保留 3 条、显式报告溢出 537 条，读取后剩余 0 条。释放引擎后，队列服务弱引用失效。 |
| Windows 进程内存 | 进程峰值工作集 8,978,432 字节，峰值提交量 3,022,848 字节。Private Bytes 在基线、关闭前、释放后分别为 1,077,248、2,510,848、1,822,720 字节。 |

| 计时（ms） | 样本数 | p50 | p95 | 最大值 |
|---|---:|---:|---:|---:|
| 已排队请求从提交到进入节点 | 80 | 1.4703 | 1.8409 | 2.0667 |
| 进入节点到观察到实际退出，含门闩等待 | 120 | 0.3533 | 2.2096 | 2.5017 |
| 从进入且已放行到观察到实际退出 | 120 | 0.3343 | 0.5122 | 0.6322 |

分位数按排序后的第 `ceil(p × N)` 项计算。并发计时用于复核本机固定输入下的资源争用，包含工作线程调度及退出通知交付时间。

## 复现

本次使用 Windows x64、MSVC 19.51、Qt 6.11.1、OpenCV 4.14.0-pre、CTest 4.4.0、Python 3.13.5。下列命令从仓库根目录在 MSVC x64 开发环境执行。两个 Release 构建目录沿用 [S6.10 的配置](development.md#1010-s610-相机采集与触发)。运行桌面负载前，将 Qt 和 OpenCV 的 Release DLL 目录加入 PATH，并设置 Qt 平台插件目录，见 [S5 负载命令](s5-acceptance.md#负载测量)。

```powershell
$guiBuild = 'out/build/s67-gui'
$headlessBuild = 'out/build/s67-headless'
cmake --build $guiBuild --config Release
cmake --build $headlessBuild --config Release
ctest --test-dir $guiBuild -C Release --output-on-failure --no-tests=error
ctest --test-dir $headlessBuild -C Release --output-on-failure --no-tests=error
& "$guiBuild/tests/ui/visionpro_acceptance_load.exe" -platform offscreen --output "$guiBuild/s612-desktop-load.json"
& "$headlessBuild/tests/workflow/visionpro_acceptance_concurrent_load.exe" > "$headlessBuild/s612-concurrent-load.json"
```

三套双入口对照分别运行 `visionpro_group_loop_handoff_tests`、`visionpro_cross_workflow_handoff_tests`、`visionpro_inspection_tcp_handoff_tests`。测试会在临时目录保存与重开方案，TCP 对端仅监听本机，自动选择可用端口。并发负载在回归退出后独立执行。按工件顺序重放的命令如下。

```powershell
& "$headlessBuild/source/app/visionpro_cli.exe" replay --solution samples/s6-cross-workflow/solution.json --sequence samples/s6-cross-workflow/runs.json
```

`replay` 在一个会话中按文件顺序运行多个流程，每步等待实际退出。序列为非空 JSON 数组，每项含 `workflow` 和可选的 `inputs`，输入值使用与 `run --input` 相同的文本表示。提交前解析全部输入。执行失败时保留已生成的运行摘要并停止后续步骤。`app.cli_replay` 覆盖成功、输入错误、重复字段和执行中止。

手工复现五路发送时，在一个终端运行 `python -B tests/app/communication_peer.py --protocol tcp --port 16111 --record-tcp`。另一个终端执行以下命令，生成目录需为新路径。可在桌面打开该目录，将第五路判定上限改为 50、保存，再把命令中的工件号改为 102。每次连接关闭后，第一个终端输出按帧记录的十六进制载荷。

```powershell
python -B samples/s6-acceptance/prepare_inspection.py out/s612-inspection --port 16111
& "$guiBuild/source/app/visionpro_cli.exe" run --solution out/s612-inspection/solution.json --workflow 5ca4f87a-ea1e-4d62-89b0-000000006910 --input part=101
```

## 待实机验证

| 待验证项 | 已有软件证据 | 现场需补的记录 |
|---|---|---|
| 工业相机 | 模拟设备覆盖帧复用、帧号、软硬触发、取消和断连恢复。本次 CLI 枚举返回 MVS SDK 4.7.1.4、实机 0 台、模拟相机 SIM-001。 | 型号、固件、像素格式，单帧、连续、软触发和 Line0 硬触发的帧号、漏帧、停止和拔插恢复。 |
| 实物 PLC | 独立 Modbus TCP 模拟器覆盖 FC03/FC06、写后丢应答、读回及写入次数。 | 现场型号、固件、单元号、寄存器映射、写入 0x1234 后的设备端值、应答与可控断连后的写入次数。 |
| 整线追溯 | 文件视觉、模拟相机和本机 TCP 可分别复现。 | 用同一工件号关联物理帧、五路判定、汇合与设备应答，核对迟到、漏触发和断连。 |
