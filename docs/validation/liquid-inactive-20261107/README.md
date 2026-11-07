# 液态玻璃失焦后保留材质

2026-11-07，Windows 11 专业版 build 26200，Qt 6.12.0 MSVC 2022 x64 Release / D3D11。

## 行为与实现

液态玻璃在失焦后继续显示原生背景，标题栏仍按真实激活状态变淡。设置、主窗口、剪贴板和欢迎窗口的清屏条件同步调整；其他风格继续使用原有失焦回退。应用关闭透明、软件渲染、系统禁止材质仍保留实色路径，菜单不新增透明。

沿用项目已有 `WindowFrame` / `WindowEffects`：仅已登记的 Acrylic 背景在 `WM_NCACTIVATE` 中保持视觉激活，实际失焦消息返回 TRUE，允许系统把焦点交给其他窗口。风格切换时同步更新既有背景的视觉状态；不使用 SetForegroundWindow / SetFocus 来维持材质，不更改 Qt 的 active 属性，不使用未公开的合成接口。系统材质与电源回退继续由 DWM 执行。

参考 PowerToys [AlwaysActiveDesktopAcrylicBackdrop](https://github.com/microsoft/PowerToys/blob/ba946c9885d189dda9b40e2990c5cbf3b4835bbc/src/common/Common.UI.Controls/Backdrops/AlwaysActiveDesktopAcrylicBackdrop.cs) 将材质状态与输入状态分开的思路。PowerToys 该组件使用 Windows App SDK；Win顺复用现有 Qt/Win32 窗口处理，没有引入或复制其 WinUI 实现。Win32 消息契约见 [WM_NCACTIVATE](https://learn.microsoft.com/en-us/windows/win32/winmsg/wm-ncactivate)。

## 验证结果

- 主程序、截图插件、OCR 搜索插件构建通过（`build-verified.log`）；最终仅调整验证夹具后的主程序构建见 `build-complete.log`。仍有既有 Qt 头文件 C4702、Preview.cpp 局部变量遮蔽警告，不是零编译警告声明。
- `tst_appearance inactiveBackdrop liquidParts`：**11 passed，0 failed**（含 init/cleanup），见 `appearance-verified.txt`。新增测试检查：未登记窗口不保留视觉激活，Acrylic/Mica 切换更新 DWM 属性，切换不改变前台窗口和 Qt 激活状态，移除背景后不再保持激活。液态玻璃部件覆盖浅深色、圆/直角、实色/原生配置。
- 生产 SettingsWindow 的失焦背景探针：**浅色 D3D11、深色 D3D11、软件渲染三种配置退出码均为 0，无 QML 警告**。每种检查实际窗口未激活、四种风格的失焦底色，以及关闭透明后的实色回退；见 `light.log`、`dark.log`、`software.log`。这是窗口清屏及绑定验证，不用来证明 DWM 桌面模糊。
- 真实 DWM 验证使用隔离运行的生产 App，后放彩色测试窗口，再让独立小窗口取得焦点。`active-verified.jpg` 与 `inactive-verified.jpg` 表明失焦后玻璃背景保持、标题栏变淡。`native-inactive.json` 同时记录：设置窗口 `active=false`、`foreground=false`、清屏色 `#00000000`；前台为独立的 `WinShun backdrop focus`。两者都是正常顶层窗口，没有修改设置窗口的置顶或所有者关系。
- 实际关闭透明后回退实色，见 `inactive-opaque.jpg`；失焦时切到浅色仍有材质，见 `inactive-light.jpg`。
- 另切到资源管理器检查跨进程焦点交接，设置窗口实际失焦且未成为前台，状态见 `native-other-process.json`。该次被前台窗口部分遮挡，未将其截图当作无遮挡的材质证明。

上述结论针对本机 Windows 11 build 26200。没有实际切换系统省电/高对比度设置，没有在 Windows 10 或其他 Windows 11 build 上验证；这些策略的原有路径保持不变。欢迎页及剪贴板共用修改后的窗口处理，但本轮没有逐一拍摄它们的真实前后台状态。

## 复现与产物

```powershell
# 测试环境使用同一 Qt 安装的 bin、plugins 和 qml 路径。
./build/ui-preview/tst_appearance.exe inactiveBackdrop liquidParts
# 用 Start-Process -Wait -PassThru 等待 GUI 探针并核对退出码：
# WinShun.exe --preview-settings <任务目录> --inactive-backdrop --renderer d3d11 [--dark]
# WinShun.exe --preview-settings <任务目录> --inactive-backdrop --renderer software
# WinShun.exe --probe-runtime <任务目录> --live --backdrop-check --style liquid --renderer d3d11 --dark
# live: cmd.txt 中写 settings 1,0；windows 输出原生/Qt 激活状态；stop 文件结束探针。
```

最终可执行文件保留在 `build/liquid-inactive-20261107/runtime/WinShun.exe`，SHA256：`29AD0000EB85E710EDEDB80EEB117D7430A51ACBF290E41DA6008C7EFFA38617`。这是工作区验证构建，未发布或替换用户运行中的实例。运行验证构建需使用本机 Qt 的 bin、plugins、qml 路径，并将既有 `build/ui-preview` 加入 PATH 提供第三方 DLL。为避免覆盖运行中的 EXE，构建时临时改变了主程序输出目录；验证后已恢复 CMake 的正常输出配置。`build/ui-preview/WinShun.exe` 仍是用户正在运行的旧构建，不能将其误当成本次结果。

独立验证进程均已退出，用户原有设置窗口已恢复，未提交或推送。构建、夹具与日志均在 `build/liquid-inactive-20261107/`；精选原始截图和日志保存在本目录。

原有 QFileSystemWatcher 清理路径报拒绝访问，残留隔离测试配置于 `build/liquid-inactive-20261107/temp/`（8 个 `WinShun-preview-*` 子目录，仅合成测试数据）。收尾检查无本任务进程后，执行策略仍拒绝了原生 PowerShell 删除该目录的操作，仅返回 `blocked by policy`；没有绕过策略或尝试其他删除方式。这些临时配置保留在本项目任务目录，未操作用户配置。
