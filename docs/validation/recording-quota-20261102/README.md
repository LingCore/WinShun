# AMD 核显笔记本录屏“配额不足”调查

2026-11-02。输入为工作区的 `WinShun-diagnostics.zip`，以及维护者确认的报错：

> 无法启动录屏工作进程：process failed to start 配额不足，无法处理此命令

## 结论

该提示发生在 `QProcess::waitForStarted()`，尚未进入图形设备、屏幕捕获或 H.264 编码初始化。已确认线上官方截图插件 1.0.0 缺少 `processes` 权限，宿主因此设置作业 `ActiveProcessLimit=1`；截图插件自身占用唯一名额，创建录屏工作进程被拒绝，真实宿主/runner 探针返回 1816。这与用户提供的提示一致，属于已定位的 #164 同类故障，不能归因为只有 AMD 核显。

微软对 [Job Object 活动进程数限制](https://learn.microsoft.com/en-us/windows/win32/api/winnt/ns-winnt-jobobject_basic_limit_information)的说明与该边界相符。录屏生产实现通过 `RecordingWorker.cpp` 的独立进程隔离驱动/编码器故障，权限声明必须覆盖这个真实行为。

工作区已经有 #164 的修正：`src/capture-plugin/package/plugin.json` 的 1.0.1 声明 `processes`，升级仍须确认新增权限。没有再次扩大权限，也没有改变录屏实现。**截至本次核对，插件库仍发布 1.0.0，本次没有发布。**

## 用户诊断包和线上包

- [提取的硬件摘要](source-summary.json)：WinShun 0.17.0，Win11 25H2 / 26200，Radeon 780M，驱动 32.0.32015.7029；两块屏幕分别为 2560×1440 / 100% 和 2048×1280 / 125%。
- 原诊断包共 5 个文件，包含主程序事件和主程序/服务日志；没有截图插件日志、插件版本清单或录屏失败事件。它能证明环境，单靠该包不能还原录屏调用。具体失败阶段来自维护者随后提供的错误原文。
- 下载当前插件库实际 `.wsplugin`，核对索引的字节数和 SHA256，再直接读取归档中的清单，确认 1.0.0 缺少 `processes`：[线上包核验](published-package.json)。没有仅依赖索引元数据推断包内权限。
- 原 ZIP 保持不变；报告只保留必要环境及来源指纹，不复制整包中的用户路径。

## 本次验证

Windows 11 / Qt 6.12.0 / MSVC 2022 x64 Release，沿用隔离 `build/issue166`，重新构建 `WinShunPluginHost`、`plugin_fake`、`tst_plugin`、`tst_recording`：[构建日志](build.txt)。

| 验证 | 结果及边界 |
| --- | --- |
| `captureManifestAllowsRecordingWorker` | 读取实际截图清单，经真实 Host/runner 创建子进程成功；仍在作业中，进程树退出清理和 2048 MB 限额保留 |
| `uiPluginKeepsInputMethodExtensions`、`jobLimitsUnelevatedToo` | 未声明进程权限的插件仍受单进程限制，子进程错误 1816；证明 UI 权限本身不会放开子进程 |
| `consentForNewPermissions` | 新增权限仍等待确认，确认前插件不运行；上述四个用例全部通过，[权限日志](permissions-tests.txt) |
| `isolatedEncoding` | 本机 NVIDIA 硬件编码及 Media Foundation 软件编码均生成 MP4，逐帧解码成功并能导出/读取 GIF |
| `isolatedHardwareFailureFallsBack` | 刻意使硬件工作进程失败，能重起软件编码进程并生成视频 |
| `isolatedStallIsBoundedAndCancellable` | 卡住的自有测试进程能在期限内回收，取消后不留下错误结果；上述三个录屏用例全部通过，[录屏日志](recording-tests.txt) |

权限测试合计 6 项通过（含初始化/清理），录屏测试合计 5 项通过（含初始化/清理），无失败或跳过。故障注入用例的退出/超时 warning 为预期；offscreen 字体目录及空字体描述有 warning，不将这些结果表述为零警告或文字渲染验收。

录屏测试使用生成画面和临时文件，没有录制用户桌面。运行器在工作区部署目录时需显式设置 `QT_PLUGIN_PATH`，并使用 ASCII 相对测试日志参数；最初测试因 offscreen 平台查找停在启动阶段，只停止了经路径和父 PID 核对的本次测试进程，随后修正测试环境重跑。没有停止用户运行的 WinShun。

这两类验证分别覆盖真实权限边界和真实媒体编解码，未将它们冒充为用户 AMD 笔记本上的整段录屏验证。用户电脑须在修复插件交付并确认权限后复验；若出现编码阶段的新错误，需要按那个新错误继续定位。
