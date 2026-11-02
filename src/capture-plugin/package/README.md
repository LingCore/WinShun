# 截图与录屏 / Screenshot and recording

Win顺 0.17.0 或更新版本的官方可选插件。设置 → 插件 → 插件库中安装并启用，确认权限后使用截图、延时截图、标注、长截图、贴图、文字识别及 MP4 / GIF 录屏。输入 `cap` 可找到截图命令；快捷键在截图设置中配置。

This official optional plugin requires WinShun 0.17.0 or later. Install and enable it from Settings → Plugins → Library, review its permissions, then use screenshots, annotations, scrolling capture, pins, OCR and MP4 / GIF recording. Type `cap` for capture commands; configure shortcuts in the capture settings.

录屏浮条提供“选区 / 全屏”切换：全屏包含原选区所在显示器的任务栏，切回选区会恢复原范围；“再录一段”沿用当前录制范围。

The recording controls switch between Area and Full screen. Full screen includes the taskbar of the monitor containing the original selection; Area restores that selection. Another take keeps the recording range.

## 权限 / Permissions

- `ui`：显示截图、编辑、贴图及录屏窗口，注册快捷键。Display capture, editing, pin and recording windows and register shortcuts.
- `clipboard:read`、`clipboard:write`：读取待贴图的图片、复制截图和识别文字，并保留可编辑截图的关联信息。Read the image to pin, copy images or recognized text, and retain editable capture references.
- `files:write`：保存截图、录屏和可编辑标注。Save captures, recordings and editable annotations.
- `notifications`：显示保存及操作状态提示。Show save and operation notifications.
- `winshun:open`：通过宿主打开已保存的文件。Ask the host to open saved files.

插件在普通用户权限下运行，通过宿主安装目录中的 Qt 运行库显示界面；安装包不重复附带 Qt。增强 OCR 组件由 Win顺 的识别设置单独安装。文件在本机处理；本插件不声明网络权限。源码位于 [LingCore/WinShun/src/capture-plugin](https://github.com/LingCore/WinShun/tree/main/src/capture-plugin) 和相邻的 `src/capture`，MIT 许可；第三方许可证见包内 `licenses/`。

The plugin runs with normal user rights and uses the host's Qt runtime. Enhanced OCR is installed separately through WinShun's recognition settings. Processing stays on this PC; the plugin requests no network permission. Sources are in `src/capture-plugin` and `src/capture` of LingCore/WinShun, under MIT; third-party notices are in `licenses/`.
