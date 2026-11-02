# 剪贴板搜索栏 Logo 补齐（2026-11-02）

此前核查将剪贴板搜索栏前的粘贴图标当作功能图标，遗漏了这里同样承担程序身份识别的用途。外侧装饰 Logo 的移除不涉及这个输入区域内的图标。

`ClipboardPage.qml` 现在使用与主搜索栏、对话框搜索栏相同的 `qrc:/branding/logo.svg`，沿用 `Theme.searchIconSize`、原来的锚点和 14 DIP 输入间距。Logo 作为装饰图片，不增加点击动作；输入、分类、预览、固定和粘贴行为继续使用既有实现。

## 当前构建验证

Windows 11、Qt 6.12.0、MSVC 2022 x64 Release、D3D11。修复版构建在独立目录 `build/remove-edge-logo-20261102`，未覆盖或退出用户当前运行的程序。

- 构建成功，见 [build.log](build.log)。被测 EXE 的 SHA256 见 [binary.json](binary.json)。
- 在既有 `checkClipboardInteractions` 中检查真实 `ClipboardWindow`：共享 SVG 加载就绪，顶部和底部两种布局下 Logo 与输入框垂直对齐、间距保持 14 DIP，搜索输入保留逻辑焦点，并保存实际窗口画面。
- 浅色 100% 和深色 150% 各运行一次生产预览，均退出 0、0 warning。使用临时历史和配置，剪贴板监听关闭，没有读写用户原生剪贴板。
- 同次预览的既有回归通过：双击预览、列表尺寸与持久化、预览开关、浏览快捷键循环、发送菜单回调。详细日志和尺寸断言保存在各主题目录。
- 人工查看实际截图，确认顶部和底部搜索框首图标均为程序 Logo，输入文字无重叠。当前运行实例尚未切换到此构建。

复现参数（分别运行）：

```text
WinShun.exe --preview-results <output>/light --clipboard-send --renderer d3d11 --scale 1
WinShun.exe --preview-results <output>/dark --clipboard-send --renderer d3d11 --scale 1.5 --dark
```

| 布局 | 浅色 100% | 深色 150% |
| --- | --- | --- |
| 顶部 | [截图](light/clipboard-search-top.png) | [截图](dark/clipboard-search-top.png) |
| 底部 | [截图](light/clipboard-search-bottom.png) | [截图](dark/clipboard-search-bottom.png) |

旧的入口核查已标记[后续更正](../dialog-search-logo-20261102/README.md)，架构说明同步补入剪贴板搜索栏。
