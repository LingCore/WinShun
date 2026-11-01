# 直角窗口边框验证

日期：2026-11-01。用户反馈直角窗口异常，随后要求使用 Windows 原生阴影和原生直角边框。

## 当前实现：原生直角边框与阴影

Windows 11 的直角和圆角模式现在统一使用 DWM 原生边框及阴影。窗口保留 `WS_CAPTION` 固定框架，通过 `WM_NCCALCSIZE` 将完整窗口交给客户区；普通弹窗不增加 `WS_THICKFRAME`，现有 `WindowFrame` 继续控制拖动和缩放。边框颜色使用 `DWMWA_COLOR_DEFAULT`，切换外观只改变圆角偏好，不再切换描边的绘制方。

`WindowEdge.qml` 只保留 Windows 10 回退。`SystemTheme.nativeWindowBorder` 恢复为按系统版本确定的常量，移除直角模式的自绘描边和原生边框禁用分支，也移除不再需要的 HWND 边框登记属性。QML 单例继续使用 `QPointer`，避免测试/预览引擎销毁后留下悬空引用。Windows 10 实机、第三方桌面美化工具仍未验证。

### 最新验证方法

- 使用生产 `WindowEffects`、`WindowFrame` 和 `WindowEdge.qml`，验证三类原生框架、亮暗主题、直角与圆角反复切换，以及透明窗口的 Mica 路径。
- 客户区检查确认 Win11 下没有 QML 描边。桌面截图检查原生边框和窗口外阴影；临时将自建测试窗口的 DWM 边框设为探测色，确认四条边都随系统属性变化，随后恢复系统默认。没有读取只支持设置的 `DWMWA_BORDER_COLOR`。其语义见 [Microsoft 文档](https://learn.microsoft.com/en-us/windows/win32/api/dwmapi/ne-dwmapi-dwmwindowattribute)。
- 保留客户区尺寸、固定窗口无缩放框、边缘命中为普通客户区，以及退出可缩放面板后阴影仍存在的检查。没有写用户设置或用户文件。
- 收藏菜单回归增加直角的亮暗主题；搜索预览支持 `--corners square`，用于真实搜索组件的交互验证。

### 最新结果

- [完整外观测试](validation/native-window-border-20261101/native-border-final.txt)：16 passed、0 failed、0 skipped，包括透明效果与两种主题。本机实际 DPR 1.5。
- 桌面原生边框补测实际 [100%](validation/native-window-border-20261101/native-border-dpr-1.txt)、[125%](validation/native-window-border-20261101/native-border-dpr-1.25.txt)、[200%](validation/native-window-border-20261101/native-border-dpr-2.txt)，每档 10 passed、0 failed、0 skipped；[软件渲染四档回归](validation/native-window-border-20261101/native-border-software.log) 通过。
- [生产收藏菜单](validation/native-window-border-20261101/native-border-menu.txt)：亮暗主题分别覆盖圆角和直角，7 passed、0 failed、0 skipped；焦点保留、导航、滚动末项及临时命令执行均通过。
- 新主程序的[直角外观设置](validation/native-window-border-20261101/native-border-settings.log)和[生产搜索交互](validation/native-window-border-20261101/native-border-search.log)均退出码 0、运行警告 0。QML lint、构建及 `git diff --check` 通过。
- 桌面证据：[默认浅色边框](validation/native-window-border-20261101/native-square-light.png)、[默认深色边框](validation/native-window-border-20261101/native-square-dark.png)、[透明窗口](validation/native-window-border-20261101/native-square-backdrop.png)、[DWM 探测色](validation/native-window-border-20261101/native-color-probe.png)。探测色只用于自建测试窗口，产品使用系统默认色。
- 新主程序及截图插件已同步，Win顺 已正常启动并显示；配置和数据保留。[产物 SHA256、运行进程与备份](validation/native-window-border-20261101/native-build.json)。未提交、推送或发布。

下面保留前两轮修复及其证据，作为实现演进记录，不作为当前描边策略。

## 复现与原因

Windows 11、Qt 6.12、D3D11 下，用真实 `WindowEffects`、`WindowFrame` 和 `WindowEdge.qml`，在自建背景上分别显示纯弹窗、可缩放窗口、带自绘标题栏的设置类窗口。

- 圆角模式：三类窗口都有系统轮廓。
- 直角模式：纯弹窗没有系统描边和阴影，可缩放窗口及设置类窗口有系统描边。
- 原先按“Windows 11 支持系统边框”统一隐藏 QML 描边，导致第一类窗口丢失边界。原外观测试只检查客户区，未发现桌面合成的差异。

新增桌面回归在修复前失败：边缘实际为背景填充 `#F9F9F9`，预期描边为 `#B4B4B4`。见 [失败记录](validation/square-windows-20261101/before.txt)。

| 修复前的直角纯弹窗 | 修复后的直角纯弹窗 |
| --- | --- |
| ![缺失描边](validation/square-windows-20261101/before.png) | ![恢复描边](validation/square-windows-20261101/after-light.png) |

## 历史：首次恢复边界

Windows 11 圆角模式继续由 DWM 绘制轮廓，保留 #158 对系统圆角及桌面美化的兼容方向。直角模式和 Windows 10 共用 `WindowEdge` 回退；对已登记的无边框窗口关闭 DWM 描边，避免设置类窗口叠加两层线。

已有和隐藏的窗口跟随外观切换，新建窗口使用当前状态。原生预览页面只登记圆角，未登记边框管理，不受此描边策略影响。`SystemTheme.nativeWindowBorder` 改为可通知属性；单例引用改用 `QPointer`，避免多个测试/预览引擎销毁后留下悬空引用。

## 历史验证

- 客户区外观回归：四档 `QT_SCALE_FACTOR`（1、1.25、1.5、2）全部通过，详见 [记录](validation/square-windows-20261101/client-scales.log)。
- 桌面合成回归：三类窗口 × 亮暗主题，每个窗口执行“直角 → 圆角 → 直角”；检查直角四角、四边及边框内侧填充，圆角时检查 QML 回退关闭。截图区域只包含自建窗口与背景。
- 本机显示缩放为 150%；桌面测试上述四档附加缩放测得实际 DPR 分别为 1.5、1.875、2.25、3。均通过，分别见 [1](validation/square-windows-20261101/native-1.txt)、[1.25](validation/square-windows-20261101/native-1.25.txt)、[1.5](validation/square-windows-20261101/native-1.5.txt)、[2](validation/square-windows-20261101/native-2.txt)。
- 按实际 DPR 补齐常用显示缩放：[100%](validation/square-windows-20261101/square-dpr-1.txt)、[125%](validation/square-windows-20261101/square-dpr-1.25.txt)、[150%](validation/square-windows-20261101/square-final-live.txt)、[200%](validation/square-windows-20261101/square-dpr-2.txt) 均通过。150% 使用完整外观测试，QtTest 报告 14 passed、0 failed、0 skipped；其余各档桌面检查报告 8 passed、0 failed、0 skipped。
- 主程序 `WinShun` 和同样使用这些组件的 `WinShunCaptureApp` 构建成功，见 [构建产物与 SHA256](validation/square-windows-20261101/build.json)。没有发布新版本。
- [真实收藏菜单回归](validation/square-windows-20261101/square-foldermenu.txt) 5 passed、0 failed、0 skipped。新主程序的外观设置页面分别运行 [直角](validation/square-windows-20261101/settings-corners-square.log) 和 [圆角](validation/square-windows-20261101/settings-corners-round.log)，退出码均为 0、运行警告均为 0，已检查实际渲染截图。`WindowEdge.qml` 的 qmllint 和 `git diff --check` 通过。
- 桌面回归需要交互桌面，使用 `WINSHUN_APPEARANCE_LIVE=<输出目录>` 显式启用；普通 CTest 不执行桌面截图。

不把客户区截图当作原生外框的验证，也不读取只支持设置的 `DWMWA_BORDER_COLOR`。未验证 Windows 10 实机及报告者的桌面美化工具。初次修复只恢复直角边界，未解决纯弹窗的原生阴影，后续处理如下。

## 后续：直角模式的 Windows 原生阴影

用户继续追问阴影。补充桌面回归后，修复前亮暗主题的纯弹窗均失败，可缩放和设置类窗口通过：自建背景上的窗口外侧像素没有阴影。直角并不禁止 Windows 阴影，缺少的是可用于合成的原生框架。

`WindowEffects` 为受管理窗口保留 `WS_CAPTION` 原生固定框架，通过 `WM_NCCALCSIZE` 保持整个窗口为客户区，再由 DWM 绘制阴影；不增加 `WS_THICKFRAME`。现有 `WindowFrame` 继续负责命中测试与拖动，可缩放窗口仍按原来的开关管理缩放框。最大化时将客户区计算交给已有逻辑。`WM_STYLECHANGING` 保留阴影需要的固定框架，因此退出可缩放面板后阴影不会消失。原生框架和客户区的关系见 [Microsoft 的自定义窗口框架文档](https://learn.microsoft.com/en-us/windows/win32/dwm/customframe)。

新增检查直接取桌面合成中窗口外侧的阴影像素，并验证客户区尺寸、固定窗口没有 `WS_THICKFRAME`、边缘命中为 `HTCLIENT`，以及退出可缩放模式后的阴影。没有使用 QML 自绘阴影。

验证结果：[修复前纯弹窗失败](validation/square-windows-20261101/shadow-red.txt)；修复后 [150% 完整外观测试 14 项通过](validation/square-windows-20261101/shadow-caption.txt)，[100%](validation/square-windows-20261101/shadow-dpr-1.txt)、[125%](validation/square-windows-20261101/shadow-dpr-1.25.txt)、[200%](validation/square-windows-20261101/shadow-dpr-2.txt) 的桌面回归全部通过；[菜单 5 项通过](validation/square-windows-20261101/shadow-foldermenu.txt)，[软件渲染四档检查通过](validation/square-windows-20261101/shadow-software.log)。[实际原生阴影图](validation/square-windows-20261101/native-shadow.png) 只包含自建窗口和背景。主程序及截图插件构建成功，并已按用户此前运行最新版的要求同步、启动；[新构建和备份信息](validation/square-windows-20261101/shadow-build.json)。用户数据和配置保留。

独立构建目录：`D:\WinShun-validation-issues156-158-20261101`。用户运行实例、用户配置和数据保留；没有执行提交、推送或发布。
