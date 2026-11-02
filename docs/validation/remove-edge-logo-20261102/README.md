# 移除窗口边缘装饰 Logo（2026-11-02）

用户确认程序 Logo 已进入搜索输入区域，要求移除重复的“窗口边缘显示 Logo”功能。

## 最终行为

- 外观设置删除该开关，移除 SettingsEditor 的 QML 属性和 AI 设置目录条目。
- 剪贴板和对话框旁不再创建独立的外侧 Logo 窗口。删除 `WindowLogo`、`PeekingLogo` 实现及构建项，移除 Logo 的几何计算、动画同步信号与显示回调。
- 对话框搜索栏仍等待新帧后显示，保留原生 HWND、前后次显示隔离、超时兜底和非激活恢复；相关测试直接检查可见性，不再依赖旧 Logo 回调。
- `Appearance/SearchLogo` 旧值不再读取，下次保存设置时清理。测试覆盖旧值 true / false，并核对相邻的任务栏图标偏好未改变。
- 主搜索栏、搜索面板和对话框搜索栏内部的 Logo 保留；主搜索的设置、钉住和居中入口照常使用。托盘及任务栏图标设置保留。

## 验证

独立构建 `build/remove-edge-logo-20261102`，复用已有依赖缓存，不覆盖用户正在运行的目录或 data。Qt 6.12.0、MSVC 2022 x64 Release。

- 四组相关 CTest 回归通过；详细文件记录 Settings 39、AI Settings 154、DialogBarVisibility D3D11 / software 各 19 项，共 231 项，无失败或跳过。
- 生产 `SettingsWindow.qml` 中英文、亮暗主题预览均退出 0、无 QML 警告。检查真实视觉树中没有旧设置行，截图确认“图标与语言”分组只保留任务栏图标和语言，布局完整。
- 复用生产 Logo 交互和主搜索 / 对话框导航检查，报告见 [Logo](search-logo.json)、[对话框](dialog-navigation.json)、[主搜索](search-navigation.json)。未用旧构建的通过结果代替本次验证。
- 最终 EXE 不含旧装饰窗口类名 `WinShun.PeekingLogo`，构建清单不再编译删除的源文件；指纹和检查统计见 [verification.json](verification.json)。

预览使用临时配置和局部测试事件，没有向用户的真实文件对话框发送输入。用户原来的主程序和截图插件继续运行；本次没有发布、提交或替换运行版本。

[中文浅色设置页](settings-light.png) · [英文深色设置页](settings-dark.png)
