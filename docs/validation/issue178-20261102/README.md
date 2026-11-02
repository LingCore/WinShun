# #178：录屏浮条增加选区 / 全屏切换

2026-11-02。实现 [#178 的全屏录屏建议](https://github.com/LingCore/WinShun/issues/178#issuecomment-6568672085)。该功能与同一 issue 的进程权限报错分开处理；权限缺失沿用此前 #164 的修正。

## 行为

- 准备浮条提供“选区 / 全屏”两个互斥选项，支持鼠标与键盘空格操作，沿用 `RecordingButton` 和项目主题。英文为 Area / Full screen。
- 全屏使用原选区所属的 `HMONITOR` 和 `rcMonitor` 物理像素范围，包含任务栏；不会根据浮条或鼠标的新位置改录另一块屏幕，也不把多块屏幕拼成一个区域。
- 切回选区恢复原范围，格式、画质、声音等设置保留。倒计时及录制开始后禁止改变范围；“再录一段”沿用当前模式和范围，再切回选区仍可恢复最初的选择。
- 无法取得原显示器或原选区已不在该显示器内时显示重新选择的提示。中英文 README、使用说明、插件说明和英文翻译已同步。
- 英文窄宽检查发现新增控件会把取消按钮挤到窗口外，已让准备态的尺寸文字优先省略，给操作按钮留足空间；历史失败保存在 [首次窄宽日志](ui-compact-en/preview.log)，最终验证通过。

## 构建和自动回归

Windows 11 / Qt 6.12.0 / MSVC 2022 x64 Release，使用 `build/issue166`。验证在包含其他任务改动的共享工作区完成；本次提交只包含全屏录屏的实现、使用说明和必要验证记录，其他编辑和暂存内容保留。

相关目标 `WinShunCaptureApp`、`WinShunCapture`、`tst_recording`、`tst_capturesettings`、`recording-probe` 构建成功，见 [构建日志](build.txt)、[英文资源更新](build-final.txt)及[窄宽修正构建](build-compact-fix.txt)。验证现场的源码和插件二进制指纹见 [fingerprints.json](fingerprints.json)，包含共享工作区当时的其他编辑，不等同于本次提交的文件指纹。

`tst_recording` 和 `tst_capturesettings` 两组 CTest 全部通过，0 失败，总计 16.26 秒：[日志](tests.txt)。英文 TS XML 解析和改动文件 `git diff --check` 通过。

## 真实插件界面和媒体验证

使用真实插件启动器、DLL、`Capture → CaptureSession → Recorder`、生产 QML 和独立录屏/GIF 工作进程。预览使用自有生成画面及临时偏好，不读写用户录屏设置；全屏录制前用自有测试窗口覆盖整个目标显示器，保存内容为生成画面。

| 场景 | 结果 |
| --- | --- |
| [中文浅色 100%](ui-light100/preview.log) | 退出码 0；鼠标切换、重复选择、键盘切换、恢复原范围、保留设置通过；浮条 665×56，选项展开 665×185 |
| [英文深色 150%](ui-dark150-en/preview.log) | 退出码 0；同样的交互检查通过；浮条逻辑尺寸 755×56，选项展开 755×185 |
| [中文全屏 MP4](full-screen-mp4/preview.log) | 退出码 0；整屏 2560×1600，倒计时、暂停/继续、范围锁定、保存、再录及前一段文件保留均通过 |
| [英文深色 150% 全屏 GIF](full-screen-gif-dark150-en/preview.log) | 退出码 0；全屏捕获后按标准 GIF 档缩为 1280×800，再录保留范围，两段文件生成成功 |
| [最终英文 624 宽完整流程](full-screen-compact-en/preview.log) | 修正后退出码 0；控件矩形无重叠、全部在窗口内；切换、开始、倒计时、暂停/继续、两次保存和再录均通过 |

交互测试向真实 `QQuickWindow` 分发鼠标/键盘事件，不冒充系统物理键鼠端到端验证。窄宽场景通过限定测试窗口宽度覆盖布局，不等同于改变 Windows 的显示设置。

四个 MP4 用独立 Media Foundation Source Reader 完整解码，均为 2560×1600，分别为 94 / 22 / 94 / 22 帧；两个 GIF 用 Pillow 完整解码，均为 1280×800，分别为 47 / 19 帧。见 [媒体汇总](media-decoded.json)；已查看解码帧，内容为全屏生成画面，录屏浮条未进入画面。最终窄宽完整流程使用窄宽修正后的二进制，其余媒体和常规界面证据在窄宽修正前产生。

常规界面与 MP4 运行无 warning；GIF 导出各有一条既有 `QFont::fromString: Invalid description '(empty)'` 工作进程提示，媒体导出和完整解码通过。没有 QML 加载错误或绑定警告，不将结果表述为全部零警告。

界面截图：

- [中文全屏按钮](ui-light100/ready-full-screen.png)
- [英文深色 150%](ui-dark150-en/ready-full-screen.png)
- [修正后的英文窄宽](full-screen-compact-en/ready-full-screen.png)

复现示例（Qt 在 PATH，工作目录为仓库根目录）：

```powershell
$captureAppDir = (Resolve-Path build/issue166).Path
build/issue166/WinShunCapture.exe --app-dir $captureAppDir `
  --preview-recording docs/validation/issue178-20261102/recheck `
  --auto --full-screen --renderer d3d11 --english --compact-width 624
# --ui-only 只检查界面；--gif 验证 GIF；--screen-index 1 选择第二块屏幕。
```

本机只有一块物理显示器，尚未进行真实双屏、负坐标、混合 DPI 或录制期间拔屏验证；范围选择的实现明确使用原显示器句柄和物理边界。本次未发布新版本、未替换用户日常运行程序。
