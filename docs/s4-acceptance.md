# 第四阶段验收与交接

2026-11-15 完成 S4.7 本轮验收。Windows x64 Release 沿用 OpenCV `4.14.0-pre`，适用回归 ON **97/97**、OFF **81/81**。固定方案在独立 CLI 进程中经保存从修订 1 升至 2，三条流程的结果、控制边和来源均通过对照。

## 固定方案与输入

方案格式、视觉工具配置及 Blob Record Schema 均为版本 1。PNG 为 12×10 UInt8 Mono，共 86 字节。BMP 为 9×9 UInt8 BGR，共 306 字节。两者均为方案目录相对的 `PerInvocation` 外部图像，样例以 Gray8 读取。输入清单记录资源 UUID、精确像素规则、工作流 UUID、关键节点及固定期望。[样例方案](../samples/s4/solution.json) [输入清单](../samples/s4/inputs.json)

| 工作流 | UUID | 两轮实测 |
|---|---|---|
| 双区域单链 | `5ca4f87a-ea1e-4d62-89b0-000000000101` | 阈值 128 后得到面积 6、9，总面积 15，判定 OK |
| 五路阈值汇合 | `5ca4f87a-ea1e-4d62-89b0-000000000102` | 阈值 100、128、180、210、250 对应计数 `[2,2,2,0,0]`，判定 `[OK,OK,OK,NG,NG]`，汇合及交付各一次 |
| 旋转 ROI 继承 | `5ca4f87a-ea1e-4d62-89b0-000000000103` | 旋转掩膜含 7 点，平移并排除后 Blob 面积为 6，质心 `(5,4)`，判定 OK |

## 样例复现

完成 ON 的 Release 构建后，从仓库根目录运行下列 PowerShell 命令。三条流程均使用样例中的固定参数。保存操作使用构建目录中的样例副本。[Release 构建命令](development.md#323-s47-release-构建与回归)

```powershell
$build = (Resolve-Path -LiteralPath 'out/build/s47-on').Path
$cli = Join-Path $build 'source/app/Release/visionpro_cli.exe'
$source = (Resolve-Path -LiteralPath 'samples/s4').Path
$openCvBin = (Resolve-Path -LiteralPath (Join-Path $env:OpenCV_DIR '../bin')).Path
$runRoot = Join-Path $build ('sample-' + [Guid]::NewGuid().ToString('N'))
$before = Join-Path $runRoot 'before'
$after = Join-Path $runRoot 'after'
$null = New-Item -ItemType Directory -Path $runRoot,$before,$after
$package = Join-Path $runRoot 'package'
Copy-Item -LiteralPath $source -Destination $package -Recurse
$catalog = Get-Content -Raw -Encoding UTF8 (Join-Path $package 'inputs.json') | ConvertFrom-Json
$manifest = '..\package\solution.json'
$oldPath = $env:Path
$env:Path = "$openCvBin;$env:Path"
try {
    Push-Location -LiteralPath $before
    try {
        foreach ($flow in $catalog.workflows) {
            $report = & $cli run --solution $manifest --workflow $flow.id
            if ($LASTEXITCODE -ne 0) { throw "Initial run failed for $($flow.id)" }
            $report | Set-Content -LiteralPath "$($flow.case).json" -Encoding UTF8
        }
        $saved = & $cli save --solution $manifest
        if ($LASTEXITCODE -ne 0) { throw 'Save failed.' }
        $saved | Set-Content -LiteralPath 'save.json' -Encoding UTF8
    } finally {
        Pop-Location
    }

    Push-Location -LiteralPath $after
    try {
        foreach ($flow in $catalog.workflows) {
            $report = & $cli run --solution $manifest --workflow $flow.id
            if ($LASTEXITCODE -ne 0) { throw "Reload failed for $($flow.id)" }
            $report | Set-Content -LiteralPath "$($flow.case).json" -Encoding UTF8
        }
    } finally {
        Pop-Location
    }
} finally {
    $env:Path = $oldPath
}
$runRoot
```

`before/save.json` 应报告 `Committed` 与修订 `2`。`before/` 和 `after/` 的 `single.json`、`five.json`、`inherited_roi.json` 可按输入清单对照各端口。跨进程比较忽略修订与运行期分配的身份，先验证每轮的来源确实指向本轮生产节点、端口、作用域和调用。

## Release 回归范围

| 配置 | 已注册 | 本轮通过 | 本地证据 |
|---|---:|---:|---|
| OpenCV ON | 104 | 97/97 | `out/build/s47-on/build.log`、`ctest.log`、`cli-imports.log` |
| OpenCV OFF | 88 | 81/81 | `out/build/s47-off/build.log`、`ctest.log`、`pe-imports.log` |

ON 选用已安装的 OpenCV `core`、`imgproc`、`imgcodecs`，实际 CLI 导入 `opencv_world4140.dll`。OFF 配置未解析 `OpenCV_DIR`，39 个 Release 可执行文件均无 OpenCV 或 Qt DLL 导入。三条固定流程的两轮输出及数值对照记录位于 `out/build/s47-on/sample-*/`。

两套配置均执行五组无文件摘要校验的基础设施回归，覆盖清单格式往返、外部路径与租约、清单诊断、保存失败回滚和保存冲突。另执行 `app.cli_external_roundtrip` 与 `app.cli_external_diagnostics`，验证外部资源的模拟五路、条件分支和故障退出。

本轮排除以下 7 项执行实际 SHA-256 的用例。

- `app.cli_roundtrip`、`app.cli_diagnostics` 使用带摘要的托管文件。
- `infrastructure.file_resource_execution`、`infrastructure.solution_diagnostic_loading`、`infrastructure.solution_save_failures`、`infrastructure.solution_save_conflict`、`infrastructure.solution_save_cleanup` 读取、写入或清理有摘要的资源。

`infrastructure.solution_roundtrip` 对摘要字段只做配置值往返，夹具缺少对应的托管文件内容。固定样例按有效像素、配置、结果和来源验收，全程未执行 SHA-256 校验。

## 场景证据

下表的测试均在本轮 ON Release 中通过。固定输入和误差要求见 [验收夹具](development.md#75-固定夹具与可复现验收数值)。

| 场景 | 实测 |
|---|---|
| V01 阈值边界 | `tools.threshold` 在阈值 128 得正相 `[0,0,0,255,255]`、反相 `[255,255,255,0,0]`。通过阈值 0、255 的等值边界及 ROI 外清零。 |
| V02 Blob 主样例 | `app.vision_image_summary` 和固定单链测得面积 6、9，总面积 15，质心 `(2,1.5)`、`(8,6)`，包围框与输入清单一致。 |
| V03 连通性 | `tools.blob` 的两个对角像素在 4 连通下分别为面积 1 的组件，在 8 连通下为面积 2、质心 `(0.5,0.5)` 的组件。 |
| V04 面积与空结果 | `tools.blob` 的单像素和有孔图形面积分别为 1、8。全零图成功，组件列表为空，最大面积为 None。 |
| V05 判断与绑定 | `app.vision_image_summary` 对计数 2 判出 OK 和 NG，NG 后继读取 `components[0].area_pixels` 得 6。空列表越界报告 `IndexOutOfRange`。 |
| V06 步长与裁剪 | `tools.threshold` 与 `tools.blob` 得到二值图 `[0,0,255;255,255,255]`、面积 4、当前帧质心 `(1.25,0.75)`，换算到根帧为 `(2.25,1.75)`。 |
| V07 旋转 ROI | `tools.roi` 逐点比对得到 7 个成员，固定样例的旋转掩膜计数同为 7。 |
| V08 继承与修正 | `tools.roi`、`tools.blob` 及独立 CLI 样例验证整幅二值图和继承 ROI 分别取源，平移并排除后面积为 6，增加来源排除后为 5。方向修正得到 `(0,1)`。 |
| V09 边界与空域 | `tools.roi` 和 `tools.blob` 验证 Reject 报 `ImageBounds`、Intersect 得 4 点、空域 Error 报 `EmptyRegion`，ValidEmptyOutput 产生空结果。 |
| V10 掩膜规则 | `tools.mask` 得到 NonZero 的 4 个成员和 AtLeastThreshold(128) 的 2 个成员，保留固定租约及其来源。 |
| V11 显式映射 | `tools.mask` 的 Nearest 平移结果是 `(1,0)`、`(2,1)`，覆盖半像素和源外取样及无有效映射的错配诊断。 |
| V12 五路 | `workflow.vision_integration` 与固定五路均验证计数 `[2,2,2,0,0]`、两路 NG 继续汇合，交付一次。集成测试逐像素核对原图和分支结果。 |
| V13 资源与往返 | `app.vision_roundtrip` 和固定样例使用无摘要外部 PNG/BMP，覆盖中文及空格路径、异目录运行、独立进程保存和重载。配置、有效像素、资源引用、结果及来源稳定，运行缓冲未入清单。带摘要的托管路径属于上节排除范围。 |
| V14 故障与隔离 | `app.vision_diagnostics` 验证坏编码触发 `InvalidImage` 和 FailFast、缺文件在受理前报告 `ResourceNotFound`。阈值、整数范围、图像格式、ROI 和掩膜错配由各工具测试校验。摘要不符属于上节排除范围。 |
| V15 结果与取消 | `workflow.vision_lifecycle` 验证旧图像跨历史淘汰和引擎销毁仍可读，受控取消拦截迟到输出及后继，实际退出后可启动新一轮。 |
| V16 单节点与快照 | `workflow.vision_lifecycle` 验证独立阈值和 Blob 只调用目标，Busy 请求的图像不进入后续调用，编辑后的快照沿用新 ROI 和阈值。 |

## 交接入口

固定方案可作为 S5 的界面一致性基准。方案通过 `load_solution` 读取，执行沿用 `WorkflowEngine` 与 `RunRequest`，通过 `RunHandle` 查询结果、诊断和来源。界面和无 GUI 入口使用相同的控制关系、类型化绑定和坐标约定。[架构与装配](architecture.md#215-s46-真实流程与命令行往返) [持久化与运行模型](data-model.md)
