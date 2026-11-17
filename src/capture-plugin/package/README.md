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

1.1.19：截图翻译可以设单独的快捷键（#356）：在截图设置“快捷键”分类里录一个“截图翻译”的快捷键（默认不设），或用托盘菜单“截图 › 截图翻译”、`cap` 搜索。按下后直接拖选一块或单击窗口，松开鼠标就识别并把译文贴在原来的位置，不用再点工具条上的“翻译”，也不显示工具条、不写入截图历史。`Esc` 或右键取消；翻译用的是“翻译”插件里设好的服务，没设好或没有文字识别组件时会打开“识别文字”窗口说明原因。贴图按 `E` 或右键“编辑”时就在贴图上编辑（#343）：贴图原地长出工具条（上方放不下时长在下面），不再打开新窗口；`Enter` 或点“完成”换成改好的图，`Esc` 放弃、贴图保持原样。点了贴图停下来时，屏幕左下角会列出贴图的按键（贴图挡着就换到右下角），一按鼠标或按键就消失，`?` 或 `F1` 随时显示 / 隐藏；关掉设置里的“截图和贴图时显示快捷键提示”就不再自动出现。框选前按 `Tab` 只框整个窗口（#360，和 Snipaste 一样）：窗口里的按钮、输入框不再单独框出，再按一次恢复；选择会记住，截图设置里的“框选时只检测窗口”是同一个开关，识别屏幕文字和截图翻译框选时也能用。框选前 `Shift`+方向键一次挪 10 像素。窗口跟着 Win顺 新的“密集”界面大小变（#352）。权限不变。

Version 1.1.19: screenshot translation has a hotkey of its own (#356): record a “Translate screen region” shortcut under Shortcuts in the capture settings (none by default), or use Screenshot › Translate screen region in the tray menu or `cap`. Drag over an area or click a window; when you let go its text is read and translated in place, without pressing Translate on the toolbar, and with no toolbar and nothing in the screenshot history. `Esc` or a right click cancels. It uses the service set up in the Translate plugin; if that is not set up, or the text reader is missing, the OCR window opens and says why. Edit on a pin (`E`, or Edit in its menu) now edits it in place (#343): the pin grows a toolbar above it (below it when there is no room above) instead of opening a window; `Enter` or Done puts the edited picture on the pin, `Esc` leaves the pin as it was. Click a pin and pause, and its keys are listed in the bottom-left corner of the screen (bottom-right when the pin is there); a click or a key takes them away, `?` or `F1` shows or hides them, and turning off “Show shortcut hints while capturing and on pins” in the settings stops the automatic list. Press `Tab` before selecting to outline whole windows only (#360, as in Snipaste): the buttons and boxes in them are no longer picked one by one, and `Tab` again brings them back; the choice is kept, “Detect whole windows only while selecting” in the settings is the same switch, and it works when reading or translating text on screen too. Before a selection, `Shift`+arrow keys move the pointer ten pixels at a time. The windows follow WinShun's new Dense interface size (#352). Permissions are unchanged.

1.1.17：截图时界面卡住超过 0.3 秒，诊断日志会记下当时程序各个线程在做什么，用来查“按下截图键后黑屏或卡一两秒”的原因。功能没有变化。

Version 1.1.17: when the interface stops answering for more than 0.3 s during a capture, the diagnostics record what each thread of the program was doing, to find why some PCs go black or stall for a second or two after the capture key. No change in what it does.

1.1.16：“识别屏幕文字”可以不显示结果卡片：截图设置“快捷键”分类里新增“识别后显示结果卡片”（默认开），关掉后识别到的文字直接复制，只有没识别到文字时才提示。窗口占用的内存更少（图标文字不再连带加载电脑里的所有字体，字体文件直接映射）。权限不变。

Version 1.1.16: Read text on screen can go without its result card: a new switch, "Show the result card after reading", on the hotkeys page of the capture settings (on by default); off, the text is copied and only finding no text is still said. The windows use less memory (icon text no longer pulls in every font installed, and font files are mapped). Permissions are unchanged.

1.1.15：截图翻译（#191）：框选后点工具条上的“翻译”，截图贴在原来的位置，里面的文字换成译文（原字用背景色盖掉，译文按原来的位置、颜色和大小排上去，粗体还是粗体，按钮里居中的字还是居中）；`Tab` 在原文和译文之间来回看，右键菜单里有“显示原文 / 显示译文”和“复制译文”，已经贴出的图右键选“翻译”也行。“识别文字”窗口里多了“翻译”按钮：上面原文、下面译文，最多 3000 字。鼠标按键动作里多了“识别文字并翻译”，工具条上的“翻译”按钮可以在设置里隐藏。翻译用的是插件库里的“翻译”插件和你自己的服务，需要 Win顺 0.30.0。打码工具点一下就遮住一行字（#129）：鼠标移到图里的一行字上出现红框，点一下盖住，再点一下取消。“识别文字”窗口加宽，英文界面下按钮放得下。权限不变。

Version 1.1.15: screenshot translation (#191): after selecting a region, the toolbar's Translate button pins the capture where it was taken with its words replaced by the translation (the original words covered with the background colour, the translation set in the same place, colour and about the same size; bold stays bold, words centred in a button stay centred). `Tab` switches between the original and the translation, the right-click menu has Show original / Show translation and Copy translation, and a pin already on the screen can be translated from its menu. The text recognition window has a Translate button: the original above, the translation below, up to 3,000 characters. "Recognize text and translate" is a new mouse button action, and the toolbar's Translate button can be hidden in the settings. It uses the Translate plugin from the plugin library with your own service, and needs WinShun 0.30.0. With the redact tool, a click hides a whole line of text (#129): the line under the mouse is outlined in red, a click covers it, another click takes it off. The text recognition window is wide enough for its buttons in English. Permissions are unchanged.

1.1.14：点一下贴图再按键就能操作：`E` 编辑、`O` 识别文字、`U` 收到屏幕顶边（再按一次展开），都标在右键菜单里，可以在截图设置的“按键 › 贴图窗口”里改（#124、#212）。打开了“用 PrtScn 键截图”后，Windows 或别的程序把这个键改回系统截图时，会自动拿回来（#322）。权限不变。

Version 1.1.14: keys on a pin after a click on it: `E` edits, `O` reads its text, `U` sends it to the top edge of the screen (again brings it back). They are shown in the pin's menu and can be changed under Keys › Pin window in the capture settings (#124, #212). With PrtScn set to take screenshots, the key is taken back when Windows or another program hands it to the system's own screenshot again (#322). Permissions are unchanged.

1.1.13：截图时可以指定选区尺寸（#308）。点选区旁的尺寸，或按 `F`，直接输入宽和高，可以锁定比例，也可以从预设里选；框选之前按 `F` 选预设，会在光标处得到固定大小的选区。预设可以自己增删。二维码识别升级到 zxing-cpp 3.1.1，小的码和 QQ 的圆点码更容易读出来（#305）。权限不变。

Version 1.1.13: a capture can be given an exact size (#308). Click the size by the selection, or press `F`, to type a width and a height, lock the shape, or pick a preset; before selecting, `F` and a preset size make a selection of that size at the pointer. The presets can be added to and removed. QR reading moves to zxing-cpp 3.1.1, which reads small codes and QQ's round, dotted ones far more often (#305). Permissions are unchanged.

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
