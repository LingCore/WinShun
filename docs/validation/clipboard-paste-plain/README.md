# 剪贴板历史：默认粘贴为纯文本

日期：2026-11-24　机器：本机 Windows 11（26200），2560×1600 150%，本地构建 `build/clip-pin-files`。

## 需求

用户（Zh-y-h）：大多数输入框会自己去掉格式，但 Word、WPS 这类文档软件会把格式带进来，而这些格式多半没用。希望单击剪贴板记录就粘贴纯文本，需要格式时再另选。

## 改法

设置 › 剪贴板 › 记录 › “粘贴动作与列表显示”里加一个开关“默认粘贴为纯文本”（INI `Clipboard/PlainByDefault`，默认关，AI 助手可读可改）。

- 开着时：单击或双击（看“单击粘贴”）、Enter、Alt+1–9 和不打开面板的快速粘贴快捷键，粘贴文字都去掉 HTML、RTF 和 WPS 自带格式，只粘贴纯文本。
- Shift+Enter、Alt+Shift+1–9 和右键菜单里的“保留格式粘贴”（只在这一条带格式时出现）按原格式粘贴。
- 文件和图片照常粘贴：文件还是文件，图片还是图片，Shift+Enter 仍是“粘贴为纯文本”（文件路径或公式文字）。
- “复制”（Ctrl+C，启动器里的复制）保留全部格式；选中多条时本来就合成纯文字，不提供“保留格式粘贴”。
- 窗口里的说明跟着变：钉住时状态栏是“单击粘贴纯文本”或“双击粘贴纯文本”；快捷键列表、预览的提示（“带格式，Shift+Enter 保留格式粘贴”）、设置里“面板内数字键粘贴”的说明都说 Shift 是保留格式。

## 验证

| 项目 | 结果 |
|---|---|
| `WinShun.exe --preview-results <目录> --clipboard-paste-plain`（真实 QML 剪贴板页面、临时历史；要写进剪贴板的内容在写入系统剪贴板之前截下，`PasteWriteProbe`） | 通过。关着时：Enter 带 HTML 和 RTF，Shift+Enter 只有文字，菜单是“粘贴为纯文本”。开着时：Enter 和 Alt+1 只有文字，Shift+Enter 和 Alt+Shift+1 带格式；带格式的一条有“保留格式粘贴 Shift+Enter”，没格式的一条没有第二种粘贴；文件照样粘贴成文件，Shift+Enter 是路径；图片照样是图片；选中多条时没有“保留格式粘贴”；复制保留格式；钉住时状态栏写“双击粘贴纯文本”或“单击粘贴纯文本”；快捷键列表和预览提示都改成“保留格式粘贴”；系统剪贴板写入线程没有启动。菜单截图：[menu.png](menu.png) |
| `WinShun.exe --preview-settings <目录> --page 7` | 剪贴板设置页正常显示，没有 QML 警告，新开关在“单击粘贴”下面：[settings.png](settings.png) |
| `ctest -R '^(tst_aisettings\|tst_settings\|tst_settingsfilters)$'` | 通过 |
| 相关的已有预览 `--clipboard-select`、`--clipboard-hover-copy`、`--clipboard-pin-files` | 通过 |
| `--shortcut-panel` | 有一项失败：“launcher: scope hint follows nonblank input, including no results”。main 的 `build/release` 不含这次改动，同样失败，和这次改动无关，另行处理 |
