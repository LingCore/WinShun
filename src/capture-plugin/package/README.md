# 截图与录屏 / Screenshot and recording

Win顺 0.18.0 或更新版本的官方可选插件。设置 → 插件 → 插件库中安装并启用，确认权限后使用截图、延时截图、标注、长截图、贴图、文字识别及 MP4 / GIF 录屏。输入 `cap` 可找到截图命令；快捷键在截图设置中配置。

This official optional plugin requires WinShun 0.18.0 or later. Install and enable it from Settings → Plugins → Library, review its permissions, then use screenshots, annotations, scrolling capture, pins, OCR and MP4 / GIF recording. Type `cap` for capture commands; configure shortcuts in the capture settings.

录屏浮条提供“选区 / 全屏”切换：全屏包含原选区所在显示器的任务栏，切回选区会恢复原范围；“再录一段”沿用当前录制范围。

The recording controls switch between Area and Full screen. Full screen includes the taskbar of the monitor containing the original selection; Area restores that selection. Another take keeps the recording range.

演示和白板（1.1.0）：托盘菜单“截图 › 演示 / 白板”或 `cap 白板`，也可在截图设置的“快捷键”里录一个快捷键（默认不设）。直接在正在使用的桌面上画，`X` 是激光笔，`Esc` 或右键让鼠标和键盘回到下面的程序，`W` 白板、`K` 黑板。

Presenter and whiteboard (1.1.0): Screenshot › Presenter / whiteboard in the tray menu or `cap whiteboard`; record a hotkey for it under Shortcuts in the capture settings (none by default). It draws on the live desktop; `X` is a laser pointer, `Esc` or a right click hands the mouse and keyboard back to the programs below, `W` is a whiteboard and `K` a blackboard.

识别屏幕文字（#198）：在截图设置的“快捷键”里另录一个“识别屏幕文字”的快捷键（默认不设），或用托盘菜单“截图 › 识别屏幕文字”、`cap` 搜索。按下后直接拖选一块或单击窗口，松开鼠标就识别并复制，不显示截图工具条，不写入截图历史，也不贴图、保存；选区旁的小卡片显示“已复制”和前几行字，“编辑”打开识别文字窗口。`Esc` 或右键取消。

Read text on screen (#198): record a separate “Read text on screen” shortcut under Shortcuts in the capture settings (none by default), or use Screenshot › Read text on screen in the tray menu or `cap`. Drag over an area or click a window; when you let go its text is read and copied, with no capture toolbar, nothing in the screenshot history, no pin and no save. A small card beside the area says Copied and shows the first lines; Edit opens the OCR window. `Esc` or a right click cancels.

1.1.0 的其他变化：贴图、保存、贴图窗口的按键可以在截图设置的“按键”里改；`WASD` 挪指针；截图时左下角有快捷键提示；工具条分两行；设置页改成卡片；共用的 PaddleOCR 文字识别读取部分有改动。

Other changes in 1.1.0: the keys for pin, save and the pin window can be changed under Keys in the capture settings; `WASD` moves the pointer; shortcut hints at the bottom left while capturing; a two-row toolbar; the settings as cards; the shared PaddleOCR text reader changed.

1.1.2：截图设置第一次打开时不再卡住、显示“无响应”，电脑很忙时也能马上打开。权限不变。

Version 1.1.2: the capture settings no longer freeze with “Not responding” the first time they open; they open promptly even on a busy computer. Permissions are unchanged.

1.1.3：“识别屏幕文字”有了自己的快捷键（#198）；贴图缩放步子更细并显示百分比，`Ctrl`+滚轮显示透明度，标注的粗细、字号、马赛克格子用滚轮或 `[` `]` 细调（#164）；显卡没有 D3D11 视频功能时录屏改用 CPU 转换画面（#214）；二维码的“打开链接”直接用浏览器打开（需要 Win顺 0.21.0，#225）；窗口跟着 Win顺 设置里的界面风格变；闲着时把内存还给系统。权限不变。

Version 1.1.3: Read text on screen has a hotkey of its own (#198); pin zoom steps are finer and show the percentage, `Ctrl`+wheel shows the opacity, and the wheel or `[` `]` set annotation width, text size and mosaic cell finely (#164); without D3D11 video support recording converts frames on the CPU (#214); Open link on a QR code opens the browser (needs WinShun 0.21.0, #225); the windows follow the interface style chosen in WinShun; idle memory goes back to Windows. Permissions are unchanged.

下一版：截图时可以指定选区尺寸（#308）。点选区旁的尺寸，或按 `F`，直接输入宽和高，可以锁定比例，也可以从预设里选；框选之前按 `F` 选预设，会在光标处得到固定大小的选区。预设可以自己增删。权限不变。

Next version: a capture can be given an exact size (#308). Click the size by the selection, or press `F`, to type a width and a height, lock the shape, or pick a preset; before selecting, `F` and a preset size make a selection of that size at the pointer. The presets can be added to and removed. Permissions are unchanged.

## 权限 / Permissions

- `ui`：显示截图、编辑、贴图及录屏窗口，注册快捷键。Display capture, editing, pin and recording windows and register shortcuts.
- `processes`：启动独立的录屏和 GIF 编码工作进程，仍受插件的作业生命周期和内存限制。Start isolated recording and GIF encoding workers, still subject to the plugin's job lifetime and memory limits.
- `clipboard:read`、`clipboard:write`：读取待贴图的图片、复制截图和识别文字，并保留可编辑截图的关联信息。Read the image to pin, copy images or recognized text, and retain editable capture references.
- `files:write`：保存截图、录屏和可编辑标注。Save captures, recordings and editable annotations.
- `notifications`：显示保存及操作状态提示。Show save and operation notifications.
- `winshun:open`：通过宿主打开已保存的文件。Ask the host to open saved files.

插件在普通用户权限下运行，通过宿主安装目录中的 Qt 运行库显示界面；安装包不重复附带 Qt。增强 OCR 组件由 Win顺 的识别设置单独安装。文件在本机处理；本插件不声明网络权限。源码位于 [LingCore/WinShun/src/capture-plugin](https://github.com/LingCore/WinShun/tree/main/src/capture-plugin) 和相邻的 `src/capture`，MIT 许可；第三方许可证见包内 `licenses/`。

The plugin runs with normal user rights and uses the host's Qt runtime. Enhanced OCR is installed separately through WinShun's recognition settings. Processing stays on this PC; the plugin requests no network permission. Sources are in `src/capture-plugin` and `src/capture` of LingCore/WinShun, under MIT; third-party notices are in `licenses/`.

1.0.1 补齐工作进程权限；从旧版更新后需确认新增的 `processes` 权限。中文输入法兼容修复还需要包含相应宿主修复的 Win顺 版本，单独更新插件不能改变宿主启动策略。

Version 1.0.1 declares the worker-process permission; updating from an older version requires approval of the added `processes` permission. The input-method compatibility fix also requires a WinShun host containing the corresponding runner fix; updating the plugin alone cannot change the host's launch policy.
