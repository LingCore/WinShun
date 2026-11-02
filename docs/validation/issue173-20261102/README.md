# #173：搜索窗口图标命中范围异常调查

2026-11-02。报告者称 0.16/0.17 在两台电脑上偶发图标中央不响应，只有左上角能悬停/点击；重启 Win顺 可暂时恢复。[Issue](https://github.com/LingCore/WinShun/issues/173)。

## 已核对的生产路径

- 图标在原生拖动区域内，QML 不直接接收这一带的普通鼠标悬停。
- `WindowFrame` 的窗口过程处理 `WM_NCHITTEST` / 非客户区鼠标事件，使用 `ScreenToClient` 转到客户区，再以 `QWindow::devicePixelRatio()` 换到界面逻辑坐标。
- 原生命中通过实际 `QQuickItem::mapRectToScene()` 获取当前控件范围；Logo/原放大镜的灰色背景通过 `frame.pointer` 换到该 Item 的坐标判断。
- Logo 替换没有绕开上述逻辑。截图中的左上角仍有效、中央不响应，使坐标/缩放状态失配成为待验证方向，但尚无证据确认具体故障环节。

`ScreenToClient` 是 [Windows 的屏幕到客户区换算 API](https://learn.microsoft.com/en-us/windows/win32/api/winuser/nf-winuser-screentoclient)。[Qt 的 QScreen 文档](https://doc.qt.io/qt-6/qscreen.html#devicePixelRatio-prop)也指出已知目标窗口时应使用窗口的 DPR；本次未在没有复现的情况下替换这个来源或添加固定坐标补偿。

## 验证与范围

扩展既有 `checkSearchLogo`，原先主要检查中心点，现在增加：

- 搜索框和搜索面板，各在紧凑、标准、宽松三档间动态切换。
- 每个 Logo 区域检查 3×3 点（含四角附近），同时验证原生拖动命中、实际灰色背景对应的悬停状态和真实原生中键动作。
- 检查四边之外不悬停、不触发动作。中键和左键共用生产 `onHandle()` 判定；原有中心左键请求设置检查仍保留，但没有把所有点都当作物理左键拖动测试。
- 测试输入点按 Win32 客户区实际尺寸与 QML 窗口逻辑尺寸的比值换算，独立于被测路径的 DPR 读取，避免用同一假设造出自洽结果。

在 Windows 11 / Qt 6.12.0 / MSVC x64 Release / D3D11，使用当前开发代码，结果为：

| 条件 | 结果 | 证据 |
|---|---|---|
| 进程缩放参数 1.25，中文浅色 | 123 项通过，0 warning | [JSON](scale-1.25/search-logo.json)、[日志](scale-1.25/preview.log) |
| 进程缩放参数 2.0，英文深色 | 123 项通过，0 warning | [JSON](scale-2/search-logo.json)、[日志](scale-2/preview.log) |

测试在私有临时索引/配置上运行，通过本地 Win32/Qt 事件驱动真实 Main、WindowFrame 和 Placement，没有向用户桌面发送全局输入，也没有修改 Windows 的显示设置。使用未被用户占用的 `build/issue166` 验证目录；用户实际使用的 `build/dialog-logo-20261102` 及其运行实例保持原样。`build/issue166` 本次产物用于诊断，不应仅依据 exe 时间戳把它当作已准备好的日常运行版本。

复现命令：

```powershell
WinShun.exe --preview-results <output> --search-logo --renderer d3d11 --scale 1.25
WinShun.exe --preview-results <output> --search-logo --renderer d3d11 --scale 2 --english --dark
```

上述仅证明当前开发代码在这些本地状态中的区域判定一致；没有复现报告者在旧版及长期运行后的故障，未做真实混合 DPI 跨屏、休眠唤醒、远程桌面切换验证。因此**当前未确认根因、没有生产修复结论**。本次只有验证路径增强，没有改动 WindowFrame 或生产 QML 的命中算法。

## 已请求的信息

> 2026-11-02 后续决定：维护者要求撤销索取日志和环境资料的请求，GitHub 原回复已改为本地验证结论及“无需再提供日志或其他材料”。以下取证请求仅保留为调查历史，不再作为等待项或后续索取依据。

[已回复并请求诊断](https://github.com/LingCore/WinShun/issues/173#issuecomment-6566378437)：Windows 版本、每屏缩放、是否跨屏/改缩放/唤醒/远程桌面触发，以及异常仍存在时导出的 `WinShun-diagnostics.zip`。既有 `DisplayDiagnostics` 会记录屏幕缩放及显示变化，有助于区分触发条件；它并不保证已记录图标每次命中的细节。

本地验证正常，尚未复现报告中的偶发故障，不再等待报告者提供材料；Issue 保持打开。构建日志和文件指纹见本目录。
