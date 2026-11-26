# 截图与录屏 / Screenshot and recording

Win顺 的官方可选插件，需要 Win顺 0.18.0 或更新版本（个别功能要更新的 Win顺，下面注明）。新装的 Win顺 安装包里已带着这个插件，欢迎引导里就能装上；也可以在 设置 → 插件 → 插件库 里安装，确认权限后启用。快捷键在截图设置的“快捷键”里录，截图时的按键在“按键”里改；输入 `cap` 能找到截图的各个命令，托盘菜单的“截图”里也都有。

## 能做什么

- **截图**：拖选一块，或单击窗口、按钮、输入框；框选前按 `Tab` 只框整个窗口（#360）。滚轮扩大到整个窗口、整块屏幕，再滚一下跨过所有显示器（#409）。按 `F` 输入精确的宽高或选预设尺寸（#308），`Shift+R` 用上次的选区，`,` `.` 翻出之前的截图接着改；放大镜里按 `C` 复制颜色。延时截图 `Ctrl+Shift+D`（3、5、10 秒）。
- **标注**：矩形、椭圆、箭头、直线、画笔、荧光笔、文字、序号（1 2 3、A B C、Ⅰ Ⅱ Ⅲ、① ② ③ 等写法）、马赛克（点一下盖住一行字）、聚光灯 `G`（可以框几块，其余压暗）。粗细、字号、马赛克格子用滚轮或 `[` `]` 细调。
- **截完之后**：复制、保存 `Ctrl+S`（选了保存文件夹就直接存，否则另存为）、贴图 `F3`、识别文字、识别公式 `Shift+F`（转成 LaTeX，可复制为 Word 公式、Markdown、Typst）、读二维码 `Q`、翻译、长截图 `Ctrl+L`、录屏 `Ctrl+R`。从文件打开的图片改完按 `Ctrl+S` 存回原文件（#427）。
- **贴图**：贴在屏幕上，滚轮缩放、`Ctrl`+滚轮调透明度。点一下贴图再按键：`E` 就地编辑（#343）、`O` 识别文字、`T` 选字（贴上后在后台识别，拖选就能复制，#423）、`Shift+C` 复制全部文字、`Shift+F` 识别公式、`U` 收到屏幕顶边（收起的小条挡路时可以沿顶边左右拖开，#456）、`R` `H` `V` 旋转翻转、`Esc` 关掉。贴图可以分组，一键显示或隐藏一组（#451）；“截图并直接贴图”快捷键选完就贴（#450）；剪贴板没变时连按贴图键，按顺序找回之前的截图和关掉的贴图（#446）。复制的文字也能贴成卡片（#388）。
- **识别文字和翻译**：“识别屏幕文字”“截图翻译”各有自己的快捷键（#198、#356），拖选一块或单击窗口，松开就复制文字或把译文贴在原处，不出工具条。识别文字窗口可以合并断开的行、复制成表格（粘到 Excel、WPS 一格一格的，合并单元格也在）、翻译；`Ctrl+Enter` 复制并关闭。翻译用“翻译”插件里设好的服务（需要 Win顺 0.30.0）。
- **长截图**：选一块会滚动的区域，自己往下滚（也能横着滚，适合宽表格和时间线），拼成一张长图；预览可以拖到一边。键 `Ctrl+L` 可以改，能设成单个字母（#455）。
- **录屏**：录成 MP4 或 GIF，选区和全屏随时切换，“再录一段”沿用范围。画质分小体积、标准、高清、超清四档（超清把不超过 1920×1080 的区域按两倍大小录，彩色文字更清楚），帧率 15 / 30 / 60，可以加鼠标拖尾；录到一半可以放弃。
- **演示和白板**：`cap 白板` 或在设置里录一个快捷键，直接在桌面上画；`X` 激光笔，`W` 白板，`K` 黑板，`Esc` 或右键把鼠标和键盘还给下面的程序。
- **提示**：截图和点住贴图时，左下角列出能按的键，`?` 或 `F1` 显示 / 隐藏；设置里可以关。

## 最近的版本

- **1.1.40**：新增“公式识别”：选好区域后按 `Shift+F` 或点工具条上的“公式”（贴图上按 `Shift+F` 或右键“识别公式”，编辑窗口里点“公式”），把图里的公式转成 LaTeX，可以改，按 LaTeX、Markdown、Word 公式或 Typst 复制，也能导出成图片、PNG、SVG；识别模型（约 115 MB）第一次用时下载，在本机识别（要 Win顺 0.50.0）。截图和贴图的按键提示改用菜单的字号，小屏幕上也看得清；贴图快捷键可以设成和“贴到屏幕”同一个键（#469）；选中的行和卡片更醒目（#474）；系统内存紧张时闲着不主动归还内存，下次截图不用从磁盘读回（#471）。
- **1.1.39**：“复制为表格”认得合并单元格了：截一张表格，识别文字后点“复制为表格”，粘进 Excel、WPS 表格或 Word，合并的单元格照样合并，一格里几行字的还在一格，表格上面的标题和下面的备注各占一行；前面带 0 的编号、很长的数字粘进去不变样。有框线的表格按画出来的线分格，没有框线的用随插件自带的表格模型看行列（PaddleOCR 的 SLANet_plus，7 MB，在本机算）。选着标注工具时，在空白处双击同样完成截图，右键、中键、侧键设的动作也照常执行，不用先放下工具。复制整屏截图时少占内存，图里的字只读一遍（#467）。
- **1.1.38**：录屏控制条按住任何空白处（边距、文字、按钮之间的空隙）都能拖，不再只认左端的手柄；截图设置里多了“录屏保存文件夹”，默认是“视频”里的 WinShun，可一键恢复默认。装了增强文字识别的较大模型或自己的模型（Win顺 0.48.0，设置 › 剪贴板）时，截图识别文字也用它，能读日文。图标按容器排版，间距统一。
- **1.1.37**：画笔、荧光笔在图上点一下就留一个圆点，不用拖（#461）；截图和编辑图片时，鼠标指针显示成笔尖的大小和颜色，画之前就知道多粗。标注工具栏最前面多了“选择”按钮（箭头），点它放下正在用的工具，回到选择和移动。
- **1.1.36**：全屏看视频时截图不用再退出全屏：装了 Win顺 的索引服务时（要 Win顺 0.46.0），截图界面由服务拉起的助手盖在全屏的播放器上，播放器不退出全屏、不弹控制条；只有贴图、识别文字、二维码、长截图、录屏、另存为这些要另开窗口的操作才先让播放器退出全屏（另存为存完会回去）。没有索引服务时和 1.1.35 一样。长截图（`Ctrl+L`）、录屏（`Ctrl+R`）的键可以在“按键 › 截图时”里改，截图时的键也可以是单个字母，比如 `L`（#455）；收在屏幕顶边的贴图可以沿顶边左右拖开（#456）。上一次截图点过“保存”后，下一次按 `Enter` 又是复制了。
- **1.1.35**：全屏播放视频时也能截图：系统自带的“媒体播放器”“电影和电视”等商店应用全屏时，截图框照常盖上来，截的是全屏画面，`Esc` 能退出；复制、保存或取消后，播放器自动回到全屏。
- **1.1.34**：修好 1.1.33 截图工具条上“贴图”直接退出、“识别文字”弹出保存的问题。
- **1.1.33**：录屏四档画质、15 / 30 / 60 帧、鼠标拖尾、可放弃本次录制；贴图分组（#451）、截图并直接贴图（#450）、找回之前的截图和贴图（#446）、存回原文件（#427）；聚光灯；长截图自动滚动；识别文字窗口合并换行、`Ctrl+Enter`；更多序号写法；翻译密钥不对时直接说明（#444）。
- **1.1.32**：用 Win顺 0.44.0 的共用窗口代码重新编译：窗口重建后边框、阴影、深色标题栏都在，任务栏显示自己的图标。
- **1.1.31**：贴图上直接选字复制（#423）；编辑时 `Esc` 先放下工具、取消选择；文字框编辑时也能拖（#415）。
- **1.1.30**：选区可以跨显示器（#409）；Windows 10 1809 能录屏，录屏出错时说明是哪一步；卡顿诊断记录更全（#325）。
- **1.1.29**：长截图后屏幕上不再留下一个空框。
- **1.1.28**：截图设置窗口记住大小（需要 Win顺 0.40.0）。
- **1.1.27**：识别很长很细的图片不再占几个 GB 内存。
- **1.1.26**：复制的文字可以贴成卡片（#388，从剪贴板历史贴要 Win顺 0.38.0）。
- **1.1.25**：识别文字可以复制成表格；长截图的预览不会被截进去，可以拖开（#378）。
- **1.1.24**：跟随 设置 › 外观 › 悬停提示 的开关。
- **1.1.23**：横向长截图。
- **1.1.22**：较早的 Windows 10（1809–1909）上录屏、截图、深色标题栏正常。
- **1.1.21**：Windows 10 上闲着时又会归还内存。
- **1.1.20**：贴图角上出现圆形关闭按钮；下拉菜单的滚动条可以拖。

更早的版本见插件库的[发布记录](https://github.com/LingCore/WinShun-plugins/releases?q=winshun.capture)。

## 权限

- `ui`：显示截图、编辑、贴图和录屏窗口，注册快捷键。
- `processes`：启动独立的录屏和 GIF 编码进程，仍受插件的作业生命周期和内存限制。
- `clipboard:read`、`clipboard:write`：读要贴的图片，复制截图和识别的文字，记住可再编辑的截图。
- `files:write`：保存截图、录屏和可再编辑的标注。
- `notifications`：显示保存和操作的提示。
- `winshun:open`：请 Win顺 打开保存好的文件。

插件以普通用户权限运行，界面用 Win顺 安装目录里的 Qt 运行库，包里不重复带 Qt。增强文字识别组件和公式识别模型由 Win顺 下载安装（设置 › 剪贴板 › 记录，或在识别窗口里点下载）。文件都在本机处理，插件不要网络权限（翻译走“翻译”插件）。源码在 [LingCore/WinShun](https://github.com/LingCore/WinShun/tree/main/src/capture-plugin) 的 `src/capture-plugin` 和 `src/capture`，MIT 许可；第三方许可证见包里的 `licenses/`。

---

An official optional plugin for WinShun 0.18.0 or later (a few features need a newer WinShun, as noted). New WinShun setups carry it, so the welcome guide installs it; it can also be installed from Settings → Plugins → Plugin library and enabled after reviewing its permissions. Record hotkeys under Shortcuts in the capture settings and change the keys used while capturing under Keys; `cap` finds every capture command, and the tray menu's Screenshot has them too.

- **Screenshots**: drag over an area, or click a window, button or box; `Tab` before selecting outlines whole windows only (#360). The wheel widens the selection to the window and the monitor, then across all monitors (#409). `F` takes an exact size or a preset (#308), `Shift+R` reuses the last selection, `,` `.` bring back earlier screenshots to work on; `C` copies the colour under the magnifier. Delayed capture: `Ctrl+Shift+D` (3, 5 or 10 seconds).
- **Annotations**: rectangle, ellipse, arrow, line, pen, highlighter, text, numbers (1 2 3, A B C, Ⅰ Ⅱ Ⅲ, ① ② ③ and more), mosaic (a click covers a line of text) and Spotlight `G` (several areas lit, the rest dimmed). The wheel or `[` `]` set width, text size and mosaic cells finely.
- **After selecting**: copy, save with `Ctrl+S` (straight into the save folder if one is chosen, otherwise Save As), pin with `F3`, read text, read formulas with `Shift+F` (as LaTeX, to copy as a Word equation, Markdown or Typst), read QR codes with `Q`, translate, long screenshot with `Ctrl+L`, record with `Ctrl+R`. A picture opened from a file saves back to it with `Ctrl+S` (#427).
- **Pins**: wheel to zoom, `Ctrl`+wheel for opacity. Click a pin, then: `E` edits it in place (#343), `O` reads its text, `T` selects words (read in the background once pinned; drag to copy, #423), `Shift+C` copies all its text, `Shift+F` reads a formula in it, `U` sends it to the top edge, `R` `H` `V` rotate and flip, `Esc` closes. Pins can be grouped, and a group shown or hidden at once (#451); a capture-and-pin hotkey pins as soon as you select (#450); with the clipboard unchanged, the pin key brings back earlier screenshots and closed pins in turn (#446). Copied words can be pinned as a card (#388).
- **Reading and translating text**: Read text on screen and Translate screen region have hotkeys of their own (#198, #356): drag over an area or click a window, and on release the text is copied or the translation pinned in place, without the toolbar. The text window joins broken lines, copies as a table (cell by cell into Excel or WPS) and translates; `Ctrl+Enter` copies and closes. Translation uses the service set up in the Translate plugin (WinShun 0.30.0).
- **Long screenshots**: select an area that scrolls; it scrolls by itself (sideways too, for wide tables and timelines) and comes out as one picture; the preview can be dragged aside.
- **Recording**: MP4 or GIF, switching between area and full screen at any time; another take keeps the range. Four qualities, Smaller file, Standard, High and Ultra (Ultra records a region up to 1920×1080 at twice its size for sharp coloured text), 15, 30 or 60 frames a second, an optional cursor trail; a recording can be thrown away halfway.
- **Presenter and whiteboard**: `cap whiteboard` or a hotkey from the settings draws on the live desktop; `X` laser pointer, `W` whiteboard, `K` blackboard, `Esc` or a right click hands the mouse and keyboard back.
- **Hints**: while capturing and on a clicked pin, the keys are listed at the bottom left; `?` or `F1` shows or hides them, and the settings can turn them off.

Recent versions: **1.1.40** Formula recognition: `Shift+F` or Formula on the toolbar after selecting an area (`Shift+F` or 识别公式 on a pin, Formula in the editor) reads the formula as LaTeX to edit and copy as LaTeX, Markdown, a Word equation or Typst, or to export as a picture, PNG or SVG; the model (about 115 MB) is downloaded the first time and runs on this PC (WinShun 0.50.0); the key hints of captures and pins take a menu's type size, readable on small screens; the 贴图 hotkey may be the 贴到屏幕 key (#469); selected rows and cards stand out more (#474); idle memory is kept while Windows is short of it, so the next capture is not read back from the disk (#471). **1.1.39** Copy as a table knows merged cells: paste into Excel, WPS or Word and merged cells arrive merged, a cell of several lines stays one cell, a title above and a note below get rows of their own, and codes with a leading zero or long numbers stay as they are; a ruled table is read from its drawn lines, one without by the table model that now comes with the plugin (PaddleOCR's SLANet_plus, 7 MB, on this PC); with an annotation tool on, a double click on the bare picture finishes the capture and the actions set for the right, middle and side buttons run, with no putting the tool down first; copying a full-screen capture takes less memory and its words are read once (#467); **1.1.38** the recording bar moves from any empty place (margins, words, the gaps between buttons), not only its grip; the screenshot settings have a Recording folder row (WinShun in Videos by default, one click back to it); with the larger or your own enhanced OCR model installed (WinShun 0.48.0, Settings › Clipboard), recognizing text in a capture uses it and reads Japanese; icons are laid out by their container. **1.1.37** a click with the pen or the highlighter leaves a dot (#461), and while capturing or editing a picture the pointer shows the brush tip's size and colour; a Select button (an arrow) first on the annotation toolbar puts the current tool down. **1.1.36** with the index service installed (WinShun 0.46.0), a capture over a full-screen Store app comes from a helper the service starts, so the player stays in full screen; only Pin, Recognize text, QR codes, scrolling screenshots, recording and Save As take it out first (it goes back after Save As); without the service it works as in 1.1.35. The scrolling screenshot's `Ctrl+L` and the recording's `Ctrl+R` can be changed under Keys › While capturing, and a capture key may be a single letter such as `L` (#455); a pin folded to the top edge can be dragged along it (#456). After Save was clicked in one capture, Enter copies again in the next. **1.1.35** captures over a full-screen video in Windows' Media Player, Movies & TV and other Store apps: the overlay comes over it with the full-screen picture and `Esc` works; after copying, saving or cancelling, the player goes back into full screen. **1.1.34** fixes 1.1.33's toolbar, where Pin closed the capture and Read text opened Save. **1.1.33** recording qualities, frame rates, cursor trail and discard; pin groups (#451), capture-and-pin (#450), earlier screenshots and pins brought back (#446), save back to the file (#427); Spotlight; self-scrolling long screenshots; the text window joins lines, `Ctrl+Enter`; more number styles; a refused translation key is named (#444). **1.1.32** rebuilt with WinShun 0.44.0's window code. **1.1.31** select words on a pin (#423); `Esc` in the editor puts the tool down first; text boxes drag while edited (#415). **1.1.30** selections across monitors (#409); recording on Windows 10 1809, with the failing step named; fuller stall diagnostics (#325). **1.1.29** no empty frame left after a long screenshot. **1.1.28** the settings window keeps its size (WinShun 0.40.0). **1.1.27** long thin pictures no longer take gigabytes. **1.1.26** text pins (#388; from the clipboard history with WinShun 0.38.0). **1.1.25** copy text as a table; the long screenshot preview stays out of the picture and can be moved (#378). **1.1.24** follows Settings › Appearance › Hover tips. **1.1.23** sideways long screenshots. **1.1.22** recording, capture and dark title bars on Windows 10 1809–1909. **1.1.21** idle memory given back on Windows 10 again. **1.1.20** a round close button on pins; draggable scroll bars in drop-down menus. Older versions: the plugin library's [releases](https://github.com/LingCore/WinShun-plugins/releases?q=winshun.capture).

Permissions: `ui` (capture, editing, pin and recording windows, hotkeys), `processes` (isolated recording and GIF workers, within the plugin's job lifetime and memory limits), `clipboard:read` and `clipboard:write` (the picture to pin, copying pictures and text, editable capture references), `files:write` (captures, recordings, editable annotations), `notifications` (save and status notices), `winshun:open` (WinShun opens saved files). The plugin runs with normal user rights and uses the host's Qt runtime; enhanced OCR and the formula recognition model are downloaded by WinShun (Settings › Clipboard › Recording, or Download in the recognition window). Everything is processed on this PC and the plugin asks for no network permission (translation goes through the Translate plugin). Sources: `src/capture-plugin` and `src/capture` of LingCore/WinShun, MIT; third-party notices in `licenses/`.
