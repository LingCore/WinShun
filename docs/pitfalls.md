# 踩坑记录

开发中遇到过、光看文档或代码想不到的问题。每条写现象、原因和现在的做法；改相关代码之前先看一眼。新遇到的坑补在对应分类里。

设计上的取舍（为什么读 MFT、为什么用 Raw Input、为什么要 D3D11 + FreeType 等）见 README 的“几个设计取舍”。

## 先想清楚：屏幕缩放和分辨率

下面很多坑的根源都在这里。设计和改动窗口、拖动、布局、图片、命中测试时，先过一遍：

- **每个数字都要标明单位**：逻辑像素还是物理像素。Win32 消息（`WM_MOVING`、`WM_NCHITTEST`、`GetWindowRect`）用物理像素，Qt 和 QML 用逻辑像素。在两者交界的地方换算一次，别的地方不要混用。
- **距离和阈值按逻辑像素定**，并想一想在 100%、150%、200% 下各是什么手感。吸附距离 12 逻辑像素，在 150% 下就是 18 物理像素。
- **小数缩放会有取整误差**：125%、150%、175% 下，逻辑像素换成物理像素可能差 1 像素。比较位置时，两边要用同一种单位，或者允许 1 像素的误差。
- **其他情况也要考虑**：多台显示器缩放不同、窗口在显示器之间移动、工作区比整个屏幕小（任务栏占掉一块）、低分辨率的小屏幕，以及程序运行中改了分辨率或缩放。

### 改完界面查像素

改了 QML 的边框、分隔线、卡片、开关、标签、弹出层，或者改了任何尺寸和位置的算法之后，过一遍这张表（原因和做法见 QML / JavaScript 一节的“边框 1 px 在 150% 下一边粗一边细”）：

1. **在 150%、125%、175% 各看一遍**（100%、200% 一般没事）。用户的屏幕是 2560×1600、150%。离屏渲染用 `WinShun.exe --preview-settings <文件夹> --scale 1.5 --renderer d3d11 --tree`（`1.25`、`1.75` 同理），要让 Qt 的 bin 在 PATH 最前面（或在 dist 里）运行；软件渲染（不带 `--renderer`）画出来的边缘和桌面上不一样，不能用来数像素。
2. **四边的粗细**：每个有边框的框，上下左右四边各数几行，应当都是 2 行（150%）、1 行（125%）、2 行（175%），亮度一样。圆角框在 `.5` 位置上会变成 50/100/50 的三行：这就是发虚。
3. **子项不压边框**：放进有边框的父项里的子项（底色、下划线、列表、`clip` 的 Flickable），内缩用 `Theme.edge`，不写 1；父项的尺寸要加上 `2 * edge`。
4. **线在整像素上**：分隔线、竖线、进度线的厚度用 `Theme.edge`；同一张卡、同一个窗口里的线粗细要一样。
5. **位置和大小不带小数**：居中、`/ 2`、`implicitWidth + 常数` 算出来的位置和尺寸，如果边缘要锐利，就用 `Theme.snap` / `Theme.snapUp`；自动排版的行高取整，下面的才都落在整像素上。
6. **文字**：不在 .5 的设备像素上居中（见“图片清晰度”和“列表滚动时整行在抖”里的做法）；同一排的文字高度一样。
7. **怎么量，不要用眼睛**：
   - 抓图用普通 GDI `BitBlt` 到 DIB（或 DXGI 桌面复制）。**不要用 GDI+ 的 `CopyFromScreen`**：它会丢掉 RGB(13,11,12)，这个颜色的像素保持位图原来的值。真窗口抓图时窗口要在最前面（先把它带到前台）。
   - 沿着每条边取一列或一行像素，数“和背景不同的连续像素有几个，边缘那一行是不是半亮”。把 `Theme.edge` 当线粗的期望值。分隔线一起数：同一张卡里的线粗细相同才算过。
   - `--preview-settings … --tree` 会为每张页面图再写一份 `<图>.png.edges.txt`：列出边缘不在整设备像素上的矩形（设备像素坐标、边框、圆角、QML 里的位置），一眼看出是哪个组件。`.02` 的小数是设置页整页的 0.02 偏移（见“列表滚动时整行在抖”），算整像素；`.5` 才是问题。
   - 真窗口（启动器、剪贴板、截图覆盖层）要实测，预览只覆盖设置窗口。输入的安全规则见“界面实测（模拟真实输入）”：截图的热键换个不撞 PixPin 的（Ctrl+1 是用户的），测完把 `WinShun.ini` 原样放回去，不往剪贴板里放东西。

## 窗口：拖动、标题栏、最大化

### Qt 的原生事件过滤器收不到鼠标和移动消息

- **现象**：在 `QAbstractNativeEventFilter` 里处理 `WM_MOVING`、`WM_NCLBUTTONDBLCLK`，吸附和双击都没反应。
- **原因**：Qt 的窗口过程把“输入消息”直接自己处理，不交给原生事件过滤器（`qwindowscontext.cpp` 的 `isInputMessage`，QTBUG-67095，有意如此，6.12 也没变）。范围包括所有鼠标、非客户区鼠标、键盘消息，以及 `WM_MOVING`、`WM_SIZING`、`WM_SYSCOMMAND`、`WM_COMMAND`、`WM_NCMOUSELEAVE`、`WM_PAINT`、`WM_INPUT`、输入法消息等。`WM_NCHITTEST`、`WM_NCCALCSIZE` 不在其中。
- **做法**：用 comctl32 的 `SetWindowSubclass` 给窗口加子类过程，它排在 Qt 的窗口过程前面，处理完用 `DefSubclassProc` 交回 Qt（`src/app/Placement.cpp`、`src/app/WindowFrame.cpp`）。`resources/app.manifest` 已声明 Common Controls 6。

### 在过滤器里写 `*result` 会崩溃

- **现象**：拖动启动器后卡住，然后崩溃（0xc0000005）。
- **原因**：`QWindow::startSystemMove()` 实际是 `PostMessage(WM_SYSCOMMAND, SC_DRAGMOVE)`。投递的消息在消息循环里也会经过过滤器（类型 `"windows_generic_MSG"`），这时 `result` 是空指针。
- **做法**：过滤器里写 `result` 之前先判空；窗口消息尽量用子类过程处理，不用过滤器。

### `startSystemMove()` 只能拖第一次

- **现象**：QML `MouseArea` 按下时调用 `startSystemMove()`，第一次能拖，之后再也拖不动。
- **原因**：系统的移动循环吞掉了鼠标松开，`MouseArea` 一直以为自己还按着。
- **做法**：和 WinUI 的标题栏区域、Electron 的 `app-region: drag` 一样，在 `WM_NCHITTEST` 里对拖动区返回 `HTCAPTION`，拖动、贴靠、双击、窗口菜单都交给系统。QML 里用 `frame.addDragArea(item)` 登记拖动区，用 `frame.addControl(item)` 登记拖动区里可以点的控件（`WindowFrame.cpp`）。

### 拖动区里 QML 收不到悬停

- **原因**：`HTCAPTION` 区域的鼠标消息是非客户区消息，QML 看不到。
- **做法**：子类过程把 `WM_NCMOUSEMOVE` 的位置换算成窗口坐标放进 `frame.pointer`，并用 `TrackMouseEvent(TME_LEAVE | TME_NONCLIENT)` 等 `WM_NCMOUSELEAVE` 来清掉。窗口隐藏时不会有离开消息，要在 `visibleChanged` 里自己清。手柄的悬停效果就靠这个。

### 拖动区里的图标也要能“点一下”

- **现象**：钉住启动器那次（linux.do #157），搜索框左边的放大镜要“按住拖动窗口，单击打开设置”。它在拖动区（`HTCAPTION`）里：按下去系统的移动循环吞掉松开，QML 收不到单击；改成 `MouseArea` 里调 `startSystemMove()` 又会撞上“只能拖第一次”。第一版用 `DragDetect` 分开拖和点，拖动却跟不上鼠标：拖 300×150 物理像素，窗口只走了 288×144。
- **原因**：`DragDetect` 在鼠标走出系统的拖动距离时才返回；之后交给 `DefWindowProc` 的移动循环，以**这时**的光标位置为起点，按下以来走的那一小段（24 步里的第一步）就永远补不回来。
- **做法**：`WindowFrame::addHandle` 登记把手。`WM_NCLBUTTONDOWN` 落在把手上时先 `DragDetect`：返回假（没走出拖动距离就松开，且不是按了 Esc）就是单击，发 `handleClicked` 并吞掉这条消息；返回真就先用 `SetWindowPos` 把窗口挪过“光标现在 − 按下的点”，再照常交给移动循环。把手上的 `WM_NCLBUTTONDBLCLK` 也吞掉，不当成标题栏双击（启动器会回到默认位置）。实测：拖 300×150、再拖回 −300×−150，窗口各走了整整 300×150；不动松开打开了设置窗口。`DragDetect` 只取鼠标和键盘消息，按住不动的那一会儿主线程不处理别的消息，松开就好。

### 吸附：`WM_MOVING` 给的位置是在上一次结果上累加的

- **现象**：加了“靠近默认位置时吸住”以后，从默认位置开始拖，窗口纹丝不动。
- **原因**：`WM_MOVING` 提议的矩形 = 上一次（被我们改过的）矩形 + 鼠标移动量。吸住时改回的位置成了下一次的起点，鼠标一点点移动，就永远出不了吸附范围。
- **做法**：第一次 `WM_MOVING` 时记下鼠标在窗口里的位置（抓取点），之后每次都按“鼠标位置 − 抓取点”算窗口位置，再限制在工作区内（`Placement.cpp`）。

### 拖动中途吸附，手感像卡顿

- **现象**：拖动启动器经过屏幕中线或默认高度时，窗口会“顿”一下，用户说不清是卡还是吸。
- **原因**：拖动过程中靠近中线或默认高度 12 逻辑像素就把窗口按在那条线上。每穿过一条线，鼠标要走约 24 个逻辑像素窗口才跟上，然后一下跳过去。中线和高度线横贯整个屏幕，随便斜着拖都会碰到。
- **做法**：拖动时窗口只跟着鼠标走（只限制在工作区内）；松手时如果离中线或默认高度在 12 逻辑像素以内，再用 220 ms 滑过去（`Placement::rememberSpot`）。实测：拖 201 步，窗口和光标的偏移始终为 0。

### 没有结果时启动器也拖不到屏幕下方

- **现象**：只剩搜索框和底栏的启动器，往下拖到屏幕中间就停住了，看不出是什么挡着。2560×1600、150% 下最低只能拖到约 42% 的高度；1080p、150% 下几乎拖不动。
- **原因**：拖动时一直给 8 行结果留着空间（约 595 逻辑像素），哪怕当时一行都没显示。
- **做法**：拖动时只按窗口当前的实际高度限制在工作区内，空窗口可以一直跟到屏幕底部。松手后如果下面放不下 3 行，就滑上去，直到放得下（`Placement::settleOn`）。结果列表显示多少行，按窗口顶部到工作区底部的距离算（`Placement.room` → `Main.qml` 的 `fitRows`），最少 3 行，最多 8 行，放不下的滚动查看。有结果时往下拖，窗口在自己的底边碰到工作区底部时停住。

### 窗口贴住工作区底部时压住任务栏 1 像素

- **现象**：150% 下启动器停在屏幕最下面时，底边比工作区多出 1 个物理像素，压在任务栏上。
- **原因**：Qt 把窗口的逻辑位置和逻辑大小分别换算成物理像素，各自四舍五入（`QHighDpi::toNativeWindowGeometry`）；`QScreen::availableGeometry()` 本身也是四舍五入来的（1528 物理像素 → 1019 逻辑像素，实际是 1018.67）。在逻辑像素里刚好贴边，换算后可能多出 1 像素。
- **做法**：窗口放下的位置在物理像素里再核对一遍：用 `GetMonitorInfo` 取物理工作区，按 Qt 的算法（屏幕原点 + 四舍五入）算出窗口边缘，超出就挪 1 个逻辑像素（`Placement.cpp` 的 `NativeArea`）。剩余空间也按物理像素算，再向下取整换成逻辑像素。拖动过程中的限制（`WM_MOVING`）本来就按物理像素算，没有这个问题。

### 最大化按钮要支持贴靠布局（Windows 11）

- **做法**：鼠标在最大化按钮上时，`WM_NCHITTEST` 返回 `HTMAXBUTTON`，系统才会显示贴靠布局。`WM_NCLBUTTONDOWN`、`WM_NCLBUTTONDBLCLK` 要吞掉（交给系统的话，它会自己跟踪一个按钮），在 `WM_NCLBUTTONUP` 里切换最大化。按钮的悬停和按下状态也由 C++ 告诉 QML（`maximizeHovered`、`maximizePressed`）。

### 自绘标题栏上点右键

- **原因**：`WM_NCRBUTTONUP` 交给 Qt 的话，Qt 会把随后的 `WM_CONTEXTMENU` 当成窗口内容里的右键。
- **做法**：在 `HTCAPTION` 上吞掉 `WM_NCRBUTTONUP`。设置窗口自己弹出系统窗口菜单（`TrackPopupMenu` 加 `TPM_RETURNCMD`，并按是否最大化启用或禁用各项）；启动器什么都不弹。

### 自绘边框的窗口最大化后超出屏幕

- **现象**：设置窗口用 `WS_OVERLAPPEDWINDOW`，`WM_NCCALCSIZE` 返回 0（整个窗口都是客户区）。最大化后四边各有一圈在屏幕外（150% 缩放下是 11 像素），贴边的内容（如关闭按钮）会被裁掉。
- **原因**：最大化时，Windows 让窗口超出显示器的宽度正好是边框宽度；Qt 又把这一圈当成边框（QTBUG-113736）。改 `WM_GETMINMAXINFO` 里的最大化位置和大小没用，Windows 不理会。
- **做法**：最大化时在 `WM_NCCALCSIZE` 里把客户区和显示器工作区取交集（`WindowFrame.cpp`）。测试时检查客户区是否正好等于工作区。

### 标题栏图标是默认的程序图标

- **原因**：`QCoreApplication::applicationFilePath()` 返回 `F:/…` 这种正斜杠路径，Shell 不认，就退回通用的程序图标。
- **做法**：传给 Shell 的路径一律先 `QDir::toNativeSeparators()`。

### 托盘菜单第一次打开慢

- **现象**：托盘右键菜单第一次打开要 350–470 ms，之后 18–20 ms。几乎全花在 `TrackPopupMenuEx` 里（日志里的 “… ms for Windows to show it”）。
- **怎么量**：
  - 用真实点击：提权脚本点开任务栏溢出区，右键 Win顺 图标，计时到 `#32768` 窗口可见。每轮都先重启 Win顺，重启后等 15 秒再点。
  - 以前向消息窗口投递托盘回调（`WM_APP + 1`）的模拟没量到这个问题，但当时的计时不可靠（没有前台）。这次只用真实点击复现；投递消息同样要经过 `TrackPopupMenuEx`，计时做对了应该也能测出来。
  - 用 ETW 抓第一次打开（`xperf` PROFILE + CSWITCH，带调用栈）。只看 CPU 采样，不要把上下文切换事件也算进去：切换事件会把“等待字体驱动”放大成头号耗时，看错方向。
  - 用 `Get-Process -Module`（要提权）看进程里有没有第三方 DLL。
- **原因**：
  - **第三方菜单钩子（约 310 ms，大头）**：Nilesoft Shell 把 `shell.dll` 注册成图标叠加处理程序（`ShellIconOverlayIdentifiers` 里的 `@nilesoft.shell`）。Win顺 取第一个 Shell 图标时（`FileIconProvider` 里的 `SHGetFileInfoW`，启动约 1.5 秒后由剪贴板窗口的预热触发），系统会把它加载进来。它挂钩了本进程的 `TrackPopupMenuEx`，第一次被调用时要读取自己的配置（大量枚举注册表），之后就直接放行。
  - **菜单字体（约 30 ms）**：第一次在内核里测量菜单字体时，要向用户态字体驱动（fontdrvhost）逐个查询字形宽度。用户态里自己建同样的字体去量，碰不到内核这一份缓存。
  - **输入法与第一次前台**：线程第一次获得焦点时要加载输入法，进程第一次成为前台也要 11–14 ms。
- **做法**：
  - `shell::prepareMenuHooks()`：在 `FileIconProvider` 取到图标之后调用，每个进程只执行一次。它在一个单独的线程上，用一个不带所有者窗口的菜单调用 `TrackPopupMenuEx`。Windows 会拒绝这次调用（`ERROR_INVALID_WINDOW_HANDLE`），但钩子已经先跑完了初始化。整个过程什么都不显示，没有菜单循环，也不抢鼠标。
  - 启动预热里激活 `ITfThreadMgr`（`win::prepareTextInput`），让输入法提前加载。这个对象永不释放，因为退出时 COM 已经卸载。
  - “开机自动启动”的勾选状态用缓存，不在弹菜单时同步查询任务计划程序（新进程里第一次查要 23–33 ms）。
  - 结果：第一次打开 61–65 ms，之后 18–21 ms；空闲 2 分钟后仍是 18 ms。剩下的差距是字体和第一次前台，属于 Windows 自身的开销。
- **试过、没用（或不该用）的办法**：
  - 预加载菜单主题，或者用菜单字体先量一遍文字（`GetThemeTextExtent`）：只要 5 ms，但量的不是内核里菜单用的那份字体，没效果。
  - `LoadLibrary` 预加载 `TextShaping`、`CoreUIComponents`：没效果。
  - 启动时偷偷弹一个真菜单，在 `EVENT_SYSTEM_MENUPOPUPSTART` 时 `EndMenu`：那时 `shell.dll` 还没加载，所以没用；放到图标加载之后再弹，能再省约 30 ms。但菜单会被系统挪回屏幕内（停在左上角 0,0），还要跑菜单循环，可能吞掉用户的点击。不用。
  - 菜单出现后约 170 ms 的淡入，是 Windows 自己的菜单淡入设置，每次都有，与预热无关。以前把它当成预热的副作用，是看错了。
- **日志**：从点击到菜单出现超过 100 ms 时，`WinShun.log` 会写一行分段耗时：等主线程处理、构建菜单、Windows 显示，各用了多久（`App::showTrayMenu`）。“点击时刻”取自 `GetLastInputInfo`，等待期间鼠标一动就会刷新，等待时间会算少。
- **再出现时**：用提权的 `Get-Process -Module` 看是不是又有别的第三方 DLL 钩了菜单；如果有，看 `prepareMenuHooks` 是不是在它加载之前就已经跑过了。

### 双击 Ctrl 和 Ctrl+点击多选

- **现象**：在同一行上快速 Ctrl+点击两次（选上又取消），会被当成“双击 Ctrl”，搜索框被关掉。
- **原因**：`DoubleTapDetector` 原来只靠“两次敲击之间鼠标移动超过 12 像素”来排除 Ctrl+点击。Raw Input 只收键盘，鼠标按键根本看不到，在同一个位置点两次就过了这一关。
- **做法**：`KeyListener` 同时用 Raw Input 收鼠标（`RIDEV_INPUTSINK`）。Ctrl 按住期间，或者一次敲击之后，只要有鼠标按键按下，就不算敲击（`DoubleTapDetector::mouseUsed`）。鼠标移动的消息只看一下按键标志就返回。Raw Input 不是钩子，不会拖慢鼠标。

### 双击 Ctrl 的其他漏触发和误触发

2026-10-09 审查代码时发现（读代码推演，加查资料核实），不是用户报告的：

- **Ctrl 的抬起收不到，之后第一次双击失灵**：按 Ctrl+Alt+Del 时，抬起发生在安全桌面上，Raw Input 收不到；别的程序的低级键盘钩子吞掉的键也一样。检测器以为 Ctrl 一直按着，把下一次按下当成自动重复，要敲第三下才弹出。做法：同一个键按着时又来一个按下，如果距上一个按下（包括重复）超过 1.5 秒，就算新的一次按下。键盘自动重复前最多等约 1 秒，两次重复之间也短于这个时间（`SPI_GETKEYBOARDDELAY` / `SPEED`）。
- **Ctrl+滚轮和触摸板捏合**：精密触摸板的双指捏合，Windows 会转成 Ctrl+滚轮（微软称这是设计如此）。原来只有鼠标按键才让这次敲击作废，快速缩放两下就会弹出搜索框。现在滚轮（`RI_MOUSE_WHEEL`、`RI_MOUSE_HWHEEL`）也作废。
- **拖动时点两下 Ctrl**：在资源管理器里拖文件时按 Ctrl 会在移动和复制之间切换，按住的鼠标键不会再有新的“按下”。Ctrl 按下时用 `GetAsyncKeyState` 看五个鼠标键有没有按着。
- **AltGr 不用特殊处理**：AltGr 发出的假 LCtrl 后面紧跟着右 Alt 的按下，已经算“中间按了别的键”。
- **两次敲击的间隔跟随系统的双击速度**（`GetDoubleClickTime()`，限制在 400–900 ms）：PowerToys 的“查找鼠标”也是这样做的，它同样用 Raw Input 检测双击 Ctrl。
- **不要丢弃 `hDevice` 为 NULL 的按键**：这样能排除模拟出来的 Ctrl，但也会排除 PowerToys 键盘管理器、AutoHotkey 映射出来的 Ctrl（比如把 CapsLock 改成 Ctrl）、屏幕键盘，以及测试脚本 `SendInput` 发的键。
- **会冲突的程序**：PowerToys 的“查找鼠标”默认就是双击左 Ctrl，Listary 默认也是双击 Ctrl。两边都会响应。2026-10-14 有用户反馈（把查找鼠标设成了双击右 Ctrl），加了“用哪个 Ctrl”：左右都行 / 左 Ctrl / 右 Ctrl（`Launcher/DoubleCtrlKey`）。不自动让开：用惯左 Ctrl 的人会以为双击 Ctrl 坏了。设置页读 PowerToys 的配置（`%LOCALAPPDATA%\Microsoft\PowerToys\settings.json` 里 `enabled.FindMyMouse`，`FindMyMouse\settings.json` 里 `activation_method`：0 双击左 Ctrl，1 双击右 Ctrl，见 PowerToys 的 `FindMyMouse.h`），撞上了就在这一行提示，并给一个“改用另一边”的按钮。

### 按住 Alt 连按两下别的程序的快捷键，弹出了搜索框

- **现象**：2026-10-14 用户反馈：他的剪贴板工具用 Alt+\` 粘贴，按住 Alt 连按两下 \`，Win顺 的搜索框出来了。他没设过这个快捷键。
- **原因**：快捷键工具用低级键盘钩子吞掉 \`，Raw Input 看不到它；Alt 按着时它还会自己补发 Ctrl 的按下和抬起，免得 Alt 一松开就激活窗口的菜单栏（AutoHotkey 的 `#MenuMaskKey`，默认左 Ctrl `vk11sc01D`；它的 `Send` 在 Alt 按着时要先放开 Alt，也会这样补发）。连按两次快捷键补发两次，两次靠得近，就成了“双击 Ctrl”。用户没说是哪个工具，这是按现象推出来的最常见的一类；`KeyListener` 现在会在“有一次是程序发出来的（`hDevice` 为 NULL）双击 Ctrl”时往日志写一行最近的按键（只记修饰键和 Ctrl 的名字，别的键只记“key”，不记打了什么），下次有人报告可以直接看。
- **做法**：Ctrl 按下时 Shift、Alt、Win 有一个按着，这次就不算敲击（`DoubleTapDetector`）。修饰键是否按着，按事件流本身的先后记，不能在处理 `WM_INPUT` 时问 `GetAsyncKeyState`：消息是事后才处理的，工具一口气发完“Ctrl 按下、Alt 抬起、Ctrl 抬起”，轮到处理 Ctrl 时 Alt 可能已经是抬起状态。只在修饰键安静超过 1.5 秒时才问 `GetAsyncKeyState`，用来清掉收不到抬起的情况（Win+L、Ctrl+Alt+Del 的抬起去了安全桌面）。同时两次敲击必须是同一个 Ctrl，左右各按一下不算。

### 双击 Shift：大写字母、输入法、JetBrains

- **做法**（2026-10-23，论坛有人要“双击 Shift 呼出搜索”，沿用双击 Alt 的做法，`DoubleTapKey=shift`）：
  - 打大写字母是 Shift 按下、字母按下、字母抬起、Shift 抬起：中间有别的键，`DoubleTapDetector` 不算敲击，`tst_core` 的 `doubleTapShift` 里有“Hello World”的例子。Ctrl、Alt、Win 按着时的 Shift（Ctrl+Shift+Esc、Alt+Shift 换输入法、Win+Shift+S）也不算；Shift+点击、Shift+滚轮由鼠标监听排除。
  - 中文输入法常把单独一下 Shift 当作中/英切换。双击会切换两次，回到原来的状态，不去屏蔽按键；设置里的说明写了这一点。
  - JetBrains 的 IDE 自己用双击 Shift（Search Everywhere）。想两边都用的人把它们加到“在这些程序里不响应双击”里，不用额外代码。
  - 剪贴板浮在别的程序上时，`KeyRouter` 对单独按的 Shift 也补发 `kOwnMask` 的 E8 再放开 Shift，`KeyListener` 跳过它们，按代码读应该照常双击（剪贴板窗口开着时没实测，怕动用户的 Windows 剪贴板历史）；单击 Shift 的拼音提示会闪两次，无害。
  - 旧版服务的 UIAccess 监听进程只报 Ctrl 和 Alt：`kReady` 带 `kReportsShift` 才把 Shift 交给它，否则 Shift 用本进程的 Raw Input（管理员窗口在前台时收不到，升级服务后就好）。

### 双击 Alt：别的程序自己也会连按两下 Alt

- **现象**：2026-10-21 做双击 Alt（GitHub #53）时查资料推出来的，没实测：用 AutoHotkey 脚本切换窗口，Win顺 的搜索框会自己弹出来。
- **原因**：程序想把窗口拿到前台，常用的办法是先模拟按一下 Alt（见下一节 `SetForegroundWindow` 的限制）。AutoHotkey 的 `WinActivate` 第一次没成功时会发 `{Alt 2}`，正好是连按两下 Alt，而且够快。
- **做法**：
  - 程序一口气发完的一次敲击（`hDevice` 为 NULL，按下到抬起不到 20 ms：按键时间约 16 ms 跳一格，紧挨着的两个事件也可能差一格）不算 Alt 的敲击（`DoubleTapDetector`）。手指按一下至少几十毫秒；用 AutoHotkey、PowerToys 映射成 Alt 的键跟着手指按下抬起，照样算。Ctrl 不加这一条，和以前一样。
  - 剪贴板浮在别的程序上时，`KeyRouter` 吞掉单独按的 Alt 的抬起，补发 E8 掩护键再放开 Alt，免得菜单栏亮起来。这两个 E8 带 `kOwnMask`（`OwnInput.h`），`KeyListener` 跳过它们，剪贴板开着也能双击 Alt；Alt+1 这类被拿走的键换成的 E8 带 `kOwnInput`，仍算“中间按了别的键”。这要靠 Raw Input 的 `ExtraInformation` 带上 `dwExtraInfo`，没实测；不带的话只是剪贴板开着时双击 Alt 不灵，不会误触发。
  - 旧版服务的 UIAccess 监听进程只报 Ctrl。它的 `kReady` 带 `kReportsAlt` 时才把 Alt 交给它，否则 Alt 用本进程的 Raw Input（管理员窗口在前台时收不到，升级服务后就好）。
  - 第一下 Alt 会让前台程序的菜单栏或 Office 的按键提示亮一下，第二下又让它灭掉，不去屏蔽：Raw Input 是事后通知，拦不住；用钩子吞 Alt 会弄坏别的程序的 Alt 快捷键。
- **查“双击 Alt 没反应”**：日志里有 `Double Alt: the service's UIAccess listener is from before double Alt` 就是服务太旧。启动 Win顺 前设环境变量 `WINSHUN_KEYLOG=1`，日志逐个记下 Raw Input 收到的按键（修饰键带 vk、scan、flags，别的键只记 key）、当时听哪个键、服务在不在报、两个检测器的状态和最后是否交出。服务是旧的、前台又是管理员窗口时，一行按键都没有；提权测试脚本自己建的窗口也是管理员窗口。

### 后台进程抢不到前台

- **原因**：`SetForegroundWindow` 只在几种情况下允许，其中之一是“调用的进程收到了最后一次输入”。按下 `RegisterHotKey` 注册的热键算收到，Raw Input 的 `WM_INPUT` 不算（Raymond Chen 2009-02-26；没有资料说 `WM_INPUT` 或低级钩子也算）。所以双击 Ctrl 之后靠 `AttachThreadInput` 和前台线程共享输入状态，绕过这个限制。
- **做法**（`win::bringToFront`）：
  - 前台窗口没有响应（`IsHungAppWindow`）时不共享，否则对方卡住的输入会把我们也卡住。
  - 激活后用 `GetForegroundWindow()` 检查。没成功就用 `SendInput` 发一个什么都不做的鼠标输入（不移动、不按键），让 Win顺 成为“最后一次输入”的来源，再试一次。PowerToys 在 2020 年也这样做过（PR #1282）。
  - 还不行就在日志里写一行 `Could not bring the window to the front over <类名> (<程序>)`。看到这行，就照它查是哪种前台窗口。

### 开始菜单开着时双击 Ctrl：窗口出来了，焦点还在开始菜单

- **现象**：2026-10-09 实测，按 Win 打开开始菜单后双击 Ctrl，搜索框显示出来了，前台却还是开始菜单（`SearchHost.exe` 的 `Windows.UI.Core.CoreWindow`），3 次都这样。打的字会进 Windows 搜索。日志里有上面那行 `Could not bring … (SearchHost.exe)`。之后搜索框一直开着、没有焦点（`activeChanged` 只在激活状态变化时触发，它从没被激活过，也就不会自己收起）。
- **原因**：开始菜单、搜索、快速设置、通知中心开着时会锁住前台。测试程序在开始菜单开着时分别试了直接 `SetForegroundWindow`、先发一个空输入、先按一下 Alt，全都返回 FALSE；只有按 Esc 关掉开始菜单后，前台才回到原来的窗口。
- **做法**：前台窗口属于 `SearchHost.exe`、`StartMenuExperienceHost.exe`、`ShellExperienceHost.exe`、`ShellHost.exe` 时，先 `SendInput` 一个 Esc，等前台换掉（最多 500 ms），再照常激活（`closeShellFlyout`）。实测：开始菜单 3 次、快速设置（`ShellHost.exe` 的 `ControlCenterWindow`）2 次、通知中心（`ShellExperienceHost.exe`）2 次全部拿到前台，用时 191–204 ms，主要是等面板关掉。
- **同一轮实测的其他结果**：从普通窗口双击 Ctrl 11 次、从资源管理器 3 次，全部在 2–7 ms 内拿到前台，双击收起 2–6 ms。间隔 450 ms 能触发，700 ms 不触发。Ctrl+滚轮两次、按住左键双击 Ctrl、同一处 Ctrl+点击两次，都没有误触发。

### 管理员或 UIAccess 窗口在前台时，双击 Ctrl 没反应

- **现象**：GitHub #31（2026-10-16，0.8.0，微信 4.1.15.55）：微信在前台时双击 Ctrl 不弹出搜索框，0.5.0 没这个问题。
- **原因**：0.8.0 起（#21）界面以普通权限运行。前台窗口是高完整性（以管理员身份运行）时，UIPI 不把按键交给普通权限进程的 Raw Input（`RIDEV_INPUTSINK`），`WM_INPUT` 一条都收不到，日志里也就什么都没有（连 `Double Ctrl ignored` 都没有）。0.5.0 整个以管理员身份运行，所以不受影响。2026-10-16 实测（`SendInput` 发左 Ctrl 两下，间隔 120 ms，每种 3 次）：普通窗口在前台、普通进程发 3/3 弹出；普通窗口在前台、管理员进程发 3/3 弹出；管理员窗口在前台、管理员进程发 0/3，日志无记录。所以挡住的是“前台是管理员窗口”，不是“发键的进程是管理员”。没装微信 4.x，没确认报告者的微信是否以管理员身份运行。
- **uiAccess 令牌能收到，不用签名**（2026-10-16 探针）：SYSTEM 进程（临时计划任务）`WTSQueryUserToken` 取控制台会话的用户令牌，`DuplicateTokenEx` 成主令牌，`SetTokenInformation(TokenUIAccess, 1)`（要 SeTcbPrivilege，LocalSystem 本来就有），`CreateProcessAsUserW` 到 `winsta0\default`。得到的进程是 uiAccess=1、未提升、完整性 0x2000。exe 没签名，放在用户可写的临时目录，照样生效：签名和“安全位置”只在按清单 `uiAccess="true"` 启动时由 AppInfo 检查。同一轮（`SendInput` 双击 Ctrl，管理员窗口在前台 3 轮、普通窗口在前台 3 轮）：这个进程的 Raw Input（`RIDEV_INPUTSINK`）和 `WH_KEYBOARD_LL` 各收到 24/24 个 Ctrl 事件；同时运行的普通进程（没有 uiAccess）只收到普通窗口那 12 个，管理员窗口前台时 0 个。只测了模拟按键，没测真键盘。参照：AutoHotkey 安装时现场生成自签名证书签 UIA.exe（会往本机根证书里加一张），Mouse Without Borders 用 System 服务，Listary 不处理。
- **要留意的安全差别**（2026-10-16 查本机 uiAccess 进程）：系统自带按清单 `uiAccess="true"` 启动的程序（`osk.exe`、`ctfmon.exe`、`TabTip.exe`）完整性都是 0x3000（高），AppInfo 启动时把它抬高了，这样同一用户的中完整性进程注入不进去。而探针里 `CreateProcessAsUser` 得到的进程停在 0x2000（普通），没被抬高，同一用户别的普通进程有可能往它里面注入代码、再借它操作管理员窗口。正式实现给独立监听子进程设置 0x3000，界面仍为普通权限。微信输入法 wetype 的进程也有 uiAccess=1、完整性 0x2000 的组合，因此排查输入问题不能只看完整性或“管理员”标志；这不等于已确认 #31 报告者的微信窗口使用了什么令牌。


- **实现**（`service/InputListener.cpp`、`platform/ServiceKeyListener.cpp`）：界面打开服务管道的新能力 `OpenKeyListener`，服务按管道报告的客户端 PID / 会话和用户 SID 启动自身的 `--listen-keys` 子进程。这个模式不启动卷服务，只运行同一份 `KeyListener`。没有额外 exe、Qt DLL、签名或根证书；只能启动服务自己的固定路径，不能指定程序、命令或其他会话。子进程使用该会话用户令牌，UIAccess=1、完整性高，禁用令牌特权；不是 SYSTEM，也不使用 linked administrator token。服务额外保留 Tcb / AssignPrimaryToken / IncreaseQuota，平时禁用，只在启动线程的私有令牌上临时启用。安装 / 升级必须重启服务，让 SCM 应用新的特权列表。
- **会话 0 的坑**：服务不能拿用户桌面的 HWND 调 `GetWindowThreadProcessId` / `GetClassName` 做验证，会失败。窗口类名和所属 PID 的检查必须由已经进入用户会话的子进程执行，服务只核对管道身份与会话。首次真正 SYSTEM 集成测试发现并修正了这一点。
- **通知和退出**：子进程只发双击 Ctrl 的时间、左右键和是否有模拟键，不发送按键或文本；触发时 `AllowSetForegroundWindow` 授权界面取焦点。每秒发送就绪心跳，管道同样每秒保活。每客户端最多一个子进程；Job 禁止它再生子进程，并在连接关闭、服务退出 / 崩溃时清理。GUI 退出时子进程也自行结束。主界面沿用游戏 / 全屏 / 排除策略；有可用子进程时本地监听只保留鼠标功能与回退，按同一输入时间戳去重。快捷键录制移除本地 Raw Input 注册时，远程双击也暂停。无服务、旧服务、断线或心跳超时都退回原来的普通监听，后台重试；设置页说明受限窗口需要服务。
- **自动化验证**：`tst_keylistener` 的通知去重、左右 Ctrl、旧 PID / 过期消息、录快捷键暂停、心跳失联、无服务取消、普通权限服务拒绝和无 UIAccess 直接启动拒绝均通过；原有 `doubleTap` / `doubleTapKeys` / `doubleClick` / `gameGuard` 通过。隔离 SYSTEM 计划任务启动当前编译的真实服务，`serviceWorker` 验证子进程 UIAccess=1、IL=0x3000、TokenIsElevated=0、同一用户会话，断开后退出；`elevatedForegroundInput` 在管理员测试窗口确实位于前台时发送三组双击 Ctrl，3/3 次只触发一次，普通权限接收窗口 3/3 取得焦点。这一轮使用生产监听 / 启动代码，接收窗口是测试程序。
- **完整程序实测**（2026-10-16，用户同意替换并重启后）：备份后更新 `dist/WinShun` 的界面和 `Program Files` 中的服务，通过 `--install` 更新服务特权并重启，诊断状态 `doubleCtrlService=true`。微信 3.9.11.19 前台模拟双击 Ctrl 三轮（左 Ctrl 两轮、右 Ctrl 一轮），每轮 `visible=true`、`active=true`，日志确认来自 UIAccess 监听。随后用户用真实键盘在微信前台试三次，反馈三次均正常弹出并直接输入搜索；此时核实微信为 IL=0x3000、已提升。实际 Win顺 界面为 UIAccess=0、IL=0x2000、未提升，监听子进程为 UIAccess=1、IL=0x3000、未提升；微信输入法 `wetype_renderer.exe` 为 UIAccess=1、IL=0x2000，`wetype_server.exe` 为 UIAccess=0、IL=0x2000。本机没有微信 4.1.15.55，原报告版本仍待验证。备份和机器实测记录在 `build/release/issue31-live-20261016-183244`。

### 游戏里连按两下 Ctrl（蹲下）弹出搜索框

- **现象**：很多游戏用 Ctrl 蹲下，玩家常连按两下。搜索框弹出来会抢走键盘；独占全屏的游戏还可能被最小化。
- **只看 `QUNS_RUNNING_D3D_FULL_SCREEN` 不够**：PowerToys“查找鼠标”的“游戏模式下不激活”只认这一个值（`src/common/utils/game_mode.h`），而 Windows 10 起的全屏优化让大多数“全屏”游戏实际跑在无边框窗口里，返回的是 `QUNS_BUSY`。`QUNS_BUSY` 在全屏视频、F11 网页、打开 Windows“演示设置”时也会出现，不能单独当游戏信号。
- **GameConfigStore 不可靠**：`HKCU\System\GameConfigStore\Children` 在这台机器上有 68 条，只有 3 条带程序路径（The Finals、Farlight 84，还有 WeGame 的后台进程 `tgp_daemon.exe`，它不是游戏），其余是微软预置的目录名。`Windows.Gaming.Preview.GamesEnumeration` 普通桌面程序用不了。
- **做法**（`GameGuard.h` 判断，`platform/Foreground.cpp` 取事实，只在双击成立的那一刻查一次）：
  - 前台程序在名单里（设置里填程序文件名），不响应。
  - “任何全屏都不响应”打开时（默认关），窗口铺满所在显示器就不响应。最大化的窗口不算（任务栏自动隐藏时它也铺满）；桌面和 Win顺 自己的窗口也不算。
  - “玩游戏时不响应”打开时（默认开）：独占全屏，或者光标被藏起来，并且被锁住（`GetClipCursor` 比整个桌面小）或窗口铺满显示器，就不响应。这是用鼠标转视角的状态，不需要知道它是不是游戏。藏光标的两种写法（`ShowCursor(FALSE)`、`SetCursor(NULL)`）都认；`CURSOR_SUPPRESSED`（触屏、笔时 Windows 藏的）不算。代价：全屏视频几秒不动鼠标、播放器藏了光标时也不响应，动一下鼠标就好。
  - 不响应时什么都不显示；日志里每个程序、每种原因记一行 `Double Ctrl ignored over <程序> as <原因>`。另设的组合键不受影响，游戏里想打开就用它。
- **实测**（2026-10-09，用测试窗口模拟）：窗口化加藏光标加锁光标、全屏加 `ShowCursor` 藏光标、全屏加 `SetCursor(NULL)`、“任何全屏”打开时的全屏窗口、名单里的程序，都不响应；窗口化只藏光标（打字时）、窗口化只锁光标（即时战略游戏）、全屏但光标可见（F11 网页）、名单还原后，都照常弹出。
- **再测一次，用 D3D11 假游戏**（同日；一个真用 D3D11 每帧渲染的程序，鼠标转视角时每帧把光标拉回中心）：DXGI `SetFullscreenState(TRUE)` 的独占全屏，翻转模型（`FLIP_DISCARD`）和老的 `DISCARD` 都返回 `QUNS_RUNNING_D3D_FULL_SCREEN`，光标可见时也不响应；无边框全屏是 `QUNS_BUSY`，光标藏起来就不响应，SDL 那种 `SetCursor(NULL)` 加中心 1×1 锁定、窗口化或无边框，也都不响应。无边框时窗口就是整个桌面（单显示器），`GetClipCursor` 看不出锁定，靠“铺满显示器”判断。
- **已知漏网**：光标换成一张全透明的图片（`CURSOR_SHOWING` 仍在，句柄不为空），会被当成光标可见，照常弹出。SDL2 只在远程桌面里这样藏光标（`WIN_ShowCursor` 的 `SDL_blank_cursor`），本机的游戏引擎一般用 `ShowCursor`/`SetCursor(NULL)`。还没在真实游戏里测。

## 图片清晰度（QML）

### `sourceSize` 的单位因图片来源而不同

- `image://` 图片提供器、SVG、PDF：`sourceSize` 按逻辑像素写，Qt 会自动乘以设备像素比。
- PNG、JPG 文件（qrc 或本地）：`sourceSize` 就是加载的像素尺寸，Qt 不乘，要自己乘设备像素比。
- 弄反了就会多缩放一次：150% 下文件图标按 72 像素请求、再画进 48 像素，出现锯齿；头像按 110 像素加载、再放大到 165 像素，发虚。
- 要像素精确：设备像素取整数，`width = 像素数 / dpr`，静止时 `smooth: false`（`ResultRow.qml`、`AuthorAvatar.qml`）。

### 动画结束后图片仍然发虚

- **原因**：衰减型动画（如 `sin(…)·exp(-k·t)`）永远不会精确回到 0，图片一直带着极小的旋转，一直被重新采样；`smooth` 跟着动画计时器走，而计时器在动作肉眼看不出之后还要跑一阵。
- **做法**：按“肉眼能看出的动作”判断（例如摆动幅度小于 0.15° 就算停了）。停了就强制回到原样（缩放 1、角度 0），并设 `smooth: false`（`AuthorAvatar.qml`）。

### 判断图片是否被缩放过，先量像素

把截图按元素的设备像素位置裁下来，和原图合成到同样的背景上逐像素比较。对不上，就是渲染流程（请求尺寸、设备像素比、缩放、平滑）的问题，不是图片素材的问题。150% 下周期为 3 像素的误差图案，说明有一次 1.5 倍的最近邻缩放。把一处的修法推广到别处之前，先确认那里是同一个原因。

### 一张慢的缩略图挡住整个列表的图标

- **现象**：结果行的缩略图如果和图标走同一个 `QQuickImageProvider`（`image://fileicon/thumb/…`），视频、大图、第一次加载解码器的缩略图要几十到几百毫秒，这段时间里后面所有行的类型图标都出不来，滚动时一片空格子。
- **原因**：Qt 6.12 每个 QML 引擎只有一条读图线程（`qquickpixmapcache.cpp` 的 `QQuickPixmapReader`），`Image` 类型的提供器在它上面同步调用 `requestImage`，一个接一个。`ForceAsynchronousImageLoading` 只是不在界面线程上调，不会多开线程。
- **做法**：缩略图用单独的 `QQuickAsyncImageProvider`（`ThumbnailProvider`，`image://thumbnail/`）。`requestImageResponse` 也在那条读图线程上被调，但只查一下内存缓存，其余交给自己的线程池（4 条），马上返回；图标不再排在缩略图后面。

### 异步图片提供器：没有图别报错，取消了也要 `finished`

- **没有图**：`errorString()` 非空时 Qt 把 Image 置为 Error，并给每一行打一条 `QML Image: Failed to get image from provider` 警告（剪贴板预览里非图片的文件就会这样）。没有缩略图时 `textureFactory()` 返回 `nullptr`、`errorString()` 留空，Image 是 Ready、尺寸为 0，QML 里用 `status === Image.Ready && implicitWidth > 0` 判断有没有图。
- **取消**：行滚走或复用时 Qt 调 `cancel()`，但不删响应对象；它只在收到 `finished` 后 `deleteLater`。所以取消了也要发一次 `finished`（不带图），否则每次取消漏一个对象。`finished` 可以在 `requestImageResponse` 返回之前就发（缓存命中时），Qt 返回后会查 `finished` 标志补上。
- **Qt 自己的图片缓存只留 2 MB 没人用的图**（`cache_limit`，写死，30 秒淘汰四分之一）：48 像素的缩略图约 220 张。滚回去、再打一遍同样的字要立即出来，提供器自己按字节留一份（`QCache`，48 MB），没有缩略图的也记下。
- **sourceSize**：image:// 提供器按逻辑像素写，Qt 乘设备像素比再请求；返回的图带着这个比，`implicitWidth` 就是“像素数 / 设备像素比”，`width: implicitWidth` 正好一个像素对一个像素（150% 实测与系统给的位图逐像素相同）。

### 列表滚动时整行在抖

- **现象**：结果列表滚动时（尤其是快停下的那段），行里的文字和图标轮流上下跳 1 像素，像在抖。
- **怎么量**：提权脚本用滚轮滚动列表，或按住拖动后甩出去，连续截屏（约 10 ms 一帧），逐帧找每一行图标上边缘和标题上边缘的位置，统计两者之差。
  - 选中行和悬停行的浅色背景会让文字顶上的抗锯齿多算一行，那是测量误差，要按行背景分开统计。
  - 结果：改之前约 20% 的帧差 1 像素。0.2.3 的做法下滚轮不抖，但拖动甩出时仍有约 18% 的帧错开。现在的做法下，静止、滚轮、拖动甩出全部一致。
- **原因**（150% 缩放，两层）：
  - **列表停在小数位置**：Qt 的 Flickable 滚轮和惯性滚动会停在任意小数位置。文字、按最近邻采样的图标、矩形各按自己的规则对齐到像素，于是轮流错开。Qt 的 `pixelAligned` 只对齐到逻辑像素，150% 下一半的位置仍在半个设备像素上。
  - **文字顶点在 .5 处随机取整**：列表对齐到设备像素后，文字还在跳。原生文字的着色器（`textmask.vert`）把每个字形四边形的顶点各自按 `floor(x × dpr + 0.5)` 取整，而且用单精度浮点。居中排版常让文字落在正好 .5 个设备像素上，这时往哪边取整由浮点误差决定，而误差随滚动位置变化，结果字形整体错开或被拉高 1 像素（标题高度在 20 和 21 之间变）。
- **做法**：
  - **只修正画面，不改滚动位置**。每一行加一个 `Translate`，把它在窗口里的设备像素位置补成整数，补的量不到 1 像素（`Main.qml` 的委托）。绘制补偿不写回 `contentY`，拖动与惯性仍用 Qt 原生，每一帧都对齐；搜索列表的离散滚轮终点由 `ListWheelScroll` 预先对齐，见后面的“搜索列表滚到中间停下后，仍会补跳”。浏览器也是这么做的：滚动位置可以是小数，只在画出来时对齐（Firefox 的 `snapped-scrolled-content` 测试，Chromium 合成器）。
    - 列表在窗口里的位置要用 `layout.y + listArea.y + list.y` 这样能通知变化的属性来算，不能用 `mapToItem()`：Column 在稍后的布局阶段才给子项定位，`mapToItem` 的结果不会因此重算，结果刚出来时第二行就落在了半个像素上。
  - **行内会随滚动移动的文字，竖直位置都放到整数设备像素上**：标题与路径所在的 Column、标题行的高度、内容搜索时跟在名字后的文件夹名、右侧的“最近”、徽标里的文字（`ResultRow.qml` 的 `onPixel` / `upToPixel`）。用 `anchors.verticalCenter` 居中的文字很容易落在 .5 上。
  - **设置页（和拾穗计划页）整页滚动，控件太多，不逐个对齐**：只给整页的 Column 加 `Translate`，补到整数设备像素后再往下多偏 0.02 个设备像素（`SettingsWindow.qml` 的 `page`，`GleaningPage.qml` 的 `Snapped`）。页里落在 .5 上的文字、边框、图片都变成 .52，取整方向固定，不再随浮点误差变；0.02 远大于单精度误差（几千像素处约 0.001），又小到看不出来。2026-10-09 实测（150%，剪贴板页，滚轮和小步滚动各上下一次，约 10 ms 一帧，相邻两帧按整数平移对齐后数对不上的像素）：改之前 168 对有变化的帧里 138 对有错开，改之后 96 对全部一致。
  - **同样处理的还有**：剪贴板预览的长文本（`ClipPreview.qml`）、剪贴板分组标签横向滚动（`ClipTabs.qml`，补的是 x）、更新对话框的更新说明（`UpdateDialog.qml`），都是整块补齐再偏 0.02。打开/保存对话框的搜索条列表（`DialogBarWindow.qml`）和结果列表一样每行补齐，标题高度用 `upToPixel`。它的列表在搜索条下面时从 49 逻辑像素开始，150% 下不滚动也在半个像素上。2026-10-09 实测（WinForms 打开对话框，列出 `C:\Windows\System32\`，滚轮和小步滚动各上下一次）：改之前 237 对里 147 对错开（集中在图标那一列），改之后 137 对全部一致。剪贴板预览也实测了（120 行的测试文字，Win顺 退出时直接写进 `clipboard.db`，不经过系统剪贴板，测完同样删掉）：几乎全是文字，同一个 Text 的字形一起移动，旧版只在每组快停下时错一次（两帧同一整数位置，某几行文字下移 1 像素），230 对里 4 对；新版 216 对全部一致。分组标签、更新说明没有实测滚动。
- **测剪贴板功能时，Windows 自带的剪贴板历史可能已经满了（25 条）**：再往剪贴板放测试内容，会把用户最早的一条挤掉，找不回来。`CanIncludeInClipboardHistory = 0` 也会让 Win顺 跳过，没法只瞒着 Windows。要一条测试记录时，趁 Win顺 退出把它写进 `clipboard.db`（文字 kind 0，hash 是 SHA-1(`'t'` + 文字的 UTF-16LE)，时间是毫秒，和 `ClipStore::add` 一样），先备份数据库，测完退出 Win顺 再删。
- **量的时候注意悬停**：光标停在列表上滚动，悬停的底色会换到另一行。按每行平均亮度找两帧之间的平移会被这条亮带带偏（往下滚却算出 -61），整帧都像错开了。改成找“差异大于 48 的像素最少”的平移，悬停底色只差十几个色阶，就不会干扰。
- **弃用的做法**（0.2.3）：自己用 `WheelHandler` 加 `NumberAnimation` 接管滚轮，并在每次 `contentY` 变化时把它对齐到设备像素。问题有两个：触屏和拖动甩出走的是 Flickable 自己的惯性滚动，惯性过程中不能改 `contentY`（会重置它的 timeline），所以仍然会抖；而且滚动手感也不再是原生的。
- **注意**：Windows 上 Qt 的滚轮事件 `pixelDelta` 永远是 0，精密触摸板也一样（`qwindowspointerhandler.cpp` 里传的是 `QPoint()`），不能靠它区分触摸板和鼠标滚轮。
- **其他办法**：`Text.QtRendering` 或 `Text.CurveRendering` 不走 `textmask.vert` 的取整，但会失去 FreeType 的垂直 hinting，中文小字会变软。`QT_SCALE_FACTOR_ROUNDING_POLICY=Round` 会把 150% 变成 100% 或 200%。都没采用。

## 窗口材质（Mica）

设置窗口和搜索面板在 Windows 11 22H2 及以上、用“显卡加速”绘制时，背后是 Mica（`DWMWA_SYSTEMBACKDROP_TYPE` = `DWMSBT_MAINWINDOW`），窗口自己的背景色设成透明。“外观”里可选关 / 开，默认按内存定：超过 16 GB 开，否则关（`Settings::lowMemory`；旧版存的 auto 读取时也这样换掉）；Windows 10 没有这个效果，那一行也不显示。

- **为什么是 Mica，不是亚克力**：按微软的规范，长时间开着的窗口用 Mica，亚克力留给菜单、弹出层这类临时界面。亚克力会透出后面的窗口，内容多时背景很花，省电模式下还会被系统关掉；Mica 只取桌面壁纸的颜色，不透出后面的窗口（`DWM_SYSTEMBACKDROP_TYPE` 文档，Windows Terminal 也用 `DWMSBT_MAINWINDOW`）。
- **配色**：有 Mica 时，卡片、选中行、悬停、分隔线、标签底色改用半透明色（取 WinUI 在 Mica 上的值）；没有 Mica 时保持原来的不透明色，一个像素都不变（`Theme.qml` 里的 `backdrop ? … : …`）。

### Mica 画成一块平的灰色

- **现象**：背景是均匀的灰色（深色约 #545454，浅色约 #D3D3D3），不带壁纸色调；系统的“透明效果”是开着的。
- **原因**：窗口激活时 DWM 靠默认处理的 `WM_NCACTIVATE` 才知道边框是激活的。Qt 对无边框窗口不把这条消息交给 `DefWindowProc`，DWM 一直以为窗口没激活，就画未激活时的回退色。
- **做法**：子类过程里先交给 Qt，再调 `DefWindowProcW(hwnd, WM_NCACTIVATE, wParam, -1)`（`WindowFrame.cpp`）。`-1` 表示不重画非客户区。
- 窗口失焦、系统“透明效果”关闭、高对比度时，DWM 也会画这种灰色，所以这时界面要铺回不透明底色（`SystemTheme.materials`、`window.active`）。
- 不要自己连发一对 `WM_NCACTIVATE` 去“刷新”材质：偶尔会让 DWM 卡在未激活状态，露出这块灰色。

### 切换深浅色后，Mica 还是旧主题的颜色

- **现象**：浅色切到深色后，界面变深了，背后的 Mica 还是浅色，白字看不清；深色切浅色则正常。
- **原因**：Qt 在主题切换后约 5 ms，会给所有窗口重设 `DWMWA_USE_IMMERSIVE_DARK_MODE`，而对无边框窗口一律设成浅色（`qwindowswindow.cpp` 的 `shouldApplyDarkFrame`），把我们刚设的深色覆盖掉。这个值可以用 `DwmGetWindowAttribute` 读回来，提权的测试程序跨进程也能读。
  - DWM 在这个值变化时会立即给 Mica 换色，不需要重新激活窗口。之前以为“要等下次激活才换色”，其实是值被 Qt 改回去了。
  - 如果“我们设深色 → Qt 设浅色 → 我们再设深色”挤在同一帧（约 16 ms）里，DWM 有时不换色。
- **做法**：主题切换时先不动这个值，等 Qt 改完；50 ms 后由一个计时器设成正确的值，之后 1 秒内每 10 ms 检查一次，被改了就改回来（`App::applyTheme`）。窗口创建时直接设一次（`win::setDarkFrame`）。
- **测试**：按“深→跟随系统→浅→深→跟随系统→深→浅→跟随系统→深”依次点主题卡片，每步记两次：切完立刻截图、读这个值，切走再切回来后再截图、再读一次。再测一遍间隔 0.3 秒的快速连点。

### 边框扩展到整个窗口，DWM 会画出系统标题栏按钮

- **现象**：`DwmExtendFrameIntoClientArea` 用 `{-1,-1,-1,-1}` 后，右上角多出一套系统的最小化、最大化、关闭按钮，和自绘的叠在一起。
- **做法**：保持 `{0,0,1,0}`。不扩展边框，Mica 照样铺满整个窗口（Windows Terminal 也是这样）。

### 半透明的窗口颜色变成纯白

- **原因**：带 alpha 通道的窗口，Qt 用窗口颜色清屏时没有预乘，DWM 按预乘解释，浅色的半透明色溢出成白色。
- **做法**：窗口颜色只用全透明或不透明；要叠一层半透明色时，用铺满窗口的 `Rectangle` 画。

### 圆角上的边框是锯齿，看着一颗一颗的

- **现象**：窗口四个角的细边框不顺滑，放大看是一级一级的台阶，台阶之间还漏出后面的颜色；直边上没问题。深色窗口在浅色背景上最明显。
- **原因**：DWM 给圆角窗口画的边框（`DWMWA_BORDER_COLOR`）在圆弧上是一条没有抗锯齿的单像素折线，而且比 DWM 自己裁出来的圆角（这个是抗锯齿的）往里偏了一点，两条曲线之间的像素既不是窗口也不是边框，透出了背景。把边框设成 `DWMWA_COLOR_NONE` 后，剩下的圆角边缘是干净的。另外，深色主题下边框色 #404040 落在 VS Code 这类深色程序上几乎看不出来（底色亮度和背景只差 1 级，系统阴影在深色背景上也显不出来），窗口像和后面的程序融在一起。
- **做法**：DWM 只负责圆角和阴影，边框设成 `DWMWA_COLOR_NONE`（`win::styleFramelessWindow`）；每个窗口在 QML 里自己画一圈抗锯齿的细线（`WindowEdge.qml`，圆角半径和 DWM 的一样是 8 逻辑像素，Windows 10 上是直角；最大化时不画）。颜色是中性灰：深色 #6A6A6A，浅色 #B4B4B4（`Theme.windowEdge`）。深色下试过 #404040、#505050、#5C5C5C、#6A6A6A：#404040 在深色背景上几乎看不见，#5C5C5C 用户觉得还不够明显，定为 #6A6A6A；浅色背景上靠阴影本来就分得开。这样画出来的角和系统给普通窗口画的（文件对话框）逐像素比过：半径一样（150% 下 12 像素），线宽 2 像素，两侧都有过渡。
- **为什么不改用系统边框那条路**：微软的说法（“Apply rounded corners in desktop apps”）是，带 `WS_THICKFRAME` 和 `WS_CAPTION`、或留出 1 像素非客户区边框的窗口，系统自动圆角并画好边框和阴影（VS Code、Windows Terminal 就是保留系统边框、只把标题栏区域划给自己）；没有边框的窗口用 `DWMWCP_ROUND` 申请圆角，这是我们的情况（Qt 的 `FramelessWindowHint` 是 `WS_POPUP`）。走系统边框那条路，启动器和剪贴板会多出可拖的缩放边、贴靠布局和打开关闭动画，Qt 算窗口位置的方式也要跟着改；现在自己画的边已经和系统的一样平滑，没有必要。Qt 6.9 起的 `Qt::ExpandedClientAreaHint` 在 Windows 上是给普通窗口自绘标题栏用的（Qt 自己画一条带图标和按钮的标题栏），也不合适。原先对话框旁的搜索框用强调色边框，太扎眼，也改成同一套；任务栏上 Win顺 的框打字时也一样（起初是 2 像素强调色，用户嫌蓝、嫌粗），只有高对比度下用系统的高亮色。
- **怎么查**：边框颜色可以从别的进程设（`DwmSetWindowAttribute(hwnd, DWMWA_BORDER_COLOR, …)`，提权的脚本对 Win顺 的窗口也行），不用重新编译就能把几种颜色和“不要边框”挨个截图对比；角上 24×24 像素放大 12 倍看。

### 日志里一直有 `Swapchain says surface has alpha but the window has no alphaBufferSize set`

- **现象**：开启 Mica 后，启动器每打开一次，`WinShun.log` 里就多一行这个警告（两天里 195 行）。
- **原因**：窗口颜色激活时透明、失焦时不透明（见上面“Mica 画成一块平的灰色”）。`QQuickWindow::setColor()` 在颜色的 alpha 变化时会改窗口的格式：变成不透明就把 `alphaBufferSize` 设成 -1。可交换链是按创建时的格式建的，一直带 alpha；Qt 的 D3D11 后端每次改大小（`QD3D11SwapChain::createOrResize`）都检查两者是否一致，不一致就打这个警告。窗口在失焦时改大小，就写一次日志，而且是在显示窗口的路上同步写磁盘。
- **做法**：`prepareBackdrop` 设好 `alphaBufferSize(8)` 后，再连上 `colorChanged`，格式里的 alpha 被去掉就加回来（`App.cpp`）。这样格式和交换链一直一致；交换链将来要是重建，也还是带 alpha 的。
- **实测**（2026-10-09）：启动器开关 5 次，旧版每次 1 行，新版 0 行。

### 透明窗口上的字有蓝橙彩边（GitHub #12）

- **现象**：开着透明效果，搜索框的字边上一圈蓝、橙色，放大看左蓝右橙，和普通 ClearType（左橙右蓝）正好反过来；关掉透明效果就正常。只在部分电脑上出现，开发机上从来看不到。
- **原因**：Qt 用 FreeType 画字时，只有 `HKLM\SOFTWARE\Microsoft\Avalon.Graphics\DISPLAY1` 下有 `PixelStructure`（跑过 Windows 的“ClearType 文本调谐器”才有）才出子像素字形（`QWindowsScreen::subpixelAntialiasingTypeHint`），控制面板里开着 ClearType 不算。子像素字形的着色器按 R、G、B 各自的覆盖率和目标混合，透明处的目标 alpha 是 0，混出来的颜色和 alpha 对不上，DWM 再叠到 Mica 上就成了反向的彩边。
- **做法**：能用 Mica 的电脑（`SystemTheme::backdropAvailable`）在建窗口前给应用字体加 `QFont::NoSubpixelAntialias`，所有字都用灰度抗锯齿（`App::start`）。Windows 11 自己的 Mica 界面也是灰度。不按“透明效果”开没开来定：字体在文字项创建时复制一次，之后改应用字体传不到已有的项，而透明效果随时能开。任务栏上的框是 Direct2D 画的，早就用灰度了（`TaskbarSearchBox.cpp`）。
- **怎么查**：开发机上用环境变量 `QT_SUBPIXEL_AA_TYPE=RGB` 就能复现。2026-10-11 用一个小程序（FreeType、D3D11、NativeTextRendering、同样的字体）截图比过：子像素字在 Mica 上平均色差 21、最大 91，方向反了；在不透明底上是正常的 ClearType；灰度在两种底上都是 0。

### 软件渲染时没有材质

- 软件渲染器把带 alpha 通道的窗口做成分层窗口，DWM 不在它背后画材质。只在用 D3D11 绘制时启用（`SystemTheme.backdropAvailable`）。
- D3D11 下 Qt 同样会给带 alpha 的窗口加 `WS_EX_LAYERED`，但 Mica 照常显示。

## 系统入口（Windows 设置检索清单）

清单是什么、怎么用，见 [architecture.md](architecture.md) 的“系统入口用 Windows 自己的设置检索清单”。

### 设置页面的 `ms-settings:` 地址不一定是 `PolicyIds` 的第一个

- **现象**：“声音设置”对到了 `ms-settings:apps-volume`（音量合成器）。
- **原因**：`PolicyIds` 列的是这个页面的所有 `ms-settings:` 名字，按字母排，不是“第一个就是页面本身”。
- **做法**：`choosePageUri`：页面 ID 里写着的优先（SettingsPageInstalledApps → `installed-apps`），其次是被其余名字当前缀的（`sound` 之于 `sound-devices`），都没有才取第一个。

### Windows 安全中心的条目只解析得出英文

- **现象**：清单里“病毒和威胁防护”等条目的名称解析失败。
- **原因**：它们写成 `@{Microsoft.SecHealthUI_8wekyb3d8bbwe?ms-resource://…}`，用的是包系列名，`SHLoadIndirectString` 只认完整包名；换成完整包名（`GetPackagesByPackageFamily`）能解析，但在中文系统上也只给英文。
- **做法**：按 HostID 跳过这些条目，在 `places.txt` 里写中文名和 `windowsdefender://` 地址。

### 中文 Windows 上搜“计算器”找不到计算器

- **现象**（用户截图）：搜“计算器”只有一堆“计算器.png”，没有计算器这个应用；记事本、画图、截图工具也一样。
- **原因**：商店 / MSIX 应用的名称按“设置 → 时间和语言 → 语言和区域”里**首选语言列表的第一个**来取，和 Windows 的显示语言无关。这台电脑显示语言是中文，列表却是 `en-US, zh-Hans-CN`，于是开始菜单和 `shell:AppsFolder` 里都叫“Calculator”；中文资源包（`split.language-zh-hans`）其实装着。`C:\Windows\SystemApps` 里的系统应用（设置）照样按显示语言叫“设置”。上一条“Windows 安全中心只解析得出英文”多半也是这个原因。
- **试过、不行的**：`SHLoadIndirectString` 不认 `SetThreadPreferredUILanguages` / `SetProcessPreferredUILanguages`，用 `@{PRI 文件路径?…}` 读中文资源包的 resources.pri 报 0x80070490；WinRT 的 `ResourceManager.Current` 在没有包标识的进程里报 0x80070002。剩下只有自己解析 PRI 格式。
- **做法**：Windows 自带应用按 AppUserModelID 记下中英文两个名字（`src/core/apps.txt` 的 `names`，原先是 `AppCatalog.cpp` 的 `kKnownApps`；中文名逐个核对过它们 zh-Hans 资源里的字符串），两个都参与匹配（含拼音），用哪个名字找到的就显示哪个：搜“计算器”显示“计算器”，搜“calc”显示“Calculator”。

### 补充的关键词要加在页面本身那一条上

- **现象**：搜“壁纸”出来的是“视差背景”。
- **原因**：一个设置页面有十几条任务，打开命令都一样。关键词加到了每一条上，同分时取了名字最短的那条。
- **做法**：有页面本身那一条（Filename 是 `AAA_<页面 ID>`）就只加在它上面。

### 循环里现建 `QRegularExpression` 很慢

- **现象**：读系统入口要 1.9 秒，其中解析资源字符串只占 0.3 秒。
- **原因**：`split(QRegularExpression(...))` 每次调用都重新编译正则，约 3000 次就是 1.5 秒。
- **做法**：用 `static const` 的正则，或者手写切分（`splitKeywords`）。改完约 0.45 秒。

## 全局快捷键

### 设置里录不到 Alt+Space

- **现象**：点开快捷键方框，按 Alt+Space，方框停在“Alt + …”，什么也没录上。说明文字里举的例子正是 Alt+Space。（2026-10-08 读 Qt 6.12.0 源码发现，没有实际按过。）
- **原因**：
  - Qt 的 Windows 插件把“只按着 Alt 时的空格”留给自己弹系统菜单，根本不生成按键事件，QML 的 `Keys.onPressed` 收不到（`qwindowskeymapper.cpp` 的 “Special handling of global Windows hotkeys”，6.12.0 仍是这样）。同一段里 Alt+Tab、Alt+Esc、Alt+F4 直接交给 Windows，Alt+F4 会把设置窗口关掉。Ctrl+Alt+Space、Alt+Shift+Space 不受影响。
  - 别的程序用 `RegisterHotKey` 占着的组合键（PowerToys Run、Copilot 常占 Alt+Space），按下去直接打开那个程序，窗口也收不到。Win+E 这类系统快捷键同理。
- **做法**：录制期间由 `ShortcutCapture` 装低级键盘钩子（PowerToys 的快捷键框也这样做），按键在 Windows、Qt 和其他程序之前被拿走，在主线程上作为普通的 `QKeyEvent` 发给设置窗口，`HotkeyRecorder.qml` 的录制逻辑不用改。Qt Quick 对这样发来的 Tab 照样切换焦点（`QQuickItemPrivate::deliverKeyEvent`）。只吞录制开始后按下的键，以及这些键的抬起；Win 键按下和抬起都吞掉，不会弹开始菜单。
- **不够的办法**：给窗口的 `keyPressEvent` 或事件过滤器加处理没用，Qt 根本没生成这个事件。应用级的原生事件过滤器按源码看能拦住 Alt+Space（事件分发器在 `TranslateMessage` 之前调用它），但拦不住别的程序占着的组合键。

## “打开 / 保存”对话框（Ctrl+G 和搜索框）

做法和取舍见 [architecture.md](architecture.md) 的“对话框里的 Ctrl+G 和搜索框只靠窗口消息”（`src/app/platform/FileDialog.cpp`、`DialogJump.cpp`，`src/app/DialogBar.cpp`）。

### 地址栏平时没有输入框

- **现象**：在对话框里按类名找地址栏的 `ComboBoxEx32` → `Edit`，找不到。
- **原因**：地址栏平时只有面包屑（`Breadcrumb Parent` 里的 `ToolbarWindow32`，窗口文字是“地址: 路径”）。输入框在第一次进入编辑状态时才创建，之后不用时隐藏。
- **做法**：输入框还没有时，往面包屑最右边的空白处投递一次单击（`WM_LBUTTONDOWN`、`WM_LBUTTONUP`），等它出现并可见；已经有了（哪怕隐藏着）就直接用。然后 `WM_SETTEXT`，再 `SendMessage(WM_KEYDOWN, VK_RETURN)` 直接交给输入框。

### 投递的单击有时不起作用，原因没查到

- **现象**：2026-10-08 从对话框下方的搜索框连选两个结果（都是文件：`windows.h`、`system32.bin`），第一次对话框跳过去了；第二次面包屑对投递的单击没反应，日志里“没出现输入框”。连点三次也一样，面包屑、焦点、鼠标捕获都没变。前后两次运行都这样。
- **单击怎么生效**：把对话框开在实验程序自己的进程里，接管面包屑和上层窗口的窗口过程记录消息：按下时面包屑通知父窗口 `NM_LDOWN`（`item -1`，没点在按钮上），松开时检查位置，在最后一个按钮右边的空白处就换成输入框。它不看真实鼠标的位置，不管有没有窗口挡住，也不管对话框在不在前台。
- **复现不了**：下面这些情况单击全部生效：同一进程里切走再切回；像 `win::bringToFront` 那样合并输入状态（`AttachThreadInput`）后切回；真正的 Win顺（管理员身份）用 `--toggle` 切到搜索框再切回、在搜索框里输入后回车；另一个进程模拟搜索框，交还前台后立刻投递单击；很深的路径（前面的按钮会折叠，右边总留着空白）；点击位置被别的窗口挡住、对话框不在前台；跳转没结束就把焦点还给文件名框；先填文件名再点击；按当时的代码完整重放（文件结果：先跳文件夹，再填名字）；Listary 在运行、钩子已注入对话框进程；双方都以管理员身份运行。所以原来认定的“对话框失去前台再拿回来”不是原因。
- **做法**：第一次编辑后输入框一直在（隐藏），往隐藏的输入框里 `WM_SETTEXT` 加回车，对话框照样跳转，不用再点。所以只在输入框还不存在时才点击（`FileDialog.cpp` 的 `goByAddress`），最多试三次；还不行，打开、另存为对话框改走文件名框（见下面“不要往文件名框里填文件夹”），日志里留一条警告。再遇到点击无效，用实验时的办法（接管窗口过程记录消息）当场记下面包屑收到了什么。

### 对话框先到前台，过一阵才显示

- **现象**：搜索框在对话框出现时没有出来；切到别的窗口再切回来，它才出现。
- **原因**：对话框成为前台窗口（`EVENT_SYSTEM_FOREGROUND`）时还没有 `WS_VISIBLE`，控件也要 60–100 毫秒才建好。当时判断“对话框不可见”就把搜索框藏了，而对话框变可见不触发位置变化的事件，之后也没人再让它出来。
- **做法**：对话框在前台期间，用挂在它线程上的钩子同时听 `EVENT_OBJECT_SHOW`、`EVENT_OBJECT_HIDE`、`EVENT_OBJECT_LOCATIONCHANGE`（一个钩子覆盖 0x8002–0x800B，回调里只认这三种、只认对话框本身的 `OBJID_WINDOW`），每次都按对话框当时是否可见、是否最小化决定搜索框显示还是隐藏。定好位置再显示，否则会先以 160×160 的默认大小闪一下。

### 对话框显示之前还会自己改一次大小

- **现象**：2026-10-09 测“给搜索框腾地方”时逐毫秒记下对话框的边框：还不可见时先是一个默认大小（1252×1036），约 200 毫秒时换成 Windows 为这个程序记住的大小（1258×789），15 毫秒后才变可见。
- **原因**：对话框按 `ComDlg32\CIDSizeMRU` 里记的大小摆好自己是在显示之前、不可见的时候做的。
- **做法**：看到它可见（`EVENT_OBJECT_SHOW`）后再按它当时的边框调大小（`DialogBar::makeRoom`），不在不可见时动它：那时的大小还不是最终的，调了也可能被它自己盖掉。代价是用户可能看到它先以原来的大小出现，最多约 80 毫秒后变小；窗口打开的动画这时还没放完，不太显眼。搜索框等它调好再出来（`m_movingTo`），不会先在旧位置旁边闪一下。

### 对话框一弹出来就是最大化的，拖不动，搜索框盖住了标题栏

- **现象**：2026-10-10 用户反馈，有的程序（这次是 Thorium 浏览器里网页另存图片）弹出的“另存为”对话框几乎占满屏幕，手动调不了大小；搜索框哪边都放不下，只好盖在对话框顶上，连标题栏和关闭按钮一起挡住。
- **原因**：对话框是最大化的（`WS_MAXIMIZE`，`IsZoomed` 为真），但新式对话框没有 `WS_MAXIMIZEBOX`：没有还原按钮，最大化的窗口也拖不动边。Windows 按程序在 `ComDlg32\CIDSizeMRU` 里连“最大化”一起记住（那条记录末尾的标志是 -1，别的程序是 0），所以这个程序以后的对话框都这样弹出来。怎么变成最大化的没查到（`Win+↑` 或把标题栏拖到屏幕顶端都会）。它的最大尺寸被限制在屏幕大小（`maxTrack` 2560×1528），所以窗口矩形正好是工作区，左右下三边还露着 11 像素透明的边，看着不像最大化。原来的规则是“最大化的不动”，搜索框就落到了“盖在顶边上”。
- **做法**：最大化而且没有最大化按钮的对话框，第一次显示时先 `ShowWindowAsync(SW_SHOWNOACTIVATE)` 还原（不等对话框的线程），搜索框先不出来；等它不再是最大化（下一次位置变化，实测 5 毫秒左右），再按平常的办法腾地方。还原后的大小事先拿不准：`rcNormalPosition` 是工作区坐标，最大化时窗口矩形和可见边框差 11 像素，还原后左右下差 9、上边差 0，所以不提前算，等它还原了再量。还原后用户自己再最大化就不管了；有最大化按钮的（XP 风格的有些有）是用户自己选的，也不动。对话框关闭时 Windows 记下还原后的大小，同一个程序下次就不再最大化弹出。实测（2026-10-10，150%，Thorium）：对话框约 1 秒后被测试脚本找到时已经还原（1299×1020），搜索框在它下方 7 像素；按“取消”关掉后记录末尾的标志从 -1 变成 0，再弹出就不是最大化了。还原前拖右边框（命中测试是 `HTRIGHT`）大小不变，还原后拖动能改大小。
- **测试时别改了用户的记录**：用单独的配置（`--user-data-dir`）另起一个 Thorium，配置里打开 `download.prompt_for_download`，打开一个 `file:///…/x.bin` 就会弹另存为对话框，不用按键。但结束它的进程时，对话框会正常关闭（原因没细查，大概是对话框所在的进程发现浏览器没了，自己关了它），Windows 照样把测试时的大小记进 `thorium.exe` 那一条。测之前先导出那条记录，测完写回去。

### 拖动启动器时，图标跟不上

- **现象**：2026-10-09 用户拖动启动器，探出头的图标落在后面。测量：黑底上拖动，每步 12 像素，步后 12 毫秒截屏，按截图里窗口的边和图标的位置比，30 帧里有 2 帧图标差了整整一步。
- **原因**：图标是另一个窗口，原来接 Qt 的 `xChanged`/`yChanged` 再挪。拖动是系统的移动循环，Qt 收到 `WM_MOVE` 后把几何变化排进自己的事件队列，过一会儿才发信号，图标就晚了窗口一帧。
- **做法**：给启动器的窗口挂 `SetWindowSubclass`，在它自己的 `WM_WINDOWPOSCHANGED` 里当场挪图标（`WindowLogo`），缩放比也直接取 `GetDpiForWindow`，不用 Qt 那边还没更新的值。改后同样测量，60 帧全部到位。按窗口坐标采样（`GetWindowRect`）测不出这个问题，两版都是 120 次全对：要比的是屏幕上合成出来的画面，所以截屏，背后放一个全屏黑窗口，好认出边。

### 打开剪贴板的预览后，第二次点到了别的按钮

- **现象**：2026-10-09 实测，剪贴板挂在启动器搜索框下面，点一行的眼睛按钮打开预览，再点同一个位置想收起，结果点中了旁边的“固定”按钮，那一条被固定了。
- **原因**：预览让窗口往右变宽 401 逻辑像素，启动器离屏幕右边不够远，窗口整体往左挪了 27 逻辑像素才放得下，眼睛按钮从鼠标下面移开了，原位置上变成了固定按钮。
- **做法**：预览的宽度取列表右边到屏幕边缘还剩的地方（`Placement::roomRight`），在 280～400 之间，窗口不挪，列表和按钮留在鼠标下面；剩下不到 280 时才往左挪。窗口变宽时让已经在鼠标下面的东西挪开，用户点第二下就会点错。

### 控制台窗口找不到输入光标

- **现象**：在经典控制台（conhost）里按 `Win+V`，剪贴板开在鼠标指针处；在别的程序里都开在输入光标下面。
- **原因**：对控制台窗口，`GetWindowThreadProcessId` 报的是里面运行的程序（cmd.exe），不是画这个窗口的 conhost.exe；拿这个线程去 `GetGUIThreadInfo` 会失败，后面的几种找法也就都没轮到。
- **做法**：失败时，如果这个窗口就在前台，改用 `GetGUIThreadInfo(0, …)` 取前台线程的，控制台的系统光标就在那里（`TextCaret::find`）。同理，UI Automation 找到的焦点元素也不要按进程号核对是不是这个窗口的，改为看光标在不在这个窗口上。

### 差一个像素：对话框调好了，搜索框却换到了另一边

- **现象**：2026-10-09，150% 缩放，搜索框设成放右边。对话框按算好的大小调窄后，右边比搜索框需要的少 1 个物理像素，搜索框换到了下方。另一次放得下，但搜索框右边超出工作区 1 个物理像素（2561，屏幕宽 2560）。
- **原因**：两处取整。一是不感知 DPI 的程序（测试用的 PowerShell 5.1），窗口大小和位置按它的逻辑像素存，150% 下只能是 1.5 个物理像素的整数倍，给 960 宽它变成 961。二是 Qt 把窗口位置和大小分别四舍五入成物理像素：逻辑 1227 × 1.5 = 1840.5，进成 1841。
- **做法**：给对话框腾地方时比判断“放得下”多要 1 个逻辑像素（`DialogBar::makeRoom`），对话框差一点也还放得下；`place()` 最后按 Qt 的取整算出搜索框的物理边，越出工作区就往里挪 1 个逻辑像素。宁可和对话框之间的空隙少 1 个像素，也不越到屏幕外或别的显示器上。

### 搜索框被资源管理器之类的窗口挡住

- **现象**：对话框在前台，搜索框也显示着（`IsWindowVisible` 为真），却压在两个资源管理器窗口下面：Z 序是对话框、资源管理器、资源管理器、搜索框。点它点到的是资源管理器。
- **原因**：以前每次显示都 `SetWindowPos(HWND_TOP, SWP_NOACTIVATE)` 把它提上来（激活对话框时 Windows 会把它的所有者一起提上来，挡住搜索框）。但 `SetWindowPos` 的说明里写着：要把窗口提到最上面，窗口所属的进程得有设置前台的权限。对话框在前台时这个权限在对话框的程序手里，Win顺 的调用悄悄不起作用。以前的测试碰巧没有窗口挡在那里。
- **做法**：搜索框是置顶窗口（`Qt::WindowStaysOnTopHint`），不再提。它只在对话框或它自己在前台时显示，置顶不会挡着别的程序；Listary 的同类窗口也是置顶的。测试时用 `WindowFromPoint` 查要点的地方最上面是谁，查 Z 序就从 `GetTopWindow(NULL)` 往下数到搜索框。

### 点进搜索框，对话框却跳到了第一个建议

- **现象**：在对话框下面的搜索框里点一下，搜索框拿到前台 25 毫秒后，对话框就跳到了列表第一行（资源管理器的文件夹），前台也回到了对话框。点“更多”或右边的文件夹按钮也一样。
- **原因**：搜索框一激活就打开建议列表。下面放不下时列表往上展开：窗口内的输入框先往下挪（QML 的布局立刻变），窗口本身往上挪要晚一点；这次单击按旧位置落下，正好落在第一行上，算作选中了它。
- **做法**：刚激活时列表先不开，等这次单击松开（事件过滤器看到 `MouseButtonRelease`）再开；60 毫秒内没有单击（双击 Ctrl、`--toggle` 激活的）就直接开。不能用 `GetAsyncKeyState` 判断“是不是点进来的”，见“界面实测”里关于模拟单击的一条。

### 测试用的对话框不在前台弹出，旁边的搜索框一直不出来

- **现象**：2026-10-10 测数字键，用 `powershell -STA` 起一个 WinForms 的“打开”对话框，Win顺 的设置窗口当时在前面；关掉设置窗口、对话框到了前台，搜索框也没出来，`Ctrl+G` 也没注册（`RegisterHotKey` 一试，它空着）。
- **原因**：对话框不在前台弹出时，地址栏里的面包屑工具栏（`ToolbarWindow32`，“地址: …”）过了好一阵才建出来；`filedialog::kind` 要靠它认出这是文件对话框，`DialogJump` 拿到前台后只每 100 ms 再看一次、共 5 次，之后就不再看了。用户自己打开的对话框都在前台弹出，不会这样。
- **做法**：测试时先让对话框在前台弹出；已经这样了，就让前台离开再回来（打开启动器再按 Esc），搜索框马上就出来。

### 对话框刚出现时地址栏是空的

- **现象**：自动转过去以后，搜索框右边没有出现“回到原来的文件夹”。
- **原因**：对话框拿到前台、焦点也在控件上了，地址栏还没显示它自己的位置（面包屑的窗口文字是空的），这时读到的原位置是空字符串。
- **做法**：第一次转之前先等面包屑有了按钮（`TB_BUTTONCOUNT`）、窗口文字不空（最多 1.5 秒，`filedialog::waitForLocation`），再读原位置、再转。这样也不会被对话框自己随后的“转到初始文件夹”盖掉。

### 刚交还前台就操作，焦点还不回去

- **现象**：从搜索框选中后，对话框跳过去了，但焦点停在面包屑上，没有回到文件名框。
- **原因**：搜索框在前台时，对话框线程没有焦点控件；`SetForegroundWindow` 交还前台后，对话框要处理完激活消息才恢复焦点。在那之前记下的“原来的焦点”是空的。
- **做法**：先等对话框成了前台、而且有了焦点控件（`GetGUIThreadInfo`，最多 1 秒，`filedialog::waitForFront`），再动手。Flow Launcher 也是先等对话框回到前台再跳。

### 不要往文件名框里填文件夹再按“确定”

- 不少工具这么做，但选择文件夹的对话框（`FOS_PICKFOLDERS`，文件名框是 `edt1` “文件夹:”）会直接选中这个文件夹并关闭；另存为对话框里已经输入的文件名也没了。地址栏怎么填都不会让对话框确定。
- 只有 XP 风格的对话框（`GetOpenFileName` 带钩子或模板时出现，没有地址栏）才用文件名框：路径末尾加 `\`。实测 XP 风格的另存为对话框：填一个不存在的“文件夹\”再确定，只弹出路径错误的提示，对话框不关，不会当成文件名保存。
- 新式的打开、另存为对话框在地址栏不听使唤时也退到这条路：先记下文件名框里的字，填“文件夹\”、按“确定”，等面包屑显示新位置（新式对话框跳过去以后会清空文件名框），再把原来的字填回去。选择文件夹的对话框不退，宁可不动。实测（拿掉地址栏那条路的测试版）：打开、另存为对话框 82–89 毫秒到位，文件名照旧；选择文件夹的对话框不动、不关。

### 单击的坐标要用对话框自己的坐标系

- **原因**：Win顺 按“每个显示器分别感知 DPI”运行，`GetClientRect` 拿到物理像素；不感知 DPI 的程序收到的鼠标坐标是缩放前的逻辑像素，150% 下按物理像素点“最右边”会点到窗口外面。
- **做法**：取尺寸前先 `SetThreadDpiAwarenessContext(GetWindowDpiAwarenessContext(面包屑))`，取完换回来。测试要覆盖两种程序：PowerShell 里的 WinForms 对话框默认不感知 DPI，开头调 `SetProcessDpiAwarenessContext(-4)` 就是感知的。

### 跳完以后焦点停在面包屑上

- **现象**：对话框换好了文件夹，但键盘焦点在地址栏的面包屑上，接着打字不会进文件名框。
- **做法**：动手前用 `GetGUIThreadInfo` 记下对话框线程的焦点控件；回车后等输入框隐藏（约 70 毫秒），再 `SendMessage(对话框, WM_NEXTDLGCTL, 原控件, TRUE)`。原来就在地址栏里的不还。

### Windows 11 资源管理器的标签页都是“可见”的

- **现象**：以为没显示的标签页窗口是隐藏的，按“第一个可见的 `ShellTabWindowClass`”找当前标签，两个标签都可见。
- **原因**：每个标签页一个 `ShellTabWindowClass` 子窗口，全都带 `WS_VISIBLE`；切换标签只改变它们的前后顺序。
- **做法**：当前标签是 `FindWindowEx` 找到的第一个（Z 序最前）。`IShellBrowser::GetWindow` 返回的正是这个标签窗口，用它把 `IShellWindows` 里的条目对上号。实测：B 在前跳 B，`Ctrl+Shift+Tab` 换到 A 后跳 A。

### Listary 也响应 Ctrl+G，`RegisterHotKey` 挡不住它

- **现象**：测试里关掉了这个功能（Win顺 没注册 `Ctrl+G`），对话框照样跳了；当前标签是“主页”时，对话框跳到了已经没有任何窗口显示的文件夹。
- **原因**：这台电脑开着 Listary（`ListaryHookHost64`），它用钩子收 `Ctrl+G`，Win顺 注册了热键它也收得到，两边各跳一次，后到的算数。
- **做法**：测试 Win顺 自己的跳转时，不发真实按键，由提权的小程序给消息窗口（`WinShun.MessageWindow`）投递 `WM_HOTKEY`（id 2）；热键有没有注册，另外用测试进程自己 `RegisterHotKey(Ctrl+G)` 试一下（成功说明没人占着，马上注销）。用户同时开着两个时两边都会动，README 里写了关掉其中一个。
- Listary 6 还会在从资源管理器切回对话框时自己把对话框转过去（关不掉），它的窗口也是置顶的、会叠在对话框下面。测“自动转过去”和点击搜索框之前先退出 Listary（`Listary`、`ListaryHookHost32/64` 进程；`Listary.Service` 是服务，不用管）。

### 资源管理器被别的程序换了文件夹，搜索框就不见了

- **现象**：对话框一直在前台，测试脚本用 `Shell.Application` 让开着的资源管理器窗口换个文件夹（`Navigate2`），对话框下面的搜索框立刻消失，`Ctrl+G` 也没反应；切到别的窗口再切回来才恢复。
- **原因**：资源管理器换文件夹时会发一个 `EVENT_SYSTEM_FOREGROUND`，事件里的窗口是它自己（`CabinetWClass`），前台其实没变，还是对话框（测试里同时记下事件和 `GetForegroundWindow()` 看到的）。以前按事件里的窗口判断前台，就当成用户去了资源管理器：藏起搜索框、注销 `Ctrl+G`。前台没变，之后也不会再来事件。别的程序让已经开着的资源管理器窗口打开某个文件夹，用户也会碰上。
- **做法**：收到事件时按当时真正的前台窗口（`GetForegroundWindow()`）判断，取不到时才用事件里的窗口（`DialogJump::onForeground`）。事件是异步送到的，本来就可能晚到，用当时的前台也更准。

### 以管理员身份运行的 dopusrt 一直不退出

- **现象**：2026-10-09 从提权的 PowerShell 运行 `dopusrt.exe /info 文件,paths` 问 Directory Opus 开着哪些文件夹，5 秒都没退出，也没写出文件；普通权限下同一条命令 50 毫秒就完。
- **原因**：`dopusrt` 把请求交给正在运行的 Directory Opus（普通权限），等它回话；Opus 发回给提权进程的消息被 UIPI 挡掉，`dopusrt` 就一直等。Win顺 总是以管理员身份运行，直接启动的子进程也是。
- **做法**：用桌面外壳的令牌以普通权限启动 `dopusrt`（`shell::startUnelevated`，`CreateProcessWithTokenW`），最多等 3 秒，超时就结束它。叫它打开文件夹时也这样，否则 Opus 没在运行时会被启动成管理员身份。Total Commander 的 `WM_USER+50` 是 Win顺 发过去、直接拿返回值，高权限发给低权限不受影响。另外，Opus 没有窗口开着时不要问：`/info` 会把它启动起来。

### Directory Opus：`Go` 之后马上 `Select`，什么都没选上

- **现象**：想做“打开所在位置”时，先 `dopusrt /acmd Go "上一级" NEWTAB` 再 `dopusrt /acmd Select "名字" EXACT`，新开的标签里什么都没选上；过一会儿单独再发一次 `Select` 就选上了。
- **原因**：`Go` 读文件夹是异步的，`Select` 到的时候列表还是空的。
- **做法**：用 `Go "完整路径" OPENCONTAINER`，一条命令开上一级并选中它，文件和文件夹都行（只写 `Go "文件路径"` 也会选中文件，但文件夹会被直接打开）。复用已有标签（`NEWTAB=findexisting`）时，那个标签里原来选中的还在，新的加进去。

### 第一次运行的 Total Commander 不理 `/O` 传过去的路径

- **现象**：测试时刚装好 Total Commander，用 `TOTALCMD64.EXE /O /L=路径` 换文件夹，左栏不变。
- **原因**：第一次运行时先后弹出“按第几个按钮”的提示框、“配置”和“图标库”对话框，都是模态的，主窗口被禁用，传过去的命令不执行。正式用户只会碰到每次启动时的那个提示框（没注册的版本）。
- **做法**：测之前把这几个对话框点掉（提示框里有一个只写着数字的标签，按对应的 `&N` 按钮）。Directory Opus 第一次运行也弹欢迎框，“替换资源管理器”“开机时运行”“双击桌面”“用 Opus 看图”默认全勾着：测试时先全取消再点确定，免得改了用户的系统。

### 回车打开文件夹、应用、系统位置，什么都没打开

- **现象**：2026-10-10 用户在启动器里对文件夹（`F:\DCF`）按回车、单击，资源管理器都没出来，也没有“打开失败”的提示；历史里记了 8 次打开。“打开所在位置”正常。同一天晚些时候，开始菜单里的应用（Listary）单击、回车都开不了，只有“以管理员身份运行”能开。
- **原因**：资源管理器（开了一天的那个 explorer.exe）的桌面从 `ShellWindows` 里掉了：`FindWindowSW(SWC_DESKTOP)` 返回 `S_FALSE`，所以 `executeAsUser` 失败（第一次一个窗口也列不出来；第二次列得出 2 个文件夹窗口，只是没有桌面）。退到 `rundll32 shell32.dll,ShellExec_RunDLL "目标"`：凡是外壳命名空间里的目标都要交给正在运行的资源管理器，它不再接，结果什么都没开，rundll32 照样退出码 0，算作成功。2026-10-10 提权探针实测，全部“成功”却什么都没开：文件夹、`shell:AppsFolder\{…}\Listary\Listary.exe`（开始菜单应用）、`shell:AppsFolder\…!App`（商店应用）、`shell:::{7007ACC7-…}`（控制面板项）、`ms-settings:storagesense`、`windowsdefender://threat`。磁盘上的 exe 不受影响（rundll32 自己启动它）。“以管理员身份运行”是 `ShellExecuteEx(runas)`，不经过资源管理器，所以能开。“在单独的进程中打开文件夹窗口”是关着的，`explorer.exe F:\DCF` 却开在了一个新进程里：转交失败时它自己开窗口。
- **做法**（`Shell.cpp`）：`executeAsUser` 找不到桌面时，借 `ShellWindows` 里任意一个文件夹窗口的 Application 对象来 `ShellExecute`（同样是普通权限，约 30 ms；上面每一种目标都实测能开）。一个窗口都没有时以桌面外壳的令牌启动：文件夹、`shell:`、`::{…}`、URI 交给新的 `explorer.exe "目标"`（约 200 ms，全都实测能开）；文件和带参数的命令仍走 rundll32。exe 不能交给 explorer.exe：程序的工作目录会变成 System32（传给 explorer.exe 的工作目录不起作用），rundll32 和借窗口两条路都是程序自己的文件夹。盘符根目录（`D:\`）不加引号：右引号前的反斜杠会把引号转义掉；带逗号的路径加了引号 explorer.exe 也认。查法：`ShellWindows` 的个数和 `FindWindowSW` 的返回值，再用 `EnumWindows` 数 `CabinetWClass` 窗口（`ShellWindows` 自己可能就不全）；打开后数目标进程、新的 `CabinetWClass` 窗口，不要信返回值。

### 从文件管理器切回对话框不自动转（issue #7）

- **现象**：开着“自动转过去”，Word 的“另存为”第一次出现能转到资源管理器的文件夹；不关对话框，切到资源管理器换个文件夹再切回来，对话框不动（Listary 会转）。
- **原因**：`DialogJump::follow` 只认前台“对话框 → 文件管理器 → 对话框”直接换。点任务栏按钮、`Alt+Tab` 时，任务栏（`Shell_TrayWnd`）、切换窗口、开始菜单会先到前台一下：对话框被当成不在了（`m_dialog` 清空），文件管理器到前台时没记下它显示的文件夹；回来时“上一个是文件管理器”也被中间那一下冲掉了。
- **做法**：不列举这些过渡窗口的类名（会漏、会变）。记“最后在前台的对话框”（`m_lastDialog`），文件管理器在它之后第一次到前台时记下文件夹、把它放进 `m_wentToManager`；对话框再到前台时取走，才算从文件管理器回来。中间到过 Win顺 自己的搜索框、剪贴板（同伴）就取消，和原来一样。

### WPS 的对话框：普通权限的 UI Automation 什么也看不到

- **现象**：2026-10-10 用 UIA 看 WPS 文字的“另存为”对话框，只有一个空的根元素，类名是窗口类 `Qt5QWindowIcon`；`Get-Process` 和 WMI 也拿不到 `wps.exe` 的路径。
- **原因**：这台机器上 WPS 以管理员身份运行，UIPI 挡住了普通权限的 UIA 客户端。
- **做法**：Win顺 本身以管理员身份运行，看得到完整的控件树（根元素 `KcfdFileDialog`）。测试脚本也要提权。WPS 提权、Win顺 没提权时就看不到，Win顺 不认这个对话框；两边都不提权的情况没试过。

### WPS 的对话框：面包屑和文件类型，UIA 给的不对

- **现象**：跳到 `F:\wsfmtest` 以后读面包屑，有时少了“磁盘 (F:)”那一节，有时几个名字全是空的，截图上地址栏却是对的；文件类型框读到“WPS文字 文件(*.wps)”，界面上选的是 docx。另外桌面、文档在面包屑里叫“桌面”“文档”，盘符叫 WPS 自己起的“磁盘 (F:)”，不是卷标。
- **原因**：Qt 的无障碍对象没跟着控件更新（大概是面包屑控件重用时没通知名字变了），没细查。
- **做法**：WPS 对话框不读当前文件夹和文件类型（`currentFolder`、`fileType` 返回空）；跳没跳到，只看地址输入框收起来没有。测试时要确认位置，就截图看地址栏。

### WPS 的对话框：投递的回车交给有焦点的控件

- **现象**：在地址输入框里填好路径，往对话框窗口投递 `WM_KEYDOWN` 回车，对话框跳过去了。但 Qt 窗口只有一个窗口句柄，回车交给的是 Qt 当时有焦点的控件。
- **原因**：焦点要是在文件名框里，这个回车就是“保存”（WPS 是管理员身份，连 `C:\Windows` 都写得进去）。
- **做法**：先用 UIA 把焦点放进地址输入框，`GetFocusedElement` 确认类名是 `KcfdLocNavigationLineEdit`、进程是对话框的，再填路径，填完再确认一次才投递回车（`kingsoft::goTo`）。对话框要在前台：不在前台时 UIA 报的焦点是别的程序的。

### WPS 的对话框：不存在的路径会弹它自己的提示框

- **现象**：测试时在地址栏输入 `Z:\nothere` 回车，弹出“输入位置有误 / 暂不支持云端路径或拼写错误的路径，请检查后重试。”（`KcfdMessageDlg`），对话框被禁用，要点“确定”才回来。
- **做法**：跳之前先 `GetFileAttributesW` 确认是文件夹，不是就不跳。测试里弹了这个框，用 UIA 找名字是“确定”的按钮 Invoke 掉。

### 拖动 Win顺 自己的选文件夹对话框，窗口远远落在鼠标后面（linux.do #222）

- **现象**：2026-10-16 用户反馈（迷你主机，核显），设置 › 高级 › 自动备份选文件夹时，拖动那个对话框延迟很高，两种界面绘制方式都一样。实测（SendInput 每 4 ms 移一步、共 300 步，另一个线程每 1 ms 读对话框的 `GetWindowRect`）：对话框平均落后光标 148–178 像素（p95 354–398，最多 474），每秒只到 81–188 个位置（鼠标给了 250 个），松手后还要 282–564 ms 才走到位；显卡加速和省内存一样。关掉对话框旁的搜索框（`[Launcher] DialogBar=false`）就只差 1 像素、2 ms 到位，和别的程序的同一种对话框一样。
- **原因**：对话框旁的搜索框（`DialogBar`）也挂到了 Win顺 自己的对话框上（本来只为别的程序的对话框做的）。`shell::pickFolder` / `pickFile` / `pickSaveFile` 的 `IFileDialog::Show` 在界面线程上跑模态循环，拖动时系统的移动循环也在这个线程上；`DialogJump` 跟着对话框位置的 `EVENT_OBJECT_LOCATIONCHANGE`（进程外钩子）也是从这个线程的消息队列送来的。于是每拖一步，同一个线程都要先把搜索框和探头的图标挪过去（实测每次 2.3–2.7 ms，几乎全在挪搜索框的窗口上：`QWindow::setGeometry` 和直接 `SetWindowPos` 一样慢，但 Qt 的窗口过程仍在同一线程上处理 `WM_WINDOWPOSCHANGED`，系统和 Qt 各占多少没分开量），再处理下一步鼠标移动：事件来得比做得快，就越积越多。核显上挪窗口更慢，落得更远。别的程序的对话框不受这个影响，因为它的移动循环在它自己的进程里，不等 Win顺。
- **做法**：`DialogJump::follow` 遇到本进程的窗口就当作没有对话框（和管理员程序的对话框一样处理），搜索框、Ctrl+G、自动转过去都不挂到 Win顺 自己的选择框上；所有选择框都走 `shell::pick*`，一处改全部受益。改后同样的测量：显卡加速平均落后 2.0–3.2 像素、省内存 1.3–1.5 像素，每秒 219–249 个位置，2 ms 到位。
- **另外记下**：搜索框跟着别的程序的对话框时，那个对话框拖动时每秒的位置数也从约 248 降到约 160（平均落后 5–6 像素，松手 3–7 ms 到位）。原因见下一条：是挪窗口本身，不是钩子在哪个线程上。
- **测试脚本**：在隐藏窗口的进程里（提权的 `-WindowStyle Hidden` 脚本）`timeBeginPeriod(1)` 不起作用（Windows 11），`Thread.Sleep(1)` 是 15.6 ms，取样和按节奏发鼠标都要忙等；来回拖时，“延迟多少毫秒”按最近一次经过该点算，别按第一次。

### 搜索框跟着别的程序的对话框，那个对话框拖动变慢：是挪窗口本身，换线程没用

- **现象**：2026-10-16 实测（i9-14900HX、RTX 4060 笔记本、单屏 2560×1600 240 Hz；PowerShell 的 `OpenFileDialog`，SendInput 每 4 ms 一步、共 300 步，另一个线程每 1 ms 读对话框和搜索框的 `GetWindowRect`，每种情况拖 3 次）：不开搜索框每秒 247–250 个位置、落后 1.3–1.9 像素；开着 161–166 个、落后 5–5.4 像素。搜索框本身跟得上：平均晚对话框 3.6 ms，差约 5 像素（拖动的一步 6 像素），松手 6–9 ms 到位。
- **原因**：每挪一次窗口，`SetWindowPos` 本身在这台机器上就要约 2 ms，其中约 1.8 ms 是本线程的 CPU 时间（`QueryThreadCycleTime`，在系统里，不是等待）。看来是在窗口管理器的全局锁里做的：对话框的移动循环每一步也要 `SetWindowPos`，只好轮流，所以别的进程挪窗口会拖慢它。逐个排除（同一测法；探针是一个只做这件事的小程序，见下）：
  - 钩子送事件：进程外 `WinEvent` 钩子挂在空线程上，回调什么都不做 246–250/s；回调里空转 3 ms、10 ms（不调窗口函数）仍是 228–250/s。发事件的线程不等我们的队列。
  - Win顺 的界面线程：搜索框显示着、钩子开着、每个事件照常处理（895 次，每次 0.24 ms），只是不挪窗口：247–249/s。
  - 挪窗口：探针在单独的进程里每个事件挪一个普通的 Win32 弹出窗口，就重现了全部变慢：169–186/s，每次 2.0–2.1 ms。和窗口大小（100×20 一样）、置顶与否、DWM 阴影和圆角、`WS_EX_LAYERED`+`SetLayeredWindowAttributes` 都无关；`SWP_NOSIZE|SWP_NOCOPYBITS|SWP_NOREDRAW|SWP_NOSENDCHANGING` 只省到 1.6 ms（181–186/s）；隐藏着的窗口没人拖时也要 1.5 ms。从钩子线程挪还是交给另一个线程挪（钩子线程只投递、合并成最新位置）都一样：171–184/s。真搜索框：`QWindow::setGeometry` 2.3 ms，直接 `SetWindowPos`（只挪不改大小）2.0 ms，去掉阴影一样。
  - 属主：搜索框没有属主。把探针窗口的属主设成对话框，对话框 206–215/s，探针自己反而只跟上 127–139/s：不是原因，也不是办法。
  - DWM 合成：上面隐藏窗口也要 1.5 ms，跟合成无关。`UpdateLayeredWindow` 挪分层窗口每次只要 0.11–0.15 ms（带位图或只给位置都一样），对话框 229–250/s，说明贵的是 `SetWindowPos` 这条路，不是“窗口在动”。
  - 在 Win顺 里把每个事件排队合并（`QueuedConnection`，处理时取最新位置）：158–163/s，没用。界面线程本来就跟得上，没有积压可合并；贵的是每一次挪动。
- **做法**：钩子留在界面线程上（`DialogJump` 不变），b2e10bc 对自己的选择框的排除也留着：Qt 的窗口只能在它自己的线程上挪，挪到哪个线程去都是同样的 2 ms。能做的是少挪或换便宜的挪法，按代价排：
  - 按屏幕刷新率限速（先挪一次，一帧之内再来的只记最新位置，到点再挪；屏幕一帧只显示一次，多挪的看不见）。原型（`WINSHUN_DIAG_HZ=60` 模拟 60 Hz）：对话框 231–237/s（逐个事件挪是 161–166），搜索框每秒 57 次，按 1500 像素/秒的拖动平均差 14 像素，松手后最多晚一帧到位。240 Hz 的屏幕上事件和帧差不多一样多，一点不省（实测 162–164/s）；省的是 60 Hz 屏（论坛用户那种迷你主机）和高回报率鼠标。定时器不对齐 vsync，拖得快时可能看出搜索框晚一帧；真要对齐可按 `DwmGetCompositionTimingInfo` 的下一次合成时间挪。
  - 拖动时藏起来：再挂 `EVENT_SYSTEM_MOVESIZESTART`/`END`，开始时藏、结束时放好再显示，一点不拖慢，但拖的时候看不到搜索框，行为变了。**2026-10-16 选了这个**（`DialogBar::dialogDragged`）：实测拖动 248–249/s（和关掉搜索框一样），松手 14–16 ms 在新位置显示，没有在旧位置闪；改大小、Alt+空格移动同样。Win+←/→ 贴边和最小化没有这两个事件，仍逐个跟随。按住标题栏不动约 0.5 秒 Windows 也算开始拖动，会藏起来。
  - 拖动时换成分层窗口的“截图”：开始时 `grabWindow` 一张、用 `UpdateLayeredWindow` 跟着挪（0.15 ms，几乎不拖慢），结束时换回真窗口；代价是复杂，拖的时候没有 DWM 的阴影。
  这台机器上 `SetWindowPos` 为什么要这么久（别处通常是零点几毫秒）没查到函数：xperf 按模块取样（探针 800 次挪动，约 1.3 s）时间几乎全在内核，`ntoskrnl.exe` 约 650 ms、`win32kfull.sys` 约 370 ms、`win32kbase.sys` 约 280 ms，显卡驱动 `nvlddmkm.sys` 13 ms；探针里虽被装进了 `UIAutomationCore.dll`（有程序挂了进程内的辅助功能钩子），它只占 3 ms，不是钩子回调。所以是窗口管理器这条路本身，别的机器上可能便宜些，也可能更贵（论坛用户的核显迷你主机上自己的选择框落后几百像素，说明那里也不便宜）。
- **测试**：探针 `follower.cpp`（模式 hook / busy / popup / owned / layered / gui，选项 notop、small、nocopy、nosize、nosend、lwa、ulwpos、vsync、throttle；`selftest` 不拖对话框、单测一次挪动的代价）和带搜索框取样的 `m.cs`（`Drag2`：搜索框落后对话框多少像素、多少毫秒）在 2026-10-16 这次会话的临时目录里；Win顺 一侧的开关（`WINSHUN_DIAG_BAR=still|nologo|noshadow|rawmove|throttle|coalesce`，日志里每次隐藏时一行 `DIAG`）只做在测试版里，没有提交。每种情况之间插一次“没有搜索框”的基准，排除机器状态漂移。

## 文件索引（NTFS）

### 机械硬盘上的盘建索引很慢：MFT 读不了

- **现象**：日志里有 `cannot read the MFT of D:: unsupported record or cluster size`，这些盘只好逐个文件夹遍历，在机械硬盘上要几分钟甚至更久（磁头在各个文件夹之间来回跑）。
- **原因**：读 MFT 的代码假定一个簇装得下整条文件记录，这样记录不会跨区段，遇到簇比记录小的盘就放弃。可是从 FAT32 转换来的分区、很老的分区、手动选了小簇的分区，簇只有 512 字节（记录 1 KB）；`format /L`（4 KB 记录）配 512–2048 字节的簇也一样。这种分区多在用了很多年的机械硬盘上，正好是最慢的盘。
- **做法**：不再挑簇的大小。读到的一块末尾如果是半条记录，把它拷到下一块数据的前面（每块前面留着一条记录的空位），接成整条再解析。`tst_core` 的 `mftReader` 用内存里的卷镜像测：区段是奇数个簇、在“磁盘”上顺序打乱，记录既跨区段也跨 4 MB 的块。真机上用 VHD 按各种簇大小格式化，`wsbench --mft P: Q: …` 读 MFT 并和遍历逐项比对。VHD 要用 `.vhd`：`.vhdx` 的物理扇区是 4 KB，格式化不出比它小的簇。

### MFT 的占用位图不一定在 0 号记录里

- **现象**：读 MFT 时按位图跳过空记录，D、E、F 盘都生效，C 盘一段都没跳，可它 4.4 GB 的 MFT 里明明有 1 GB 是空的。
- **原因**：大的 MFT 的 0 号记录装不下全部属性，改放一份属性列表（`$ATTRIBUTE_LIST`），`$BITMAP` 搬到了扩展记录里（本机 C 盘在 21 号记录）。只在 0 号记录里找，自然找不到。属性多时，属性列表本身也可能不在记录里（非常驻），一个属性的区段表也可能分成几段放在不同记录里。
- **做法**：先看 0 号记录有没有属性列表，按列表里 `$BITMAP` 的每一段（起始 VCN、所在记录）逐段读出区段表、接起来；读不到就照常全读，不会漏记录。`wsbench --mft` 会打印位图在哪、为什么没读到，以及“没有一条在用的块里，位图标了几条在用”（应为 0）。`tst_core` 的 `mftReader` 覆盖了位图在 0 号记录、在别的记录、分两段这几种情况。

### 改了名字记录的格式，内容搜索筛候选变慢

- **现象**：扩展名长度从每项挪进名字记录后（每项 20 → 17 字节），`wsbench --content-search` 的“filter pass”单线程从 12.5 ms 变成 15.7 ms。
- **原因**：`ContentFilter::accepts` 对每个文件都要扩展名长度，以前在项里，现在要先读名字记录；而且先 `extLength` 再 `name`，同一条记录解了两遍。
- **做法**：先排除只看标志位的（文件夹、已删、离线、低优先级），再用 `FileIndex::fileName(e, extLength)` 一次读出名字和扩展名长度，单线程 15.2 ms。剩下的约 3 ms 是读名字的内存访问，搜索时 16 个线程分块跑，实际约多 0.2 ms，接受。以后再往名字记录里挪东西，先用 `--content-search` 量一下这一步。

### 按“最早在前”排，前面一整屏都是 1970 年 1 月 1 日

- **现象**：“文件”里搜 report，切到“最早”，前面全是 `%USERPROFILE%\.codex\plugins\…` 里的 report.css 之类，日期都是 1970年1月1日（2026-10-11 实测）。
- **原因**：不记时间的压缩包、npm 之类的包管理器解出来的文件，修改时间是 Unix 纪元 0（或 DOS 纪元 1980-01-01）。索引里是真实读到的值，不是 0（“不知道”），所以按时间排时它们是“最早的”。
- **做法**：1980-01-02 00:00 UTC 以前的时间一律当作不知道（`FileIndex.h` 的 `knownTime`，各时区下的 DOS 纪元也算进去）：排序时和 0 一样放最后（`NameSearch` 读时间时就换掉），“最近修改”不选，结果行不显示日期（`ResultModel::modifiedText`）。索引和快照里仍存原值。测试数据的时间要用 2023 年以后的（`T + …`），用 10、1000 这样的小数会被当成不知道。

### “不搜索的文件夹”里加了整个盘符，那个盘照样建索引

- **现象**：alist（rclone）把网盘挂成本地盘 X:，在“不搜索的文件夹”里加了 `X:\`，Win顺 还是一直遍历它（GitHub #17）。
- **原因**：`X:\` 规整后是 `X:`。排除规则只在遍历时比对“子文件夹”的完整路径，盘的根本身从不被比对；`Crawler::isExcludedPath` 从第一个反斜杠之后才开始查祖先；`removeExcluded` 还特意跳过根。盘作为“卷”照样被列出、读 MFT 或遍历。
- **做法**：在 `IndexService::run` 列出卷之后，把根被排除的卷直接去掉，接下来 `dropStaleRoots` 会像拔掉的盘一样删掉它的根和条目、内容索引也不再读它；取消排除时规则一变就重新列卷，它会回来并重读。`isExcludedPath` 也先查盘根，`applyRuleChanges` 不去探 `X:`（那探的是该盘的当前目录）。`wsbench --exclude-drive P: Q:`（管理员）走一遍“建 → 排除 → 取消 → 再排除 → 两种重启”，用 VHD 挂一个 NTFS、一个 exFAT 分区测。

### 搜索面板按路径排序，一次要 5 秒

- **现象**（2026-10-15，`wsbench --order path " "`，330 万项）：不输入、按“路径”列排序一次 5.1 秒；按名称 9.5 毫秒、按大小 6.1 毫秒。
- **原因**：比较两项的路径时各自造它们所在文件夹的路径，按分片各缓存一份。MFT 的记录号不按文件夹连续，一个文件夹的文件散在很多分片里，每个分片都把它的路径再造一遍，几百万次。
- **做法**：每次搜索先给所有文件夹按树的顺序编号（`NameSearch.cpp` 的 `folderRanks`：根和子文件夹各按名字排，父在前），比较时只比两个整数，相同再比名字。42 万个文件夹编号约 38 毫秒，整个浏览 42 毫秒；搜到几千项时也是这 38 毫秒打底。

### 打包的文件大小：16777215 + 0.5f 等于 16777216

- **现象**：`unpackSize(packSize(16777215))` 测出来是 16777216。
- **原因**：大小存成 float 的位（`FileSize`，4 字节，1600 万以内精确），解包时想四舍五入写了 `f + 0.5f`，这一加在 float 里就进位了。
- **做法**：先转成 double 再加 0.5 取整。顺带一提：float 的位按无符号整数比较就是按大小比较，排序和筛选不用解包；`0xFFFFFFFF` 是 NaN，当“不知道”用。

## 内容索引

### 索引说“可能含有”的文件太多，内容搜索还是慢

- **现象**：索引建好了（0 个待读），在“内容”里搜一行代码 `const auto lock = index->readLock();` 仍要 2.7 秒才出第一条、6.5 秒才搜完；再搜一遍又是这样。
- **原因**：先量再猜：`wsbench --content-search "一行代码"` 用程序自己的索引走一遍搜索，报出“可能含有”的文件有 1392 个、共 1 GB，真正含有的只有 12 个。片段只记单词内部的三个字母（`con`、`aut`、`loc`），这些在代码里到处都是；几十 MB 的 `*.min.js`、导出的 JSON 几乎含有所有片段，每次都要整个读一遍，读之前还要等 Defender 扫描。
- **做法**：片段带上空格和标点（剩 13 个），按种类给文件大小设上限。改片段规则时，`Collector`（读文件）和 `ofPhrase`（查词）必须按同一套规则拆，并升 `kStateVersion`：旧索引按旧规则记的，用新规则去查会把真含有的文件排除掉。改完跑 `wsbench --content-index --verify`，它把每个被排除的文件都实际读一遍，应当一个都不含。

### 后台模式读文件，开多少线程都快不起来

- **现象**：内容索引第一次建立时每秒只读一两百个文件，33 万个文件要 25 分钟以上；没人用电脑时从 4 个线程加到 12 个，几乎没变快。
- **原因**：读线程用了后台模式（`THREAD_MODE_BACKGROUND_BEGIN`）。它不只降低 CPU 优先级，还把线程的磁盘请求降到“极低”，打开文件时 Defender 的扫描也跟着排在后面。2026-10-09 实测（`wsbench --content-index --read-only --sample 12000`，每组换一个 `--seed`，免得 Defender 记得扫过的文件）：后台模式 4 线程 141 个/秒、12 线程 157、16 线程 183；普通线程 4 个 683、8 个 1184–1290、16 个 2105–2338、32 个 2836。只把 CPU 优先级降到最低（`--lowest`，磁盘不降）也慢两三成：Defender 的扫描线程把 CPU 占满时，我们的线程要排队才能接着干。
- **做法**：读文件的线程用普通优先级，开销靠“同时读几个文件”控制：打开一个文件 Defender 要花约 7 ms CPU，同时读几个就大约占几个核（8 线程时整机多用约 8 个核，16 线程约 15 个）。有人用电脑时用 CPU 核数的 1/4，没人用时 1/2（`ContentIndexer.cpp` 的 `readers`）；机械硬盘上的文件同时最多读 2 个。量开销时看整机 CPU（`GetSystemTimes`），不要只看自己进程，钱花在 Defender 的进程里。
- **文档要有自己的线程**：Word、PDF 由提取进程去读，读的线程只是干等。和文本文件共用线程时，文档扎堆的地方（用户文件夹）一半线程在等提取进程，`wsbench --service --content` 实测速度掉到每秒一两百个。现在文档另有几个线程（有人用电脑、文本还没读完时用一半提取进程，否则全用上），不占读文本的名额。
- **整体怎么量**：`wsbench --service --content`（管理员）照第一次运行建文件索引和内容索引，每 10 秒报一次进度和“多久没碰键盘鼠标”。2026-10-09：旧做法重建 25 分钟；改后有人用电脑时约 5.6 分钟（337 秒），没人用时 163–186 秒（两次；文档后来单独占线程、读完文本再加提取进程，文本读完后 8 秒内收尾）。同一天关掉 Defender 实时保护再量，剩下的全是自己的开销：内容部分 75–94 秒，按下面几条改完是 27 秒（文件名索引另 4 秒）。要看时间花在哪，在 `ContentIndexer::pass` 里给读、交给索引、等合并分别计时，打到 stderr，量完删掉。

### 文档提取进程用后台模式，建索引时慢了近四倍

- **现象**：2026-10-09 关掉 Defender 实时保护量第一次建立：文本 22 秒读完，文档却拖到 81 秒，平均每个 93 ms，还有一个超过 20 秒被当成超时结束（日志 `the document reader took too long`）。单独读同样的 5815 个文档（`wsbench --extract --sandbox --threads 8`）只要 17–19 秒，每个约 25 ms。
- **原因**：提取进程带 `--background` 启动，自己设了 `PROCESS_MODE_BACKGROUND_BEGIN`，磁盘请求和内存页都降到最低。单独读时看不出差别（`--background` 17.1 秒、不带 19.3 秒）；建索引时十几个线程同时在读文件，提取进程读文档、把被挤出内存的页读回来，都排在最后。和上一条读线程的坑是同一回事，只是换了个进程。
- **做法**：提取进程只降 CPU 优先级（建索引时 `IDLE_PRIORITY_CLASS`），不用后台模式；文档的句柄本来就设了 `IoPriorityHintLow`（低，不是极低）。改后每个文档约 25 ms，内容索引第一次建立 75 秒 → 34.5 秒。这类问题要在“同时还在干别的”的场景里量（`wsbench --service --content`），单独量看不出来。

### 合并时停住读线程，十几秒在干等

- **现象**：第一次建立的 75 秒里，读线程有 13.6 秒停着等合并，可合并本身只用了 3.7 秒。
- **原因**：每次合并（包括把 8 个小段文件合成一个的那种）前都要等手上正在读的文件全部交进索引、不再开新的，因为合并会扔掉没有文件再用的内容号，期间新加的文件可能正好要用。而手上正在读的往往有文档，一个 PDF 能读好几秒，其他线程就一起干等。
- **做法**：小段合并（`mergeTails`）保留段里所有的内容号，扔不扔留给整体合并去做，于是可以边读边合，读线程不停；只有整体合并和重写文档文字文件才停住读线程。`tst_core` 里有一项一边加文件一边合并。

### 多线程合并按字节切分，线程加了也不快

- **现象**：整体合并改成按片段范围分给多个线程，10 万个文件：1 线程 3.2 秒，4 线程 2.8 秒，16 线程 1.4 秒；4 线程时进程 CPU 只相当于 1.2 个核在忙。
- **原因**：两处估错了工作量。一是按字节切：几乎每个文件都有的英文三字片段，内容号列表编码后几乎不占字节，解码时却有几万个号，按字节均分就把最重的一段整个切进了同一份。二是只拿一处来估：段文件压缩过，比内存里攒着的那批还小，于是拿内存那批（最后读的文件，多是中文文档）来切，英文片段全落进一份。
- **做法**：工作量按“每个片段有几份内容”算（读片段目录里的个数，每个片段再加 8），所有段文件和内存一起抽样（`Segment::work`、`ContentIndex::splitKeys`），每个线程分 8 份。改后 4 线程 1.0 秒、16 线程 0.4 秒；第一次建立里的整体合并 13.3 秒 → 1.0–1.5 秒。多线程写出的段文件和单线程逐字节相同，`tst_core` 有一项专门比。

### 写段文件挪出锁以后，加文件被拒

- **现象**：写段文件（排序、编码、写盘）改成在锁外做后，`wsbench --content-index` 每秒读 3.2 万个文件，可索引里只有 29.9 万个文件（应为 33.1 万）。
- **原因**：以前写段时一直锁着，加文件的线程只能等，内存里攒的片段不会超过一个段。挪出锁后它们照样往内存里加，比写段还快（16 线程、不开杀毒），攒到上限（400 万对）后 `ContentIndex::add` 返回 false。测试程序没看返回值，文件就丢了；程序里会把这一轮当成“没地方了”中断，1 分钟后重来。
- **做法**：攒过两个段的量就等正在写的段写完（`m_flushed`），不拒绝；`wsbench` 现在报告被拒了几个。另外，凡是中途可能放锁的步骤（写段、等写段）都放在 `add()` 开头，查有没有相同的内容、文字文件换没换，都在这之后做，不用放锁前查到的结果。（后来查相同的内容挪到了共享锁下、独占锁之前：它占了独占锁八成的时间。查到的内容号只有整体合并重新编号时才失效，`m_generation` 变了就重查；这期间新来的内容在内存里，独占锁里补查一次。）

### 多线程排序不比单线程快：新内存的缺页

- **现象**：写段文件的排序改成 8 个线程后，`wsbench --content-index` 里一个段（100 万对）还是 9 ms，单线程是 10 ms。
- **原因**：每个段都新申请 8 MB 的缓冲（排序结果、草稿各一块）。这么大的块，堆直接向系统要新页，系统给的是清零的新页，每 4 KB 第一次写入时缺页一次，一个段约四千次，比排序本身还费时。还有一处：每个段按片段范围分成十几份各自编码，每份新建几百 KB 的缓冲，用完就放，堆把它们留在进程里，合并完常驻私有内存多出 20 MB（32 → 53 MB）。
- **做法**：写段用的缓冲（下一块内存、排序结果、草稿、各份的编码结果）留给下一个段（`ContentIndex::FlushBuffers`），整体合并后才放。排序 9 → 5 ms，常驻内存回到 37 MB。量多线程的提速时，先看是不是每次都在要新内存。

### 整体合并时正好在写段

- **现象**：没有实际遇到，是改写段时看出来的。整体合并期间，内容搜索读的文件照样交给索引，可能正好攒够一个段、开始写；合并装好（内容全部重新编号）之后，这个段写完也装了进去，里面还是旧的内容号，搜索结果会错。
- **原因**：整体合并装之前只看文件数和内存里的片段数变没变。写段开始时文件已经加进来了（在这之前合并就该放弃），但如果写段是加文件的开头为了腾地方而触发的，文件数还没变，片段数也不变（以前写段时片段留在内存里，现在交给 `m_writing`，两处加起来不变），合并照样装。
- **做法**：整体合并装上时 `m_generation` 加一，正在写的段写完看到它变了，就把段扔掉（它的片段已经在合并的结果里）。内存里的片段数按 `m_memory` 加 `m_writing` 算。

### 内容号重排后，短列表反而变大

- **现象**：整体合并用递归图二分重排内容号，倒排表 79 MB → 59 MB，片段目录却从 12.2 MB 涨到 16.9 MB。
- **原因**：四个以内的短列表直接记内容号（LEB128）。这些多是中文的两字片段，大部分出自少数几份长文档；按 MinHash 排时长文档的最小值多半最小，排在最前，号小，一两个字节就够；二分把它们散开，号变大，每个要三个字节。
- **做法**：段文件里另存一张表，列出短列表用到的 1.6 万份内容，按用到的次数排；短列表记在这张表里的位置（段文件 v5）。短列表 4.3 MB，比 MinHash 排时还小。只有整体合并写这张表，内存写出的段和小段合并照旧直接记号。

### 升级后内容索引 15 分钟才开始重建

- **现象**：2026-10-09 部署了换内容索引格式的新版，旧的段文件删了，可几分钟里一个新段都没写，程序几乎不占 CPU。`wsbench --service --content --from 旧数据的副本` 一样：文件索引 0.5 秒就绪，内容索引 150 秒都没开始。
- **原因**：快照一加载完，文件索引就标成 Ready（这样马上能搜文件名），可哪些磁盘在跟着变更日志（`contentVolumes`）要再过一会儿才登记。内容状态读不了（换格式）时文档数为 0，内容索引按“第一次运行”不等那 30 秒，正好在这个空档里跑了一轮：一个磁盘都没有，什么也没读，接着睡到 15 分钟后的下一轮。平常重启有那 30 秒，第一次安装是读完 MFT 才 Ready，所以都没撞上。
- **做法**：给内容索引的“就绪”再加一条：磁盘已经登记（`m_volumesKnown`），登记后叫醒它。`wsbench --service --content --from 文件夹` 从一份已有的索引文件夹开始，用来复现升级后的这一次。

### 列目录得到的文件大小是旧的

- **现象**：2026-10-09 测文档文字文件（`.texts`）搬家，`QDir::entryInfoList` 报的大小比索引记的少了最后追加的那 45 字节。
- **原因**：列目录（`FindFirstFile`、`QDir`、PowerShell 的 `Get-ChildItem`）读的是目录项里存的一份大小。文件开着被追加写时，这份要等 `FlushFileBuffers` 或句柄关闭才更新；按路径查（`GetFileAttributesEx`、`QFileInfo(路径)`）和按句柄查（`GetFileSizeEx`）是准的。实测：追加 45 字节后列目录 3000、按路径 3045，刷盘后两者都是 3045。
- **做法**：要准确的大小（尤其是 Win顺 自己正开着写的 `.texts`），按路径或句柄查，别用列目录的结果。`indexfolder::filesIn` 用列目录的大小只算复制进度，差一点无妨。另外，`CopyFileEx` 可以复制别的句柄正开着写的文件（同一次测试证实），复制前不必关掉写句柄。

## 剪贴板历史

做法和取舍见 [architecture.md](architecture.md) 的“剪贴板历史和搜索共用一个窗口”（`src/core/ClipStore.cpp`、`src/app/platform/ClipboardWatcher.cpp`）。

### Win+V 注册不上

- **现象**：`RegisterHotKey(Win+V)` 返回失败，错误 1409（热键已被注册）；`Win+Shift+V` 也一样。
- **原因**：资源管理器自己注册了 `Win+V`（打开 Windows 的剪贴板历史）。
- **做法**：资源管理器启动时读 `HKCU\Software\Microsoft\Windows\CurrentVersion\Explorer\Advanced\DisabledHotkeys`，写在里面的字母它不注册。加上 `V` 后要等资源管理器重启（或重新登录）才生效，所以设置里有“现在重启资源管理器”。不用低级键盘钩子去抢（为什么不用钩子见“双击 Ctrl 用 Raw Input”）。

### 空字符串写进 SQLite 变成 NULL

- **现象**：保存条目失败，日志里是 `NOT NULL constraint failed: clips.sourcePath Unable to fetch row`。
- **原因**：`QString()`（null 字符串，比如没取到来源程序时）绑定到 Qt Sql 的参数上就是 SQL 的 NULL，和空字符串 `""` 不一样。
- **做法**：文字列绑定前把 null 换成空字符串（`ClipStore.cpp` 的 `text()`）；该是 NULL 的二进制列（没有 HTML、RTF 格式）明确绑定空的 `QVariant(QMetaType::fromType<QByteArray>())`。单元测试 `tst_clipboard` 覆盖了这种情况。

### 复制一大片带格式的内容，Win顺 卡一下

- **现象**：复制几十万字、带十几 MB 网页格式和 RTF 的内容（一大片 Excel、Word）时，主线程卡 150 ms 左右，这时剪贴板窗口、启动器都没反应。
- **原因**：原来 `ClipStore` 在主线程上写 SQLite、写图片，写成功才放进列表。数据库的日志检查点也正好在提交时做。
- **做法**：数据库连接搬到 `ClipStore` 自己的写库线程上（Qt 的数据库连接只能在建它的线程上用）。复制、置顶、分组、过期清理都是先改内存，再把写库排进队列；新条目的 id 在内存里分配，接着 `sqlite_sequence` 往下排，不用等插入。要读磁盘的操作排在同一个队列里等结果，所以总能读到刚排进去的内容，包括粘贴时读全文和格式、删除时为撤销读整条、新建分组。图片写好之前不给列表图片地址，粘贴和打开图片会先等它写完。代价有两个：写库失败时，条目会先出现再消失（`Listener::lost`）；Win顺 被强行结束时，最后零点几秒的复制可能没存上。粘贴、删除、撤销这种大条目仍然在主线程上等读写（约 25、120、470 ms），要改就改成异步读、删除时只打标记。

### 从历史里粘贴，目标程序有时什么都没收到

- **现象**：剪贴板里已经是要粘贴的内容，Ctrl+V 也发到了目标窗口，文本框里却是空的。同样的步骤时好时坏：实测里把文件条目粘贴为纯文本那次失败，粘贴文字那次成功。
- **原因**：Win顺 写完剪贴板，自己的监听也收到这次变化，40 ms 后打开剪贴板读条目编号。这正是目标程序处理 Ctrl+V、打开剪贴板的时候。剪贴板同一时刻只能被一个程序打开，普通编辑框打开失败就什么也不粘贴，也不重试。
- **做法**：自己写的内容不读回来。写完记下 `GetClipboardSequenceNumber()`，监听看到同一个序号就跳过；条目往前挪，直接在写的地方通知（`ClipboardWatcher::writeNow`）。粘贴流程里任何“写完再读一遍”的操作都会和目标程序撞上。

### 日志里的 `Retrying to obtain clipboard.`

- **现象**：连续复制时，`WinShun.log` 里出现一串 `Retrying to obtain clipboard.`（两天里 161 行）。
- **原因**：这条是 Qt 自己打的。Qt 启动时就注册了剪贴板监听，剪贴板一变，每个可编辑的 QML 文本框（`TextInput`、`TextEdit`）都在主线程把剪贴板的整段文字读一遍，只为更新 `canPaste`（`qquicktextinput.cpp` 的 `q_canPasteChanged`）。剪贴板这时被别的程序占着，就睡 50 ms 重试，最多 3 次（Qt 6.12 `qwindowsclipboard.cpp`）。复制之后马上打开剪贴板的程序都会撞上：别的剪贴板工具、Windows 自己的剪贴板历史，还有 .NET 的 `Clipboard.SetDataObject(..., true)`（先 `OleSetClipboard` 再 `OleFlushClipboard`，中间一直占着）。和 Win顺 自己的监听无关，那是另一个线程直接用 Win32 读的。
- **做法**：`main.cpp` 里 `QGuiApplication::clipboard()->blockSignals(true)`。我们没用 `canPaste`，也没有别的地方连 `QClipboard` 的信号；粘贴（`QQuickTextInputPrivate::paste`、`textfield::pasteOneLine`）都是按下时才读剪贴板，不受影响。以后要监听剪贴板变化，用 `ClipboardWatcher`，不要连 `QClipboard::dataChanged`，它收不到。
- **实测**（2026-10-09）：测试脚本往剪贴板写一段文字，再占住剪贴板 80 ms，模拟别的剪贴板工具。旧版每复制一次重试 2 次（主线程卡约 100 ms），新版 0 次；Ctrl+V 往搜索框里粘贴照常。测试文字带 `ExcludeClipboardContentFromMonitorProcessing` 格式，Windows 和 Win顺 的剪贴板历史都不会记下它。

### 剪贴板页里 Alt+1、Alt+2 没反应

- **现象**：这台电脑上，剪贴板页里按 Alt+1、Alt+2 不粘贴，Alt+3 到 Alt+9 正常。
- **原因**：别的程序把 Alt+1、Alt+2 注册成了全局热键（`RegisterHotKey` 报 1409）。全局热键先于前台窗口拿到按键，Win顺 收不到。
- **做法**：用方向键加 Enter，或双击条目。测试前先用 `RegisterHotKey` 试一下要发的组合（成功就马上注销），被占用就换一个。
- **后来**：在别的程序上打开的剪贴板不抢焦点，按键由它自己的低级键盘钩子先拿到，全局热键排在钩子后面，`Alt+1`、`Alt+2` 在那里能用了（剪贴板开着时，别人的这两个热键不响应）。从 Win顺 自己的输入框打开时剪贴板还是激活的，那里照旧。

### 重启资源管理器后，任务栏没回来

- **现象**：在设置里点“现在重启资源管理器”，任务栏消失了，十几秒后也没回来；进程里只剩一个开着文件夹窗口的 `explorer.exe`。注册表里 `AutoRestartShell` 是 1。
- **原因**：原来的做法是结束任务栏所在的 `explorer.exe`（退出码非零），等 Winlogon 重新启动它。但 Winlogon 只管它在登录时启动的那个外壳；外壳之前被别的程序或用户重启过的话，结束后就没人再启动它。
- **做法**：结束前先复制这个进程的令牌，3 秒内没有新外壳就自己启动：`CreateProcessWithTokenW`，不行再以用户的 `sihost.exe` 为父进程（`winv::restartExplorer`）。不能用 Win顺 自己的令牌，那样外壳带着管理员权限，任务栏上打开的一切也都是。测完查新外壳令牌的 `TokenElevation`，应该是未提权。用户手动恢复：`Ctrl+Shift+Esc` → 运行新任务 → `explorer`。

### 粘贴偶尔没贴进去（目标窗口刚回到前台）

- **现象**：选好条目按 Enter，剪贴板里的内容是对的，原来的窗口也回到了前面，文本框里却是空的；同样的步骤再做一次又正常。
- **原因**：目标窗口一到前台就发了 `Ctrl+V`。它这时可能还没把焦点放回输入框，按键落在窗口本身，丢了。
- **做法**：到了前台再等 60 ms 才发（`Clipboard.cpp` 的 `kSettleMs`）；改后原来出错的场景连做 5 次都成功。不用 `GetGUIThreadInfo` 查焦点：UWP 程序的输入在另一个进程里，查不准，反而要等满超时。

### 钉住的剪贴板，连点两条，第二条只挪到了最上面

- **现象**（issue #8 实测，9 次里 1 次）：剪贴板钉住，在记事本里打字，隔一秒左右连点两条。第二条在历史里挪到了最上面（剪贴板写进去了），记事本里却没有它。
- **原因**（推测，没抓到现场）：目标程序一直在前台时，原来写完剪贴板马上发 `Ctrl+V`。别的程序也在监听剪贴板（Windows 的剪贴板历史、别的剪贴板工具），每次变化都会打开剪贴板读一遍；目标程序这时去打开，打不开就什么也不粘贴，也不重试（同“从历史里粘贴，目标程序有时什么都没收到”）。
- **做法**：目标在前台的这条路也走 `m_focusTimer`：等 60 ms（`kSettleMs`），剪贴板还被谁开着（`GetOpenClipboardWindow()`）就再等，最多 300 ms（`kClipboardWaitMs`），然后照发。没回到前台的那条路本来就要等。再出现时看剪贴板窗口底下有没有提示：有“那个窗口没有回到前面”，就是目标没在前台，不是这里。
- **复测**（2026-10-11）：钉住后隔约 0.7 s 连点 10 次，10 次都粘进去了；没钉住时按 Enter 粘贴，约 0.14 s 到（测量误差 ±0.3 s），没有变慢。
- **另外两处**（同一次实测）：钉住时按 Enter 粘贴后，搜索框里的字还在，下次接管键盘新打的字接在后面：现在键盘一转过来（`keysRouted`）就全选搜索框，打字就替换。合并粘贴后那几条一直是多选：钉住时粘贴完就清掉多选（`Clipboard::onWritten`）。

### 在 Win顺 自己的搜索框里按 Win+V，粘贴不进这个框

- **现象**（用户反馈）：在启动器的搜索框里按 `Win+V`，选一条按 Enter，搜索框里什么也没有，内容贴到了打开启动器之前的那个程序里。对话框旁的搜索框和设置窗口里的输入框也一样，只是提示“没有找到要粘贴进去的窗口”。
- **原因**：粘贴的目标只在启动器从隐藏状态打开时记一次前台窗口。搜索框和剪贴板页是同一个窗口的两页，剪贴板页一出来搜索框就看不见了，目标还是之前的程序；对话框旁的搜索框在启动器到前台时跟着隐藏；设置窗口是 Win顺 自己的，`paste::usableTarget` 不收。何况往自己的窗口发 `Ctrl+V` 也要先切回那一页、等焦点回来。
- **做法**：打开剪贴板页时先看键盘是不是在 Win顺 自己的输入框里，是的话粘贴时回到那个框，文字用 `QInputMethodEvent` 直接给它，不发按键（见 architecture.md 的“粘贴到 Win顺 自己的输入框”）。光标和选区要在剪贴板页拿走焦点**之前**记下：焦点在同一个窗口里移走时，`TextInput` 会丢掉选区（`QQuickTextInputPrivate::handleFocusEvent`，除非 `persistentSelection`）；窗口失去激活（`Qt::ActiveWindowFocusReason`）时不丢。切回搜索页时 `Main.qml` 还会全选一次，所以恢复选区放在它之后。

### 在 VS Code 的命令面板里按 Win+V，命令面板关了

- **现象**（用户反馈，2026-10-09）：VS Code 里按 `Ctrl+P` 打开命令面板，再按 `Win+V`（这台机器上是 `Win+Alt+V`）打开剪贴板，命令面板一下就关了，选好的条目也粘贴不进去。浏览器地址栏的建议列表、各种自动补全、下拉框都一样。
- **原因**：剪贴板窗口打开时 `requestActivate` 抢了前台。这类弹层一失去焦点就关（VS Code 的 `workbench.quickOpen.closeOnFocusLost`）。Windows 自带的 `Win+V` 面板不激活，所以不会这样（见“界面实测”里那一条）。
- **做法**：在别的程序上打开时不激活，键盘用一个只在窗口开着时存在的低级键盘钩子转过来（`KeyRouter`，做法见 architecture.md 的“不抢焦点”）。实测：测试窗口全程没有失活、失焦，`Enter` 粘贴进了 VS Code 命令面板的输入框。

### 不激活的窗口，拖标题栏时被激活了一下

- **现象**：剪贴板不激活地开着，拖它的底栏（`HTCAPTION`）挪位置，之后的 `Esc` 落进了后面的程序，剪贴板一直留着。
- **原因**：系统处理 `WM_NCLBUTTONDOWN`（`HTCAPTION`）时先要把窗口放到前台，`EVENT_SYSTEM_FOREGROUND` 报了剪贴板窗口；`App::routedForeground` 以为它被激活了，停了按键转发。可后面的程序其实没有失活（WinForms 的 `Deactivate` 没来），前台随后又是它。
- **做法**：不激活的时候，`WindowFrame` 自己接住标题栏上的 `WM_NCLBUTTONDOWN`，直接 `DefWindowProc(WM_SYSCOMMAND, SC_MOVE | HTCAPTION)` 开始拖，不经过激活那一步。`routedForeground` 也不只信事件里的窗口，处理时再看一眼 `GetForegroundWindow()`。改后拖动中每 30 ms 查一次前台，8 次都是后面的程序。

### 转给 Qt 的 Alt 按下，`event.modifiers` 里没有 Alt

- **现象**：剪贴板不激活时按住 `Alt`，前 9 行的数字不出来。`Alt` 的按键事件确实送到了 `ClipboardPage.qml`。
- **原因**：`QKeyEvent::modifiers()` 对修饰键本身的事件会把对应的位翻一下（`QInputEvent::modifiers() ^ Qt::AltModifier`）：平台插件给的是按下之前的状态，翻过之后“按下”带上它、“松开”去掉它。构造事件时已经把 Alt 加上，翻一下就没了。
- **做法**：修饰键事件的 modifiers 给按下之前的状态（低级钩子里 `GetAsyncKeyState` 读到的正是这个），由 Qt 自己翻（`KeyRouter::handleModifier`）。

### 钩子收得太早，按键的松开单独落进了程序

- **现象**：剪贴板不激活时按 `Enter` 粘贴、按 `Esc` 关闭，测试窗口在粘贴之后又收到一个单独的 `Enter` 抬起（`Esc` 同理）。
- **原因**：按下被钩子拿走，窗口随即收起、钩子卸掉，按键松开时已经没人拦，就落到前台程序里。多数程序不理没有按下的抬起，但不能指望。
- **做法**：收起时钩子先不卸，只是不再拿新按键，等它拿走过的键都松开再卸，最多 2 秒（`KeyRouter::drain`）。改后测试窗口不再收到这些抬起。

### 多行文字粘贴进搜索框，灰色的字铺满整个窗口

- **现象**（用户截图）：往启动器的搜索框里 `Ctrl+V` 一大段多行文字，框里只看到最后一行，“0 个结果”；同时一行行灰色的字铺满整个窗口，压在搜索栏和底栏上，左右两边都被截掉。
- **原因**：Qt 的单行 `TextInput` 粘贴时不去掉换行，换行留在 `text` 里，只是显示成空格（`QQuickTextInputPrivate::updateDisplayText`）。查询里带着换行，搜索什么也找不到；空状态的“没有找到“%1””用的是 `Text`，纯文本里的换行照样分行，又没限宽度和行数，就从 80 像素高的区域里上下溢出、左右超出窗口。
- **做法**：源头上，多行文字粘贴进单行的框合成一行（`App::eventFilter` + `textfield::pasteOneLine`，见 architecture.md 的“多行文字粘贴进单行的框合成一行”）。显示上，凡是把用户输入放进 `Text` 的地方，先把空白合成一个空格，再定宽度、`elide`；`ElideMiddle` 只对单行文字有效，所以合并空白这一步不能省。

### 删光了历史，clipboard.db 还是几十 MB

- **现象**：作者电脑上的 `clipboard.db` 有 29.5 MB，里面的历史只剩几条；删掉再多也不变小。
- **原因**：SQLite 删除只把页放进空闲列表，文件不缩，以后插入再用。默认的 `auto_vacuum=NONE` 永远不还给磁盘；大图、长文字删掉后，空页就一直占着。改成 `INCREMENTAL` 再定期 `PRAGMA incremental_vacuum` 也不行：Qt 的 `QSqlQuery::exec` 只走第一步，`next()` 遇到没有结果列的语句直接返回假，不会再往下走，每次只释放一页（`PRAGMA freelist_count` 前后只差 1）。
- **做法**：`auto_vacuum=FULL`，每次提交时就把空页截掉（`ClipStore::open`，要在 `journal_mode=WAL` 之前设）。已有的库不能只改这个值，得 `VACUUM` 一次才生效：打开后在写库线程上看 `PRAGMA auto_vacuum`，不是 1 就设好再 `VACUUM`（`reclaimSpace`）。`journal_size_limit` 让 WAL 用完截到 1 MB 以内。`tst_clipboard` 的 `removedEntriesFreeTheDisk` 覆盖新库和旧库两种情况。

### 剪贴板开着时，AutoHotkey 改的键不起作用（GitHub #9）

- **现象**：用户用 AutoHotkey 把组合键改成方向键（比如 `CapsLock+J` 当 `↓`），剪贴板在别的程序上开着时不起作用，搜索框里打出的是 `j`。
- **原因**：低级键盘钩子后装的先调用（`SetWindowsHookEx` 把新钩子插在链头）。`KeyRouter` 在窗口打开时才装，总排在 AutoHotkey 前面；它要的键直接返回 1，后面的钩子根本看不到。
- **做法**：剪贴板要的键先 `CallNextHookEx` 交给后面的钩子。返回非 0 说明后面有钩子拿走了，就不拿；返回 0 才自己拿下，再返回 1，前台程序照样收不到。拿下的键，它的松开和自动重复也先交给后面的钩子（返回值不管），这样 AutoHotkey 记的按键状态和实际一致（`KeyRouter::handleKey`）。不能按 `LLKHF_INJECTED` 放行：映射出来的 `↓` 正是要剪贴板接住的键（见下面“开始菜单打字”那条）。
- **实测**（2026-10-11）：两个进程各装一个低级钩子，后装的先调用 `CallNextHookEx`，前一个拿走的键返回 1，没拿的返回 0。前一个钩子在自己的过程里 `SendInput` 发出的替代键，在后装的钩子还停在 `CallNextHookEx` 里时就嵌套进来，比被替代的键先处理完，所以 `CallNextHookEx` 之前不能改任何状态。再用真的 `KeyRouter.cpp` 加一个“J 换成 ↓”的进程测：改前转给剪贴板的是 A、J、B，改后是 A、↓、B，测试窗口一个键也没收到。
- **另一半不是代码的问题**：Win顺 以管理员身份运行，Windows 不让普通权限的程序拦截或模拟发给管理员窗口的按键（UIPI）。所以启动器、对话框旁的搜索框、拿到焦点的剪贴板在前台时，普通权限的 AutoHotkey 本来就不起作用（AutoHotkey 的 FAQ 写明了）。要让它起作用，用 AutoHotkey 的 “Run with UI Access” 运行脚本，或以管理员身份运行；README 的常见问题里写了。

### 图片里的文字：Windows 自带的文字识别

- **现象和做法**（2026-10-11，GitHub #9，`TextRecognizer`）：
  - **不打包的程序也能用。** 有资料说 `Windows.Media.Ocr` 只给有 package identity（MSIX 打包）的程序用，本机实测并非如此：Windows PowerShell 5.1 和 C++/WinRT 写的测试程序都能直接调用。
  - **语言要自己挑。** 这台机器的语言列表英文排第一，`TryCreateFromUserProfileLanguages` 给的是英文识别器，中文全认不出来。做法：先找语言列表里第一个中文、日文或韩文，用装了识别器的同一语言、同一文字（`zh-CN` 在识别器里是 `zh-Hans-CN`）。中文识别器也认英文和数字，反过来不行。这台机器上可用的有 `en-US`、`zh-Hans-CN`、`zh-Hant-HK`。
  - **中文每个字是一个词。** `OcrLine::Text()` 在字和字之间加空格（“搜 索 剪 贴 板”）。按 `Words()` 自己拼：两边都是中日文字或全角标点就不加空格。
  - **透明的地方要先铺白底。** 预乘 alpha 的透明像素是黑的，黑字落在上面就认不出来。
  - **尺寸**：`MaxImageDimension` 这里是 10000（有资料说约 2600）。超过就先缩小。1560×1197 的截图约 110～130 ms，第一张多出建识别器的时间（约 180 ms）。
  - **存法**：认出的字放在 `clips.ocr` 列，NULL 表示还没读过（读过但没有字是空字符串），这样旧图只补读一次。旧库在打开时 `ALTER TABLE` 加这一列（`tst_clipboard` 的 `picturesWords`）。

### 图片里的文字：换成 PP-OCRv6（ONNX Runtime）

- **现象**（2026-10-13）：Windows 自带的识别在截图上错得多：“压铆”认成“压泖”，“选项”认成“选顶”，“Ctrl”认成“CtrI”，“13”认成“15”，“38.6”拆成“38 ． 6”。拿剪贴板里的 98 张真截图比，PP-OCRv6 tiny（PaddleOCR，Apache-2.0；ONNX 模型来自 RapidOCR 3.10）几乎都认对，现在默认用它（`PaddleOcr`）。语言列表里日文或韩文排在中文前面时还用 Windows 的（tiny 不认日文、韩文），PaddleOcr 加载失败也退回 Windows 的。
- **原因和做法**：
  - **照 RapidOCR 的默认设置跑很慢**（中位数 1.7 秒）。一是线程数默认 −1，在 32 线程的机器上用满反而慢约 3 倍；二是检测前默认把短边放大到 736（`limit_type: min`），274×65 的小条被放大 11 倍。截图里的字本来就是原尺寸：只在宽于 2000 时缩小，太高的图分段（2000 高，重叠 160）检测，不整体缩小。线程最多 4 个，用 ORT 的全局线程池，两个模型共用，不自旋，线程优先级低于普通（自己的建线程函数）。这台机器（i9-14900HX）上 98 张图 4 线程中位 136 ms，1 线程 242 ms，1902×1316 约 0.4 秒。
  - **System32 里有个旧的 onnxruntime.dll。** 不链接 onnxruntime.lib，按完整路径 `LoadLibraryExW` 程序旁边的 DLL，`OrtGetApiBase()->GetApi(ORT_API_VERSION)` 拿不到就当加载失败；C++ 头文件用 `ORT_API_MANUAL_INIT` + `Ort::InitApi`。
  - **onnxruntime.dll 要 msvcp140_1.dll**：部署时跟其他 C++ 运行库一起复制（`build.ps1`）。
  - **字表在模型里**：识别模型的元数据 `character`，一行一个字；前面补“空”（CTC 的 blank），后面补空格，数目要等于输出的类别数（tiny 6904 + 2 = 6906）。
  - **不用 OpenCV**：DBNet 后处理自己写（阈值 0.3、2×2 膨胀、八连通的块、凸包上转卡壳求最小外接矩形、框内平均分 ≥ 0.5、按 面积×1.6/周长 往外扩）。矩形往外扩后最小外接矩形还是同一方向的矩形，不用 pyclipper。行从原图切（不是从缩小的图），所以 4K 截图检测虽缩小，识别仍清楚。和 RapidOCR 的结果逐张比，字符重合 98.4%。
  - **扩过的框很高**（约 2.6 倍字高），同一行的判断：竖直方向重叠不少于矮的那个的一半。两段之间两边都是中文、间隔小于字高时不加空格。
  - **换了识别器要重读旧图**：剪贴板库 `state` 里记 `recognizer`（`ppocr-v6-tiny` / `windows`），不一样就在启动 20 秒后把所有图片重读一遍（没读过的先读），全部读回来才记下；中途退出下次再来。
  - **还认不对的**：很细的斜杠（“0 / 8” 认成 “018”，换 small 也一样）；图标常被认成字母（“X”“A”“2”）。
- **测法**：`wsbench --ocr <图片或文件夹> [--threads N] [--quiet]`，打印每张的字和用时；`tst_ocr` 用画出来的字测小字、深色、长图分段。

### 剪贴板在别的程序上开着时打不了中文（GitHub #42）

- **现象**：Win11 自带的输入法，在别的程序上打开剪贴板，按 `Shift` 切不到中文，搜索框里只打得出字母。窗口有焦点时（从 Win顺 自己的框打开、起分组名）能打中文。
- **原因**：这时剪贴板不抢焦点，按键由 `KeyRouter` 的低级键盘钩子拿走，`ToUnicodeEx` 换成字交给 Qt，不经过任何输入法；单独松开的 `Shift` 还特意先补一个 `vkE8` 再放给前台程序，免得它的输入法切换（见 architecture.md 的“不抢焦点”）。两边都不切。
- **做法**：仍然不抢焦点，中文靠本来就有的拼音搜索（打 `tupian` 就找到并高亮“图片”）。前台程序的键盘布局是中文（`PRIMARYLANGID == LANG_CHINESE`）时，单按 `Shift` 只提示一句“直接打拼音：tupian → 图片”，别的都不变；别的布局什么都不变。几处要小心：
  - **认单按**：按下时没有别的修饰键和 `Win`，松开前没有别的键、鼠标按键（滚轮也算）；`Shift` 自己的自动重复不算。补的 `vkE8` 和 `Shift` 抬起带 `kOwnInput`，在清状态之前就放过了，不会把自己这一下算成“中间有别的键”。松开照旧补键放给前台程序，之后才通知（`KeyRouter::Callbacks::shiftTapped`）；主线程看前台程序的布局（`App::clipboardShiftTapped`），是中文才让页面提示（`Clipboard::pinyinHintRequested`）。
  - **提示放在哪**：搜索框空着，就换掉占位文字，到窗口收起为止；不空，就占页脚按键提示的位置，打了下一个键（单按修饰键不算）、搜索词变了、键不再转发或过了 5 秒，就换回按键提示。不加行、不改高度：窗口可能开在光标上方，页头页脚换到另一头（GitHub #38），高度一变就要重新摆。字要短，最大字号下占位文字也放得下。
  - **试过拿焦点，放弃了**：单按 `Shift` 时让剪贴板拿焦点、把它的输入法设成中文，中文是能打了，可 VS Code 的 `Ctrl+P` 和命令中心一失去焦点就关（`closeOnFocusLost`），焦点还回去也回不来，粘贴没了去处。内容要总能粘回原来的输入框；而系统的输入法不论哪家都要有键盘焦点才组字，`workbench.quickOpen.closeOnFocusLost` 也只管得了 VS Code。
  - **拿焦点被拒不能停转发**：原来 `activateClipboard` 不管 `bringToFront` 成没成功都停转发，被拒时键回了前台程序，窗口又没激活，两头落空。现在前台确实是剪贴板窗口才停（窗口是本线程的，`SetForegroundWindow` 成功后当场就是它），被拒就接着转发。
  - **组字时按键归输入法**：窗口有焦点时（从 Win顺 自己的框打开、开在开始菜单或提权的程序上、起分组名）照常用输入法打。搜索框的 `preeditText` 不空时页面不处理按键（`ClipboardPage.qml`），`Alt+1` 不会在半截拼音时粘贴，`Esc` 不会关窗口。转发时不看这一条，残留的组字不能让按键失灵。普通键不接，输入法已经处理了；带 `Ctrl`、`Alt` 的键要接下（吞掉）。实测组字 `tu` 时按 `Alt+5`，框里成了 `tu5`，`Esc` 取消组字后 `5` 留在搜索词里筛了列表。这个 5 是 Qt 插的，不是输入法组的字：`QWindowsKeyMapper` 把 `WM_SYSCHAR` 的 `5` 当成事件文字，`QInputControl::isAcceptableInput` 只拒 `Ctrl` 和 `Ctrl+Shift`，`QQuickTextInput` 就把它插进了文字（`Esc` 只去掉了组字）。`Ctrl+V`、`Ctrl+Z` 同理会改文字。
- **验证**（2026-10-19，微软拼音，提权 SendInput，不提权的 WinForms 测试窗口和单独的测试 VS Code）：中文布局单按 `Shift` 仍在转发、测试窗口的输入法不变，空框的占位字变成提示，打 `tupian` 后提示消失；有搜索词时底栏提示约 5 秒、下一个键就收；英文布局和 `Shift+↓` 都没有提示。VS Code 里 `Ctrl+P` 开着时打开剪贴板、单按 `Shift`，快速打开框一直开着，`qing` 搜到“请”并高亮，Enter 把那条粘进了快速打开框。测剪贴板粘贴前先看 Windows 自带的历史满没满：满了（25 条）时粘贴一次会挤掉用户最老的一条，删掉测试那条也回不来。

### Ctrl+A 选了全部条目，按 Delete 却删了搜索框里的字（GitHub #55）

- **现象**：搜索框里有字时按两次 `Ctrl+A`（第一次选字，第二次选条目），搜索框的字仍是选中的，`Delete` 被当成“删字”让给了输入框，条目没删。
- **做法**：`ClipboardPage.qml` 里有选中的条目时，`Delete` 不再看搜索框的选区和光标（`Ctrl+C` 早就是这样）。`Ctrl+A` 只在搜索框没字或字已全选时才选条目；选条目用 `ClipModel::selectListed` 一次换掉整个选择、只发一次 `dataChanged`，几千条也不卡。

### 从历史里粘贴 Word、WPS 的公式，粘出来是死图或一行字（GitHub #58）

- **现象**：用户想把 WPS、Word 里公式编辑器写的公式复制下来，从 Win顺 粘进别的文档，还要能预览。原来 Word 自带的公式在列表里是一行线性文字（`x=(-b±√(b^2-4ac))/2a`）；嵌入的公式对象（Equation 3.0、MathType、WPS 的公式编辑器）粘回去是一张改不了的图。
- **原因**：只存文字、HTML、RTF，没有文字时只存图片。2026-10-22 用 Word 2024（LTSC，16.0.17932）实测：单独复制一个公式，给 `MathML`、`MathML Presentation`、`Object Descriptor`、RTF（76 KB，带 `\moMath`）、文字（线性格式）、EMF、`Embed Source`（Word 文档类）、`Native`、`Link Source` 等，`HTML Format` 要了也不给；文字加 RTF 放回去，Word 就粘成公式（`OMaths.Count` 1），所以 Word 的公式缺的只是预览。单独复制一个嵌入对象（用 Package 代替，这台机器没装公式编辑器），给 `Embedded Object`、`Object Descriptor`（对象自己的类）、RTF（带 `\objdata`）、EMF、PNG、DIBV5，没有文字：原来只存 PNG，粘回去是图片（`InlineShape.Type` 3）。文字里夹着公式时不给 `MathML`，靠 RTF 粘回去，照旧是文字条目。
- **做法**：见 architecture.md 的“公式和嵌入的对象照原样粘回去”。实测（`ClipboardWatcher` 的源码编进一个小程序，对着放回剪贴板的 Word 原格式判断、存、再由 `writeNow` 写出，粘进新建的隐藏文档）：公式 `OMaths.Count` 1，对象 `Type` 1、ProgID 仍是 Package；预览图是 EMF 画出来的，275 × 87。
- **测试从 Office 复制，不进两边的历史**：复制一返回就去开剪贴板（每 1 ms 试一次），读出全部格式，清空后原样放回，带上 `ExcludeClipboardContentFromMonitorProcessing` 和 `CanIncludeInClipboardHistory = 0`。Win顺 等 40 ms 才读，读到的已经带标记，不记；之后的粘贴测试都从存下的格式放回去（带标记）。只用 Word 的 COM：已开着的 Word 里 `Documents.Add(..., Visible: false)`，测完不保存关掉自己的文档。WPS 没测（这台机器上没装），它复制公式对象给不给 `Embedded Object`、`Embed Source` 加 `Equation.KSEE3` 的类，要在装了 WPS 的机器上看。

## 搜索面板（Ctrl+E，linux.do #209）

### 面板里的筛选输入框把键盘留住了

- **现象**（2026-10-15 测试脚本）：在“自定义”日期框里输完日期，接着按 Ctrl+A、退格、打字、Esc，全进了那个小框：搜索词没清掉，Esc 也关不了窗口，脚本后面按坐标点的都点偏了。
- **原因**：InputBox 里的 TextInput 拿到焦点后一直留着；启动器的按键处理只接搜索框发来的 `keyPressed`，别的输入框不认识 Esc、Ctrl+A。
- **做法**：`Panel.qml` 的 `BoundBox`：Enter 和 Esc 都把键盘交还搜索框（`SearchBar.focusAtEnd()`，光标放在词尾、不全选），Tab 跳到旁边那个框。展开、收回时也用 `focusAtEnd`，不像打开时那样全选：用户正在打的词不该被下一个键替掉。

### 两个搜索框，`findChild("searchField")` 找到的是面板那个

- **现象**：面板有自己的一份 `SearchBar`（和框的那份换着显示，各自有 TextInput），`App` 用 `findChild<QQuickItem*>("searchField")` 给任务栏框里的输入法定位，`findChild` 按创建顺序取第一个——面板在 `Main.qml` 里写在前面，拿到的就是面板的框。
- **做法**：面板那份的 `objectName` 另起名（`panelSearchField`）。同一个 `objectName` 以后再出现第二份时先想想谁在 `findChild`。

### 展开、收回时窗口尺寸的绑定要让开

- **现象**：`Main.qml` 的 `width: 760`、`height: layout.implicitHeight` 是绑定；C++ 给面板 `setGeometry` 之后，只要列表增减一行、`layout.implicitHeight` 一变，窗口就被绑定拽回框的尺寸。
- **做法**：用 `Binding { when: !window.panel; restoreMode: Binding.RestoreNone }` 挂这两个绑定：面板时绑定整个拿掉，大小归原生窗口（用户拖边框改大小）；收回时绑定回来，窗口自己缩回去，然后再 `placeOn` 放回原位。

### 测试脚本：先确认是框还是面板，再按坐标点

- **现象**（2026-10-15）：上一批命令最后把面板收回成了框，下一批还按面板的坐标点“止”输入框，点在了框的第 7 行上——打开了一个 zip（Bandizip 弹出来）。守卫只查前台是不是 Win顺。
- **做法**：`agent.ps1` 加了 `need panel` / `need box`：先 `--dump` 读出当前模式，不对就整批作废；每批开头都写一条。在结果行附近点之前先打一个没有结果的词（zqxwvj）这一条以前就有，这次没照做。另外两条顺手记下：`--profile <文件夹>` 的设置文件在 `<文件夹>\WinShun\WinShun.ini`，不是 `<文件夹>\WinShun.ini`（`QSettings::setPath` 的 UserScope 会再加一层组织名）；测试数据里的反斜杠别经 bash 的 heredoc 写（会被吞掉，favorites.json 里的路径成了 `F:wspaneltestdocs`），用 PowerShell 或编辑工具写。

## 任务栏搜索（任务栏按钮、Win+S）

### Windows 自带的搜索按钮隐藏不了

- **现象**：往 `HKCU\Software\Microsoft\Windows\CurrentVersion\Search` 写 `SearchboxTaskbarMode=0`（资料上的隐藏方法）返回错误 5（拒绝访问）。键的权限明明给了用户完全控制。
- **原因**：Windows 11 的用户选择保护驱动（UCPD，`sc query ucpd` 在运行）挡住了这个值：PowerShell、自己编译的程序、管理员权限的进程，2026-10-09 在 25H2（26200）上全都写不进去。只有 Windows 自己（设置应用、资源管理器）能改。
- **做法**：不改，只读（组策略 `HKLM\SOFTWARE\Policies\Microsoft\Windows\Windows Search` 的 `SearchOnTaskbarMode` 优先）。设置里显示它还在不在任务栏上，在的话给一个按钮打开 `ms-settings:taskbar`，让用户自己选“隐藏”。EverythingToolbar 的设置向导也是让用户自己去关（它的“隐藏”开关直接写这个值，在 25H2 上同样写不进去，只记一行日志）。

### 拦下 Windows 搜索按钮的点击，它的效果也没了

- **现象**：2026-10-10 试过让 Windows 自带的搜索按钮打开 Win顺（低级鼠标钩子拦下点在它上面的左键）。功能是通的，可搜索框的按下、展开效果都没了，“输入框完全是一个按钮了”；打开时放大镜也不像 Windows 自己的那样变成彩色。
- **原因**：这些效果是资源管理器在处理这次点击、打开它自己的搜索时才播放的。点击被拦下，它就不知道有这回事。没有正规的接口能让它照常播放效果、打开的却是别的程序。
- **做法**：不接管 Windows 的，在任务栏上放 Win顺 自己的入口（固定的按钮），推荐用户隐藏 Windows 的搜索（EverythingToolbar 也是这样）。

### 点任务栏上自己的子窗口，前台窗口照样失去激活

- **现象**：Win顺 的搜索框是任务栏的子窗口，`WM_MOUSEACTIVATE` 返回 `MA_NOACTIVATE`，本意是点它时启动器保持在前台。实测（2026-10-10）点一下，启动器还是收到失去激活，框的线程处理这次点击时 `GetForegroundWindow()` 已经是空的。
- **原因**：点在别的输入队列的窗口上，系统在交给那个线程之前就把前台从原来的队列拿走了；`MA_NOACTIVATE` 只决定被点的这一边激活不激活（顶层窗口要不抢前台，得有 `WS_EX_NOACTIVATE`，而这里的顶层窗口是资源管理器的任务栏）。
- **做法**：接受它失去激活。失去激活时鼠标在框上，就先不隐藏，等框报来这次点击再把启动器拿回前台（`App::m_boxPressGrace`，400 毫秒没报来才隐藏）。收到点击的是 Win顺 的线程，`SetForegroundWindow` 是允许的。

### 打开 `ms-screenclip:` 弹出“你要如何打开”

- **现象**：`ShellExecute("ms-screenclip:?source=HotKey")`（网上的写法），以及带三个斜杠的 `ms-screenclip:///?source=HotKey`，都弹出“打开方式”对话框，截图界面不出来。协议明明注册给了截图工具（`HKCR\AppXfeq5…`，UserChoice 也指向它）。
- **原因**：没查清，看样子是这台电脑上 UserChoice 的校验没通过，Windows 就当没选过。资源管理器按 `Win+Shift+S` 时不走用户的关联：截图工具进程的命令行是 `SnippingTool.exe ms-screenclip:///?source=HotKey`，父进程是 explorer.exe。
- **试过、不行的**：截图工具的命令行别名 `SnippingTool.exe ms-screenclip:///?source=HotKey`：它已在后台运行时打开的是它的主窗口（“新建”按钮那个），不是截图遮罩（别名启动算“普通启动”，不是协议激活）。`IApplicationActivationManager::ActivateForProtocol`：返回 0x80270254，不支持这种完全信任的桌面打包应用。
- **做法**：`ShellExecuteEx` 加 `SEE_MASK_CLASSNAME`，`lpClass` 填截图工具自己为 `ms-screenclip` 注册的类。类名从 `HKCU\Software\Classes\Local Settings\Software\Microsoft\Windows\CurrentVersion\AppModel\Repository\Packages\Microsoft.ScreenSketch_*\App\Capabilities\URLAssociations` 的 `ms-screenclip` 值读，不写死（`src/stub/main.cpp`）。截图工具在不在运行，都直接出遮罩。

### 管理员进程启动的截图工具也是管理员

- **现象**：Win顺 拿到 `Win+Shift+S` 后自己用上面的办法打开截图，截图工具的完整性级别是“高”（0x3000）；普通权限的测试脚本按 Esc 关不掉它的遮罩（UIPI 丢掉了按键），遮罩一直盖在屏幕上，只能用管理员权限结束进程。
- **做法**：交给 `WinShunSearch.exe --screenclip`，经资源管理器以普通权限启动它（`shell::run`），由它去打开截图。实测截图工具的完整性级别是“中”（0x2000）。

### 关掉 Win+S 后，截图键没了

- **现象**：设置里关掉“用 Win+S 打开”，重启资源管理器后，`Win+S` 回到了 Windows，`Win+Shift+S` 却谁都不响应（`RegisterHotKey` 一试，它空着）。
- **原因**：关掉以后、资源管理器重启之前（releasing），Win顺 仍注册 `Win+Shift+S` 来替用户截图。资源管理器重启时，`TaskbarCreated` 让 Win顺 重新登记热键，这时新的资源管理器还没注册它的键，Win顺 抢先拿到 `Win+Shift+S`；资源管理器注册失败，不会再试。3 秒后 Win顺 再登记一次时发现 `Win+S` 已是资源管理器的，就放掉了 `Win+Shift+S`，于是它空着。
- **做法**：只在 Win顺 确实拿着 `Win+S` 时注册 `Win+Shift+S`。资源管理器的热键只在启动时注册一次：它启动前后的这段时间里，Win顺 不要占着它要的键。

### 请求固定到任务栏：一直“不允许”

- **现象**：小启动器在前台、开始菜单里也有它的快捷方式，`TaskbarManager.IsPinningAllowed` 却一直是 false，只能退回到打开文件夹让用户自己固定。桌面程序支持（`ITaskbarManagerDesktopAppSupportStatics`）在，这台机器也不需要受限功能的令牌。
- **原因**：未打包的程序靠 AppUserModelID 认“开始菜单里的哪一项是我”。程序没设明确的 ID、快捷方式上也没有，就对不上。
- **做法**：程序启动时 `SetCurrentProcessExplicitAppUserModelID(L"LingCore.WinShun.Search")`，开始菜单快捷方式写上同一个 `PKEY_AppUserModel_ID`（安装程序的 `AppUserModelID:` 参数，或 `taskbar::buttonShortcut` 补写），写完 `SHChangeNotify(SHCNE_UPDATEITEM)`。实测写上后约 3 秒变成 true，所以请求前最多等 6 秒。
- `propkey.h` 要放在 `shobjidl.h` 之后包含，否则 `PROPERTYKEY` 还没定义，报一串语法错误。

### 上一个固定请求没回答，新的就不允许

- **现象**：Windows 的问题是右下角的一条通知。测试时结束了还在等回答的小启动器，通知却还在；再点“固定”，`IsPinningAllowed` 又是 false，走了退路。
- **原因**：同一个程序的请求还挂着时，不能再请求（微软示例里写着）。
- **做法**：不用处理，退路本来就能让用户自己固定。测试时别结束正在等回答的进程，让通知被回答或过期。另外：请求一发出就关掉为它弄到前台的透明窗口，否则等待期间用户打的字都进了这个看不见的窗口（第一版就是这样，挂了好几分钟）。

### 开始菜单打字：别按“是不是注入的”放行按键

- **现象**：第一版钩子放过所有注入的键（`LLKHF_INJECTED`），本意是放过 Win顺 自己重新发的键和关开始菜单的 `Esc`。结果测试脚本模拟的打字一个都截不到；用户用 AutoHotkey、PowerToys 改过键位，或用屏幕键盘打字时也一样。
- **做法**：Win顺 自己发的键都带 `kOwnInput` 标记（`dwExtraInfo`），钩子只放过带标记的。关开始菜单的那个 `Esc`（`win::bringToFront` 里的 `closeShellFlyout`）以前没带标记，补上了。

### 截到的键经过输入法

- **现象**：开着微软拼音（中文模式）在开始菜单里打 `not`，转到 Win顺 的搜索框后是输入法的组字（候选“农田、弄他”），不是直接搜索；接着按 `Esc` 取消的是组字，Win顺 不关。
- **原因**：截到的键重新发给 Win顺 时，和用户直接在搜索框里打字一样经过输入法。在 Windows 自带的搜索里打字也是这样，是有意的：用户打的是拼音还是英文，由他自己的输入法状态决定。测试脚本按 `Esc` 想关窗口时，要按两次，或先确认没有组字。

### 任务栏搜索框为什么固定位置

- **现象**：0.7 到 0.11 的任务栏搜索框跟着任务栏的图标走（在图标旁边，或在开始按钮右边的空当里，还能拖），开关窗口时总比图标晚几十毫秒，或者先跳到一边再回来。为了在开始按钮右边留出空当，还得往资源管理器里注入 WinShunTap.dll，和 TranslucentTB 这类工具一起开时会一直停在“正在留出位置…”（GitHub #35）。2026-10-20 用户定下改成固定位置，去掉空当、拖动和记住的位置。
- **原因**（这几轮实测和读源码得来，以后换做法还用得上）：
  - 图标一个接一个地滑，后一个比前一个晚约 16.6 毫秒（一帧），往哪边滑就从那边的图标先开始。逐帧比对屏幕，每个图标约 255 毫秒滑完；按动画本身算是 333 毫秒，曲线约 `(0.4,0,0.2,1)`，最后几十毫秒挪不到 1 像素，两个数说的是同一段动画。一排图标全部停下，要 333 毫秒再加上图标数乘 16.6 毫秒。
  - 位置变化的 WinEvent 只在开头来（隐藏的旧 `Start` 窗口最先动，比图标还早），滑的过程中一个也没有。所以“最后一个事件之后等多久”要按整段滑动来算。
  - UI 自动化走在屏幕前面：还没轮到的图标先报终点，再跳回原处，然后才滑。走一个按钮时，开始按钮从约 20 毫秒起一直报终点，到约 400 毫秒为止（按钮越多越久）。
  - 读数本身慢，还要看资源管理器忙不忙：加按钮时每段的第一次读数要 18–94 毫秒，滑的过程中最慢 59–151 毫秒；连框架外的托盘图标一起搜整棵树（`TreeScope_Descendants`）要 48–164 毫秒。跟着读数走的框，读数晚多少它就晚多少；按速度往前推，图标减速时框又会往回晃。
  - XAML 诊断（`InitializeXamlDiagnosticsEx`）可以有好几个使用者，但树的变化只告诉最后连上的那个（WinUI 的 `DiagnosticsInterop` 只记一个）：TranslucentTB 在我们之后连上，我们的 DLL 就看不到新建的开始按钮，边距设不上（#35）。DLL 一旦被资源管理器加载就卸不掉，要等它重启。
- **做法**：
  - 不跟图标走：图标居中时放在任务栏最左端（那里有小组件按钮就放在它右边；经典任务栏放在开始按钮右边），图标靠左时放在右端、托盘的左边。图标来来去去碰不到它，只有那一端的东西变了（小组件出现、托盘变宽）才换位置，直接跳过去，不滑。经典任务栏（StartAllBack、ExplorerPatcher）也一样。
  - 每种样子只有一个宽度：整框 240 逻辑像素、图标加“搜索”、仅图标（44）。放不下就换窄一档，连图标都放不下就先藏起来（`Hidden::NoRoom`）。
  - 不逐帧量。变化后马上量一次，之后每 125 毫秒量一次，直到安定；这些没安定的读数只在图标或托盘真压到框上时才用（马上变窄或藏起来），不拿来变宽、挪位置。最后一个事件过去约 1 秒、相隔约 250 毫秒的两次读数一样，才算安定，再按它放；事件一直不停，最多等约 4 秒。
  - 不再往资源管理器里放 DLL。安装、卸载时删 `%ProgramData%\WinShun\tap`；被资源管理器占着的那份登记成重启后删（`MoveFileEx` 加 `MOVEFILE_DELAY_UNTIL_REBOOT`），删不掉也不让安装失败。设置里旧的 `Taskbar/StartGap` 和 `state.ini` 的 `[SearchBox]` 读到就删。
- **怎么查**：scratchpad 里的 `tbmotion`（一个不抢前台的窗口进出任务栏，同时每毫秒读一次框的 `GetWindowRect`，逐帧记下屏幕上任务栏那一条）和 `tbcrowd`（一个个加按钮把任务栏挤满再拿走；每个窗口一个 AppUserModelID，免得合并成一个按钮）。

### 任务栏上的框描边发虚

- **现象**：用户说框的描边“看着不正常，有的发虚”。150%、直角，逐像素读：上边、左边 1 行，下边、右边 2 行，而且那 2 行深浅不一（亮度 93、84）。
- **原因**：描边是 1 逻辑像素 = 1.5 物理像素，没取整。直角关了抗锯齿，1.5 像素宽的线按像素中心落格，靠外的两边落 1 行、另两边落 2 行；圆角开着抗锯齿，也是糊成两行半透明的。另外底色只铺到描边的中线，描边内侧压在底色上、外侧没有，同一条边的两行颜色就不一样。我们的 Qt 窗口（`WindowEdge.qml`，`border.width: 1`）在 150% 下实测是 2 行一样亮的像素，Qt 把它取整了。
- **做法**：描边宽度按 Qt 的办法取整（`std::round(s)`，至少 1），底色铺满整个框，描边整条画在上面（`TaskbarSearchBox.cpp`，图标形态的那圈边也一样）。改完四边都是 2 行，每边两行亮度相同（差 1 是背景渐变），和启动器的边一致。框的位置总在整像素上（2026-10-20 起不再滑动；以前滑动中按小数位置画，那时的边虚一点看不出来）。
- **怎么查**：截下框的窗口，横竖各读一行像素的亮度，数每条边几行、每行多亮；别只看放大图。

### 透明任务栏（TranslucentTB）上，收起后启动器的一截留在任务栏里

- **现象**：用户用 TranslucentTB（桌面状态 `blur`，颜色全透明）。从任务栏上的框打开再收起后，任务栏左半段发白，隐约是启动器顶上“全部 / 文件 / 内容”那一排，一直不走；点一下别的窗口才消失。截图总是截不到：截图的快捷键会换前台，残留先没了。
- **原因**：那时启动器收起是滑到任务栏后面（滑动期间放在任务栏下面一层），打开也从后面滑出来。这个做法默认任务栏不透明。任务栏透明或模糊时，滑动中后面的启动器整个透出来（实测任务栏那一条的平均亮度 34 → 60 多）；窗口隐藏后，任务栏的模糊还停在最后一帧的样子，直到前台变化（TranslucentTB 在前台变化时重新设一次效果）。2026-10-17 实测（150%，后台每 30 毫秒用 `CopyFromScreen` 量开始按钮左边那段的亮度，同时读启动器的 `IsWindowVisible`、位置）：`vis=0` 之后亮度停在 62.3 好几秒一动不动，存下的图里正是模糊的“全部 / 文件 / 内容”，宽度和启动器一样（447–1587）。所以它在合成后的画面里，普通截屏能截到，只是用户手动截图时会先触发前台变化。
- **做法**：不再进任务栏后面。打开淡入、收起淡出，只在窗口和任务栏之间那段空隙里移动（`Placement::nearOffset`，最多 `kTaskbarGap` 12 逻辑像素），整个过程都在任务栏上方（用户的要求：“一切操作都应该在任务栏上面完成”）。下面三条（插到任务栏后面、滑动中被激活、z 序里排在前台窗口后面）因此都没有了，相关代码（`goBehindTaskbar`、`m_under`、`hiddenOffset`、logo 的 `setOverTop`/`slideUnder`）一起删掉。
- **怎么查**：看不见的残留先别猜，开一个后台脚本，按帧记屏幕上那一条的亮度和窗口状态，让用户复现；亮度跟着 `vis` 变还是停在原处，一眼就分清是窗口还在还是画面没刷新。别用 `Measure` 当 PowerShell 函数名，它是 `Measure-Object` 的别名，优先于函数。

### 启动器从任务栏后面滑出，却盖在任务栏上面

- （2026-10-17 起不再滑到任务栏后面，见上一条；这一条和下面两条留作记录。）

- **现象**：启动器在任务栏上方打开时从下往上滑，滑的头一段压在任务栏上面，不像开始菜单那样从任务栏后面出来。
- **原因**：两个都是置顶窗口，激活的启动器排在任务栏前面。用窗口区域（`SetWindowRgn`）把下面裁掉也不行：有窗口区域的窗口，DWM 不再给它圆角和阴影。
- **做法**：滑动前把启动器插到那个任务栏窗口后面（`SetWindowPos(hwnd, 任务栏, …, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE)`，两个都还是置顶的，激活状态不变），任务栏自然挡住还没出来的部分（EverythingToolbar 也这样做）。滑到下面一个显示器上会露出来：只滑到不跨过去的地方（`Placement::hiddenOffset`）。

### 滑动途中被激活，启动器又盖到任务栏上面

- **现象**：有时启动器滑动时压在任务栏上面（用户说“有概率会遮挡任务栏”）。实测：正常打开、按 Esc 收起都没事；展开途中又点一下框，或者收起途中再点框把它叫回来，有 40～140 毫秒盖在任务栏上。
- **原因**：上一条只在滑动开始时把它插到任务栏后面一次。之后激活它（`SetForegroundWindow`、`BringWindowToTop`、Qt 的 `requestActivate`）会把它提到置顶窗口的最前面。收起途中再点框时，`showLauncher` 甚至是先调 `slideIn`，再激活。
- **做法**：滑动期间在子类化的窗口过程里接 `WM_WINDOWPOSCHANGING`，只要这次要改 z 序（没有 `SWP_NOZORDER`），就把 `hwndInsertAfter` 改成那个任务栏窗口，滑完再放开（`Placement::m_under`）。谁来提、什么时候提都一样，一帧都露不出来。改完再测，6 种情况采样都是 0。

### 收回途中再点框，启动器又被关上

- **现象**：按 Esc 后启动器往下收，途中马上再点框：有一次它滑回来了一下又接着收下去；同样的操作再测就正常。
- **原因**：点任务栏让启动器失去焦点，Qt 收到这个消息和收到框报来的点击，先后不一定。点击先到时启动器已经重新打开，失去焦点后到，又走了“点别处就关”的路。
- **做法**：框模式下，打开后半秒内失去焦点也先等 400 毫秒（和点框时一样，`App::m_launcherShown`），之后真没拿回焦点才关。只在前台是空的（点框后就是这样）时等，见下一条。

### 连续快速展开、收起，收起慢半拍

- **现象**：点框展开后马上点别处（桌面、别的窗口），启动器过了 400 多毫秒才开始往下收，620 毫秒才隐藏；慢慢点就没事。
- **原因**：上一条的“打开后半秒内失去焦点先等 400 毫秒”不分焦点去了哪里，用户真要关的那一下也等。另外收起曲线用的是 Windows 规范里“温和的退场” `(1,0,1,1)`，头 70 毫秒只走 4%，又显得晚了一截。
- **做法**：先看前台：点框后前台是空的，开始菜单收起时是它那一类；换成了别的窗口（任务栏也算，除非鼠标在框上），就是点到了别处，马上收起（`App.cpp` 里 `activeChanged` 那段）。收起曲线换成 `(0.3,0,1,1)`，一开始就动。实测 20 毫秒内开始收、213 毫秒隐藏。

### 打开时停稳了又抖一下

- **现象**：双击 Ctrl 打开启动器，滑上来停住之后又“抖一下”（用户的话）。
- **怎么量**：`dupcap` 探测程序（scratchpad）用 DXGI 桌面复制抓下打开过程中 DWM 合成的每一帧（这台 240 Hz，约 4 毫秒一帧），同时每毫秒记一次窗口矩形；再逐帧和最后一帧比，看边框、搜索图标、文字、各行图标各自偏了多少，以及停下之后每帧哪里还在变。窗口矩形本身一路单调，没有回跳。
- **原因**：内容比窗口晚一点跟上的那段位移（`Placement::contentShift`，`Main.qml` 里 `layout` 的 `Translate`）是逻辑像素、带小数，没对齐到设备像素。和“列表滚动时整行在抖”同一个道理：150% 下文字和按最近邻采样的图标各自取整。曲线 `(0,0,0,1)` 的尾巴很长，位移小于 1 像素时还要跑几十毫秒：边框和文字都停了，各行的文件图标才一行接一行往上跳 1 像素（250–300 毫秒），位移最后归零时整条搜索栏又跳一下（约 325 毫秒）。行里的取整补偿（`resultRow` 的 `Translate`）只算了 `layout.y`，管不到这个 transform。
- **做法**：`stepSlide` 里把位移按设备像素取整再交给 QML，和窗口本身一样一格一格地走，小于半个像素时就已经是 0。改完再测 3 次：内容约 247 毫秒停下后，画面上只剩 logo 出现和光标闪烁。
- **还有一处**：用户接着说“搜索框里的文字也在变”。位移取整以后，搜索框的字在滑动的最后两三个像素里仍比停下时高 1 像素（字形底边那几行不一样）。原因同上一条的第二层：搜索栏里的图标、输入框、范围切换都是 `anchors.verticalCenter` 居中，150% 下落在半个设备像素上。做法和结果行一样，`y` 用 `onPixel(...)` 放到整设备像素上（`SearchBar.qml`）。

### 类型条收起时，底栏闪没了一帧

- **现象**：在“文件”里按 Shift+Tab 回到“全部”，类型条收起、窗口变矮的那一下，底栏（排序、快捷键提示）整个不见了约 8 毫秒（240 Hz 下两帧，60 Hz 下一帧），再出现在新的底边。
- **怎么量**：和上一条一样用 DXGI 桌面复制抓每一帧（scratchpad `ts/stripcap.exe`，只抓窗口左边 160 像素宽的一列，不到 1 MB 一帧），每毫秒记窗口矩形；再拿最后一帧里底栏的排序图标和第一行的图标，逐帧上下找它们在哪。
- **原因**：窗口变小后，DWM 先拿交换链里上一帧裁掉底部来显示，直到新尺寸的一帧画好。上一帧里底栏还在旧的底边，正好在被裁掉的那 60 像素里。只是等 `frameSwapped` 也不够：那时帧只是交给了系统，紧接着改大小，这一帧就被丢掉，屏幕上仍是更早那帧（实测，加了等待也照样闪一帧）。
- **当时的处理（已被下节方案替换）**：类型条收完后先不还高度，只把底栏画到它最后的位置（`Main.qml` 的 `stripLeaving`，底栏的 `Translate`），`WindowFrame::afterNextFrame()` 等这之后同步的那一帧交换完、再 `DwmFlush()` 一次（等 DWM 合成过它），发 `frameShown` 时才还高度。被裁掉的就只是空白。实测两次收起，底栏都直接从旧位置落到新位置，没有缺底栏的帧；只剩窗口底边那条 1 像素的系统边框晚了约 12 毫秒才补上。打开时窗口先长高，上一帧的内容留在上面、下面是空白，底栏晚两帧落到底，不刺眼，当时没改。

### 范围双向切换：内部有动画，外框却不同步（2026-10-17）

- **根因与修正**：补上双向动画后，继续预留完整高度会让外层边框停住，内部先收起、窗口最后才跳变。`Main.qml` 已移除 `stripLeaving`、空白占位和底栏的补偿位移；`TypeStrip.height` 直接参与窗口高度，边框、列表和底栏共用同一进度。`afterNextFrame` 仍供展开面板收回时使用。
- **覆盖范围**：“全部 → 内容”“内容 → 全部”“全部 → 文件”“文件 → 全部”共用 200 ms OutCubic 展开、收起；“文件 ↔ 内容”筛选条高度相同，只更换芯片。动画中途反向从当前高度接着走；收起时保留原芯片、选中项和提示，立即停用点击；系统关闭动画时直接到位。任务栏上方的满高窗口由列表让出筛选条空间，窗口保持满高。
- **D3D11 的首次 resize 停顿**：直接逐帧缩放时，本机默认交换间隔下首次变化要等约 118–127 ms，原生高度会一次跳 51–57 个设备像素，整段只有 4–8 个不同高度。搜索窗口在 `create()` 前单独用 `QSurfaceFormat::setSwapInterval(0)`，避免呈现等待拖住这段缩放；其它窗口不改。只在 QML 中调时长、曲线或延后收回高度，解决不了这个呈现等待。
- **验证方法踩坑**：只看内部布局或动画终点，会漏掉外框提前变大、最后跳小的问题；需同时检查 `GetClientRect` 的原生客户区、底栏底边和桌面上的边框像素。同步 `grabWindow()` 会扰动 GPU 动画时钟，软件离屏结果也不能代表 D3D11 桌面表现；最终用自然事件循环、独立线程桌面取样。临时空索引还会异步清空结果、改变列表高度，测动画时应固定结果区，避免把搜索结果变化当成动画故障。
- **本次验证及边界**：实际编译的 `Main.qml`、真实 Launcher 和原生窗口，在隔离资料与固定结果区下验证。D3D11、150% 缩放测到展开 26、收起 28 个不同原生高度，最大一步分别 7、5 个设备像素；底栏与原生底边无偏差，48 张展开、46 张收起的独立桌面截图未发现边框缺失。100%、200% 的软件渲染检查也通过；中途反向、“文件 ↔ 内容”和组件禁用动画检查通过。系统动画设置未改动，未把固定结果区测试当成真实文件搜索端到端验证。
- **本地复查记录**：`build/strip-animation-validation-20261017/verify.ps1` 与 `probe.cpp` 是本次临时验证工具；最终 GPU 结果在 `outer-window-format0.log`，100%、200% 软件结果在 `outer-software100.log`、`outer-software200.log`。打包程序另做了 D3D11 预览冒烟验证，退出码为 0；可执行文件与 SHA256 记录在 `outer-staged.json`。这些是本地构建产物，旧版进程和用户资料均保留。

### 启动器旁边的 logo 停稳后才冒出来

- **现象**：用户说左上角的 logo“动画不对”。窗口滑上来、停稳，过 70 毫秒左右 logo 才突然出现；收起时它先没了，窗口才往下走。
- **原因**：滑动期间特意把它藏起来（`WindowLogo::conceal`），怕它从任务栏上冒出来；滑完的回调里才 `reveal`。
- **做法**：让它跟着窗口走（`WindowLogo::follow(Placement*)`）。每次移动（`WM_WINDOWPOSCHANGED`）和每一步（`Placement::slideStepped`）都按 `slideOffset()`、`slideOpacity()` 摆：边沿按停下的位置选（不然移动途中可能因为“不在工作区里”换到侧边），再加上偏移；透明度跟窗口一样。（以前还跟着窗口待在任务栏下面一层，2026-10-17 不再滑到任务栏后面，这部分删了。）实测（`dupcap`）：每一帧 logo 和窗口上边沿偏移一样，打开、收起都一起淡入淡出。

### 从任务栏框打开，启动器整个看不见

- **现象**：前台有别的程序（VS Code）时点任务栏上的框，启动器的窗口位置、可见、没有隐藏，透明度 255，屏幕上却一点都没有，只看得到 logo。前台什么都没有时正常。
- **原因**：滑动期间把启动器插在任务栏后面（`SetWindowPos(hwnd, 任务栏, …)`）。可 Windows 11 上，前台的普通窗口（不是置顶的）常常在 z 序里排在置顶的 `Shell_TrayWnd` **上面**（`GetTopWindow` 往下数就能看到），只是两者不重叠，平时看不出来。插在任务栏后面，也就在那个窗口后面。滑完后也没有任何代码把它往前提，所以一直看不见。
- **怎么查**：`dupcap` 每毫秒顺着 `GW_HWNDPREV` 往上找第一个和启动器重叠的可见窗口。滑动中是 `Shell_TrayWnd`，滑完那一刻变成 `Chrome_WidgetWin_1`（VS Code）。
- **做法**：插之前先把任务栏提到置顶窗口的最前面（`SetWindowPos(任务栏, HWND_TOPMOST, …, SWP_NOACTIVATE)`，点一下任务栏时 Windows 也这样做）。滑完（以及被按住拖走、`settleSlide`）时把启动器提回 `HWND_TOPMOST`。

### `QVariantAnimation` 停着时改起止值，会按旧进度报一次值

- **现象**：启动器每次收起的头一下，窗口先被推到任务栏后面（滑动的终点），一两毫秒后又回到原处再往下滑，可能闪一帧。每几毫秒读一次 `GetWindowRect` 才看得出来。
- **原因**：上一段动画跑完后停在终点（`currentTime` 等于时长）。这时 `setStartValue`、`setEndValue`、`setDuration` 会按这个进度重算当前值并发 `valueChanged`，而接这个信号的代码已经换成了这次的动画，窗口就被放到这次的终点。半路打断再反向（收起途中再点框）也一样，会被放到这次动画的半路。
- **做法**：设起止值时用 `QSignalBlocker` 挡住信号，再自己走第 0 步、`start()`（`Placement::startSlide`、`glideTo`）。

### 等屏幕的下一帧：别用 `DCompositionWaitForCompositorClock`

- **原因**：Windows 11 的这个函数在显卡被重置（混合显卡的笔记本睡眠后）、远程桌面断开后，可能永远卡在内核里等一个不会来的垂直同步，进程成了结束不掉的僵尸（Zed 编辑器踩过，zed-industries/zed#36934，后来退回 `DwmFlush`）。
- **做法**：框的动画用一个单独的线程每帧调 `DwmFlush`；它连续几次立刻返回（没有显示器）就每次睡 15 毫秒，不空转（`waitForFrame`）。

### TranslucentTB 自己会让 Explorer 崩

- **现象**：2026-10-16 测试时，我们的 DLL（0.11 及以前给开始按钮右边留位置用的，已删）在一个开了 4 天的 Explorer 里，再开 TranslucentTB，Explorer 立刻崩了重启；TranslucentTB 弹出“初始化 XAML 诊断失败（0x80070102 超时）”。
- **原因**：崩在 `ExplorerTAP.dll`（TranslucentTB 的），c0000409 子码 7（`FAST_FAIL_FATAL_APP_EXIT`，即 `std::terminate`）：它的 `VisualTreeWatcher` 构造时起一个线程 `check_hresult(AdviseVisualTreeChange())`，失败就抛异常、没人接。WinUI 的 advise 要往每个 XAML 线程的 `DispatcherQueue` 投递，有一个投不进去就失败。本机的 WER 记录里 9/17、9/24、9/25、10/1 还有四次一样崩在 `ExplorerTAP.dll`，那时 Explorer 里没有我们的 DLL。新开的 Explorer 里两种先后都没再崩过；这一次是不是和我们有关没能复现确认。
- **做法**：查 Explorer 崩溃先看 `%ProgramData%\Microsoft\Windows\WER\ReportArchive\AppCrash_explorer.exe_*\Report.wer` 里的出错模块和有没有我们的 DLL，别先怀疑自己。要抓 dump：`HKLM\SOFTWARE\Microsoft\Windows\Windows Error Reporting\LocalDumps\explorer.exe`（DumpType 2 一个 1 GB），测完删掉这个键和 dump。

### 切换 Windows 搜索的样式时 Explorer 崩在 SystemTray.dll

- **现象**：用设置 › 个性化 › 任务栏 › 搜索来回切“仅搜索图标 / 搜索图标和标签 / 隐藏”量 Windows 自己的按钮，有一次 Explorer 崩了重启。
- **原因**：栈在 `SystemTray.dll`：`NotifyIconView::OnLoaded` 里 `INotifyIconDragVisual::BitmapSource` 是空的（麦克风的托盘图标），栈上没有任何 TAP 的帧。看着是 Windows 自己的毛病，当时我们的和 TranslucentTB 的 DLL 都在里面，不排除有关。
- **做法**：量完一种样式就等任务栏静下来再切；测试前先记下 `SearchboxTaskbarMode`，测完改回来、核对。

### 任务栏上子窗口的提示压在任务栏上

- **现象**：给任务栏里的仅图标按钮加了系统的 tooltip（`TTF_SUBCLASS`），提示出在鼠标处，盖住了旁边的开始按钮，还是浅色的；Windows 自己按钮的提示在任务栏上方、深色。
- **原因**：tooltip 控件默认放在鼠标下面，屏幕底下放不下就翻到鼠标上面，还是在任务栏里；它不知道“任务栏”这回事。主题跟着应用，不跟任务栏。
- **做法**：处理 `TTN_SHOW`，自己 `SetWindowPos` 到框的正上方，底边离任务栏 12 逻辑像素，返回 TRUE；任务栏是深色时 `SetWindowTheme(tip, L"DarkMode_Explorer", nullptr)`，主题变了再设一次（`readTheme`）。tooltip 不能属于任务栏的窗口（别的进程），建成无主的 `WS_EX_TOPMOST` 弹出窗口，加 `TTS_ALWAYSTIP`。

### 测试：量任务栏按钮的悬停

- **现象**：想量 Windows 搜索按钮的悬停底板：`SetCursorPos` 到按钮上，截图前后一样；UI 自动化 `FromPoint` 抛异常；改完 Settings 里的搜索样式找不到菜单项；用记事本加一个任务栏按钮测，测完按 pid 关不掉。
- **原因**：XAML 的悬停要真的鼠标移动消息，`SetCursorPos` 不发；`SendInput` 的 `INPUT` 结构在 64 位下要按 `MOUSEINPUT` 的对齐算大小，多了填充 `SendInput` 返回 0；`AutomationElement.FromPoint` 要 `System.Windows.Point`，得先加载 WindowsBase；菜单项叫 `仅“搜索”图标`（中文引号），不是“仅搜索图标”；Windows 11 的记事本是商店应用，窗口属于另一个进程；而且脚本自己发的输入会把“用户空闲多久”清零。
- **做法**：`SetCursorPos` 到目标旁 1 像素，再 `SendInput` 相对移动 1 像素；UI 自动化先 `Add-Type -AssemblyName WindowsBase`；按 UI 自动化读出的名字选菜单项；加任务栏按钮用自己 PowerShell 进程里开的 WinForms 窗口；等空闲放在发输入之前。`TaskbarAl` 这类值测前不存在的，测后要删掉而不是写回 0，再广播 `TraySettings`。

### 脚本里启动了 Win顺，`Start-Process -Wait` 就一直不返回

- **现象**：提权跑测试脚本用 `Start-Process powershell -Verb RunAs -Wait -File test.ps1`，脚本早就跑完了，命令却一直不返回，直到超时。
- **原因**：`-Wait` 等的是整个进程树，脚本里启动的 Win顺（`--background`）也算，它不退出就一直等。
- **做法**：会启动 Win顺 的脚本不加 `-Wait`，让脚本把结果写进日志文件，外面轮询日志里的结束标记。

### 装了 StartAllBack，任务栏搜索框放不上去

- **现象**：用户装了 StartAllBack（3.9.28，Windows 11 25H2），打开“在任务栏上放 Win顺 的搜索框”后设置页一直说“读不到任务栏上的图标”。日志：`no TaskbarFrame in UI Automation; in the taskbar: Start, TrayNotifyWnd, …, ReBarWindow32, MSTaskSwWClass, MSTaskListWClass`。
- **原因**：StartAllBack（ExplorerPatcher 的 Windows 10 任务栏也一样）换回的是 Windows 10 那种 Win32 任务栏，没有 XAML，量图标用的 `TaskbarFrame` 根本不存在。
- **做法**：`MSTaskListWClass` 可见就按经典任务栏量：任务按钮是它在原生 UI 自动化里的子元素，其余都是窗口，`GetWindowRect` 就行（`Measurer::readClassic`，见 architecture.md 的“经典任务栏”）。不能只看有没有这几个窗口：Windows 11 自己的任务栏里也有 `ReBarWindow32`、`MSTaskSwWClass`、`MSTaskListWClass`，只是 `MSTaskListWClass` 隐藏；再加一条：它有可见的 `Windows.UI.Composition.DesktopWindowContentBridge`（XAML），经典任务栏没有。

### 经典任务栏的按钮来去，不发位置变化

- **现象**：经典任务栏上框放好了，开一个程序，任务按钮多了一个，框却不让开，被新按钮压在下面。
- **原因**：框靠 `EVENT_OBJECT_LOCATIONCHANGE` 知道图标动了。实测（2026-10-16，StartAllBack，挂任务栏线程上 `EVENT_OBJECT_CREATE`…`EVENT_OBJECT_END` 的全部事件，开、关一次字符映射表）：经典任务栏一次位置变化都不发，只发 `EVENT_OBJECT_CREATE`（新按钮）、`EVENT_OBJECT_DESTROY`（按钮走了）和 `EVENT_OBJECT_STATECHANGE`，都来自 `MSTaskListWClass`，对象 `OBJID_CLIENT`，子 id 是按钮的序号。
- **做法**：经典任务栏另挂 `EVENT_OBJECT_CREATE`…`EVENT_OBJECT_DESTROY` 的钩子，只认 `MSTaskListWClass` 的、子 id 不是 0 的，收到就量（之后照常逐帧量到停下）。`STATECHANGE` 不要：鼠标悬停在按钮上也发，会让测量线程一直逐帧跑。

### .NET 的 UI 自动化读不到经典任务栏的按钮

- **现象**：PowerShell 用 `System.Windows.Automation` 探 StartAllBack 的任务栏，`MSTaskListWClass` 底下一个子元素都没有（`FindAll(Descendants)` 是 0），以为 UI 自动化读不到按钮。
- **原因**：.NET 那套是托管的老客户端，用自己的客户端代理；原生的 COM UI 自动化（`CUIAutomation8`，Win顺 用的）经 MSAA 代理能拿到每个按钮（名字、矩形），`AccessibleObjectFromWindow` 直接问 MSAA 也有。
- **做法**：探任务栏要用和 Win顺 一样的原生 COM 接口（写个小 C++ 探测程序），别拿 .NET 的结果下结论。

### 改 StartAllBack 的注册表，任务栏不跟着变

- **现象**：测试时往 `HKCU\Software\StartIsBack` 写 `TaskbarCenterIcons=1`，任务栏图标没有居中。它的设置窗口是 Delphi 写的，UI 自动化里看不到选项，没法替用户点。
- **原因**：StartAllBack 只在资源管理器启动时读这些值。
- **做法**：写好值后重启资源管理器（顺便测了框在资源管理器重启后重新挂上）。重启前先记下开着的文件夹窗口。

## 主题和语言

### 名为 `onXxx` 的属性不随主题变化

- **现象**：设置里从深色切到浅色，开关圆点、单选点、强调色按钮上的字还是黑色，其他颜色都变了。
- **原因**：`Theme.qml` 里的颜色属性叫 `onAccent`。QML 里 `on` + 大写字母开头的名字是信号处理器的写法，这样命名的属性在依赖变化后没有重新求值。
- **做法**：属性不用 `on` + 大写字母开头的名字（现在叫 `accentText`）。

### `QLocale::uiLanguages()` 不等于 Windows 的显示语言

- **现象**：中文版 Windows 上，“跟随系统”显示成了英文。
- **原因**：`uiLanguages()` 取的是“首选语言”列表，列表里英语可以排在中文前面，而 Windows 本身仍按中文显示。
- **做法**：用 `GetUserDefaultUILanguage()` 判断 Windows 的显示语言（`Settings::resolveLanguage`）。

## QML / JavaScript

### 边框 1 px 在 150% 下一边粗一边细

- **现象**（2026-10-23，150%，2560×1600，D3D11，逐像素读）：截图放大镜的右边框是 1 屏幕像素，另外三边 2 像素（8f3275b）；顺着查下去，同一类问题到处都有：设置页同一张卡里行与行之间的分隔线有的 1 行、有的 2 行（深色真窗口里连着四条是 2、1、2、1 行）；启动器底栏的分隔线 1 行、顶上的 2 行；剪贴板顶部的分隔线 1 行；开关、分段标签、小标签这些圆角框，上下边是半透明的三行（亮度 179、102、174），左右边才是干净的两行；设置卡片的下边框也比上边软。
- **原因**（三层，都是 1 逻辑像素 = 1.5 屏幕像素）：
  - **用矩形当线**（`Rectangle { height: 1 }`）：1.5 个屏幕像素的无抗锯齿矩形按像素中心落格，落 1 行还是 2 行看 y 的小数部分（.52 落 1 行，.02 落 2 行，正好整数时在临界点上，由浮点误差决定）。`border.width: 1` 不会这样：Qt 把边框宽度取整成整数屏幕像素（1.5 → 2，`pixelAligned`），所以边框总是 2 行。
  - **子项按边框的逻辑宽度内缩**（`x: 1`、`width: parent.width - 2`、`anchors.margins: 1`）：内缩 1.5 像素，边框 2 像素，子项盖住第二行的一半（放大镜右边就是这样丢了一行）。
  - **圆角框落在半像素上**：圆角矩形开着抗锯齿，边缘在 .5 上就是 50%、100%、50% 三行。位置和大小是“文字高度 + 常数”“居中”算出来的，取整的是逻辑像素不是设备像素；奇数逻辑像素 × 1.5 = x.5。
- **做法**：`Theme.qml` 里三个函数，`dpr` 传 `Screen.devicePixelRatio`（窗口换到另一块屏幕会自己重算）：
  - `Theme.edge(dpr)`：1 px 的线或边框的厚度，`max(1, round(dpr)) / dpr`（150% 是 2 个屏幕像素）。分隔线的高度（竖线的宽度）、放在有边框的父项里的子项的内缩、父项加子项的尺寸（`2 * edge`）都用它，不写 1。放大镜、设置页的 `SettingRow`、各处分隔线都这样改了。
  - `Theme.snap(v, dpr)`：取到最近的整设备像素；`Theme.snapUp(v, dpr)` 向上取（尺寸不能缩小时用）。要锐利边缘的圆角框，大小和位置都用它们：居中写 `y: Theme.snap((parent.height - height) / 2, Screen.devicePixelRatio)`，不用 `anchors.verticalCenter`（`ToggleSwitch`、`SettingRow` 的右侧控件）；文字加常数的宽度写 `Theme.snapUp(label.implicitWidth + 24, dpr)`（`Chip`、`ScopeTabs`、`SettingsSearchBox` 的键帽）。
  - 自动排版的容器里，每一项的高度取整，下面所有项才都在整像素上：`SettingRow.implicitHeight`、`SettingsSection` 的卡片高度。取最近的（`snap`），不要一律向上，几十行会累积出几个像素。
  - 在 150% 下，偶数逻辑像素正好是整数设备像素（2 → 3），奇数不是；125% 和 175% 要 4 的倍数才行。所以常数本身不能指望，要靠上面的函数。
  - 截图覆盖层（`CaptureOverlay.qml`）本来就按屏幕像素排（`px()`、`py()`、`Math.round(v * dpr) / dpr`），实测边框、手柄、工具条的四边都是 2 像素，没动。
  - 旁边已有九处各写各的 `onPixel`（`ResultRow`、`ClipRow`、`DialogBarRow`、`Footer`、`KeyHint`、`Panel`、`EditWindow`、`LongCapturePanel`、`OcrPanel`），新代码用 `Theme.snap`，旧的没合并。
- **实测**（150%）：设置页深色真窗口里的分隔线 2、1、2、1 行 → 2、2、2、2 行；启动器底栏分隔线 1 → 2 行；剪贴板顶部分隔线 1 → 2 行；打开状态的开关，上边亮度 43、77、113（半行）→ 43、43、112（整行）。设置页整页渲染（`--preview-settings --renderer d3d11`）里有半像素边缘的圆角框 286 → 92 个，页面总高度每页多 0 到 3 个设备像素。
- **没处理**：125% 和 175% 下设置页仍有 330 到 350 个半像素圆角框（水平方向的常数 18、12、10 乘 1.25 或 1.75 就不是整数）；`ThemeCard` 里的缩略图是按比例缩的示意图；其余窗口（启动器、剪贴板）在 150% 下已经落在整像素上，没发现问题。
- **怎么查**：见上面的“改完界面查像素”。

### `import QtQuick.Shapes`（或 Effects）编译通过，部署后程序起不来

- **现象**：结果行里加了 `import QtQuick.Shapes` 画描边数字，编译和 qmllint 都通过，换进 dist 后程序启动 8 秒内就退出了。日志：`module "QtQuick.Shapes" is not installed`，`Main.qml` 里 `Type ResultRow unavailable`。
- **原因**：`dist\WinShun\qml\QtQuick\` 下有 Shapes、Effects 等目录，但 dist 只带了 Qt6Core、Gui、Qml、Quick 这几个 DLL，没有 `Qt6QuickShapes.dll`、`Qt6QuickEffects.dll`，插件加载不了。
- **做法**：界面只用 `QtQuick` 本身能做的。数字的描边改成多份错开的 `Text` 放进一个 `layer.enabled` 的 Item，整体调半透明（`ResultRow.qml`）。真要用新模块，先把对应 DLL 加进部署和安装包，再在 dist 里实际启动一次。部署脚本会在新版起不来时自动换回旧版。

### QML 文件和 C++ 类同名，加载的是 C++ 那个

- **现象**：对话框搜索框的窗口建不出来，日志 `Could not load DialogBar, as the type is uncreatable`。
- **原因**：同一个 QML 模块里，C++ 类 `DialogBar`（`QML_ELEMENT`、`QML_UNCREATABLE`）和 `DialogBar.qml` 都叫 `DialogBar`，`QQmlComponent(engine, "WinShun", "DialogBar")` 拿到的是 C++ 类型。编译和 qmllint 都不报错。
- **做法**：QML 文件换个名字（`DialogBarWindow.qml`），和 `Launcher` / `Main.qml` 一样。

### 用子项的 `visible` 决定父项显不显示，隐藏一次就回不来

- **现象**：设置行的下方区域（`SettingRow` 的 `bodyShown`）写成 `bodyShown: a.visible || b.visible`，开关一拨，区域隐藏后，再满足条件也不出来了。
- **原因**：QML 的 `visible` 读出来的是实际可见性，父项隐藏时子项读到的也是 false。父项因为子项都是 false 而隐藏，子项又因为父项隐藏而是 false，就锁死了。
- **做法**：用条件本身算（`bodyShown: missing && !done`），子项的 `visible` 也绑同样的条件，不要互相读 `visible`。

### 以 `(` 开头的一行会接到上一行

- **现象**：在搜索结果上点右键，菜单不出来，日志里有 `Main.qml:103: TypeError: true is not a function`。
- **原因**：`menuLoader.active = true` 的下一行是 `(menuLoader.item as ContextMenu).popup(...)`。JavaScript 不会在 `(` 前面自动补分号，两行连成了 `true(...)`。
- **做法**：不要让一行以 `(`、`[` 或模板字符串开头；先存进一个变量再调用（`Main.qml`）。

### 为了建立依赖单独读一下属性，会被编译器删掉

- **现象**：剪贴板页右边的预览不跟着变。搜索结果从一条链接换成一条文字时（行数没变，当前行还是第一行），预览还是那条链接；固定一条后，预览里也不出现“在“固定”里，不会过期”。
- **原因**：绑定写成 `{ page.previewRevision; return ... }`，想靠单独读一下 `previewRevision`，让绑定在它变化时重新求值。qmlcachegen 把绑定编译成 C++ 时，把这种结果没用上的读取当成死代码删掉了，绑定就不依赖它。拿一个小例子用 qmlcachegen 编一下就能看到：单独读的那个属性在生成的代码里没有对应的 lookup。
- **做法**：让它参与计算，比如写进条件：`page.previewRevision >= 0 && ...`（`ClipboardPage.qml`）。生成的代码在 `build\<目录>\src\app\.rcc\qmlcache\*_qml.cpp`，每个绑定前有 `// expression for 属性名 at line N`，可以对照着查。

### 圆里的数字偏下

- **现象**：剪贴板多选时的序号气泡，数字明显偏下（150% 下低 2 个物理像素）、偏左。改成按 `TextMetrics.tightBoundingRect` 居中后，还低 1 个像素；同样的代码在 `qml` 工具里看却是正的。
- **原因**：有两层。一是 `anchors.centerIn` 按整行居中，行高里给下伸部分留了空间，数字用不到；`tightBoundingRect` 的底边也比基线低约 0.45 逻辑像素，可数字明明站在基线上。二是 Win顺 用 FreeType 字体引擎（见 architecture.md），原生渲染的字形位置和 `qml` 工具默认的 DirectWrite 不一样，会偏下一点，所以在 `qml` 工具里试准的位置不能直接搬过来。
- **做法**：`CenteredNumber.qml`：按基线和字形顶端居中（不用 `tightBoundingRect` 的底边），用 `Text.CurveRendering` 绘制，字形轮廓按给定位置精确画出，不经过字体引擎栅格化。验证时量像素：取圆盘蓝色像素的范围和里面深色笔画的范围，比较两者中心，要在应用自己的截图上量。

### 自绘的棋盘格，格子之间有缝

- **现象**：剪贴板预览里半透明颜色的色块（`ColorSwatch`，一个 `QQuickPaintedItem`），棋盘格每两格之间多出一列颜色居中的像素，看着像细缝；同样的组件放在列表里却是清楚的。
- **原因**：预览栏落在半个物理像素的位置上。`QQuickPaintedItem` 的纹理按设备像素画好，贴上去时默认线性插值（`smooth` 为 true），每条边都被平均成一列过渡色，圆角边框也跟着发虚。列表的行正好在整像素上，所以看不出来。
- **做法**：不会被缩放的自绘组件，构造时 `setSmooth(false)`，用最近邻贴纹理：纹理一个像素对一个设备像素，半像素偏移只是整体挪一格，不再混色。圆角不要用 `setClipPath` 裁（栅格引擎裁剪不抗锯齿），先画满，再用 `CompositionMode_DestinationOut` 填掉圆角外面那一圈（`ColorSwatch.cpp`）。验证时在截图上沿一行数连续同色像素的长度：150% 下 8 逻辑像素的格子应该正好是 12、12、12，中间没有第三种颜色。

### `font.pixelSize` 只能是整数

- **现象**：想让结果行按钮的图标在 150% 下正好 32 物理像素，需要 64/3 ≈ 21.33 逻辑像素。但写进 `font.pixelSize` 会被截成 21，实际只画 31.5 物理像素。
- **原因**：QML 的 `font.pixelSize` 是 `int`。
- **做法**：非整数的字号用 `font.pointSize`（实数）：像素 × 72 / 96。开了高 DPI 缩放后 Qt 的逻辑 DPI 固定是 96（`Glyph.qml`）。

### 快速连点，第二下没有 `clicked`

- **现象**：`MouseArea` 的 `onClicked` 里做事，快速连点时每两下只响应一下。
- **原因**：两下点得够快就是双击：第二下只发 `doubleClicked`，不发 `clicked`。
- **做法**：每一下都要响应的（`AuthorAvatar.qml`），用 `onPressed`；连点时要防的是重复的效果本身（叠在一起、反复从头开始），在处理函数里判断，不靠漏掉的事件。

### 连链接都不行的系统组件：延迟加载

- **现象**：直接链接 `mfplat.lib`（Media Foundation），程序在没有它的 Windows 上（N 版没装媒体功能包）根本启动不了，报缺少 DLL。
- **做法**：`/DELAYLOAD:mfplat.dll`（加 `delayimp`），用之前先 `LoadLibraryExW(..., LOAD_LIBRARY_SEARCH_SYSTEM32)` 确认在不在，不在就不用；延迟加载的函数第一次调用时才找 DLL，找不到会抛 SEH 异常。`dumpbin /dependents WinShun.exe` 里它应该出现在 “delay load dependencies” 下。

### QML 的列表属性不是数组，`Array.prototype.indexOf` 什么也找不到

- **现象**：设置窗口的搜索里按 ↓，当前项有时不动，有时跳到别的行；结果的先后时对时错。
- **原因**：`children` 这类列表属性（`QQmlListProperty`）到了 JavaScript 里不是真正的数组。`Array.prototype.indexOf.call(item.parent.children, item)` 不报错，但总是返回 -1，每一行的排序键都一样，顺序就成了 C++ 里哈希表的顺序。
- **做法**：用下标循环逐个比较（`SettingRow.qml` 的 `orderKey()`）。查这类问题时加一行临时的 `console.warn` 打出中间值，输出在 `%LOCALAPPDATA%\WinShun\WinShun.log`，比反复看截图快。

### 只锚了右边又写了 `x`，输入框的宽度是 0

- **现象**：设置窗口的搜索框里看不到占位文字“查找设置”，打的字贴着右边往左长。
- **原因**：`TextInput` 写了 `x: 36` 和 `anchors.right`，没写 `anchors.left`。只有右锚时，宽度取隐式宽度，`x` 由右锚算出来，写的 `x` 不起作用。空的 `TextInput` 隐式宽度接近 0，铺满它的占位文字也就没有宽度了。
- **做法**：左右都锚住（`anchors.left` + `anchors.leftMargin`）。

### `StandardKey.PreviousChild` 在 Windows 上，按 Ctrl+Shift+Tab 不触发

- **现象**：设置窗口的标签用 `StandardKey.NextChild` / `PreviousChild` 切换，Ctrl+Tab 能切到下一个，Ctrl+Shift+Tab 没反应（Shift 先按、Ctrl 先按都一样）。用 `qml.exe` 单独开一个最小窗口注入按键：Ctrl+Shift+Tab 作为 `Key_Backtab`（修饰键 Ctrl+Shift）原样落进了 `Keys.onPressed`，快捷键没接住；同一个 StandardKey 的另一组按键 Ctrl+Shift+F6 能触发。
- **原因**：Qt 6.12 在 Windows 上给 `PreviousChild` 绑的是 `Ctrl+Shift+Backtab`（`qplatformtheme.cpp`），和实际的按键事件匹配不上。2026-10-10 实测：`Shortcut` 写成 `"Ctrl+Shift+Tab"` 或 `"Ctrl+Backtab"` 都能触发，`"Ctrl+Shift+Backtab"` 不行。
- **做法**：写成 `sequences: ["Ctrl+Shift+Tab"]`（`SettingsWindow.qml`）。拿不准某个组合键能不能触发时，先用 `qml.exe` 开个最小窗口，注入按键看 `Keys.onPressed` 和 `Shortcut` 的输出，比改 Win顺、编译、部署快得多。

### 在当前行上面插入一行，ListView 的当前行跟着往下走

- **现象**：内容结果按相关度插入后，更好的一条排到了第一行，高亮却落在第二行，回车打开的也是第二行。
- **原因**：模型 `beginInsertRows` 插在 `currentIndex` 及以上时，ListView 让 `currentIndex` 跟着原来那一项往下移，保持“当前项”不变，而不是保持“第几行”不变。
- **做法**：要“当前行仍是第一行”，得插完后显式把 `currentIndex` 设回 0（`Launcher::betterFirstRow` → Main.qml）。插入的位置在 C++ 里按插之前的当前行算，一次批量插完再发信号：中途 Binding 已经把移动后的行号写回来了。

### 页面里弹出的菜单被后面的卡片盖住

- **现象**：设置里“插件”页点“添加插件”，弹出的菜单（插件文件夹 / 压缩包）被下面第一张插件卡片压住，只露出一半；卡片的“…”菜单也一样，被下一张卡片和它的开关盖住。
- **原因**：两个菜单都是就地写在按钮旁边的一个 Rectangle，靠 `z` 抬高。但 `z` 只在同一个父项的子项之间排先后，盖不过父项后面的兄弟：菜单在标题行里，卡片列表在标题行后面，整块都画在它上面。给卡片设 `z: menuOpen ? 1 : 0` 也只能压过同一列里的卡片，压不过后面的设置行。
- **做法**：弹出的东西挂到窗口的内容项上（`parent: anchor.Window.contentItem`、`z: 1000`），和 `HoverTip.qml` 一样。现在两个菜单都用 `DropMenu.qml`：位置按按钮换算，下面放不下就翻到上面；整窗一层 MouseArea，点别处或滚轮就关（事件照样往下传）；按在打开它的按钮上时这层不关，让按钮自己的点击去切换，所以再点一次按钮就收起。旧写法的问题正在这里：遮罩按下时先关了菜单，松开时按钮的点击又把它打开，看上去“再点关不掉”。规矩是：X2 打开的 X1，再触发一次 X2 就关上 X1（按钮、快捷键都一样）。菜单宽度随最长的一项变宽，免得英文溢出。2026-10-11 查过其他弹出：启动器、剪贴板、对话框旁搜索框的右键菜单（`ContextMenu.qml`）是独立的窗口，不受影响；`ScopeTabs` 的“文件 › 文件夹”是就地展开，不盖别的东西；悬停提示本来就挂在内容项上。以后再加就地弹出的东西，直接用 `DropMenu` 或照它挂到内容项上。同一天顺着这条规矩查了一遍：右键菜单（遮罩吞掉按下，再点“…”就是关）、剪贴板预览（`togglePreview`）、托盘、双击 Ctrl、任务栏框、Win+V 都已经是开关；“网页快捷方式”的“添加”打开表单后自己变灰、点不了，改成了再点一次收起表单。离屏预览可以验证：`--preview-plugins out.png --menu add|card` 会模拟点三次按钮，要依次是开、关、开，否则退出码非 0（Preview.cpp；卡片是 Repeater 建的，没有 QObject 父对象，`findChildren` 找不到，要从 `contentItem()->childItems()` 里找）。

### 切到“内容”要缓一下才有反应

- **现象**（用户反馈）：点“内容”后要等一会儿。逐帧录屏：切换后 250 毫秒旧行变暗，没结果的查询 360 毫秒才出“没有…包含”，“WinShun”1.1 秒才出结果；`wsbench --content-search` 测同样的搜索只要 20 毫秒和 236 毫秒（第一个命中 106 毫秒）。
- **原因**：一是内容搜索为打字设的 300 毫秒防抖（`m_contentDebounce`）切范围时也套上了，什么都没开始读就先等 300 毫秒，250 毫秒的变暗也是在等它；“全部”里名字结果出来后的内容那一段同样如此。二是第一批结果：过了 50 毫秒还一条没有，就改成每 100 毫秒才发一次，第一个命中最多再晚 100 毫秒。
- **做法**：搜索的文字就是正在显示的那个（换了范围、排序、类型）时不防抖，马上开始（`Launcher::m_contentAtOnce`），打字时照旧等停顿；没有结果时一直每 5 毫秒看一次，有了就发（`SearchEngine.cpp` 的 `first`）。改后录屏：没结果的 55 毫秒出“没有…”，“WinShun”105–215 毫秒出结果，旧行不再先变暗。冷文件（杀毒软件还没扫过的）首次打开仍然慢，那是另一回事。

### 从“内容”切回“全部”，窗口先缩成只剩搜索框再撑开

- **现象**（用户反馈“看着不舒服”）：查询在“内容”里没有结果时按 Tab 回到“全部”，逐帧录屏（DXGI 桌面复制，每毫秒记窗口大小）看到 50 毫秒里换了四种样子：“没有找到”被截掉一半 → 只剩搜索框和底栏（窗口 159 像素高）→ 窗口撑到 893 像素、下面一片空 → 结果。
- **原因**：“没有找到”的条件里有 `!launcher.searching`。新搜索一开始它就不成立，列表又是空的（上一个范围什么也没有，`Launcher.stale` 只在有行时才留着旧行），列表区和空状态都收起来；20 毫秒后结果到了再撑开。
- **做法**：空状态像旧行一样留到新搜索有结果或也没结果（`Main.qml` 的 `settleEmpty`）。显不显示、标题、提示、下面那几行都从快照属性来，只在 `settleRows` 里（`Qt.callLater`，范围、查询、搜索状态都改完以后）一起写，和 `shownRows` 同一次改：直接绑 `emptyTitle` 会先闪一帧新范围的措辞。留得超过 250 毫秒就调暗（同 `dimRows`），因为它说的是上一个查询。改后录屏：两个方向各只改一次窗口大小。撑大时还会有一两帧新窗口下面是空的（先改大小、后画内容），每次结果变多都这样，不是这个问题。

### 行里的 `HoverHandler` 永远是 `hovered: false`

- **现象**：结果行上，鼠标停在日期上不出完整时间的提示，停在划掉的快捷键角标上也不出“被别的程序占用”的提示。截图里那一行有悬停底色，光标确实在那个元素上（2026-10-13 实测）。
- **原因**：整行盖着一个 `hoverEnabled: true`、`anchors.fill: parent` 的 `MouseArea`（悬停底色、按钮、拖出去都靠它）。它在声明顺序上排在日期、角标后面，也就是叠在它们上面；悬停事件被它收下，下面那些元素里的 `HoverHandler` 收不到。
- **做法**：在行的 `MouseArea` 下面的元素，用这个 `MouseArea` 的坐标判断：`area.containsMouse && item.contains(item.mapFromItem(area, area.mouseX, area.mouseY))`（`ResultRow.qml` 的 `under()`）。按钮（`RowAction`）自己有 `MouseArea`，叠在行的上面，不受影响。

## 设置窗口的搜索

### 好几行共用的文字一被命中，整页都出来

- **现象**：最初的版本里，搜 `kjj` 出来 16 项（要的只是“快捷键”），搜 `zt` 除了“主题”还带出整个剪贴板页，搜“剪贴板 图片”出来 8 项。
- **原因**：分类关键词、分类名和节的提示语是整页或整节共用的，命中一次就算这一页、这一节的每一行都命中：`快捷键` 被写进了“打开 Win顺”的分类关键词；剪贴板页的关键词“粘贴”首字母也是 `zt`；剪贴板这一节的提示语里有“图片”，又和分类名“剪贴板”凑成了两个词都命中。
- **做法**：分类关键词只放分类本身的别名（`pageKeywords`），不放任何一行的标题和关键词；分类名和分类关键词至少 3 个字母才按拼音匹配（`SettingsMatch.cpp`）；节的提示语不参与匹配。加关键词后搜一下那个词，看出来的是不是只有想要的几行。

## 安装程序（Inno Setup）

### 装完点“完成”，搜索框没有出来

- **现象**（用户反馈，2026-10-09）：安装 0.5.0 后点“完成”，屏幕上什么也没出现，用户不知道 Win顺 有没有启动；8 秒后去托盘里找它（日志里的托盘菜单记录）。
- **原因**：“完成”后安装程序才启动 `WinShun.exe`（不带参数，本该打开搜索框），而这是新文件的第一次启动：杀毒软件要把 exe 和几十个 Qt DLL 逐个读一遍，`main()` 之前就要等（见“界面实测”里的“新 exe 第一次启动很慢”），这段时间窗口、托盘图标都还没有。等它出来，用户可能已经点了别处，搜索框一失去焦点就收起。另外，第二次启动把命令交给正在运行的那份时，那份要是还没建好消息窗口（它在启动器窗口之后才建），`FindWindow` 找不到，命令就丢了；那份正忙着启动时 `SMTO_ABORTIFHUNG` 也会让发送失败。
- **做法**：安装程序在文件装好后就在托盘里启动它（`[Run]` 里不带 `postinstall` 的 `--background`），慢的第一次启动在最后一页开着时做完；“完成”只让它打开搜索框，不勾“运行”就让它退出（`installer/WinShun.iss`）。第二次启动等那份的消息窗口最多 30 秒，命令只发一次（超时的发送之后仍可能送到，`--toggle` 发两次会抵消）。不要给这个启动加 `runhidden`：隐藏的启动参数会落到它以后显示的第一个窗口上（见“界面实测”）。
- **实测**：用测试安装包覆盖安装，按向导走，最后一页一出现就点“完成”（那份还没建好窗口），0.55 秒后搜索框在前台，8 秒内没有收起；不勾“运行”再装一次，点“完成”后 0.4 秒内退出。测完用正式的 0.5.0 安装包静默装回（`/VERYSILENT`，exe 的哈希和发布的一致）。向导的按钮在 UI Automation 里是 `Pane`（类名 `TNewButton`，名字如“下一步(N)”“完成(F)”），按 `Button` 类型找不到。这台电脑测时 Defender 实时保护是关的，测不出杀毒软件那几十秒，只验证了流程。

### 中文语言文件要带 BOM

- **现象**：向导里的中文全是乱码。
- **原因**：Inno Setup 把没有 BOM 的 `.iss`、`.isl` 当成 ANSI 读。从 Inno Setup 仓库下载的 `ChineseSimplified.isl` 没有 BOM。
- **做法**：`installer/` 下的 `.iss`、`.isl` 都存成带 BOM 的 UTF-8。

### 深色模式用另一套图片

- **现象**：设了 `WizardSmallImageFile`，深色模式下右上角还是 Inno 自带的光盘盒图。
- **原因**：`WizardStyle=... dynamic` 在深色模式下读的是 `WizardImageFileDynamicDark`、`WizardSmallImageFileDynamicDark`，没设就用内置图。
- **做法**：浅色、深色两组都设（`installer/WinShun.iss`）。

### 卸载程序在确认之前就会跑 `InitializeUninstall`

- **现象**：卸载时一弹出“确认要完全移除吗？”，Win顺 就已经被关掉了，点“否”也回不来。
- **做法**：关闭程序放在 `CurUninstallStepChanged(usUninstall)`，那时用户已经确认。

### `schtasks /Change` 会要密码

- **现象**：安装程序里用 `schtasks /Change /TN WinShun /TR ...` 把开机自启改指向安装位置，命令停在“Please enter the run as password”，隐藏窗口里一直卡着。
- **原因**：这个任务是“只在用户登录时运行”（交互式令牌）的，`schtasks` 修改它时要求输入账户密码；任务计划程序的 COM 接口注册时不需要。
- **做法**：安装程序运行 `WinShun.exe --take-autostart`，由程序自己用 COM 接口重新注册（`main.cpp`）。查询和 `/Delete /F` 不要密码，卸载时照用。

### 旧版不认 `WM_CLOSE`

- 0.2.0 及更早的版本收到 `WM_CLOSE` 只会销毁消息窗口，进程还在。所以安装程序按进程（`tasklist`）判断是否退出，等 15 秒不退就 `taskkill /F`。0.2.1 起消息窗口把 `WM_CLOSE` 当成 `--quit`。

### 资源管理器的右键动词不能直接指向 WinShun.exe

- **现象**：想给资源管理器右键加「用 Win顺 搜索此处」（GitHub #20），最直接的做法是注册表动词 `WinShun.exe --query "%V\ "`。
- **原因**：WinShun.exe 的清单是 `requireAdministrator`，每次右键都会先弹 UAC，`main()` 里的单实例转交来不及跑。换成普通权限的小程序转交，运行中的实例（高完整性）也收不到：`MessageWindow` 只对中完整性放行了 TaskbarCreated，`WM_COPYDATA` 被 UIPI 拦下（`SendMessageTimeout` 返回 0，错误 5）。
- **做法**：动词运行 `WinShunSearch.exe --search-here "%V"`（普通权限）。`MessageWindow` 另开一个只收消息的窗口 `WinShun.SearchHere`，只对它放行 `WM_COPYDATA`，而且只认一个盘符或共享上的文件夹路径；能 quit、settings 的那个窗口仍只对提权进程开放。`"%V"` 在盘根是 `"D:\"`，`CommandLineToArgvW` 会把 `\"` 当成转义的引号，所以小程序自己读命令行的原文。

### 开机任务启动的不一定是这一份 Win顺

- **现象**：在 dist\WinShun 测右键「搜索此处」，Win顺 没在运行时，小程序启动了 Win顺 却等了 30 秒也没把文件夹交过去（2026-10-14）。
- **原因**：小程序先用开机任务 `\WinShun` 免 UAC 启动，而这台机器的任务指向 `C:\Program Files\WinShun` 里装的旧版，旧版没有 `WinShun.SearchHere` 窗口。便携版和安装版同时在时，用户也会遇到同样的事。
- **做法**：`runAutostartTask` 先读任务的操作，只有启动的正是小程序旁边的 WinShun.exe 才用它，否则直接启动旁边那份（弹一次 UAC）。测试 dist 里的版本时，留意别把装好的旧版启动起来留在那里跑。

### WinShun.exe 设了“以管理员身份运行”，安装卡在替换文件上（GitHub #68）

- **现象**：每个版本安装都卡住：替换 `fonts\AlibabaPuHuiTi-3-55-Regular.ttf`（或 `msvcp140.dll` 等）时“MoveFile/DeleteFile 失败；错误代码 5”；跳过后最后启动 Win顺 报 `CreateProcess 失败；错误代码 740`（请求的操作需要提升）。
- **原因**：exe 的兼容性里有 `RUNASADMIN`（`HKCU`/`HKLM\Software\Microsoft\Windows NT\CurrentVersion\AppCompatFlags\Layers`，值名是 exe 全路径；用户勾的，或程序兼容性助手加的）。这时普通权限的 `CreateProcess` 一律 740（2026-10-24 实测；提权进程启动不受影响）。“只为我安装”的安装程序没提权：`--quit` 起不来，`taskkill` 杀不掉以管理员身份运行的 Win顺，`CloseWinShun` 又不报错，Win顺 一直占着它加载的字体和 DLL。`[Run]` 的 `runasoriginaluser` 启动、开机任务（普通权限）也都 740。
- **做法**：`PrepareToInstall` 先 `RemoveRunAsAdmin`（只去掉 `RUNASADMIN` 这个词，别的兼容性设置保留；只剩 `~`/`$` 标记就删值），再关 Win顺；还关不掉就弹窗让用户从托盘退出后重试，取消则停在准备页，不再装到一半。实测：去掉标记后，普通权限的 `--quit` 能让以管理员身份运行的 Win顺 退出。测试安装程序要换一个 AppId（复制 .iss 改 GUID 编译），不然“只为我安装”会被已有的全员安装挡住，或者覆盖装好的那份。

## 读文档（WinShunExtract.exe）

### 大 PDF 在沙箱里读到一半退出，退出码 0xE0000008

- **现象**：一份 94 MB 的 PDF 在主程序里直接读没问题，在 WinShunExtract.exe 里读却失败；管道断开，进程退出码 `0xE0000008`。
- **原因**：这是 PDFium（Chromium）内存不够时主动终止用的退出码。某一页有巨大的矢量图，读的时候瞬间要 1.2 GB，超过了作业对象当时设的每个进程 1 GB 上限。
- **做法**：上限改为 2 GB（只是上限，不预先占用）。还是超了的文件记为读不出，文件改动后才重读，不会反复重试。排查办法：`wsbench --extract --sandbox` 看哪些文件 failed，设 `QT_FORCE_STDERR_LOGGING=1` 能看到退出码；再用不带 `--sandbox` 的 `wsbench --extract` 看同一文件在本进程里的峰值内存。

### 读 PDF 慢，时间在 PDFium 里面，外面能省的只有堆

- **现象**：`wsbench --extract --sandbox` 里 1–5 MB 的文档占了读文档时间的七成，看着像读文件、管道或起进程的开销（2026-10-19）。同样条件连跑两次，PDF 平均能差 17%。
- **原因**：一是 `--extract` 不跳过副本，这一档 2615 个里 1431 个和前面的同样大小，建索引时根本不读（`Copies`）；二是逐段计时（不重复的 961 个 PDF、27357 页，本进程单线程）：`FPDF_LoadPage` 66%、`FPDFText_LoadPage` 28%，读文件（`m_GetBlock`）2%，打开文档、找系统字体（每个进程只枚举一次，约 3 毫秒）、取文字、我们整理文字、关页都在 1% 上下；沙箱里一个进程读这 961 个 38.4 秒，本进程 36.5–37.2 秒，差不到 5%，8 个进程一直复用。采样（xperf）看 PDFium 66%、ntdll 的堆 27%：pdfium.dll 静态链接 C 运行库，`malloc` 直接是进程堆的 `HeapAlloc`，150 个 1–5 MB 的 PDF 就分配了 4100 万次。
- **做法**：WinShunExtract.exe 的清单里设 `heapType` 为 `SegmentHeap`：本进程 5–7% 快（沙箱里一个进程读 961 个 38.4 → 35.6 秒），读完这些的峰值内存 1.22 GB 只多约 10 MB，5847 个文档的文字和位置逐字节相同（SHA-256 比较）。换成自己的分配器（导入表里换掉 `HeapAlloc`）试过：每类大小一条空闲链表的理想情况也只快 11%，还丢了堆的加固，不值。整个文件先读进内存（`FPDF_LoadMemDocument64`）不快（读文件本来就只占 2%），字体换成别的映射会改字宽、可能改出的文字，都不做。比较前后要交替跑、各跑几次，看 PDF 平均和总时间，不要拿两次单独的结果比。

## 构建、升级 Qt

### 从构建目录运行 WinShun.exe：PATH 里加了 Qt 也弹“找不到 Qt6Quick.dll”

- **现象**：在普通权限的终端里 `$env:PATH = "C:\Qt.12.0\msvc2022_64in;" + $env:PATH` 后运行 `build\issues\WinShun.exe --preview-settings …`，退出码 0xC0000135，屏幕上弹出好几个“系统错误：找不到 Qt6Quick.dll”（2026-10-11，用户不在电脑前，框一直挂着）。
- **原因**：Win顺 发现自己没以管理员身份运行，会提权重启自己；提权后的新进程拿不到命令里改过的 PATH。
- **做法**：不往构建目录拷 Qt 的 DLL。用 build.ps1 里那套 `windeployqt` 参数把 WinShun.exe（和 `fonts`）部署到自己的 scratchpad 文件夹再运行；或者在本来就是管理员的进程里运行。

### 从 Git Bash 调 `powershell.exe` 编译，构建目录被弄坏

- **现象**：在 Git Bash 里用 `powershell.exe -Command "./scripts/build.ps1"` 编译，报 `ninja: error: FindFirstFileExA(../../../??????/src/app)`。之后在 PowerShell 里正常编译也报同样的错。
- **原因**：项目路径里有中文，经过 Bash 再传给 Windows PowerShell 时编码错了，路径变成问号。ninja 把这个乱码路径写进了依赖缓存 `build\release\.ninja_deps`，以后每次编译都会读到它。根子在控制台的代码页：Bash 里起的控制台是 1252，编译器 `/showIncludes` 输出的中文路径就成了问号；第一次编译不读依赖缓存，所以能过，第二次才报错。
- **做法**：编译只在 PowerShell 里运行（`./scripts/build.ps1`），不要经过 Bash。已经坏了的话，删掉 `.ninja_deps` 再编译（它只是依赖缓存）。脚本里 `Select-String` 只能看到标准输出，要看到编译错误得用 `*>&1 | Out-String -Stream`。
- **新建编译目录也别在 Bash 里配置**：CMake 配置时记下的 `/showIncludes` 前缀（`CMakeFiles\rules.ninja` 里的 `msvc_deps_prefix`，中文版编译器是“注意: 包含文件:”）也会变成问号，ninja 从此认不出依赖，每次都全部重编。2026-10-09 碰到过：删掉 `CMakeCache.txt` 和 `CMakeFiles`，在 PowerShell 里重新配置就好。

### `WinShun_qmllint` 打不开 QML 文件

- **现象**：`cmake --build --preset release --target WinShun_qmllint` 对每个文件都报 `Failed to open file F:/电脑便捷工具/src/app/qml/Footer.qml: ???` （2026-10-10）。
- **原因**：qmllint 拿到的绝对路径里有中文，在它那里编码错了。
- **做法**：在 `src\app\qml` 里用相对文件名直接跑：`qmllint.exe -I F:\电脑便捷工具\build\release\src\app Footer.qml Main.qml`（`-I` 指向放 `WinShun\qmldir` 的那一层）。它能查出例如内联组件里的委托用到外层 id 却没写 `pragma ComponentBehavior: Bound`。

### 两个会话在同一个编译目录里编译

- **现象**：`cmake --preset release` 报 `ninja: error: failed recompaction: Permission denied`，`CMake Generate step failed`（2026-10-10，另一个会话同时在 `build\release` 里编译）。
- **做法**：并行干活的会话各用自己的编译目录（`cmake -B build\<名字>`，同一套源码），不要在别人的目录里配置或编译。

### 编译输出接 `Select-Object -First`，编译停在一半

- **现象**：脚本里 `cmake --build … | Where-Object {…} | Select-Object -First 60`，打出一串警告后报“built: WinShun 7,124,992 bytes”，看着编译完了；再编译一次，ninja 又链接出一个 7,411,200 字节的（2026-10-10）。
- **原因**：pwsh 7 里 `Select-Object -First` 拿够条数就叫停整条管道，前面的 cmake 进程也被结束。Qt 头文件的 C4702 警告很多，60 行早早就满了。
- **做法**：先把编译的输出整个收进变量（`$out = cmake --build … 2>&1`），看 `$LASTEXITCODE`，再从变量里筛。拿不准时再编译一次，看到 `ninja: no work to do` 才算完。2026-10-24 又踩了一次：先确认 exe 真的生成了、时间对，再说“编译通过”。

### 编译目录放在 scratchpad 里，报 C1083 “Permission denied”
- **现象**：`cmake -B <scratchpad>\build` 编译到 `qmlcache\WinShun_qml_CaptureOverlay_qml.cpp.obj` 报 `fatal error C1083: 无法打开编译器生成的文件 … Permission denied`（2026-10-24）。
- **原因**：scratchpad 在 `%LOCALAPPDATA%\Temp\claude\<项目>\<会话 id>\scratchpad` 下，再加上 `src\app\CMakeFiles\WinShun.dir\.rcc\qmlcache\…` 超过 260 个字符，cl 写不了这个 .obj。
- **做法**：另开的编译目录放在短路径（如 `F:\wsb67`），并用 `-DWINSHUN_BUILD_TESTS=OFF` 只编 WinShun；要跑单测再对同一目录 `-DWINSHUN_BUILD_TESTS=ON`。

### 从 Bash 里编译，新头文件的路径变成问号

- **现象**：从 Bash 调 `powershell.exe -File build-tb.ps1` 编译过一次新文件后，再编译 ninja 报 `FindFirstFileExA(F:/??????/src/app/platform): 文件名、目录名或卷标语法不正确`，什么都不编。`ninja -t deps` 里那个新头文件的路径是 `F:/??????/…`。
- **原因**：这台电脑非 Unicode 程序的代码页（ACP）是 1252。那样启动的 cl 用 ACP 输出 `/showIncludes`，路径里的中文成了问号，ninja 照原样记进依赖日志。另外 `rules.ninja` 里 CMake 检测到的 `msvc_deps_prefix` 也是 `??: ????:`：从 PowerShell 工具（UTF-8）编译时，cl 输出的“注意: 包含文件:”对不上，这些编译记下的依赖是 0 个，只改头文件时依赖它的 .cpp 可能不重编。
- **做法**：用 PowerShell 工具运行编译脚本。已经记坏的那一条，在 `.ninja_deps` 里把 `??????` 原地换成同样长度的 ASCII（如 `xxxxxx`）：路径变成不存在的文件，ninja 认为它缺了就重编那个文件。
- **依赖记成 0 个的修法**（2026-10-10）：从 PowerShell 工具编译时 cl 的 `/showIncludes` 是 UTF-8 的“注意: 包含文件:”，把编译目录里 `CMakeFiles/rules.ninja` 的 `msvc_deps_prefix` 换成这几个字的 UTF-8 字节（scratchpad 的 `fix-deps.py`），之后编译的 .obj 都记下了头文件（`ninja -t deps` 看，main.cpp 记了 200 个）。设 `VSLANG=1033` 想让 cl 改说英文，在这台电脑上没用。CMake 重新生成 `rules.ninja` 后要再改一次。修好之前编过的 .obj 还是 0 个依赖，改了它们用到的头文件时，要手动碰一下那个 .cpp（改了类的成员时尤其要紧：`main.cpp` 里的 `App` 大小不对，运行时就是内存错乱）。

### `uiautomation.h` 报 C2146

- **现象**：新文件包含 `<uiautomation.h>`，`UIAutomationCore.h` 在接口的前置声明处报 C2146。
- **原因**：项目定义了 `WIN32_LEAN_AND_MEAN`，`windows.h` 不再带进 `ole2.h`，UI 自动化头文件要的 COM 类型还没定义。
- **做法**：先 `#include <ole2.h>`，再包含 `uiautomation.h`（`TextCaret.cpp`）。

### moc 解析不了原始字符串字面量

- **现象**：在测试类（`Q_OBJECT`）的函数里写 `R"({"tag_name": ...})"`，编译报 `AutoMoc ... Parse error at "}"`。
- **原因**：moc 自己的词法分析器不认识 C++11 的原始字符串，里面的引号和大括号打乱了它对类体的解析。
- **做法**：`Q_OBJECT` 类里用普通字符串和转义（`tests/tst_core.cpp`）。

### lupdate 填了译文，却标成“未完成”

- **现象**：新加的 `qsTr("最近")` 在 .ts 里已经有了 `Recent`，但带着 `type="unfinished"`；按“没有译文的条目”去补，会漏掉它。未完成的译文 lrelease 默认不收，界面上显示的仍是中文。
- **原因**：别的界面文件里已经有一模一样的源文，lupdate 拿那条译文先填上，但要人确认，所以标成未完成。
- **做法**：跑完 lupdate 后找所有 `type="unfinished"`，不只是空的；核对一遍再去掉这个标记。

### lupdate 把别的会话没提交的界面改动也收进 .ts

- **现象**：2026-10-11 改底栏排序后跑 `update_translations`，.ts 少了 131 行：另一个会话正在把按钮上的“添加文件夹…”改成“添加文件夹”（还没提交），lupdate 把这些旧条目整条删掉，新条目译文为空，英文界面上这些按钮会变回中文。
- **原因**：lupdate 扫的是磁盘上的工作区，不分是谁改的；而且这个项目的配置不保留过时条目。
- **做法**：跑之前看 `git status`，有别人没提交的界面文件时，不跑 lupdate，手工改 .ts 里自己的几条（源文、译文、所属的 `<name>` 上下文都对上）；或者跑完 `git diff` 只留自己的条目。

### lrelease 在中文路径下打不开 .ts

- **现象**：`qt_add_translations` 生成的编译步骤报 `lrelease error: Cannot open F:/??????/src/app/i18n/winshun_en.ts`。lupdate 没问题。
- **原因**：lrelease 按 ANSI 代码页读命令行参数，这台机器是 1252（英文系统区域），中文路径变成问号。lupdate 的文件列表是写在 JSON 项目文件里的，所以不受影响。
- **做法**：只用 `qt_add_lupdate`；lrelease 自己写成 `add_custom_command`，在源码目录下用相对路径调用，生成的 .qm 用 `qt_add_resources` 嵌进去（`src/app/CMakeLists.txt`）。

### Qt 6.12：QML 导入扫描在中文路径下失败

- **现象**：编译报 `qmlimportscanner: No such file or directory F:/电脑便捷工具/src/app`。
- **原因**：6.12 起编译时就做 QML 导入扫描，是否扫描在 `qt_add_qml_module` 里就定了，之后再设 `QT_QML_MODULE_NO_IMPORT_SCAN` 目标属性已经晚了。
- **做法**：用共享库版 Qt 时，直接在 `qt_add_qml_module` 里写 `NO_IMPORT_SCAN`（`src/app/CMakeLists.txt`）。部署时由 windeployqt 按 `--qmldir` 扫描。

### 编译目录的路径一长，配置时 qmlimportscanner 又读不了中文路径

- **现象**：2026-10-20 在 scratchpad 里另开编译目录（`cmake --preset release -B C:\Users\…\Temp\claude\…\scratchpad\build-tb`），配置报 `qmlimportscanner: No such file or directory: "F:/电脑便捷工具"`、`Failed to scan target capture-probe-qt for QML imports`。同样的预设在 `build\release` 里没事。
- **原因**：`capture-probe-qt`（`CMakeLists.txt` 里的 `qt_add_executable`）配置时会扫一次 QML 导入。参数超过 240 个字符时，Qt 改用响应文件（`Qt6QmlMacros.cmake` 约 4885 行），qmlimportscanner 按 ANSI 代码页读它，中文的源码路径就坏了。编译目录的路径短，参数直接在命令行上，就没事。
- **做法**：把长路径映射成一个盘符再配置、编译：`subst T: <scratchpad>`，然后 `cmake --preset release -B T:\build-tb`，用完 `subst T: /d`。CMake 没改。

### aqtinstall 装 6.12 要另装 TaskTree 模块

- **现象**：配置时报 `Could NOT find Qt6TaskTree`。
- **原因**：6.12 的 Qt Qml（资源下载器）依赖新拆出来的 TaskTree 模块，aqtinstall 默认不装。
- **做法**：`aqt install-qt windows desktop 6.12.0 win64_msvc2022_64 -m qttasktree -O C:/Qt`。

### 换了 Qt 版本，发布脚本还找旧的

- **现象**：发布 0.4.0 时 `release.ps1` 一开始就报 `Qt not found`。用户环境变量 `QTDIR` 早已改成 6.12.0，可脚本拿到的是 `C:\Qt\6.11.1\msvc2022_64`，那个文件夹已经删了。
- **原因**：环境变量是程序启动时拿到的一份副本。VS Code 在改 `QTDIR` 之前就开着，从它里面起的终端和脚本拿到的还是旧值，要重启 VS Code 才会更新。`release.ps1` 又把这个旧值当成用户指定的路径传给 `build.ps1`。
- **做法**：`build.ps1` 发现 `QTDIR` 里没有 Qt 时，就改用 `C:\Qt` 下最新的版本，并提示一行；用 `-QtDir` 明确指定的路径照旧检查，错了就报错。`release.ps1` 只在明确指定了 `-QtDir` 时才往下传。

### `windows.h` 把 `near`、`far` 定义成了空宏

- **现象**：新文件先包含了 `windows.h`，`DoubleTapDetector.h` 里的成员函数 `near(...)` 报 C2062“意外的类型 int”。
- **原因**：`minwindef.h` 有 `#define near` 和 `#define far`（16 位时代的遗留），函数名被替换掉了。`NOMINMAX` 只管 `min`、`max`。
- **做法**：函数和变量不要叫 `near`、`far`（现在叫 `closeTo`）。

### lambda 里分成几行写的 `u"…" "…"_s` 编译不过

- **现象**：`ClipStore.cpp` 把 SQL 挪进写库线程的 lambda 后，原来好好的多行 SQL 报 C3688“文本后缀‘_s’无效”，附注说文本运算符要 `const char *, std::size_t` 参数。
- **原因**：MSVC（VS 2022）在 lambda 里把 `u"a" "b"` 拼成了窄字符串，Qt 的 `_s` 没有窄字符串的版本。同样的写法在普通函数里没事；拆成一行也照样报错，只要是拼接的就不行。
- **做法**：lambda 里拼接的每一段都写 `u` 前缀：`u"a" u"b"_s`。

### `qmllint --version` 会弹出消息框

- **现象**：在终端里运行 `qmllint.exe --version`，命令一直不返回；屏幕上弹出一个“qmllint 6.12.0”的消息框，跳到前台，打断了正在进行的界面测试。
- **原因**：Qt 的命令行工具在 Windows 上用消息框显示 `--version` 和 `--help` 的输出（没有控制台时）。
- **做法**：查 Qt 版本看 `C:\Qt` 下的文件夹名或用 `qtpaths`；不要在自动化脚本里运行带 `--version` 的 Qt 工具。检查 QML 直接 `qmllint -I build\<目录>\src\app 文件…`，它没有问题时什么都不输出。

### qmllint 在中文路径下打不开文件

- **现象**：`WinShun_qmllint` 目标对每个 QML 文件都报 `Failed to open file F:/电脑便捷工具/src/app/qml/….qml: ???????????`，然后失败。
- **原因**：和 lrelease 一样，命令行参数（这里是 .rsp 文件里的路径）按 ANSI 代码页读，中文路径坏了。
- **做法**：把 `src\app\qml\*.qml`、编译目录里 `src\app\WinShun\` 下的 `WinShun.qmltypes` 和去掉 `prefer` 那一行的 `qmldir`，一起复制到一个纯英文路径下名为 `WinShun` 的文件夹，再 `qmllint -I <它的上一级> <那里的文件>…`。C++ 类型从 qmltypes 里来，要先编译过一次。

### 编译目录里不要放 Qt 的 DLL

- **现象**：换到 6.12 后，单元测试报 0xc0000139（找不到入口点）。
- **原因**：`build\release` 里有个早先手动拷进去的 6.11 版 `Qt6Core.dll`。程序优先加载自己目录里的 DLL，盖过了 PATH 上的新版。
- **做法**：编译目录只放编译产物；要直接运行的程序，用 `./scripts/build.ps1 -Deploy` 生成到 `dist\WinShun`。

### 换 Qt 版本要改 `QTDIR`，并重新配置每个编译目录

- `scripts/build.ps1` 和 `CMakePresets.json` 都优先用环境变量 `QTDIR`；只有没设它时，脚本才会取 `C:\Qt` 下最新的版本。
- 编译目录的 CMake 缓存里记着旧 Qt 的路径（`Qt6_DIR` 等），要用 `--fresh` 重新配置（如 `cmake --preset release --fresh`）。`build\` 下的每个目录都要做，包括 `asan`。
- 顺序：装新版 → 改 `QTDIR` → 用 `--fresh` 重新配置、编译、测试 → 部署并实测 → 确认没问题后再删旧版 Qt 和旧的部署文件夹。

### `build.ps1` 报 “Qt not found”

- **现象**：用户环境变量里的 `QTDIR` 已经是 6.12，`./scripts/build.ps1` 仍然报找不到 Qt。
- **原因**：VS Code 和它的终端是在改 `QTDIR` 之前启动的，进程里还是旧值（`C:\Qt\6.11.1`），而那个版本已经删了。脚本优先用 `QTDIR`。
- **做法**：重启 VS Code，或者编译时显式传 `-QtDir C:\Qt\6.12.0\msvc2022_64`。

### 编译报错的文件自己没改过

- 可能是另一个会话正在改代码、改到一半。先看 `git status`，不要去动别人的文件，等它改完再编译。提交时也要把两边的改动分开。
- 编译通过也不代表能跑：2026-10-08 另一个会话的 `Main.qml` 已经用上了还没登记进 CMake 的 `ClipboardPage`，主目录编出来的 exe 一启动就报 `ClipboardPage is not a type`，起不来。要单独测自己的改动，用 `git worktree add --detach F:\wsdlg HEAD` 建一个只放自己改动的工作区（顺带避开中文路径的坑），在那里编译部署。往里同步文件时，两边都改过的共享文件（`App.cpp`、`Settings.*` 等）不能整个复制，只能把自己的几处改动重新加上。反过来，要编一个“主目录现在的样子、只是不带对方改到一半的文件”的版本（2026-10-09 用过）：把主目录整个同步到工作区（`robocopy /MIR`），再在工作区里 `git checkout HEAD -- 对方正在改的文件`，那几个就回到改之前；对方改的文件越来越多时，按“比上一次能编过的 exe 新”的文件找。

### 两个会话都改了主目录，`git apply -3` 报 “does not match index”

- **现象**：2026-10-10 把在工作区做好的改动（`git diff` 出来的补丁）`git apply -3` 到主目录，几十个文件全报 `does not match index`，一个也没打上。
- **原因**：`-3` 要拿暂存区当底，主目录的改动都没暂存，工作区文件和暂存区对不上。
- **做法**：`git stash create` 给主目录现在的样子拍一个提交（不动工作区，也顺带留了备份），在另一个工作区 `git checkout --detach` 到它，在那里 `git apply -3`、编译、跑测试，再把改过和新增的文件复制回主目录（复制前确认主目录这期间没再变）。

## 网络共享

### 探测说服务器在线，其实那个地址根本没人

- **现象**：`wsbench --share \\10.255.255.1\x`（一个不存在的地址）：探测 1.2 毫秒“连得上”，接着列共享、读目录各等了 10 秒才报错。
- **原因**：这台机器开着代理软件的 TUN 模式（Clash 一类），所有 TCP 连接都先被本机的虚拟网卡接下，`connect` 立刻成功，之后才发现对面没人；有的 VPN 客户端也这样。只看 445 端口连不连得上，在这些机器上永远是“在线”。
- **做法**：连上之后再发一个 NEGOTIATE，要对方在时限内用 SMB 回话才算在线（`share::probe`）。现在同一个地址 1.5 秒内判为离线。发什么见下一条。

### 只会 SMB1 的老 NAS 被当成离线

- **现象**：WSL 里的 Samba 设成 `server min/max protocol = NT1`：探测“连不上”（3 毫秒），读共享报 64（“指定的网络名不再可用”），卡片上写“离线”，用户看不到“只支持 SMB1”的提示。
- **原因**：探测原先直接发 SMB2 格式的 NEGOTIATE，只会 SMB1 的服务器收到就挂断。Windows 11 默认没装 SMB1 客户端，连这种服务器只会得到 64，不会给 `ERROR_SMB1_NOT_AVAILABLE`（384）。
- **做法**：探测改发 Windows 自己先发的那种：SMB1 格式、同时提供 `NT LM 0.12`、`SMB 2.002`、`SMB 2.???`。会 SMB 2 的服务器用 SMB 2 回答（`\xFESMB`），只会 SMB1 的用 SMB1 回答（`\xFFSMB`）；对这种格式直接挂断的，再用 SMB2 格式问一次。只会 SMB1 的仍算“在线”（装了 SMB1 客户端的电脑能用它）；一轮读取失败、探测又说它只会 SMB1，才标成“只支持 SMB1”（384）。

### 来宾被拒时，列共享报的是“找不到网络路径”

- **现象**：只允许来宾的 Samba（`map to guest = Bad User`、`guest only`），Win11 默认策略（`EnableInsecureGuestLogons=0`、要求签名）：读共享正确地得到 1272，`NetShareEnum` 却返回 53，设置页“查看共享的文件夹”会说“没有列出共享（错误 53）”。
- **原因**：`NetShareEnum` 走 `IPC$` 上的 RPC，会话被拒时它把原因换成了“找不到”。
- **做法**：`NetShareEnum` 失败且不是“需要登录”时，`GetFileAttributesW("\\服务器\IPC$")` 问一次真正的原因（1272、1326 …）；还是“连不上”一类、探测又说只会 SMB1 的，报 384。

### 这台开发机不把当前账户发给服务器，来宾共享先表现为“需要登录”

- **现象**：没有存凭据时，访问 Samba 上的来宾共享得到 1326 而不是 1272；Samba 日志里客户端收到 NTLM 质询后就断开了，没发用户名。存一个服务器上不存在的用户名（`cmdkey /add:IP /user:nobodyhere /pass:x`）后，Samba 按 Bad User 映射成来宾，才得到 1272。
- **原因**：推测是这台机器的账户没有可用于网络登录的密码（PIN 登录等）。公司电脑多半会把当前账户发过去，直接得到 1272。
- **做法**：两种都对：1326 时卡片是“需要登录”，用户随便输个账户登录后变成 1272，卡片换成来宾的说明。测来宾共享用不存在的用户名；同一个地址失败过之后 Windows 会记住失败、不再真去连（几毫秒就报错），换一个地址（WSL 里 `ip addr add 172.x.x.y/20 dev eth0`）再测。

### 提权的 Win顺 用不上资源管理器里输过的密码

- **现象**（推断，这台机器复现不了）：资源管理器能打开 `\\192.168.1.10\资料`，Win顺 读它却是“拒绝访问”或“用户名或密码不正确”。
- **原因**：SMB 连接按登录会话分，提权进程和资源管理器是两个会话。资源管理器里输了密码但没勾“记住我的凭据”，密码只在它那个会话的连接上。这台开发机 `EnableLinkedConnections=1`，两个会话的连接会镜像，看不到这个问题；公司的电脑多半不是这样。
- **做法**：凭据管理器是按用户的，两个会话都读。“登录…”按钮弹 Windows 的凭据框，`WNetAddConnection2` 连上，勾了“记住”就 `CredWrite` 存一条以服务器为名（IP 就是 IP）的 Windows 凭据；之后两边都不用再输。后台核对从不弹框。要验证就在 `EnableLinkedConnections=0` 的电脑上试。

### 网络上的变更通知：缓冲不能超过 64 KB，Samba 只通知经网络做的修改

- **现象**：对网络路径用大于 64 KB 的缓冲调 `ReadDirectoryChangesW` 会失败（`ERROR_INVALID_PARAMETER`，资料所说）。实测 Samba 4.19（WSL）：经 SMB 做的新建、改名、删除、5000 个文件的批量增删都实时到达（15–80 毫秒）；在服务器上直接改（`touch`、`mv`、`rm`，相当于 NAS 的同步、下载套件）只有共享根目录那一层报，子文件夹里的一概不报。`change notify = no` 时监听照样建得起来（看着在“实时跟踪”），却一条也不报。
- **原因**：SMB 的 CHANGE_NOTIFY 一次回复有大小上限；Samba 只把经它自己做的修改转给各个监听者，本机的改动靠 inotify，而 inotify 不递归，Samba 只监视了被监听的那一层。
- **做法**：`ChangeWatcher` 本来就是 64 KB。监听只当加速：每 1 小时整个核对一遍才是保证（`[Network] VerifyHours`，原来 4 小时；10 万项的共享核对一次不到 1 秒），读一轮很久的共享按耗时的 60 倍放宽。

### 在界面线程上问网络文件存不存在，启动器卡半分钟

- **现象**：读代码时发现：回车打开、“打开所在位置”、删除之前，`Launcher::perform` 在界面线程上 `QFileInfo::exists(r.path)`；文件在连不上的共享上时，这一问要 10–45 秒，整个启动器卡住。
- **原因**：Windows 对一个不回答的 SMB 服务器要等 TCP 超时和重试。
- **做法**：先 `share::reachable`，每次都重新问（在线的服务器几毫秒，不通的 1.5 秒封顶；多选时一台服务器只问一次），不通就在底栏说“连不上 …”，不再往下问。不能用留着的答案：实测 smbd 刚停，Windows 问一个已收录的文件要 6.1 秒，而留着的“在线”最多还有 30 秒有效。

### lupdate 读错 QML 字符串里的反斜杠

- **现象**：`qsTr("\\\\192.168.1.10\\共享名")` 收进 `.ts` 的原文成了 `\192.168.1.10共享名`，和运行时的字符串对不上，英文界面里一直显示中文。C++ 里的 `tr("…\\\\%1\\资料")` 没问题。
- **原因**：lupdate 解析 QML 时对转义的处理和 QML 引擎不一样。
- **做法**：QML 里要翻译的文字不放反斜杠，路径用 `%1` 填进去：`qsTr("%1共享名").arg("\\\\192.168.1.10\\")`。

### Windows 10 LTSC 上一启动就报“无法定位程序输入点 NotifyNetworkConnectivityHintChange”

- **现象**：用户在 Win10 LTSC 上运行 0.7.x，弹窗“无法定位程序输入点 NotifyNetworkConnectivityHintChange 于动态链接库 …\WinShun.exe 上”，程序根本起不来（2026-10-12）。
- **原因**：`NetworkNotifier` 直接调用了 `NotifyNetworkConnectivityHintChange`，它在 iphlpapi.dll 里，Windows 10 2004（19041）才有。LTSC 2019 是 1809（17763），LTSC 2016 是 1607。静态导入的函数缺一个，加载器就拒绝启动整个程序。`_WIN32_WINNT=0x0A00` 挡不住它，因为 SDK 头文件里所有 Windows 10 版本都算 0x0A00。
- **做法**：比 1809 新的 API 用 `GetProcAddress` 取，取不到就跳过（这里少了连通性提示，网卡和地址变化照样报）。发布前用 `dumpbin /imports` 过一遍自己的 exe 和 dll，看有没有新加的系统函数；Qt 6 本身最低就是 1809。

### 退出时等网络共享的线程：先存索引，再等；别用 `CancelSynchronousIo` 去催

- **现象**：退出（包括关机、注销）时 `IndexService::shutdown` 先 join 共享线程再存索引。NAS 断电或断网时，线程卡在 `CreateFileW`/读目录上，要等 Windows 的 SMB 超时（几十秒），关机时 Windows 只给几秒，索引来不及存（2026-10-13 审查发现）。
- **试过的路**：在 join 时对卡住的线程反复 `CancelSynchronousIo`。实测（匿名管道上一个同步 `ReadFile`，scratchpad `cancelio/`）：30 次里 24 次是**调用 `CancelSynchronousIo` 的线程自己**卡在内核 `ZwCancelSynchronousIoFile` 里，被取消的线程还在 `ZwReadFile`。它要等那个 IRP 完成才返回，IRP 不响应取消就一直等。SMB 上管不管用这台机器测不了。要是放在 stop 回调里，会在请求停止的线程（退出时就是界面线程，还持着 ShareIndexer 的锁）上卡住，正好卡在存索引之前。
- **做法**：`ShareIndexer::requestStop()` 只请求停止；`IndexService::shutdown` 存完索引再 `m_shares->stop()` 去 join（周期保存本来就是共享线程照跑时存的）。`share::Lister` 在停止后不再发新的网络调用。剩下的：服务器正好断在一次调用中途时，存完索引后退出仍要等那一次超时。别在界面线程或持锁时调 `CancelSynchronousIo`。

## 搜索线程卡住（结果变灰）

### 结果全是灰的：新的搜索在等一个盘回答

- **现象**：客户截图（2026-10-10）：搜索框里是“线切割”，下面却是上一次搜“tibo”的结果，全部调暗（文字亮度 127，正好是 0.45 的不透明度），底栏还是 tibo 的“10 个结果 · 18 毫秒”。
- **原因**：新查询的结果出来之前，旧的一屏调暗留着（`Launcher::stale`）；搜索只有一个线程，它一卡，结果就一直不来。搜索时要确认最近打开的项目还在不在（`QFileInfo::exists`）：名字匹配的几个，清空搜索框时是列出来的全部。文件在连不上的网络共享上时，问一次要 10–15 秒（本机实测 `\\10.255.255.1\share\…`：第一次 15 秒，之后 10 秒），睡着的移动硬盘、出毛病的 U 盘也要几秒；一项接一项地问，后面的搜索全排着。
- **做法**：搜索线程不碰磁盘。`RecentOnDisk` 给每个盘（盘符或 `\\服务器\共享`）一个线程去问，搜索最多等 100 毫秒；没回答的这次用上次的答案，没有就不列；一个盘手上的询问已经超过 100 毫秒，就不再等它。慢的盘在日志里记一条 `drive … took … ms`（一直慢只记一次）。等了 1 秒底栏写“正在搜索…”；一次搜索超过 2 秒，日志里记下排队、等锁、找名字、其余各花了多久。复现：Win顺 退出后，在 `%LOCALAPPDATA%\WinShun\history.txt` 最前面加一行 `\\10.255.255.1\share\线切割测试.dxf`、Tab、`1`、Tab、毫秒时间（UTF-8 无 BOM、LF），再启动，先搜别的再输入“线切割”：旧版的结果灰着停 5 秒以上，新版 0.4 秒内出结果。测完换回原来的文件。

### 保存索引时有改动进来，搜索就要等保存完

- **现象**：半小时一次的快照保存期间正好有文件变动，搜索就停到保存结束（本机 325 万项保存约 0.27 秒；盘慢、索引大时更久）。
- **原因**：Windows 上的 `std::shared_mutex` 是 SRW 锁，有写者在排队时，新来的读者也排在它后面。保存拿着读锁写文件，变更日志的写者来了排队，搜索的读锁再来就排在写者后面。小程序实测：读者拿锁 3 秒，200 毫秒时写者排队，400 毫秒时来的读者等到 3015 毫秒。保存处原来的注释“搜索照样跑，它们也只要读锁”只在没有写者时成立。
- **做法**：长时间的读（保存快照、内容索引扫一遍全表、`contentUnsureOf`）用 `FileIndex::longReadLock()`：先拿“写者门”的共享锁，再拿读锁；`writeLock()` 先拿写者门的独占锁。长读期间写者停在门外，不占读写锁上的队，搜索照常进；几个长读之间仍可同时进行。`tst_core` 的 `longReadLetsReadersPastWriters` 测这一点。

### 启动器和对话框旁的搜索框共用一个搜索引擎，互相作废

- **现象**：读代码时发现，没在用户那里见过：对话框旁的搜索框一搜，启动器正在等的结果就不来了，旧结果一直灰着，直到再打一个字；反过来也一样。
- **原因**：`SearchEngine` 只做最新的一个请求，新的 `submit()` 让之前的作废；两边共用一个引擎，又各自只认自己的请求号，被作废的那边永远等不到结果。
- **做法**：对话框的搜索框用自己的引擎（`App::m_dialogEngine`，打开这个功能时才建）。

## 后台开销和主线程卡顿

### 什么都不做时也有线程在醒

- **Qt 的垂直同步线程**：只要有 D3D11 窗口（藏着也算），Qt 就开一个 `QDxgiVSyncThread` 一直 `WaitForVBlank`，240 Hz 屏幕上每秒醒 240 次，只为了给 `QWindow::requestUpdate()` 对时（`qdxgivsyncservice.cpp`）。Qt Quick 的渲染线程靠呈现（Present）掌握节奏，不靠它；没有它时 `requestUpdate` 用一个 1–5 ms 的定时器，只在真要刷新时才跑。做法：`main.cpp` 里在建 `QGuiApplication` 之前设 `QT_D3D_NO_VBLANK_THREAD=1`。实测（2026-10-09）：空闲 5 秒醒 1209 次 → 这个线程没了。
- **双击 Ctrl 的鼠标监听**：原来键盘和鼠标的原始输入一直都收，鼠标每动一下都唤醒监听线程（游戏鼠标每秒上千次）。鼠标只用来判断连按期间有没有点击、拖动、滚轮，所以改成按下 Ctrl 时才登记鼠标，松开后超过连按间隔、第二下不可能再来时撤掉（`KeyListener::listenToMouse`）。撤掉靠定时器，按下时就开始计时，到点时 Ctrl 还按着就接着等：Ctrl 的抬起可能收不到（见上面的 Ctrl+Alt+Del），只在松开时计时的话，锁屏回来后鼠标一直开着，直到下次按 Ctrl。按着 Ctrl 时已经按下的鼠标键，照旧用 `GetAsyncKeyState` 补查。实测：每毫秒挪一次鼠标、共 5 秒（约 2770 次），监听线程从醒 1050 次降到 7 次；双击 Ctrl 的测试（含 Ctrl+滚轮、按住鼠标键、两次 Ctrl+点击）16 项照样全过。
- **每个窗口一套显卡驱动线程**：启动器和剪贴板窗口各有自己的 D3D11 设备（Qt Quick 每个窗口一个渲染线程、一个设备），NVIDIA 驱动给每个设备开约 54 个线程（这台 32 线程的 CPU 上；VS Code 的 GPU 进程也是 54 个），其中一个按系统时钟每秒醒约 65 次。窗口销毁时这些线程随设备一起收回（设置窗口关掉后，它那一份就没了），不会越积越多。`D3D11_CREATE_DEVICE_PREVENT_INTERNAL_THREADING_OPTIMIZATIONS` 对 NVIDIA 驱动没用（单个设备 21 个线程，加了还有 19 个）。常驻的两个窗口要一按就出来，这份开销保留；要省只能让多个窗口共用一个设备，那得改用 basic 渲染循环、在主线程上画，不划算。
- **怎么查**：提权运行的程序隔几秒用 `NtQuerySystemInformation(SystemProcessInformation)` 取两次 Win顺 每个线程的上下文切换次数和 CPU 时间，相减就是这段时间谁醒了多少次；再用 `NtQueryInformationThread(ThreadQuerySetWin32StartAddress)` 和 `GetThreadDescription` 认出线程是谁的（Qt 的线程有名字，驱动线程的起始地址在 `nvwgf2umx.dll` 里，我们的 `std::thread` 起始地址在 `ucrtbase.dll` 里）。普通权限读不到提权进程的线程。

### 主线程在常用操作里卡不卡

- **实测**（2026-10-09）：测试程序每 2 ms 给 Win顺 主线程上的窗口发一次 `WM_NULL`（`SendMessageTimeout`），看多久得到回应，同时走一遍打开启动器、搜文件名、输一个字母出大量结果、切到“内容”搜文字、关闭、打开和关闭设置窗口。约 8500 次里超过 8 ms 的只有：打开设置窗口两次（53、61 ms，每次重新创建窗口），关掉它一次（18–39 ms）。搜索时一次也没有，搜索都在工作线程上。
- **这种探针有盲区**：主线程在等 COM 调用或别的线程时，Windows 会顺带处理别的线程发来的消息，探针会被“插队”回答，看不出这段卡顿。要量某个命令的总耗时，就比第二个进程的寿命：`WinShun.exe --background` 什么都不转交（只是启动，47 ms），`--settings` 要等 Win顺 处理完才退出（95 ms），差值就是处理的时间。
- **剪贴板历史原来在主线程上存**。读剪贴板、算哈希、转 PNG 都在监听线程上，但单独量 `ClipStore::add`：一段普通文字 0.08 ms，100k 字 0.7 ms，截图 3 MB 约 3 ms，照片 12 MB 约 8 ms，2M 字纯文本约 15 ms，100 万字带 4 MB 网页格式约 27 ms。最坏的是网页格式和 RTF 各 16 MB（复制一大片 Excel），约 140–160 ms。
- **写库挪到单独的线程后**（见“复制一大片带格式的内容，Win顺 卡一下”）：主线程上的 `add` 不论多大都不到 0.05 ms，写库线程照样花这些时间。真机对比（2026-10-09）：探针跑着时依次复制 50 万字加网页格式和 RTF 各 15 MB 的文字、一张 1999×1499 的图片（PNG 9 MB）和一段短文字。旧版在复制大段文字时卡了 152 ms；新版整段测试没有一次超过 8 ms，三条都在库里，图片文件也写好了。

## 界面实测（模拟真实输入）

改了窗口和鼠标交互，编译通过不等于能用，要在部署好的程序上用模拟的真实输入试一遍。

- **测试程序要以管理员身份运行**：Win顺以管理员身份运行，普通权限进程用 `SendInput` 发的输入会被 UIPI 悄悄丢掉，不报错。全局热键也一样：前台是普通程序时，普通权限发的 `Win+S` 能触发 Win顺 的热键；Win顺 自己在前台时，同样的组合键整个被丢掉，热键不触发，看起来像“再按一次收不起来”。但重启资源管理器要在普通权限里做（管理员启动的资源管理器也是管理员），所以这类测试拆成两段：普通权限的脚本改设置、重启资源管理器，中间那段发键的脚本提权运行。
- **坐标按物理像素**：测试程序先调 `SetThreadDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2)`，否则拿到的窗口位置是按缩放换算过的。`SendInput` 的绝对坐标加 `MOUSEEVENTF_VIRTUALDESK`，按整个虚拟桌面归一化到 0–65535。
- **检查拖动区不用动鼠标**：`SendMessage(hwnd, WM_NCHITTEST, 0, 屏幕坐标)` 直接问窗口某一点是拖动区、按钮还是边框。
- **从 `dist\WinShun` 测**，不要从 `build\release` 启动：那里没有 Qt 插件，会报 “no Qt platform plugin”。替换前保留旧版（只换 exe 时把旧的改名为 `.bak`，整个文件夹要换时把旧文件夹改名），新版起不来就自动换回去。
- **测完让新版本留着运行**：不要在脚本结尾重启 `C:\Program Files\WinShun` 里的旧版。2026-10-08 测试脚本最后重启了旧版，用户随后测的就是那个没有新功能的版本，被误导了。只有新版起不来时才换回旧版。
- **测完恢复现场**：测试中改过的用户设置（比如点主题卡片改了 `WinShun.ini` 里的主题）要改回原值，测完核对一遍；测试临时加的日志行要从 `WinShun.log` 里删掉。
- **新 exe 第一次启动很慢**：刚复制过去的 exe 会先被杀毒软件扫描，等窗口出现要留够时间（40 秒），15 秒不够，会被误判成“窗口没出来”。
- 部署和测试放在同一次提权运行里，只弹一次 UAC；测试期间不要碰鼠标。提权的 PowerShell 用 `-WindowStyle Hidden` 启动，否则它的窗口会挡住截图、抢走焦点。
- **C# 里声明 `INPUT` 结构体别加多余的填充字段**：64 位下 `INPUT` 是 40 字节（`type` 之后因对齐空 4 字节，接着是 32 字节的 `MOUSEINPUT`）。多写两个 `int` 会变成 48 字节，`SendInput` 返回 0（参数错误），光标根本不动，测试看起来像“窗口拖不动”。要检查 `SendInput` 的返回值。
- **给 Win顺 自己的窗口打字，测试脚本也要提权**：测别的程序的对话框时不需要，测试脚本以普通权限运行就行；一旦按键要进对话框搜索框这样的 Win顺 窗口，就会被 UIPI 丢掉（屏幕上有光标，就是没字）。提权的脚本启动的对话框进程也是提权的。
- **用 `-WindowStyle Hidden` 启动的测试脚本，自己建的第一个窗口是看不见的**：Windows 把启动参数里的“隐藏”用在了进程第一次显示的窗口上。窗口照样能成为前台（`GetForegroundWindow` 就是它），屏幕上却没有，鼠标点击会穿过它落到下面的程序上。2026-10-09 测双击 Ctrl 时，这样点进了 VS Code。做法：窗口出来后再 `ShowWindow(SW_SHOW)` 一次，并检查 `IsWindowVisible`；每次按鼠标键、转滚轮之前，先把光标移到自己的窗口上，并用 `WindowFromPoint` 确认那个位置最上面就是它，不是就中止。
  - **Win顺 自己也一样，别用 `-WindowStyle Hidden` 启动它**：2026-10-09 发布 0.5.0 后用 `Start-Process WinShun.exe --background -Verb RunAs -WindowStyle Hidden` 重新启动，用户的设置窗口就打不开了。`--background` 下第一个显示的普通窗口是设置窗口，它一显示就被系统按启动参数藏起来，Qt 不知道，仍当它可见：不发 `visibleChanged`，不释放窗口，之后的 `show()` 什么都不做。重启一次（不带隐藏）就好。启动 Win顺 用 `-WindowStyle Normal`（它没有控制台窗口，不会弹出东西）。
- **测别的程序时，开单独的实例**，不碰用户自己开着的窗口和配置：Edge 加 `--user-data-dir=临时文件夹 --no-first-run --new-window`；Firefox 用 `-no-remote -new-instance -profile 临时文件夹`，文件夹里放一个关掉欢迎页的 `user.js`；Windows Terminal 用 `wt -w new --title 名字 --suppressApplicationTitle`（提权启动时标题前面会加“Administrator:”，按包含查找）；经典控制台直接启动 `conhost.exe cmd.exe /k title 名字`。点一下控制台会进入“选择”模式（标题前面出现“选择”），之后的按键算作选择操作。新版记事本别拿来测：用户开着它时，打开文件会变成用户窗口里的一个标签。
- **Windows 11 资源管理器开着几个标签时，`WM_CLOSE` 只关当前那个标签**：测试结束关窗口后，剩下的标签还开着。关之前看一下这个窗口还有几个标签，关完再查一遍。
- **会删除、移动、覆盖文件的测试，动手前先核对目标**：结果列表随时可能变。比如另一个会话重启了 Win顺，列表变成“最近使用”，脚本照原步骤点下去，就会对真实文件下手。2026-10-08 的多选测试就差点把 OneDrive.exe、msinfo32.exe 送进回收站（文件都没事）。做法：
  - 测试文件放在专门的文件夹（如 `C:\wsmulti`；`%TEMP%` 不会被索引，搜不到）。
  - 每一步先用 Ctrl+Shift+C 复制选中项的路径，读剪贴板核对，路径全在测试文件夹里才继续。
  - 每一步前检查 WinShun 进程的启动时间没变，变了立即停止。
  - 测完把测试文件从回收站里彻底删掉。
- **SendInput 发方向键要带 `KEYEVENTF_EXTENDEDKEY`**：不带的话会被当成小键盘方向键。NumLock 开着时，Windows 会在 Shift+小键盘键前后插入假的 Shift 抬起和按下，程序收到的就是不带 Shift 的方向键，Shift+↓ 测出来像“不能多选”。Home、End、PgUp、PgDn 同理。
- **找搜索框窗口要按类名和大小**：WinShun 进程里可能还开着设置窗口（别的会话或用户打开的），只取最大的可见窗口会找错。搜索框的类名是 `Qt6120QWindowToolSaveBits`，默认宽 760 逻辑像素；用户可以拖宽（`state.ini [Launcher] Width`，640 到工作区宽），别按宽度认，按类名和 `--dump` 的 `geometry` 认。
- **截图取色**：`Graphics.CopyFromScreen` 截的是 DWM 合成后的画面，能截到 Mica 和亚克力。取色坐标按物理像素，而且要落在窗口里面：150% 下 720 逻辑像素高的窗口是 1080 物理像素，按逻辑像素 800 取色就越界了。
  - 要确认亚克力的模糊是真的，在窗口后面垫一个彩色竖条纹窗口再截图；背后是一片纯色时，模糊和平涂看不出区别。Mica 只取桌面壁纸，垫东西没用。
- **每次发按键前确认前台是 Win顺**：窗口“可见”不等于在前台。启动后的隐身预画也算可见；用户正在用电脑时，前台随时会变。2026-10-08 一次测试把 Ctrl+A、Ctrl+V、Ctrl+Shift+C 发到了别的窗口。做法：`GetForegroundWindow()` 属于 Win顺 进程才发；不是就中止测试，不重试。
- **不要发 Ctrl+1/2/3**：这台电脑上的截图工具 PixPin 把它们注册成了全局热键，发出去就会进入截图模式（之后的截图全是冻结的画面）。2026-10-08 两次测试都踩了。切换范围用 Tab；窗口也会记住上次的范围。
- **用 `SendKeys` 往搜索框打字会进输入法**：中文输入法（如微信输入法）会把 `readme` 当拼音组合，不会真的搜索。要测搜索，先切到英文输入，或者用剪贴板粘贴。
- 贴靠布局的浮层没有出现在自动测试的截图里（最大化按钮的悬停高亮有），它是否正常弹出要手动确认。
- **PowerShell 的坑**：
  - 函数名别和内置别名重名：别名优先于函数，如 `r`（Invoke-History）、`sp`（Set-ItemProperty）。
  - `@(...)` 里逗号比 `+`、`-` 结合得更紧：`@($a + 1, $b)` 要写成 `@(($a + 1), $b)`。
  - pwsh 7 的 `Start-Process -Wait` 会等所有子孙进程，测试脚本里启动了 Win顺就会一直卡住；改用 `-PassThru` 再 `.WaitForExit()`。
  - 要用 `System.Drawing` 截图量像素时，用 Windows PowerShell 5.1。
  - 给 Windows PowerShell 5.1 运行的脚本里有中文时，要存成带 BOM 的 UTF-8，否则中文（如找窗口用的标题“设置”）会读成乱码。
  - 变量名不区分大小写：`$seq` 和参数 `$Seq` 是同一个变量；2026-10-09 一个脚本用 `$t` 暂存设置文件的内容，把放测试文件夹路径的 `$T` 冲掉了，打进搜索框的成了整个设置文件。参数声明了 `[string]` 时，给它赋一个数组会被转回一个字符串，`foreach` 只循环一次。局部变量换个名字。开关参数也一样：函数有 `[switch]$Aware` 时，`$aware = 0` 会报“无法转换为 SwitchParameter”。
  - PowerShell 把中文的弯引号也当引号：`’` 会结束单引号字符串，`“”` 会结束双引号字符串。英文翻译里的 `Windows’` 写进单引号字符串就是语法错误。用占位符代替，运行时再换成 `[char]0x2019`。
  - `$null` 传给 C# 方法的 `string` 参数会变成空字符串：`FindWindowEx(h, 0, '类名', $null)` 实际在找标题为空的窗口，有标题的（如资源管理器的标签页）就找不到。要传 null 的调用写在 C# 里。
  - 刚关掉的资源管理器窗口在 `Shell.Application` 的 `Windows()` 里还会列一会儿。测试新开一个窗口后按路径找它，要排除开之前就有的窗口，否则会拿到正在关闭的旧窗口。
- **测剪贴板时，测试内容也进了 Windows 自带的剪贴板历史**：用户开着 Windows 的剪贴板历史时，测试脚本复制的每一条都会进去。那里最多 25 条，会把用户原来的挤掉，而且没法恢复。测试前告诉用户，测试条目尽量少。
- **别的功能的测试改了剪贴板，也会进 Win顺 自己的剪贴板历史**：比如对话框搜索框的“剪贴板里的路径”要往剪贴板里放路径。在带剪贴板历史的版本上测完，要把这些测试条目从历史里删掉（2026-10-08 删过两条），再把用户原来的剪贴板文字放回去。更省事的办法是一开始就不让它们进去：历史按剪贴板所有者的程序过滤，测试期间把 `pwsh.exe` 和 `WinShun.exe`（Win顺 自己的“复制路径”）临时加进 `WinShun.ini` 的 `[Clipboard] ExcludedApps`；结束时先放回用户的剪贴板、等一下，再还原设置文件。前后用只读方式（`sqlite3` 的 `mode=ro`）数一下 `clipboard.db` 里 `clips` 的条数核对。
- **模拟单击的按下和松开在同一次 `SendInput` 里**：程序处理激活（`WM_ACTIVATE`）时，`GetAsyncKeyState(VK_LBUTTON)` 已经是松开的，按“鼠标是不是按着”来判断“是不是点进来的”在测试里永远不成立，真人点击却成立，测不出问题。要判断就看随后到来的鼠标事件（见“点进搜索框，对话框却跳到了第一个建议”）。
- **提权运行的测试脚本没跑起来时，读到的是上一次的日志**：用 Windows PowerShell 5.1 运行只有 pwsh 7 才能解析的脚本，提权的隐藏窗口里报错谁也看不见，日志文件还是上一次的，结果一字不差。读日志前先看时间、窗口句柄这些每次都变的值，或者先把旧日志改名。
- **Qt 会在窗口标题后面加上程序的显示名**：标题设成“剪贴板”，Windows 里的标题是“剪贴板 - Win顺”（`QPlatformWindow::formatWindowTitle`，标题和显示名相同时不加，所以启动器还是“Win顺”）。按标题找窗口要按开头匹配，并限定 WinShun 进程。Windows 自己的设置应用标题也是“设置”：只按“设置”开头找，2026-10-10 找到的是它，滚轮差点发给它。Win顺 的设置窗口要匹配“设置 - Win顺”。
- **Win顺 刚启动时，测试脚本写剪贴板可能失败**：`Set-Clipboard` 报 “Requested Clipboard operation did not succeed”，剪贴板正被别的程序打开着（Win顺 的监听线程在读）。写剪贴板要隔一会儿重试几次。
- **PowerShell 的 `System.Windows.Automation` 有时读到的任务栏是空的**：2026-10-10 有一阵 `AutomationElement.FromHandle(Shell_TrayWnd)` 一个后代都没有，测试脚本找不到 `SearchButton`；同一时刻 Win顺 里用 COM 的 `IUIAutomation`（`CUIAutomation8`）照样找得到，过了一会儿 PowerShell 也恢复了。脚本找不到元素时，先换个时间或换用 COM 的接口再确认，别急着断定界面变了。
- **截图脚本要先开 DPI 感知**：Windows PowerShell 5.1 不感知 DPI，150% 时 `GetWindowRect` 给的是换算后的逻辑坐标，`CopyFromScreen` 却按物理像素拍，拍到的是别处（2026-10-10 第一次拍任务栏，拍成了上面的 VS Code）。脚本开头先 `SetThreadDpiAwarenessContext(-4)`（每显示器 V2），窗口位置、鼠标位置和截图就都是物理像素了。
- **前台窗口类名是 `GHOST_WindowClass`，不是程序卡死了**：那是 Blender 的窗口（它的窗口库叫 GHOST）。Windows 给没响应的程序换上的幽灵窗口，类名是 `Ghost`。
- **Windows 自带的剪贴板面板不抢前台**：按 `Win+V` 后 `GetForegroundWindow()` 还是原来的窗口，判断面板是否打开要截图；前台换了它也不关，要点它的关闭按钮。
- **磁盘弹出和锁定不需要真硬件**：用 diskpart 建一个 VHD，挂上并格式化成 NTFS（挂上的 VHD 算固定磁盘，会被索引）。`FSCTL_LOCK_VOLUME` 模拟格式化、chkdsk 的锁定，`CM_Query_And_Remove_SubTreeW` 模拟弹出。弹出后 `diskpart detach vdisk` 会失败（0x80070057），改用 `Dismount-DiskImage`。
- **PowerShell 把 `$null` 传给 .NET 的字符串参数时变成空串**：`FindWindowExW(0, $h, '#32770', $null)` 实际传的是 `""`，只找标题为空的窗口，一个对话框都找不到。要传真正的空指针，写 `[NullString]::Value`。
- **PowerShell 字符串里 `"$h:"` 是语法错误**：冒号前的变量名被当成作用域（`$env:` 那种），整个脚本解析失败、一条命令都不执行。写成 `"${h}:"`。
- **测试脚本写日志不能抛异常**：读日志的一方偶尔会占着文件，`AppendAllText` 抛出“文件被占用”，异常一路传上去，提权的测试脚本就退出了（2026-10-10 一次），后面的命令全没人执行。写日志失败就隔 50 毫秒重试，几次后放弃，不往外抛。
- **启动器按 `Tab` 的顺序是 全部 → 文件 → 文件夹 → 内容**：要到“内容”是按两次（从“文件”开始）或三次（从“全部”开始），算成两步一次就会停在“文件夹”上。范围不跨重启保存，Win顺 刚启动时总在“全部”。
- **钉住的启动器，关掉别的窗口后它就在前台**（2026-10-15）：测试脚本关掉刚从启动器打开的窗口，Windows 把前台交给 z 序里下一个窗口，就是钉住、置顶的启动器。脚本接着发 `--toggle` 想“叫它到前台”，它已经在前台，按开关的规矩收起了，后面的按键全落空。发开关命令之前先读一下前台是谁（或者用点击把它激活）。
- **任务栏框的测试里，“别的窗口”别放在左下角**：从任务栏框打开的启动器贴着任务栏、在框上方，满高；脚本要点的另一个窗口放在左下角就被它盖住，点击的守卫（`WindowFromPoint` 的根窗口）会中止。放在右上角。

### `Start-Process -ArgumentList` 把带空格的参数拆开

- **现象**：测试脚本 `Start-Process WinShun.exe -ArgumentList '--query', 'bm Tinkercad'`，搜索框里只有 `bm`，书签网站图标的检查得出假阴性（2026-10-13）。
- **原因**：Windows PowerShell 5.1 把数组元素用空格直接拼成命令行，不给带空格的元素加引号。
- **做法**：自己拼引号：`-ArgumentList ('--query "' + $q + '"')`。拿不准时截一张屏，看程序实际收到了什么。

### 用脚本往 Excel 里加宏：`AccessVBOM` 删了又被 Excel 写回去

- **现象**：测“浏览文件夹”对话框（2026-10-16）时，脚本临时把 `HKCU\Software\Microsoft\Office\16.0\Excel\Security\AccessVBOM` 设成 1，加完 VBA 模块马上删掉；跑完一看又是 1。`$xl.Quit()` 之后 EXCEL.EXE 还要好几秒才退。
- **原因**：Excel 启动时读这些信任设置，退出时把内存里的那份写回注册表，盖掉了中途删的结果。
- **做法**：等 EXCEL.EXE 都退出了再删 `AccessVBOM`，删完再读一次确认。宏里弹的对话框属于 EXCEL.EXE（不是脚本的进程），按进程名找它的 `#32770`。

## README 和帖子的截图（tools/screenshots/）

用法见 `tools/screenshots/README.md`，一次拍全部用 `agent.ps1 scenes\all.txt`，出图用 `shots.py render`。流程：`profile` 做一个只索引演示文件夹的配置，`WinShun.exe --profile <文件夹>` 用它启动；`scene` 换主题和壁纸；用真实输入把窗口摆出来，`shoot` 抠出来；`compose` 放到背景上（README 和帖子里封面以后的几张），`cover` 做帖子的封面（窗口立体地斜放，配大标题、桂冠里的两句话和两个 Ctrl 键）；最后 `restore`，用户自己的 Win顺 重新启动。

### Mica 的颜色来自桌面壁纸，不是窗口后面的东西

- **现象**：同一个窗口放到暖色背景上，窗口自己却偏冷灰，像贴上去的。
- **原因**：Windows 11 的 Mica 只取桌面壁纸的颜色（模糊后），窗口后面是什么都不影响。
- **做法**：拍之前 `scene --wallpaper dawn` 把壁纸临时换成要合成的那张背景（`SPI_SETDESKWALLPAPER` 只带 `SPIF_SENDCHANGE`，不写进用户设置），窗口的底色就和背景同一色调；`restore` 换回原来的壁纸。原来的壁纸不是图片文件（幻灯片、纯色、聚焦）时 `scene` 不换，免得换不回去。

### 提权进程新建的窗口，`SetWindowPos(HWND_TOPMOST)` 报成功却没置顶

- **现象**：抠图用的黑、白背板要插在被拍的窗口和其它窗口之间。被拍的窗口是置顶的（启动器），背板跟着置顶，结果它留在 VS Code 下面，抠出来的图里混进了 VS Code 的一块。
- **原因**：2026-10-10 实测：提权运行、前台是别的程序时，刚建的窗口调 `SetWindowPos(HWND_TOPMOST)` 返回成功，Z 序却没变。非置顶窗口本来也放不到前台窗口上面。
- **做法**：背板创建时就带 `WS_EX_TOPMOST`，再把被拍的窗口按原来的前后顺序重新放到它上面；放好后检查和拍摄区域相交的其它窗口有没有在背板前面，有就提示。`shoot` 拍三张（黑、白、黑），两张黑的不一样（光标闪了、动画没停）就重拍。

### 剪贴板窗口不在前台，拍到的是记事本

- **现象**：开着记事本按 `Win+Alt+V`，`shoot` 拍的是记事本。
- **原因**：剪贴板窗口和 Windows 自带的一样不抢焦点，前台还是记事本。
- **做法**：`shoot --winshun`，拍 Win顺 最前面的那个窗口，不看前台。

### 演示配置里混进自己的东西

- **现象**：剪贴板历史里出现了 VS Code 里复制的代码；对话框旁的搜索框 `Ctrl+G` 转到的是自己正开着的文件夹。
- **原因**：演示用的 Win顺 照样记下这段时间里复制的一切；`Ctrl+G` 去的是资源管理器里开着的文件夹。
- **做法**：每次用演示配置启动前清空它的剪贴板文件夹（只在有 `made-for-screenshots.txt` 标记的文件夹里删）；剪贴板内容由输入代理放进去（`scenes/clipboard.txt`），在记事本里粘贴再复制，来源就是“Notepad”。拍对话框前先让资源管理器打开演示文件夹里的一个文件夹。逐字发 Unicode 输入往记事本里打中文，打出来的不对，所以文字一律走剪贴板。

### 抠出来的窗口带着自己的阴影，阴影在截图边上断掉

- **现象**：合成后窗口下面有一片阴影，到某一行突然断掉，小红书的图里很明显。
- **原因**：抠图时 Windows 给窗口画的阴影也一起抠了下来。阴影比截图区域（窗口外 64 px）宽，在截图边上被切断；深色主题下更浓，贴着窗口处透明度到 0.44～0.49。
- **做法**：合成前把这层阴影去掉（`lifted`），另外生成阴影：三层，一层比一层软、往下落得更远，颜色取自落到的背景。在比画面大一圈的画布上算，窗口跑出画面时阴影也跟着出去，哪里都不会有边。

### 去掉窗口的阴影时，窗口和 logo 要分开算

- **现象**：去完阴影，窗口里面一块发白一块正常。
- **原因**：阴影是透明度为 s 的黑色。它在窗口下面：看到的颜色只是窗口自己，C = l·L/a。探出来的 logo 在窗口后面，窗口的阴影盖在它上面：C = (1−s)·l·L/a。窗口也按后一种算，每个像素都被除以 (1−s)；窗口里的 s 是从四周推出来的，一圈一圈不一样，就成了一块块亮斑。
- **做法**：只在 logo 的范围里按“阴影在上面”算，其余按“窗口在上面”算。对话框和下面的搜索条之间只隔 6 px：离窗口多近算“看不出阴影多浓”的范围只能取 2 px，取 3 px 时缝里整条都推不出来，留下一道暗线。

### 测试脚本点不到任务栏上的搜索框

- **现象**：点击前的保护检查报“点的地方是别的程序”，任务栏搜索框点不开。
- **原因**：任务栏搜索框（类名 `WinShun.TaskbarSearchBox`）是 `Shell_TrayWnd` 的子窗口。`WindowFromPoint` 再取 `GA_ROOT` 拿到的是任务栏，属于 explorer，不是 WinShun。
- **做法**：只对这个框单独放行：检查 `WindowFromPoint` 本身（不取根窗口）的类名是 `WinShun.TaskbarSearchBox` 再点。别为了点它把“根窗口属于 WinShun”这条检查整体放松。

## AI 助手（MCP）

### 管道一个线程在读，另一个线程就写不进去

- **现象**：WinShunMcp.exe 连上 Win顺 以后，第一条转发过去的通知之后就没有反应了：主线程卡在往管道写，读线程卡在读。
- **原因**：以同步方式（没带 `FILE_FLAG_OVERLAPPED`）打开的管道句柄，一个句柄上的 I/O 是排队做的：读线程的 `ReadFile` 等着 Win顺 发东西，这时主线程的 `WriteFile` 要等那个读完成才轮到，而 Win顺 不收到写过去的请求就不会发东西，两头互等。
- **做法**：客户端一端（`pipe::Client`）用 `FILE_FLAG_OVERLAPPED` 打开，读和写各用自己的 `OVERLAPPED` 和事件，`GetOverlappedResult` 等完成；这样一个线程一直读着，另一个线程照样能写。服务端（`pipe::Server`）本来就是重叠 I/O。关闭时 `CancelIoEx` 让读线程的读返回。

### 管理员进程建的管道，普通权限的进程连不上

- **现象**：Win顺（管理员）建的命名管道，AI 客户端以普通权限启动的 WinShunMcp.exe 打不开（拒绝访问）。
- **原因**：提权进程的默认 DACL 只给 Administrators 和 SYSTEM，普通权限的令牌里 Administrators 组是“仅拒绝”，对不上；而且提权进程建的对象完整性标签是“高”，中等完整性的进程写不了。和任务栏按钮用的事件是同一个问题（`taskbar::createEvent`）。
- **做法**：建管道时明确给安全描述符：`D:P(A;;GA;;;SY)(A;;GA;;;BA)(A;;FRFW;;;<用户 SID>)S:(ML;;NW;;;ME)`——用户自己能读写，标签降到“中”（`NW`：低完整性的进程仍然连不上，浏览器沙箱里的东西进不来）。管道名里带用户 SID（`WinShun.Mcp.S-1-5-21-…`），同一台机器上两个用户各用各的；第一个实例带 `FILE_FLAG_FIRST_PIPE_INSTANCE`，别的程序抢先建了同名管道就报错而不是接上去。

### 管道客户端刚连上就取它的令牌，取不到

- **现象**：`ConnectNamedPipe` 一回来就 `ImpersonateNamedPipeClient`，失败（`ERROR_CANNOT_IMPERSONATE`）。
- **原因**：客户端的安全上下文随它的第一次写入到达，没写过数据就无从模拟。
- **做法**：第一次读到客户端的数据之后再模拟，`OpenThreadToken` 复制出一个模拟令牌（客户端允许到什么级别就复制到什么级别：先试 `SecurityImpersonation`，不行再 `SecurityIdentification`），`RevertToSelf`，令牌交给会话；之后的访问检查都用它（`Visibility`）。鉴别级别的令牌不能用来打开文件，但 `AccessCheck` 接受；Win顺 自己的链接令牌（`TokenLinkedToken`，没有 SeTcbPrivilege 时）也只是鉴别级别，所以权限判断一律走 `AccessCheck`，不走“模拟后 CreateFile”。

### 测试里用 QProcess 驱动中继程序，隔一两条就收不到回复

- **现象**：tst_mcp 用 `QProcess` 启动 WinShunMcp.exe，写几行、读几行，第三四条起 `readAllStandardOutput()` 一直是空的，而中继的 stderr 显示它已经回了。
- **原因**：没有细究 `QProcess` 在 Windows 上异步读写的时机（`waitForBytesWritten` 和事件循环驱动的读之间），复现不稳定；另一半是上一条的管道互等，当时还没找到。
- **做法**：测试直接用 Win32：`CreatePipe` + `CreateProcessW`，一个线程 `ReadFile` 它的 stdout，主线程 `WriteFile` 它的 stdin，中间用 `QTest::qWait` 跑事件循环（服务端在测试线程上，要靠事件循环回答）。AI 客户端启动 MCP 服务器就是这样做的，测的也更真。

### 不抢焦点的通知窗口：`HoverHandler` 不能直接放在 `Window` 里

- **现象**：`SettingsNotice.qml` 想在鼠标停在窗口上时暂停自动收起的计时器，把 `HoverHandler` 直接写在 `Window { }` 下，加载时报它没有父 Item。
- **原因**：`Window` 不是 Item，指针处理器要挂在 Item 上。
- **做法**：窗口里放一个 `Item { anchors.fill: parent; HoverHandler { id: hover } }`，计时器的 `running` 绑 `!hover.hovered`。窗口不激活（`WS_EX_NOACTIVATE`）也照常收到悬停和点击，所以按钮可以点而 AI 客户端仍在前台。

### QML 调不到 App 的私有 `Q_INVOKABLE`

- **现象**：AI 改完设置后的通知上点“撤销”，按钮有按下的样子，设置却没变；预览渲染和单元测试都看不出来。
- **原因**：`undoSettingsBatch` 等写在 `App` 的 `private:` 里。moc 照样登记它，用 `metaObject()->indexOfMethod` 连接（确认卡片的 `answered` 就是这样连的）能调到；但 QML 引擎从对象上找方法时跳过私有方法，`host.undoSettingsBatch(...)` 只在日志里报 TypeError。
- **做法**：要让 QML 直接调用的 `Q_INVOKABLE` 放在 `public:` 里。新加的 QML → C++ 调用要真点一次，离屏预览不会触发按钮。

### 用 Python 改 `.ts`：Windows 上文本模式把换行写成 CRLF

- **现象**：脚本填完译文后 `git diff --stat` 整个 `winshun_en.ts` 都变了，还有“CRLF will be replaced by LF”的提示。
- **原因**：`open(p, 'w')` 在 Windows 上把 LF 写成 CRLF，而仓库里的 `.ts` 是 LF。
- **做法**：用 `open(p, 'wb')` 写字节，或 `open(p, 'w', newline='')`。检查有没有丢条目：用 `xml.etree` 把 HEAD 和现在的 `(context, source, comment)` 各读成一张表比较（`git diff` 里 lupdate 插入新条目会让旧条目看起来像删了又加）。

### 两个搜索引擎的请求编号会撞，内容搜索还先发一个空结果

- **现象**（2026-10-24，给内容搜索单独一个引擎时）：要是两个引擎的 `resultsReady` 接到同一个处理函数、只比请求编号，名字搜索会拿到一个空的结果就收尾了。
- **原因**：每个 `SearchEngine` 的请求编号各自从 1 数起（`++m_counter`），两个引擎上同时在跑的请求可以都是 1；`runContentSearch` 一开始就发一个 `resultsReady(id, {}, 0, 0)`（给界面清空列表用），这个空结果的编号正好对得上另一个引擎上的名字搜索。
- **做法**：`ToolHost` 每个引擎一条车道（`Lane`），信号按车道接，只认自己引擎发的；在跑的那个记着是不是内容搜索，名字的车道不收内容的信号。以后再加引擎也一样：编号只在一个引擎里唯一。

### `kind: "files"` 回来的文件比 limit 少，甚至一个没有

- **现象**：用户设了“文件夹在前”，查询又匹配很多文件夹时，`search_files` 的 `kind: "files"` 只回几个文件或一个没有，`truncated` 却是 true。命令行的上限放到 50 000 以后更容易碰到。
- **原因**：引擎把文件和文件夹分开各取到 `limit` 个（`kindsApart`），再按 `kindOrder` 排在一起、截成 `limit` 个；文件夹在前时，被截掉的是排在后面的文件，之后才按 `kind` 把文件夹去掉。
- **做法**：要文件时请求里用 `KindOrder::FilesFirst`（不管用户的设置），截掉的是文件夹；要文件夹时本来就是 `Scope::Folders`。

### 超过 16 MB 的一行，客户端直接断开，不报错

- **现象**：回答太大时（命令行一次要 50 000 条、路径又长，或 `read_file_text` 要几百万字），客户端那边像是 Win顺 掉线了，收不到任何错误。
- **原因**：`pipe::Client::readLine`（ws.exe 和 WinShunMcp.exe 都用）读到 `kMaxLineBytes`（16 MB）还没见到换行就当连接坏了，断开。服务端照写不误，所以服务端的日志也看不出问题。
- **做法**：`McpServer::response` 序列化完先量长度，超了就改回一个 `isError` 的工具结果（命令行的会话带 `structuredContent.error: "too_large"`），文字说多大、上限多少、怎么要少一点（小一点的 limit 或 max_chars、少几个文件夹、写细查询、`size: false`），mcp.log 记一行，会话照常。实测 50 000 条约 8.9–9.5 MB（每条 178–190 字节），每条平均超过约 335 字节才会撞线。可这样一撞线就什么也拿不到（见下面“回答一大就整个变成错误”），这只留作最后一道关。

### 列 50 000 条时，时间的文字比搜索本身还慢

- **现象**（2026-10-24，`wsbench --mcp-host --frozen`，真实索引 367 万项）：ws 要 50 000 条（不带大小）服务端用 2.0–2.4 s，引擎只占约 0.33 s，访问检查 0.6–0.9 s，剩下约 1 s 是组条目。
- **原因**：几乎都在 `isoTime`：`QDateTime::fromSecsSinceEpoch` 按本地时间换算、`offsetFromUtc`、`toOffsetFromUtc`、`toString(Qt::ISODate)`，每条约 15–20 µs；组 `QJsonObject` 和放进数组只占零头。
- **做法**：默认 100 条只占约 2 ms，先没改。要改就按 UTC 的一天缓存本地偏移（一天的两头偏移相同就整天用它，不同就逐条换算）、自己拼 ISO 文字，并用 `QDateTime` 的结果逐个对照测试（包括偏移为 0 和有夏令时的时区）。量这类开销时别把计时器套在会调用同一函数的两处：`fileItem` 里面也调 `isoTime`，分开计会算两遍。

### 设置页的“命令行”放不进“连接客户端”标签（2026-10-24）

- **现象**：想把“启用命令行”放在客户端列表旁边（那里还有 Claude Code 的“装 Skill”）。可“允许 AI 助手和命令行连接”关着时，这一页没有标签：`SettingsWindow.qml` 的 `pageTabs` 在 `editor.mcp` 为假时给 `[]`，只剩第一个标签（权限）里的开关；放在第二个标签里的“启用命令行”，开关关着时点不到，而它正是用来打开开关的。
- **做法**：“命令行”这一节（`AiCommandLineSection`）放在**权限**标签里、开关那张卡的下面（`tab` 不写，和开关同一个标签）；“装 Skill”留在客户端那一行，它要开关开着才有那一行。代价：开关开着时，这一节在六行权限的下面，一屏（720 px 高的窗口）看不到，要往下滚，设置搜索“命令行”“ws”“终端”会直接带到它。不想滚的话，要把权限那些行拆成另一张卡，`unlocks` 跨节引用（现在靠 `lockedElsewhere` 传）要跟着改，没有为这一项动。

### 设置搜索“ws”：带出一堆含 “Windows” 的行（2026-10-24）

- **现象**：设置里搜 `ws`，二十项：隐藏任务栏上的 Windows 搜索、用 Win+S 打开 Win顺 ……都在里面。
- **原因**：说明文字按字面（子串）匹配，“Windo**ws**” 里有 “ws”；不是新加的关键字造成的（搜 `ws` 的关键字命中分数更高，“ws 命令”排第一，回车去的是它）。
- **做法**：不动匹配器（它是整个设置共用的，规则在 `SettingsMatch.cpp`）；两个字母以内的词本来就容易撞上说明里的词。别因此把“ws”去掉：这是用户最先会敲的词。

### 改用户的 PATH：读原始值，只追加，只拿自己记下的那一项（2026-10-24）

- **现象**：（设计阶段想到的，测试里钉死的）用 `QSettings` 读 `HKCU\Environment` 的 `Path` 再写回，值被展开：`%USERPROFILE%\bin` 变成 `C:\Users\某人\bin`，类型从 `REG_EXPAND_SZ` 变成 `REG_SZ`，别的程序的条目就被改了；用户改了用户名或移动了文件夹之后那些条目是死的。
- **原因**：`QSettings` 的注册表后端读 `REG_EXPAND_SZ` 时展开环境变量，写回只写 `REG_SZ`。
- **做法**：`cli::readValue` 用 `RegGetValueW` + `RRF_NOEXPAND` 读原始文字和类型；`withEntry` 只在末尾追加（已有就原样返回），`withoutEntry` 只去掉对得上的项和它的一个 `;`，引号、`%VAR%`、空项一个字符不动；写回时类型照旧，没有这个值才建 `REG_EXPAND_SZ`。“在用户 PATH 里”不等于“Win顺 加的”：“只为我”安装的安装程序也写 `HKCU\Environment`，所以设置页只拿 `HKCU\Software\WinShun\CliPathEntry` 记着的那一项（`cli::addToUserPath` 真写了才记；已经有就不记）。测试在 `HKCU\Software\WinShunTest\<guid>` 的键里做，不碰真的。Inno 里同理：`RegQueryStringValue` 不展开，`RegWriteExpandStringValue` 写回，不用 `[Registry]` 的 `{olddata}` 拼接，也不用 `uninsdeletevalue`（那会删掉整个 PATH）。

### Windows 上设环境变量：`qputenv` 走 ANSI，`SetEnvironmentVariableW` Qt 读不到（2026-10-24）

- **现象**：`tst_commandline` 里把 `CLAUDE_CONFIG_DIR` 设成带中文的临时路径，`McpClients::skillPath()` 读到的是乱码（`é…•ç½®`）；改用 `SetEnvironmentVariableW` 后，`qEnvironmentVariable` 读到的还是原来的值。
- **原因**：`qputenv` 按本地 8 位代码页把字节交给 C 运行库（UTF-8 字节被当成 GBK / 1252）；Qt 读环境变量读的是 C 运行库自己的那份拷贝，`SetEnvironmentVariableW` 只改操作系统的那份，拷贝不更新。子进程拿的是操作系统的那份。
- **做法**：测试和预览用 `_wputenv_s`（宽字符，两份一起更新）。提权运行时 `CreateProcessWithTokenW` 起的子进程拿的是令牌的环境，拿不到 Win顺 进程里临时设的变量：`tst_commandline` 的装 Skill 一项提权时跳过，否则 ws 会写进真的 `~\.claude`。

### 从界面程序起一个控制台程序并拿它的输出：`startUnelevated` 不够（2026-10-24）

- **现象**：“装 Skill”要运行 `ws.exe skill --install claude`，成功读标准输出里的路径，失败读标准错误里的原因。`shell::startUnelevated` 只回一个进程句柄，没有输出；而且不带 `CREATE_NO_WINDOW`，界面程序（`WinShun.exe` 是 GUI 子系统）起的控制台程序会开一个黑窗口。
- **做法**：`shell::runUnelevatedCaptured`：一根匿名管道，标准输出和标准错误都接到写端（ws 成功和失败不会同时写），标准输入接 `NUL`；没提权时 `CreateProcessW` 带 `PROC_THREAD_ATTRIBUTE_HANDLE_LIST`，只有这两个句柄进子进程；提权时 `CreateProcessWithTokenW` + `STARTF_USESTDHANDLES`（见上面“`CreateProcessWithTokenW` 不接受 `EXTENDED_STARTUPINFO_PRESENT`”，句柄由 seclogon 复制进去；提权时在测试里实测过，输出、退出码、超时结束都对）。读不用阻塞的 `ReadFile`：每 10 毫秒 `PeekNamedPipe` 一次再等进程，这样超时能真的结束它，不会卡在读上。

### 一行三个按钮，窗口最窄时按钮旁边的文字只剩一百来个像素（2026-10-24）

- **现象**：Claude Code 那一行加了“装 Skill”，在 760 逻辑像素宽的窗口里，三个按钮占掉一多半，说明只剩约 110 px，“已装 Skill · C:\Users\…”这种路径在字当中折断（`Text.Wrap` 遇到太长的词就在任意位置断）；命令行那一行的“另一个 ws 在 C:\Users\…”也一样断在 “C:” 后面。
- **做法**：路径单独成行（`"…：" + "\n" + 路径`，和“其他支持 MCP 的客户端”那一行一样），Skill 的状态放进行的 `body`（占满整行，`bodyShown` 在没有字时关掉，否则别的行底下多出 12 px 空白）。用 `--preview-settings … --page 9 --width 760` 看最窄的样子。

### 回答一大就整个变成错误，找到的一条也给不了（2026-10-24）

- **现象**（代码审查发现，测试里复现）：`ws find budget -n 0` 搜到 2 万个长名字（每条约 860 字节），回答 16.5 MB，ws 只拿到 `too_large`；`ws text` 读一个满是控制字符的 3 MB 文件也会这样（每个字转义成 6 字节，算下来 18 MB）。
- **原因**：一行最多 16 MB，旧的做法是整个回答做好了再量，超了就换成错误，前面找到的全丢。条数上限管不住字节数：路径长、中文多、控制字符多时，同样的条数能差好几倍。
- **做法**：每个回答一个字节预算（`kAnswerBudget` 15 MB；AI 客户端三分之一，它的回答连同文字副本发两遍，文字里的引号、反斜杠还要再转义一次），条目按 JSON 的字节数（`jsonBytes`：控制字符按 6、引号和反斜杠按 2、汉字按 3，只会往多里算）一条条放，放不下就停，`truncated: true`；文字放到预算为止（`fitText`，不切开代理对），`next_offset` 接着读。`too_large` 只剩切不开的（插件工具的回答）。

### 拿着索引的读锁等线程池，所有搜索跟着卡住（2026-10-24）

- **现象**（代码审查发现）：两个 `recent_files` 同时跑（AI 代理一次起好几个 ws）时，第二个拿着索引的读锁在等线程池；这时变更日志要写索引，之后所有名字搜索都卡住，直到第一个扫完（命令行 50 000 条时一秒多）。
- **原因**：`WorkerPool::parallelFor` 一次只给一个调用者（`m_callMutex`），第二个在里面排队，可它进去之前已经拿了 `readLock`；`std::shared_mutex`（SRW 锁）有写的人在等时不再放新的读者进来，于是写的人等第二个，搜索等写的人。
- **做法**：先拿 `ToolHost::m_scanMutex`（一次一个扫描）再拿索引的锁，绝不拿着索引的锁等线程池；长时间的读用 `FileIndex::longReadLock()`（读者先过 `m_writerGate`，写的人在闸门外等，不占着 SRW 锁，普通 `readLock` 的搜索照常）。`list_folder` 列几十万项、`list_documents` 翻所有文档也用它。

### 按字面比对“不让 AI 看到的文件夹”，联接点、短名、`\\?\` 都绕得过去（2026-10-24）

- **现象**（代码审查发现，测试里复现）：排除了 `…\private-folder`，`read_file_text` 读 `…\link\p.txt`（`mklink /J …\link …\private-folder`）照样读出内容；8.3 短名 `…\PRIVAT~1\p.txt`、`\\?\` 开头的同一路径、有排除时的 `\\localhost\C$\…` 也一样。
- **原因**：排除只比对调用者写下的字面（`nativePath` 后按前缀比），Windows 打开时才跟着联接点、符号链接走，才把短名换成长名；`\\?\` 开头的路径连 `.`、`..`、`/` 的整理都跳过。
- **做法**：设备路径（`\\?\`、`\\.\`、`\??\`，`/` 写的也算）直接拒绝；其余先打开（只要 `FILE_READ_ATTRIBUTES`，`BACKUP_SEMANTICS` 让文件夹也能开，`OPEN_NO_RECALL`），`GetFinalPathNameByHandleW(FILE_NAME_NORMALIZED)` 取最终的长路径，按它比对排除、做访问检查，读就读这个句柄（`ReOpenFile` 加读权限，检查完路径再被换掉也读不到别处）。有排除时，网络路径的主机是本机、或打开后卷序列号和某个排除文件夹的卷相同，也当看不到。没堵上的：硬链接（最终路径就是打开用的那个名字）、指进排除文件夹的 `.lnk`（`open_item` 交给资源管理器，它跟着快捷方式走）。测试先看临时文件夹本身是不是短名写的（`GetLongPathNameW`），是就跳过：最终路径是长名，和短名写的排除对不上。

### 列不了的文件夹里的文件被说成“是文件”（2026-10-24）

- **现象**（代码审查发现）：`search_files` 的 `folders`（`list_folder` 的 `path`、`recent_files` 的 `folder` 也一样）给一个调用者列不了的文件夹里的文件，回“The path is a file, not a folder”，等于告诉它那里有这么个文件；不存在的文件回的却是“不在索引里”。
- **原因**：先看索引里是不是文件夹，后做访问检查。
- **做法**：`checkFolders` 按这个顺序：在掉线的共享上 → 索引里没有 → 是文件时先看调用者看不看得到，看不到就和不存在说法一样（`not_found`）→ 看得到才说是文件 → 文件夹列不了也说不在。

### 掉线的共享上的结果，两次访问检查等了 2.7 秒（2026-10-24）

- **现象**（把修改改回去试的）：索引里有一个不存在的服务器上的共享（`\\ws-test-gone\share`），搜到它上面的两条，回答用了 2.7 秒，mcp.log 记着“2 access checks, 2710 ms”；命令行一次几千条时就是几分钟，还占着处理回答的线程。
- **原因**：`GetNamedSecurityInfo` 对 UNC 路径要连服务器，服务器不在时等名字解析和 SMB 连接超时；读大小（`sizeOf`）也一样。
- **做法**：先问 `ShareIndexer::offline`（查表，很快）或 `SearchResult::offline`，掉线共享上的条目直接跳过，不检查、不读大小；点名的文件夹在上面回 `not_found`；路径参数在网络上（或是映射到网络的盘）时先 `share::reachable`（1.5 秒，结果缓存），连不上就不去打开。测试用 `Services::offline` 冒充掉线，别真的去碰一个不存在的服务器。

### `truncated` 一到上限就是 true（2026-10-24）

- **现象**：正好 3 个文件、`limit: 3`，`search_files` 回 `truncated: true`，ws 就提示还有更多；文件夹里有被排除的子文件夹时，`list_folder` 的 `total` 比看得到的多一个，`truncated` 也是 true。
- **原因**：旧的判断是 `totalMatches > results.size() || items.size() >= limit`：到上限就算还有；引擎的总数还算上了看不到、被排除的。`list_folder` 的 `total` 数的是所有子项。
- **做法**：只有真看到了上限以外还有一条看得到的（或回答被字节预算截断，或引擎按它自己的上限截掉了同一种的）才是 true；`total` 不数排除的。`recent_files` 也回 `truncated`：每块索引留要的两倍，有一块装不下就算还有。

### 排在后面的内容搜索，`timeout_ms` 从开始搜才算（2026-10-24）

- **现象**（ws 实测）：一个内容搜索在跑，另一个 `timeout_ms: 800` 的排在后面，等前一个跑完（最长 30 秒）才回答。
- **原因**：超时的计时器在这次搜索开始时才启动，排队的时间不算。
- **做法**：从调用到达时算：排着队时到点（`QTimer::singleShot`，按 `weak_ptr` 找它）就从队列里拿掉，回 `complete: false`、没有条目和一句英文说明；开始搜时只给剩下的时间。

### 只能移动的句柄放不进 `std::function`（2026-10-24）

- **现象**：把复制的调用者令牌（`win32::UniqueHandle`）按值抓进交给工作线程的活（`std::function`），编译不过。
- **原因**：`std::function` 要可复制的可调用对象（`std::move_only_function` 是 C++23），只能移动的捕获让 lambda 不可复制。
- **做法**：令牌放进 `std::shared_ptr<void>(copy, ::CloseHandle)`：可复制，最后一个持有者关句柄，活在它的线程上做完就关了。

### 后台模式不能叠着开（2026-10-24）

- **现象**：（Windows 的文档写着，写 `QuietIo` 时照它做的）线程已经在后台模式时再 `SetThreadPriority(THREAD_MODE_BACKGROUND_BEGIN)` 会失败（`ERROR_THREAD_MODE_ALREADY_BACKGROUND`）；忘了 `END` 的线程，下一件活一开始就是后台模式，读固态硬盘也慢好几倍（见上面“内容索引”里后台模式的实测）。
- **做法**：只在要换时调用（记着自己是不是开着），活做完（`QuietIo` 析构）一定 `END`；线程池的线程上尤其要配对，下一件活不知道上一件留下了什么。测试 `quietIo` 连开两次，第二次还能进后台模式，说明第一次收干净了。

## 插件系统（阶段 0：探针）

`tools/plugin-probe.cpp`（`build\release\plugin-probe.exe --parent --out 文件`，提权和不提权各跑一次）实测的，2026-10-11，Windows 11 26200。结论：插件进程不由提权的 Win顺 直接起；由它用资源管理器的令牌起一次不提权的 runner，插件由 runner 用普通 `CreateProcessW` 起。

### `CreateProcessWithTokenW` 不接受 `EXTENDED_STARTUPINFO_PRESENT`

- **现象**：带 `STARTUPINFOEX` 调 `CreateProcessWithTokenW`，属性表里哪怕什么都没有，也返回错误 87（`ERROR_INVALID_PARAMETER`）；提权与否都一样。所以句柄列表（`PROC_THREAD_ATTRIBUTE_HANDLE_LIST`）和缓解策略（`PROC_THREAD_ATTRIBUTE_MITIGATION_POLICY`）都传不进去，起出来的进程没有扩展点禁用、镜像加载限制（探针 A-v0：ep=0 nr=0 nl=0）。
- **原因**：这个 API 把请求交给 Secondary Logon 服务（seclogon）在另一个进程里 `CreateProcessAsUser`，传过去的只有 `STARTUPINFO` 的基本字段；属性表里是调用进程的句柄和内存，传不过去，干脆拒绝。
- **做法**：用它只能 `STARTUPINFO` + `STARTF_USESTDHANDLES`。三个标准句柄由 seclogon 复制进子进程（stdin、stdout 两个方向都通），别的可继承句柄**不会**进去（探针 A-v0 leak=0；普通 `CreateProcessW` 不带句柄列表时 leak=1），所以不用句柄列表也不泄漏。`CREATE_SUSPENDED`、`CREATE_NO_WINDOW`（子进程没有控制台窗口）、`CREATE_UNICODE_ENVIRONMENT`（`CreateEnvironmentBlock` 的环境块原样到达，含中文值）都有效，挂起后 `AssignProcessToJobObject` 再 `ResumeThread` 照常。缓解策略在创建时设不了，所以插件不直接用它起：Win顺 用它起**一次** runner，runner 自己调 `SetProcessMitigationPolicy`（扩展点禁用、镜像加载两项运行后能设，实测生效；ASLR 两项只能在创建时设，runner 是自己的程序，链接时就开着），插件由 runner 用普通 `CreateProcessW` 起，句柄列表、缓解策略、作业都是普通路径，和 `DocExtractor` 一样。

### 提权进程拿资源管理器的令牌 `CreateProcessAsUserW`，错误 1314

- **现象**：同一个令牌，`CreateProcessWithTokenW` 行，`CreateProcessAsUserW` 报 `ERROR_PRIVILEGE_NOT_HELD`；不提权的进程用它 `CreateProcessAsUserW` 反而行。
- **原因**：没有 `SeAssignPrimaryTokenPrivilege`（管理员也没有，只有服务账户有）时，`CreateProcessAsUser` 只接受和自己同一登录会话的令牌（`TokenStatistics.AuthenticationId` 相同，或自己令牌的受限版本）。提权的 Win顺 和资源管理器是两个登录会话（探针里 `452da` 和 `45c4d`），不提权时是同一个。`CreateProcessWithTokenW` 只要 `SeImpersonatePrivilege`：管理员有，普通用户没有（不提权时它报 1314）。
- **做法**：提权时用 `CreateProcessWithTokenW`；不提权（开发环境）时直接 `CreateProcessW`，自己就是用户。Runner 的启动方式按这个选，之后 runner 里的代码两种情况完全一样。

### 链接令牌起不了进程

- **现象**：`TokenLinkedToken` 拿到的令牌 `DuplicateTokenEx` 成主令牌失败，错误 1346（`ERROR_BAD_IMPERSONATION_LEVEL`）。
- **原因**：没有 `SeTcbPrivilege` 时链接令牌是鉴别级别的模拟令牌，只够 `AccessCheck` 用（`Visibility` 用它正是这样），变不成主令牌，也就起不了进程。而且它在提权进程里是不提权的那半，在不提权进程里是**提权的那半**（完整性 12288、`TokenIsElevated=1`），后一种拿来起插件就是提权。
- **做法**：这条路作废。插件令牌只有两个来源：资源管理器的令牌，和自己的令牌 `CreateRestrictedToken` 降权（没有资源管理器、或资源管理器是以管理员身份运行的时候）。

### 判断令牌是否提权，别看 `TokenElevationType`

- **现象**：从提权令牌 `CreateRestrictedToken` 出来、标签设成“中”的令牌，`TokenElevationType` 仍是 `TokenElevationTypeFull`（2），`TokenElevation.TokenIsElevated` 却是 0。
- **原因**：ElevationType 说的是这个登录会话里它是拆分令牌的哪一半，`CreateRestrictedToken` 不改它。
- **做法**：硬检查只看 `TokenIsElevated == 0` 且完整性 RID 低于 12288（高）。探针用它拒绝了提权进程自己的令牌和模拟的“以管理员身份运行的资源管理器”，放行了降权的令牌；用户真把资源管理器以管理员运行时，这个检查让 Win顺 改用降权的自己令牌，设置页提示“网络位置可能看不到”。

### 作业的剪贴板限制：`OpenClipboard` 成功，`GetClipboardData` 才失败

- **现象**：`JOB_OBJECT_UILIMIT_READCLIPBOARD` 下 `OpenClipboard`、`CountClipboardFormats`、`EnumClipboardFormats` 都正常（看得到剪贴板有几种格式），`GetClipboardData` 返回空、错误 5。`ActiveProcessLimit = 1` 下子进程里 `CreateProcess` 报 1816（`ERROR_NOT_ENOUGH_QUOTA`）。
- **做法**：测试断言盯 `GetClipboardData` 的错误码和 1816，不要断言 `OpenClipboard` 失败。对脚本插件表现是“读到空/抛异常”，设置页的说明按这个写。

### 父进程在作业里时，子进程一出生就在作业里

- **现象**：探针的子进程 `AssignProcessToJobObject` 之前 `IsProcessInJob` 就已经是 TRUE（从终端里启动的父进程本身在一个作业里；经 seclogon 起的也一样，seclogon 把父进程设成调用者）。
- **原因**：子进程继承父进程的作业；Windows 8 起作业可以嵌套，再 Assign 到自己的作业照样成功，子进程里 `QueryInformationJobObject(NULL)` 看到的是最里层（自己的）作业的限制。
- **做法**：测试不能只看 `IsProcessInJob`，要读 `QueryInformationJobObject` 的 `LimitFlags`、`ActiveProcessLimit`、`UIRestrictionsClass` 确认是自己的作业和限制。Runner 的作业只能带 `KILL_ON_JOB_CLOSE`，不能带 `ActiveProcessLimit`，否则它起不了插件；每个插件在 runner 里再建带限制的嵌套作业（探针 B' 里子进程看到的就是嵌套的那个：limit=1、剪贴板受限，runner 自己 limit=0）。

### 起一个进程到它的第一行输出要 12–15 毫秒，哪条路都一样

- **现象**：无 Qt、静态 C 运行库的探针，`CreateProcess*` 调用本身 1–2 ms，从调用到读到第一行中位 12–14 ms、p95 15–17 ms；资源管理器令牌经 seclogon 只多约 1 ms；经 runner 多一跳，父进程看到的整次往返中位 15 ms。echo 到观察到进程退出 1.5–2 ms，偶尔 12–28 ms。
- **做法**：这是 Windows 进程创建（含 Defender）的底价，和令牌来源无关；解释器另算（Python 几十 ms、PowerShell 几百 ms）。L1 每键起进程的预算先扣掉这 15 ms，120 ms 的防抖是对的；常驻（L2）一次往返 0.02–0.05 ms。

## 插件系统（阶段 1：核心库）

`src/plugin/`、`WinShunPluginHost.exe`、`tests/tst_plugin.cpp`，2026-10-11。设计见架构文档“插件系统”。

### 作业的 UILIMIT_HANDLES 一开，给了剪贴板权限也读不到

- **现象**：manifest 写了 `clipboard:read`，作业的 UI 限制里也确实没有 `READCLIPBOARD` 了，插件 `OpenClipboard` 成功、`GetClipboardData` 却返回空，错误 6（`ERROR_INVALID_HANDLE`）；探针里不带 UI 限制的对照组读得到。
- **原因**：没有 `ui` 权限时作业带 `JOB_OBJECT_UILIMIT_HANDLES`（不许用作业外进程拥有的 USER 句柄）；剪贴板里的数据句柄正是别的进程的，所以被挡。
- **做法**：`sandbox::makeJob` 只在既没有 `ui` 也没有任何剪贴板权限时才加 `UILIMIT_HANDLES`；桌面、退出 Windows、系统参数、显示设置这几项照旧。`tst_plugin::jobsAndPermissions` 提权跑时断言有权限的插件 `clipRead = true`，这一条就是它抓出来的。

### moc 把原始字符串里的 `//` 当注释，整个测试类就没了

- **现象**：`tst_plugin.cpp` 链接时报 `TestPlugin::metaObject` 等三个符号找不到；autogen 目录里的 `tst_plugin.moc` 是 0 字节，手工跑 moc 说 “No relevant classes found”。
- **原因**：文件里的 JSON 原始字符串 `R"(…"https://github.com/…"…)"` 含 `//`，moc 的预处理器在原始字符串里也照样当注释处理，后面的 `)"` 和整个类声明都被吃掉。
- **做法**：带 `Q_OBJECT` 的源文件里，原始字符串不要含 `//`（也别含 `/*`）；网址写成 `x.com/…`（`normalizeWebUrl` 会补 https://）或 `mailto:`。要写完整网址的 JSON 就用普通字符串拼（`"{\"url\": \"https://…\"}"`）。2026-10-11 在 `tst_core.cpp`（网页搜索导入的测试）又踩了一次，症状一样：链接报 `CoreTest::metaObject` 找不到。

### 作业只许一个进程时，`py.exe` 起不了 Python

- **现象 / 原因**：Python Launcher（`py.exe -3`）自己是一个进程，再起 `python.exe` 当子进程；作业 `ActiveProcessLimit = 1` 下第二个进程报 1816。Store 版 Python 在 `WindowsApps` 下的 `python.exe` 是执行别名，同样是转一手。
- **做法**：`programs::find(Python)` 先读注册表 `HKCU/HKLM\Software\Python\PythonCore\<版本>\InstallPath`（`ExecutablePath`，没有就目录里的 `python.exe`），再查 PATH 但跳过 `WindowsApps`；起的是 `python.exe -u -X utf8 <脚本>`（`-u`：管道默认块缓冲，常驻插件 `print` 的行不然会压在缓冲区里）。

### Windows PowerShell 5.1 读没有 BOM 的 .ps1 按 ANSI 解释，脚本里的中文全成乱码

- **现象**：`tests/plugins/bench.ps/main.ps1` 里的 `"回声："` 输出成 `å›žå£°ï¼š`，而从 stdin 收到的中文查询原样回来了。
- **原因**：5.1 把没有 BOM 的脚本按系统 ANSI 代码页读（这台机器是 1252），UTF-8 的字节被逐个当成 Latin-1。输出编码已经在脚本开头设成 UTF-8，所以“错一次”而不是“错两次”。
- **做法**：PowerShell 模板和示例一律存成带 BOM 的 UTF-8（pwsh 7 两种都认）；阶段 4 的“新建插件”生成模板时要写 BOM。另外这台机器的 OEM 代码页是 437（语言列表英文在前），`encoding: oem` 只能救出 Latin 字符，中文在 5.1 默认输出里就是 `?`——所以模板第一行固定 `[Console]::OutputEncoding = UTF8`，`oem` 只是给不改模板的人兜底。

### 信号和方法同名，`connect(&Runner::ready, …)` 就选不出来

- **现象**：`Runner` 里既有信号 `void ready()` 又有 `bool ready() const`，`connect(m_runner, &Runner::ready, …)` 和 `m_runner->ready()` 都报二义性。
- **做法**：查询状态的方法叫 `isReady()`；本仓库的 QObject 里信号名独占。

## 插件系统（阶段 2：接进启动器）

`src/plugin/Merge.{h,cpp}`、`Launcher`、`wsbench --launcher-sim`，2026-10-11。

### 不带上下文对象 connect 引擎的信号，槽在引擎的工作线程上跑

- **现象**：`wsbench --launcher-sim` 里 `QObject::connect(&engine, &SearchEngine::resultsReady, [&] {...})` 的 lambda 改了主线程的列表和计时变量，输出里“最后一键到插件行”的时间忽有忽无，调试打印的先后顺序和代码顺序对不上。
- **原因**：`SearchEngine` 在自己的线程上 emit；只给 lambda、不给上下文对象的 connect 是直接连接，槽就在那个线程上执行，和主线程的 `processEvents` 循环同时改同一批变量。Launcher 里一直是 `connect(m_engine, …, this, …)`，有 `this` 做上下文，所以是排队的。
- **做法**：给跨线程的信号 connect 时总带一个住在接收线程的 QObject 做上下文（这里用 `&host`），Qt 就按线程自动排队。

### `using namespace ws` 和 `ws::plugin` 同时打开时，`Entry` 是两个

- **现象**：`tst_plugin.cpp` 里写 `const Entry* e = catalog.find(...)` 报 C2872 不明确。
- **原因**：`ws::Entry` 是索引的条目（`FileIndex.h`），`ws::plugin::Entry` 是插件目录的条目。
- **做法**：测试里写 `plugin::Entry`。

## 插件系统（阶段 3：设置页）

`src/app/PluginSettings.{h,cpp}`、`Plugins*.qml`、`src/app/Preview.cpp`，2026-10-11。

### 离屏渲染的窗口里 `parent.width` 全是负数

- **现象**：`--preview-plugins` 把设置页画成 PNG，所有靠右对齐的控件（开关、“…”、输入框）都不见，长文字从右边截掉；`--tree` 一打印，`QQuickRootItem` 是 0×0，页面 Column 的宽是 -48。
- **原因**：窗口放在屏幕外（20000, 20000）并且从不激活，Windows 不给它 resize / expose，`QQuickWindow` 只在这两个事件里把 `contentItem` 的尺寸设成窗口大小；软件渲染器照样能 `grabWindow()`，画出来的就是尺寸没定的那一层。
- **做法**：`Preview.cpp` 自己 `contentItem()->setSize(窗口大小)`，而且要两遍：高度跟着宽度变（文字换行），布局（polish）又只在出一帧时做，所以每遍先 `grabWindow()` 一次扔掉，直到 `contentItem` 的高等于窗口的高。正常的界面不受影响，窗口在屏幕上就有事件。

### 没出帧，动画就停在半路

- **现象**：带对话框的那张预览里，对话框半透明、底下的字透出来；另一张（深色 150%）又是实的。
- **原因**：`Behavior on opacity` 靠渲染循环推进，离屏渲染只在 grab 时出一帧，抓到哪一帧看运气。
- **做法**：`PluginDialog` 加 `animated` 属性，预览里设 false；真实窗口里照常有 150 ms 的淡入。

### 提权运行 build 目录里的 WinShun.exe 会卡住，日志一行都没有

- **现象**：`Start-Process build\release\WinShun.exe -Verb RunAs` 跑 `--preview-plugins`，进程一直不退出，连 `prepare()` 的第一行日志都没写。
- **原因**：build 目录里没有 Qt 的 DLL（UI 测试的安全规则就是不放），加载器弹的“找不到 Qt6Core.dll”消息框在提权进程里，桌面上看不到，进程就等在那里；ctest 靠 `ENVIRONMENT_MODIFICATION` 把 Qt 的 bin 加进 PATH，`-Verb RunAs` 起的进程拿的是干净的环境，加不进去。
- **做法**：先用 build.ps1 里那套 `windeployqt` 参数把 WinShun.exe 和 DLL 部署到一个临时文件夹（不是 dist，不动用户在跑的那份），从那里提权运行；脚本给进程设上限，到点 `taskkill /PID`（只杀自己起的那个 pid，用户的 WinShun 是前一天起的）。

### qmltypes 里指针属性的类型名要带 `ws::`

- **现象**：`Q_PROPERTY(PluginSettings* plugins …)` 编译、运行都好，qmllint 却说 `Type "PluginSettings" of property "plugins" not found`。
- **原因**：qmltyperegistrar 把属性类型原样写进 qmltypes（`type: "PluginSettings"`），而类型本身登记的名字是 `ws::PluginSettings`，对不上。
- **做法**：和 `Clipboard.h` 的 `ws::ClipModel*` 一样，`Q_PROPERTY` 里把命名空间写全：`ws::PluginSettings*`、`ws::PluginLogModel*`；只在 C++ 里用的 model 类加 `QML_ANONYMOUS`，qmllint 才认得它们的方法。

### `required property bool enabled` 遮住了 Item 自己的

- **现象**：qmllint：`Member "enabled" shadows member "enabled" from base type "QQuickItem"`；委托里 `enabled` 既是 model 的角色又是 Item 的开关，整张卡片会跟着插件的开关变灰。
- **做法**：角色名叫 `pluginEnabled`（`roleNames()` 里改），委托 `required property bool pluginEnabled`。角色名永远别和 `QQuickItem` 的属性同名。

### 关键字判定要收输入框原文，trim 了就没有“kill ”这个状态

- **现象**：改成“关键字 + 空格”才进模式后，打 `kill ` 还是搜名字。
- **原因**：`Launcher::search` 以前把 `m_query.trimmed()` 交给插件，尾部空格先被抹掉。
- **做法**：`startPluginQuery(m_query)` 原文传；`Catalog::match` 自己去掉前导空格、留着尾部；正则 / 推送触发器用 trim 过的词。`wsbench --launcher-sim` 同样改成传原文。

### Credential Manager 的 generic 凭据：删除和读回

- **做法**：目标名 `WinShun/plugin/<id>/<key>`，`CRED_TYPE_GENERIC`，值按 UTF-16 存 blob；`CredDeleteW` 返回 `ERROR_NOT_FOUND` 当成功；按前缀列出用 `CredEnumerateW("WinShun/plugin/<id>/*")`。测试里用 RAII 清掉，断言失败也不留；提权和不提权跑的是同一个用户的凭据库。

## 插件系统（阶段 4：随包插件、SDK、MCP 桥）

`plugins/`、`src/plugins-sdk/`、`src/tools-plugin/`、`src/plugin/AiBridge.cpp`，2026-10-11。

### `Get-Process` 后对每个进程读 `MainWindowTitle`，一次查询 2.5 秒

- **现象**：`winshun.kill` 的 `kill 记事本` 稳态 2.5 秒；测试用的 `tests/plugins/kill.ps`（同样 `Get-Process | Where ProcessName -like`）只要十几毫秒。
- **原因**：筛选条件里多了 `$_.MainWindowTitle -like "*$text*"`：`MainWindowTitle` 是惰性属性，每读一次枚举一遍那个进程的窗口，三百个进程就是三百遍。
- **做法**：只按进程名（或 PID）筛；窗口标题和 `Path` 只对要显示的那几十行读。之后 14–20 ms。

### 给提权批处理写的 .ps1 没带 BOM，中文参数全成了 `æµ‹è¯•`

- **现象**：`Start-Process powershell.exe -Verb RunAs -File 脚本.ps1` 里起 `测试进程.exe`、传 `kill 测试进程`，脚本里的中文到了子进程全是 Latin-1 乱码，`Start-Process` 还悄悄失败（路径不存在）。
- **原因**：和“Windows PowerShell 5.1 读没有 BOM 的 .ps1 按 ANSI 解释”是同一条，这次中招的是测试脚本自己，不是插件。pwsh 7 两种都认，`powershell.exe` 5.1 不认。
- **做法**：Write 工具写出来的文件没有 BOM；凡是要给 `powershell.exe` 跑的 .ps1 / .psm1，写完用 `[IO.File]::WriteAllBytes` 补上 `EF BB BF`（`Format-Hex -Count 3` 核对），路径用绝对路径——`[IO.File]` 的相对路径按进程的当前目录解析，不是 PowerShell 的 `Set-Location`。

### `tst_mcp::relayOnline` 提权跑会失败：中继继承了提权令牌

- **现象**：提权的 ctest 里 `tst_mcp` 的 `relayOnline` 报“Compared lists have different sizes”：只给管理员的 `secret` 文件夹也在结果里。`tst_plugin` 同一批里 53 passed, 0 skipped。
- **原因**：测试用普通 `CreateProcess` 起 `WinShunMcp.exe`，它继承测试进程的提权令牌，管道那头 `ImpersonateNamedPipeClient` 拿到的就是管理员，`AccessCheck` 当然看得到。真实的中继是 AI 客户端不提权起的。
- **做法**（2026-10-11 改）：受限目录那条断言单独成 `relayRestricted()`，提权时 `QSKIP`（原因写在测试里：中继继承了管理员令牌，这条检查在提权下没有意义；真实的中继由 AI 客户端以用户身份启动，永远不提权）；`relayOnline()` 只测中继本身（连接、断开后自答、重连）。这样“提权跑 `ctest --preset release` 全绿”就是干净的验收标准，不提权跑时受限目录照样检查。要让它提权也过，得让测试用资源管理器的令牌起中继——没有必要。

## 插件系统（真机实测后的修正）

提权的 SendInput 实测六个随包插件，2026-10-11。

### TextInput 的 textChanged 早于排版：此时问 positionToRectangle 得到 0

- **现象**：关键字后面的占位文字（`SearchBar.keywordHint`，“算式，或 12 inch to cm”）在打出 `= ` 进关键字模式的那一刻画在输入框最左边，盖住了 `=`；Esc 清到 `= ` 时却位置正确。
- **原因**：`QQuickTextInputPrivate::finishChange` 先 `emit textChanged()`，再 `updateDisplayText()` → `updateLayout()`。在 `onTextChanged` 里调 `positionToRectangle(length)`，查的是**上一段文字**的布局：文字变长时旧布局里没有这个位置，`lineForTextPosition` 无效，返回 `QRectF()`，x = 0；文字变短时旧布局里有这个位置、前缀又一样，所以碰巧正确。用 `qml.exe` 跑一个 10 行的探针（设 text 后分别在两个信号里打印 x）可复现：textChanged 0 / displayTextChanged 20、40、140。
- **做法**：要按文字位置摆东西，监听 `displayTextChanged`（`updateLayout()` 之后才发）而不是 `textChanged`；`fontChanged` 在 `updateLayout()` 之后发，可以照旧。

### 关键字模式进出不是“刷新”：trim 后一样的文字可能是两张列表

- **现象**：打 `=` 再打空格进关键字模式，光标停在第 2 行（第 1 行是算式提示行）；Esc 清到 `= ` 后又对了。
- **原因**：`Launcher::search` 用 `m_query.trimmed()` 和 `m_shownText` 比较来判定“同一个查询的刷新”（刷新不发 `resultsReplaced`，光标留在原处）。`=` 和 `= ` trim 后相同，所以从“名字结果（提示行在第 0 行、光标在第 1 行）”换成“计算器的回答”被当成刷新，`ResultModel::assign` 原地改行，ListView 的 currentIndex 还是 1。反过来 Backspace 退出模式也一样，而且刷新路径会把插件行原样塞回名字结果里。
- **做法**：`showRows` 记下这张列表是谁的（`m_shownKeyword` = `插件/命令` 或空），`search` 在算 `m_refresh` 之前先用无副作用的 `matchKeyword()` 看这次文字开的是哪个命令，不一样就是新列表。关键字匹配要在 `startPluginQuery` **之前**算：零代码命令在 `Host::query` 里同步回答，`showRows` 那时已经要用 `m_replacePending`。

### 在第 0 行插入提示行，ListView 的光标跟着原来的行下移

- **现象**：单独打 `=`，提示行 `= — 计算`（“按空格进入；Enter 或 Tab 也行”）在第一行，但光标在第二行（一个叫 `=` 的文件夹），Enter 开的是那个文件夹，和提示文字矛盾。
- **原因**：`showRows` → `resultsReplaced` → `selectFirst()` 之后才 `Merge::namesShown()` → `placeHint()` → `insert(0, …)`；ListView 在 currentIndex 之上插行时让 currentIndex 跟着原来的项走（0 → 1）。`placeHint` 以前又没有设 `m_betterFirstRow`，`Main.qml` 的 `onBetterFirstRow` 没机会把光标拉回来。
- **做法**：`placeHint` 插在第 0 行时，`m_sink.currentRow() == 0` 就置 `m_betterFirstRow`（和 `apply()` 一样），`showRows` 随后 `emit betterFirstRow()`，`Main.qml` 把 currentIndex 设回 0。顺手把提示行从“最近打开之后”改到第一行：提示文字承诺 Enter 进入，它就得是当前行；名字结果一个 ↓ 就到。`tst_plugin::keywordNeedsSpace` 断言新顺序和 `takeBetterFirstRow()`。

### 零代码插件没有自己的进程，权限对话框别说“内存上限 0 MB”

- **现象**：打开 `winshun.go` / `winshun.web` 的开关，对话框第一行“以普通用户权限运行，不是管理员；内存上限 0 MB”。
- **原因**：`PluginSettings::permissionRows` 一句话写死，`m.runtime` 为空时 memoryMB 取 0。
- **做法**：按实现分三句：没有 `runtime` → “不运行自己的程序：Win顺 按它的清单打开网址、程序或文件夹，用你的普通权限”；`sandbox: strict` → “在严格沙箱里运行：低完整性……写不了你的文件（只能写它自己的数据文件夹）”（runner 用 `restrictedToken(SECURITY_MANDATORY_LOW_RID)`，子进程、剪贴板的限制来自权限表，不属于 strict）；其余照旧。`loadPreview()` 的假插件各用一种，`--preview-settings` 能看到三种。

### 插件行的 Enter 不一定是“打开”，底栏提示要跟着当前行

- **现象**：关键字模式里底栏仍是“Enter 打开 · Ctrl+Enter 打开位置 · Tab 切换范围”，而 Enter 实际是复制 / 结束进程 / 打开网址，Ctrl+Enter 什么也不做，Tab 在关键字模式里不切范围。
- **做法**：`Launcher::keyHints`（`refreshKeyHints`，在 `refreshStatus` 和 `setCurrentRow` 里算，变了才发信号）：按 `menuItems()` 同样的分支给出 Enter 的文字（插件行是 `actions[0].title`，没有动作就按 open / search / setQuery / 复制）、第二个键（`actions[1]` 或有位置的行的“打开位置”）、Tab（`completionRow` 找得到可补全的行就“补全”——当前行的，或列表里关键字前缀的提示行；关键字模式里否则不显示）。`Footer.qml` 三个 `KeyHint` 读它，空的隐藏；“有排序开关时不显示 Enter”的规则保留。`currentRow` 从 `MEMBER` 改成有 setter 的属性，才知道光标换了行。

### 插件的 `open.url` 认 `winshun:settings`：让“还没设置”的行能一键打开设置

- **做法**：`Launcher::performPlugin` / `applyEffects` 看到 `winshun:settings` 不交给 `shell::openUrl`，而是 `emit pluginSettingsRequested(行的 pluginId)`（只能开自己的卡片）；`SettingsWindow.showPluginPage(id)` 切到插件页、`revealRow` 滚到卡片、卡片标记 3 秒（`PluginsSection.revealed`）。`App` 里把信号接到 `showSettings()` + `showPluginPage` 这一步归 claude-02 的 App.cpp，见那次的报告。`winshun.translate` 没配密钥的行用它。App.cpp 的接线已补上（`App::showPluginSettings`）。
- **现象**（真机）：设置窗口已经开着时，Enter 滚到卡片并标记；设置窗口是这次才新建的，就停在插件页顶上。
- **原因**：`layoutNow()` 只排各节（`page.children`），节里装卡片的 `Column` 要到下一帧 polish 才排；刚建好的窗口里每张卡片的 `y` 都还是 0，`revealRow` 算出来就是顶上。
- **做法**：当场照样 reveal，另记下 `revealAfterFrame`，窗口下一次 `frameSwapped` 时再 `revealRow` 一次（`Connections` 只在有待办时启用）。

### 结束不了提权的进程：插件以用户身份跑，别把英文异常原样丢到底栏

- **现象**：`kill` 一个以管理员运行的记事本，底栏“没有结束 Notepad：Cannot stop process …”被截断。
- **原因**：插件进程是中完整性，对高完整性进程拿不到 `PROCESS_TERMINATE`（`Stop-Process` 的内层 `Win32Exception` NativeErrorCode 5）；读它的路径（`Process.Path` 要 `PROCESS_VM_READ`）也拿不到，所以这种行本来就没有路径。
- **做法**：`main.ps1` 的 `-OnAction` 按错误码 5 / `PermissionDenied` 类别（`taskkill` 看输出里的 denied / 拒绝）换成“X 以管理员身份运行，插件结束不了”（按 locale 中英）；查询时 `$p.Path` 读不到的行副标题加“无权结束：管理员或系统进程”，用户按之前就知道。改 `.ps1` 后核对 BOM 还在（`Format-Hex -Count 3`）并用 `Parser::ParseFile` 过一遍语法。

## 合并后的收尾（设置窗口整窗渲染）

`src/app/Preview.cpp` 的 `--preview-settings`，2026-10-11。五个功能合进主目录后，整个 `SettingsWindow.qml` 从来没有真正渲染过：qmllint 抓不到跨组件的运行时绑定错误，一旦出错整个设置窗口打不开。

### 真实的 SettingsWindow.qml 可以在 C++ 里离屏驱动，不用给 QML 加测试钩子

- **做法**：`--preview-settings <文件夹>` 用真实的 `SettingsEditor`、`WindowFrame(true)`（不 `setWindow`：窗口过程子类在屏幕外没用）和 `Updater`（不 `setAutomatic` / `check`：不联网）造出真实的窗口，`QMetaObject::invokeMethod(window, "showPage", …)` 逐页逐标签切换，`qmlContext(window)->objectForName("flick")` 取到 QML 里的 id（`flick`、`searchBox`、`settingsSearch`，Qt 6.2 起有），搜索框的 `TextInput` 用 `findChildren` 按类名找；设 `input.text` 就是用户打字，`goToCurrent()` 就是回车。整页截图：`setMaximumHeight(32000)` 后把窗口拉到 `flick.contentHeight + 标题栏` 那么高（屏幕外的窗口 Windows 不限高），每页一张完整的图，不用滚动拼接。拾穗计划页切到后先等 3.6 秒，让 `AuthorClock` 的问候动画播完再抓。
- **不能碰正在运行的 Win顺**：不调 `refreshNumberKeys()`（它用 `RegisterHotKey` 逐个试 Ctrl+1…9）；它只挂在 `onActiveChanged` 上，屏幕外从不激活的窗口不会触发。`refreshFileManagers` / `refreshTaskbarState` / `refreshMcpClients` 只读，照常调。插件页仍用 `loadPreview()` 的假插件：真 `plugin::Host` 会起 `WinShunPluginHost.exe`。
- **“运行中的渲染方式”要和设置里的一致**：第一版传 `"software"`，设置默认 `d3d11`，`restartRequired` 为真，高级页多出一个“立即重启”——是预览的毛病，不是界面的。传 `Settings::resolveRenderer(settings.renderer)`。

### 不提权也能从 build 目录跑预览：`__COMPAT_LAYER=RunAsInvoker`

- **现象**：上一条（“提权运行 build 目录里的 WinShun.exe 会卡住”）说要先部署再提权运行；每改一行 QML 都这样太慢。
- **做法**：预览本身不需要管理员。环境变量 `__COMPAT_LAYER=RunAsInvoker` 让 `requireAdministrator` 的清单失效，进程以启动它的身份跑，不弹 UAC；`PATH` 前面加 `C:\Qt\6.12.0\msvc2022_64\bin`，QML 模块和平台插件从 Qt 自己的目录加载。四种配置（浅/深 × 100%/150%，共 24 张图）每种约 35 秒。最终验收仍从带 Qt DLL 的部署目录提权跑一次，和用户装好的情形一样。
- **数的时候别只看图**：1 逻辑像素的中性边框用脚本采样最外两圈像素核对（`#B4B4B4` / `#6A6A6A`，100% 时 1 个设备像素、150% 时 2 个），眼睛看不出 1 像素的差别。

### exe 在带中文的路径下时界面字体悄悄变成了微软雅黑

- **现象**：同一个 `WinShun.exe`，从 `F:\电脑便捷工具\build\release` 跑出的预览每行比从 `F:\wsstage\WinShun` 跑出的矮几个像素，“”引号是半角的；预览日志里加了 `QFontInfo` 一行才看清：前者 `Microsoft YaHei UI`，后者 `Alibaba PuHuiTi 3.0`。用户自己在跑的 `dist\WinShun` 也在中文路径下，README 截图（`tools/screenshots/work/shots/L-settings.png`，0.5.0）里的行高和前者一样——推断它一直在用雅黑（没有读过那个进程的字体，是按行高推的）。
- **原因**：`QFontDatabase::addApplicationFont(路径)` 在 FreeType 字体引擎（`fontengine=freetype`，显卡加速渲染时用）下把路径交给 FreeType 用 C 运行库打开，ANSI 代码页（本机 1252）放不下的字符就打不开，静静地返回 -1，`applyUiFont()` 什么也没设，Qt 退回系统字体。GDI 引擎（省内存渲染）按文件名走的是 Windows 的宽字符 API，应该不受影响（未验证：预览固定用 freetype）。装到 `D:\软件\WinShun` 这样路径的用户都会中招；修好后在这种路径下界面字体会从雅黑变成普惠体，每行高几个像素。
- **做法**：`main.cpp` 的 `applyUiFont()`：路径不全是 ASCII 时读出文件用 `addApplicationFontFromData` 装（两个字体文件约 17 MB 留在内存里；ASCII 路径照旧按文件名装，字体留在磁盘上按需读）；一个都没装上时 `qWarning` 一句（进 WinShun.log，预览里算一条警告）。预览日志固定打印平台插件路径和实际用的字体，两处跑出的行高不一样时先看这一行。

### 五个模板标签挤不进“新建插件…”那一行

- **现象**：模板从三种加到五种，`ScopeTabs` 和名字输入框、“创建”按钮同在一个 `Row` 里，离屏预览里“创建”被挤出右边界只剩半个字。
- **做法**：`ScopeTabs` 单独占一行放在 `body` 的第一项，名字和按钮在第二行。预览图（`--preview-plugins`）是发现这类溢出最快的办法，改完再出一张。

### AI 的 `_run` 工具靠行的 id 回找：假插件的 id 不能带 queryId

- **现象**：`plugin_fake --serve` 的行 id 是 `q<queryId>`，AI 先列再按 id 执行动作时，第二次查询的 queryId 不同，找不到那行。
- **原因**：`AiBridge` 的动作工具不把 item（含 `data`）交给模型，而是用同样的 `query` 重新查一次、按 `id` 找回那行——这要求 id 对同一文字稳定，真实插件（`pid-1234`、书签的网址）本来就是。
- **做法**：假插件改成 `q-<text>`；文档里把“稳定 id”写成 item 字段的第一条。

### 外部工具替换时在途调用的 `Tool*` 悬空

- **现象 / 原因**：`ToolHost::m_tools` 是 `vector<Tool>`，`Call::tool` 存裸指针；插件的工具表每次目录或设置变了整表重建，正在等插件回答的调用就指向释放的内存。
- **做法**：外部工具单独放 `vector<shared_ptr<Tool>>`，`Call` 多存一个 `shared_ptr<Tool> keep`，替换只影响之后的调用；`toolsChanged` 按名单差异发，不是每次 `setExternalTools` 都发。

### `Json::Object {{"id", id}, …}` 里 `const char*` 到 `winshun::Json` 的隐式转换要自己写构造函数

- **做法**：单头文件 SDK 的 `Json` 给 `const char*`、`std::string`、`int`、`long long`、`unsigned long`、`unsigned long long`、`double`、`bool` 各一个非 explicit 构造函数，`Object` 是 `std::map<std::string, Json>`，这样 `Json::Object {{"protocolVersion", "1"}, {"name", name()}}` 一行能写完；`long` 没给，`DWORD` 是 `unsigned long` 不受影响。

## 网站图标（网页搜索）

### 测试文件里一个原始字符串字面量，链接时报 `metaObject` 找不到

- **现象**：`tst_core.cpp` 加了一段 `R"(<link rel="icon" …>)"` 的 HTML，编译过了，链接报 LNK2001：`CoreTest::metaObject`、`qt_metacast`、`qt_metacall` 无法解析。
- **原因**：moc 也读这个文件（末尾 `#include "tst_core.moc"`），它不认原始字符串，里面的引号和尖括号让它读乱，`Q_OBJECT` 类没生成代码。
- **做法**：moc 要读的文件里不用原始字符串字面量，写成几段普通字符串相连（`"<link rel=\"icon\" …>\n"`）。

### 苹果触摸图标更清楚，却不是浏览器里那张

- **现象**：先按“越大越好”取了 apple-touch-icon（120–512 px），B 站的图上带着“WEB”字样的条幅，和浏览器标签页上的小电视不一样；知乎、小红书、淘宝是同一张图铺满成方块。
- **做法**：用户要的是地址栏左边那个，先取 `rel="icon"` 和 `/favicon.ico`（ICO 里取最大的一帧：百度 64、淘宝 64、京东 96，其余 32），只有都只到 16 px 或没有时才用触摸图标。32 px 在 150% 下放大到 48 有点软，但认得出是哪个网站更要紧。

### 黑色的网站图标在深色主题下看不见

- **现象**：GitHub 的 favicon 是透明底上的黑猫（猫身是挖空的），深色主题的行（#202020）上几乎看不见。
- **做法**：画的时候量一下：不透明像素里 90% 以上和行的底色 RGB 距离 < 64，就垫一块另一主题颜色的圆角底板。阈值别低：抖音的黑方块 68%、百度的白方块 63% 和底色贴近，但上面的图案看得清，垫了反而像套了个框（先用 60% 时就是这样）。用像素算距离，不用亮度：百度的蓝色亮度和深灰差不多，颜色却分得很开。

### cn.bing.com 在这台电脑上 TLS 握手失败

- **现象**：访问 `https://cn.bing.com/` 在 TLS 握手时就断开，`www.bing.com` 正常（国内访问会被它自己转到中文版）。
- **做法**：自带的必应用 `https://www.bing.com/search?q=%s`。

## 把结果拖出去

### `SHCreateDataObject` 的父级传 `nullptr`，条目整个没了

- **现象**：拖出去的数据对象里只有自己 `SetData` 进去的两种格式（`Preferred DropEffect` 和一个数字），没有 `Shell IDList Array`，资源管理器不认。
- **原因**：父级为空时外壳不收 `apidl` 里的条目，也不报错。
- **做法**：父级传桌面的 ID 列表（`SHGetKnownFolderIDList(FOLDERID_Desktop)`），每个条目是自己的完整 ID 列表，不同文件夹的结果放在一起也行。这样的对象外壳不生成 `CF_HDROP`，要自己写一份（`DROPFILES` + 宽字符路径，两个 `\0` 结尾）；`tst_dragout` 把对象里的格式列出来检查。

### 管理员进程往普通程序拖文件，UIPI 不挡

- **现象 / 担心**：Win顺 以管理员运行，普通程序往它拖东西会被 UIPI 挡（低往高），反过来会不会也不行？
- **实测**（2026-10-11）：不挡。普通权限的进程解封管理员进程的 `IDataObject` 并 `GetData(CF_HDROP)` 成功，不需要 `CoInitializeSecurity`；真拖到 `RegisterDragDrop` 的窗口、`DragAcceptFiles` 的窗口和资源管理器都收到了。测试程序在 scratchpad：管理员的源窗口自己按下鼠标、`DoDragDrop`，另一条线程用 `SendInput` 挪到目标窗口再松开；按下和松开前都用 `WindowFromPoint` 确认点在自己的窗口和目标上，松开放在 `__finally` 里，事后查 `GetAsyncKeyState(VK_LBUTTON)`。

### 拖完以后 Qt 还当鼠标按着

- **现象 / 原因**：`DoDragDrop` 的模态循环自己处理松开按钮的消息，Qt 收不到 `WM_LBUTTONUP`，`MouseArea` 一直是 pressed；等它以后补上这次松开，落在行上就是一次“单击”，会打开文件。
- **做法**：拖完给窗口补发一个 `WM_LBUTTONUP`（坐标是指针此刻的位置，一般在别的程序上，不算单击），`ResultRow` 还记着这次按下已经拖出去了，`onClicked` 直接返回。

## 收藏夹（真机实测后的修正）

### 从资源管理器拖进设置窗口，什么都没发生

- **现象**（2026-10-11 实测）：把文件从资源管理器拖到 设置 → 收藏夹 的列表上，列表没有“松开就加入收藏”的反应，松开后也没加进去；插件页的“拖到这里也能安装”同理。
- **原因**：Win顺 始终提权，资源管理器不提权。UIPI 不让低完整性进程的 OLE 拖放到达高完整性窗口，Qt 注册的 OLE 拖放目标（QML 的 `DropArea` 就靠它）根本收不到。反方向（从 Win顺 拖出去）不受影响。
- **做法**：`FileDropTarget`（`SettingsWindow.qml` 里一个）。窗口一显示就 `RevokeDragDrop` 撤掉 Qt 的 OLE 目标（只要还注册着，资源管理器就先问它，不会退回老办法），再 `DragAcceptFiles`，用 `ChangeWindowMessageFilterEx` 放行 `WM_DROPFILES`、`WM_COPYDATA` 和 `0x0049`（`WM_COPYGLOBALDATA`，它带着拖的数据）。原生事件过滤器收 `WM_DROPFILES`，按当前页交给收藏夹或插件。代价：拖动途中没有反馈，只知道松开了。以后任何要接资源管理器拖进来的窗口都要这样做，`DropArea` 只对同样提权的来源有用。

### 服务器不通的收藏，查过一次就成了“已不存在”

- **现象**：收藏 `\\不在的主机\x`。第一次打开启动器时照常显示，再搜一次就变暗、标“已不存在”。设置页却照常显示。
- **原因**：`RecentOnDisk` 第一次等了 100 毫秒没等到回答，记为“不知道”。过一阵后台线程等到了 `QFileInfo::exists() == false`（其实是 `ERROR_BAD_NETPATH`），记成了“不在”。
- **做法**：用 `GetFileAttributesExW` 判断，只有 `ERROR_FILE_NOT_FOUND`、`ERROR_PATH_NOT_FOUND` 算“不在”。服务器不通、共享名不对、没有权限、驱动器里没盘，都算“不知道”，照常显示。设置页用同一条规则：网络上的先 `share::reachable` 探一下再问，本机盘的结果先显示。

## 录快捷键（GitHub #4）

### 进程登记了键盘 Raw Input，自己的低级键盘钩子排第一个时不会被调用

- **现象**：设置里点快捷键框，按 Win 只显示“ + …”，Alt+Space 弹出系统菜单：`ShortcutCapture` 的 `WH_KEYBOARD_LL` 装上了（返回成功、线程在 `GetMessage` 里等），钩子过程却一次都没被调用。另开一个提权进程装个只放行的钩子，它排到前面以后反而能收到。同样的代码放进独立程序就正常。
- **原因**：`KeyListener` 为双击 Ctrl 给整个进程登记了键盘 Raw Input（`RIDEV_INPUTSINK`）。同一进程里有键盘 Raw Input 时，Windows 不调用这个进程排在钩子链第一个的低级键盘钩子（独立复现：只多一句 `RegisterRawInputDevices`，钩子从 6 次变 0 次）。
- **做法**：录制期间 `ShortcutCapture` 先用 `GetRegisteredRawInputDevices` 记下键盘那一条，`RIDEV_REMOVE` 撤掉，结束后原样登记回去（双击 Ctrl 录制时本来就不该响）。以后进程里再加低级键盘钩子，也要想到这一条。

### 同一情况下，排在后面的别人的钩子也一个键都收不到（GitHub #10、#9）

- **现象**：启动器在前面时，以管理员或 UI Access 运行的 AutoHotkey 改键（CapsLock+J 当 ↓ 之类）全部失效，按键原样进了搜索框；剪贴板开在别的程序上时正常。把“用数字键直接打开”关掉就好了，Ctrl、Alt 两种都坏。
- **原因**：数字键的低级键盘钩子（`NumberKeys`，窗口激活时装）和上一条是同一个局面：进程有键盘 Raw Input，自己的窗口在前台，钩子排在链头。这时不只是自己的钩子不被调用，**整条链后面的钩子都不被调用**。独立复现（提权的“AutoHotkey”钩子先装，再起一个进程把窗口放到前台、发 3 次 F23）：只有 Raw Input 或只有钩子，前面那个钩子 3/3；Raw Input 加钩子，0/3，自己的钩子也是 0 次；钩子装着时撤掉 Raw Input，又 3/3；同一个进程（Raw Input 加钩子）窗口不在前台、别的程序在前台时，两边都 3/3。所以剪贴板的 `KeyRouter`、开始菜单打字的钩子（前台都是别的程序）不受影响。
- **做法**：数字键不用钩子了，就是窗口自己收的按键（双击 Ctrl 开着时它本来就是这样工作的：钩子从没被调用）；别的程序的全局快捷键照样先给它们（`numberkeys::heldMask` 只用来把那几行划掉）。以后要在 Win顺 自己的窗口在前台时装低级键盘钩子，要么先撤掉键盘 Raw Input（像 `ShortcutCapture`），要么换别的办法。

### 录制框藏起来或窗口离开以后，焦点还留着，回来又接着吞 Win

- **现象**：在快捷键框里点一下再切到别的页面，或者切到别的程序再回来，按 Win 开始菜单不出来；把设置窗口最小化才好。
- **原因**：`recording` 绑的是 `activeFocus`，页面藏起来、窗口失去焦点都不会把 `focus` 拿掉；窗口一回到前面，录制又开始，Win 被钩子吞掉。点空白处的 `clearFocus()` 也没用：把焦点 `forceActiveFocus` 给 `contentItem`，它又转回里面原来有焦点的那一项。
- **做法**：`HotkeyRecorder` 在自己不可见、或所在窗口不再 active 时 `stop()`，判断用 `focus` 而不是 `recording`（窗口离开时已经没有项有 active focus）。`clearFocus()` 改成把当前 `activeFocusItem` 的 `focus` 设为 false。

### 原生事件过滤器里写 `*result`，拖文件进设置窗口就崩

- **现象**：从资源管理器往设置窗口的收藏夹拖文件，收藏是加上了，Win顺 随即退出（事件查看器：WinShun.exe 0xc0000005）。看上去像是“列表没刷新”，其实是进程没了，再打开设置时它被重新启动。
- **原因**：`WM_DROPFILES` 是投递来的消息，Qt 的事件分派器把它交给 `QAbstractNativeEventFilter::nativeEventFilter` 时 `result` 是空指针（只有窗口过程那一路才给结果指针）。`FileDropTarget` 无条件 `*result = 0`。
- **做法**：只在 `result` 不为空时写。凡是处理投递消息（`WM_DROPFILES`、`WM_HOTKEY`、线程消息）的原生过滤器都一样。

## 剪贴板面板

### 叠在填充图标上的轮廓才有针：PinnedFill 单用只剩针头

- **现象**：剪贴板面板钉住后，右上角的蓝色图钉只有针头，针不见了。
- **原因**：Segoe Fluent / MDL2 里 `PinnedFill`（E841）只是填充层，只画针头那一块实心；针是线，只在轮廓里。
- **做法**：`Glyph.overlay`（`RowAction.glyphOverlay`）在填充上用同一字体、同一颜色再叠一层轮廓 `Pin`（E718）。不能叠 `Pinned`（E840）：Fluent（Win11）里 E840、E841、E718 都是斜的，叠哪个都对；MDL2（Win10）里 E841 和 E718 是横的，E840 却是斜的，叠上去错位。挑叠加的字形时两套字体都画出来比一比；其他 `…Fill` 图标单独用之前，也先看一眼是不是缺了线条部分。

### 切换分类后 `positionViewAtBeginning()` 停在第一行顶上，`topMargin` 被滚掉了

- **现象**：在剪贴板面板里切换分类（以及每次打开面板）后，列表看着在顶部，其实往下偏了 `topMargin`（4 px），滚动条也不在最顶。
- **原因**：`ClipModel::setRows` 分类相同顺序时是逐行删、逐行插，`originY` 随之漂移；之后 `positionViewAtBeginning()` 常常把 `contentY` 停在 `originY`，不算 `topMargin`。只在第一行不变时才对。先 `forceLayout()` 也没用（用 qml.exe 离屏实验逐种比过）。
- **做法**：`positionViewAtBeginning()` 之后再 `contentY = originY - topMargin`（和 `ListScrollBar.minY` 同一个起点），之前先 `cancelFlick()`。搜索结果（`Main.qml::selectFirst`，含展开面板）也已这样处理；对话框栏（DialogBarWindow.qml）的列表也有 `topMargin: 4`，遇到同样现象照此处理。
- **搜索列表还要处理异步首批结果（2026-10-17）**：只改 `selectFirst()` 不够。真实启动记录中，`shown` 和 `resultsReplaced` 发生时列表为空、已停在 `-4`；稍后 256 条结果到达，列表高度还为 0，`onCurrentIndexChanged` 的 `positionViewAtIndex(0, Contain)` 又把坐标改成 `0`，之后高度扩为 488 也不回顶。第一行被选中时也要显式落在 `originY - topMargin`（Main、Panel）。在临时配置中加载实际 QML、Launcher、ResultModel，在插入首批结果后、排队调整列表高度之前 `forceLayout()`，修改前稳定失败、修改后通过；真实启动记录也确认首批结果到达后保持 `-4`。
- **验证边界**：对 `resultsReplaced` 的隔离测试只能说明通知后的定位行为，不能证明用户报告的“滚到顶部后延迟抽动”已修复。该抽动后来由用户实际操作录到，原因是下面的绘制取整，与启动偏移分开处理。

### 搜索列表回顶后，动画尾段再跳一个像素

- **现象**：150% 缩放下，用户实际操作的两次记录中，第一行看似停住约 426 / 670 ms 后又向下移了一个设备像素；`contentY` 仅从极接近 `-4` 变成精确的 `-4`，窗口、选中项、结果数、`originY` 都没变。
- **原因**：对行的绝对绘制位置做 `Math.round()`。列表起点是 65 个逻辑像素，加顶部留白 4，终点恰好在 `69 × 1.5 = 103.5` 个设备像素上；动画尾段一直略小于 103.5，被取成 103，最后达到精确终点才取成 104。只检查 `contentY` 或最终是否到顶，测不出这个可见跳动。
- **做法**：`ScrollPixelAlignment.qml` 分别取整“在顶部静止时的位置”和“相对顶部的滚动位移”。位移在最后半个设备像素内时，绘制已经落到终点；该组件本身不改动滚动坐标。搜索框的结果行、尾部提示和展开面板共用它。滚轮的终点还需要下面的单独处理。
- **验证**：回放用户两次记录中的全部帧坐标到实际 `Main.qml`：旧逻辑尾段均多跳一次，新编译的 QML 均不再跳动。`tst_scrollalignment` 在 100%、125%、150%、175%、200% 下检查半像素终点、尾段反向移动、整设备像素绘制、各行同步移动，以及不改动 `contentY`。

### 搜索列表滚到中间停下后，仍会补跳

- **现象与证据（2026-10-17）**：用户指出上面的绘制修复没有覆盖中途停下。隔离窗口发实际 `QWheelEvent`，150% 下从 `contentY=120.4` 向下滚一格，先趋近 192.4，结束时突然变为 192，绘制反向走一个设备像素；小步滚动出现约 130–170 ms 不动后再移动。
- **原因**：Qt 6.12 的 `QQuickFlickable::wheelEvent()` 使用 `OutExpo` 到当前小数位置加滚轮步长，结束回调再执行 `fixup()`，按逻辑像素及速度方向取整。绘制对齐只能稳定每帧，不能消除动画结束时坐标被另一次取整改写。
- **做法**：主搜索框和展开面板共用 `ListWheelScroll.qml`。收到离散滚轮事件时先计算步长、边界及相对顶部的设备像素终点，再用 200 ms `OutCubic` 朝这个终点减速；没有结束回调补位置。连续同向输入累加距离，反向输入丢弃尚未走完的旧方向距离。重新打开、切换选中项、滚动条或其他来源改坐标时取消旧动画。尊重系统滚动行数和关闭动画的设置。
- **和旧的弃用方案的区别**：只对齐目标，不逐帧取整并回写原生惯性坐标；行的绘制仍由 `ScrollPixelAlignment` 对齐。Qt 标记的触摸板/合成手势，以及触屏/拖动，继续走原生路径。Windows 某些触摸板报告为普通 Mouse angleDelta，它们与普通滚轮走相同路径。`WheelHandler` 的 `acceptedDevices: Mouse` 在分发前排除合成手势；不能在 `onWheel` 里靠 `event.accepted=false` 转交，Qt 6.12 的 handler 不读取该值。
- **验证边界**：`tst_scrollalignment` 用实际滚轮事件复现旧路径的四组失败，再验证中途停止、连滚、反向、外部接管、上下边界和关闭动画；五种缩放通过。隔离配置加载编译后的真实 Main/Panel，原先四组输入均无尾段反向移动，打开/异步首批结果/重开/结果替换的回归也通过。这些是自动化验证，用户手动操作的最终体验需另行确认。
- **独立评审后的核查与加固（2026-10-17）**：用户已手测确认抽动消失。自然滚动无需再乘一次 `-1`：`QWheelEvent.inverted` 描述的是已经反转过的 delta，继续使用事件中的符号即可（[Qt 文档](https://doc.qt.io/qt-6/qwheelevent.html#inverted-prop)）。补测实际事件确认 `inverted=true/false` 下相同已交付 delta 的方向一致；删除首行导致 `originY` 漂移时，现有外部接管逻辑会取消旧动画。
- **两个补测发现的边界**：滚动行数为 0 时应不滚动，不能按一页处理；Windows Qt 6.12 的主题读取系统行数 0，但“按页”特殊值反而被 Qt 回退成 3 行，不能把 0 当作该特殊值（[固定版本源码](https://github.com/qt/qtbase/blob/v6.12.0/src/plugins/platforms/windows/qwindowstheme.cpp)）。关闭动画或相邻输入已完成动画时，下一次同向小增量仍需保留 `requestedY` 的小数余量；否则逐次设备像素取整会累计多滚或少滚。外部移动或取消清除方向，只有仍处于自己上次对齐终点时才延续余量。两项修改前均有失败回归；0 行通过组件的系统参数绑定入口注入，不改 Windows 设置。

### 剪贴板列表滚轮一格一顿

- **现象**：剪贴板列表用鼠标滚轮滚动，每一格都“顿一下”，回到顶部时尤其明显，像在抖；启动器列表没有。
- **原因**：行高两种（图片行 80、其余 `Theme.rowHeight`），ListView 用可见行的平均高度估 `originY`/`contentHeight`，每创建或释放一行估值就变 → `setContentHeight()`。Qt 6.12 默认滚轮（`wheelDeceleration` 15000 > 14999）走 `moveBy(OutExpo, 3 × fixupDuration / 4)`（默认 `fixupDuration=400`，即 300 ms）并置 `vData.fixingUp`；`setContentHeight()` 见 `fixingUp` 就 `fixupY()`：打断正在滑的动画，并把 `contentY` 取整，取整方向和滚动方向相反，退 0.1–0.75 px（150 % 下退 1 个设备像素）；每格也只走约 33 px 而不是 72。行高都一样时 `contentHeight` 不变，没这回事。往上滚时以为是 `originY` 修正让行跳了，量下来不是：行之间始终首尾相接。
- **做法**：`ClipboardPage` 使用共用的 `ListWheelScroll`，启用 `logicalPixelSteps`：混合行高在布局过程中还会取整中间坐标，因此保留逻辑像素步进，避免布局打断动画；行的绘制再由 `ScrollPixelAlignment` 对齐设备像素。边界目标保留正负无穷的含义，在范围重新估算时跟随实际两端；反向输入从当前位置重算。自身写入保护用嵌套层数计数：一次 `contentY` 赋值里可能触发布局、边界重算和另一次赋值，内层退出不能清掉外层保护。外部修改、重新打开、隐藏和拖动时取消动画。行拖动状态叫 `rowDragging`，避免覆盖 Flickable 自身的 `dragging`。

### 其它滚动界面沿用旧路径（2026-10-17 审核及修复）

- **对话框搜索栏**：首批结果在零高度到达时，`Contain` 及 `positionViewAtBeginning` 仍停在 0，真实顶部是 -4；同样补上取消旧动画、按 `originY-topMargin` 定位和首行选中分支。委托改用 `ScrollPixelAlignment`，离散滚轮使用共用控制器。
- **设置页与剪贴板长文本预览**：原先仅给整块内容做绝对坐标取整再偏 0.02，仍挡不住原生滚轮末尾的坐标 fixup。`ListWheelScroll.view` 扩展为 Flickable，非列表不连接 `currentIndexChanged`；两处页面共用预先对齐终点，绘制使用相对位移取整并保留 0.02 偏移。切换设置分类、标签和搜索定位时显式取消自定义动画，包括尚未产生第一帧、坐标仍是 0 的情况。
- **类型横条**：自动 reveal 原先直接动画写 `contentX`，水平滚轮或拖动接管后仍会被拉回。现在动画写自己的位置属性，只有自身写入受保护；外部坐标、原生移动和拖动会取消运行中及排队中的 reveal。水平 WheelHandler 用 `blocking:false` 只观察接管，不截走原生事件；新 reveal 开始前取消原生惯性。
- **预览换条目**：`Clipboard::preview` 提供稳定的条目 id（字符串），`ClipPreview` 只在 id 改变时取消动画、回到顶部；同条目的文字识别、图片和元数据刷新保留阅读位置，不能每次 `info` 更新都清零。
- **验证**：五种缩放的滚动回归加入整页 Flickable 停止及混合行高滚到两端；隔离配置加载编译后的实际 Main、Panel、DialogBarWindow、SettingsWindow、ClipboardWindow、ClipPreview、TypeStrip，回放审核时的同组滚轮输入。文字和图片混排使用临时 ClipStore；切换长文本预览从 300 回到 0，同条目元数据更新保持 200；类型横条反向滚轮和外部归零均可接管。无 Windows 剪贴板写入。实体触摸板和跨屏 DPI 切换仍未新增手测。

### 单元测试的输出在管道里看不见

- **现象**：`ctest --output-on-failure` 报 `tst_core` 失败却一行输出都没有；直接在 PowerShell 或 Git Bash 里跑 `tst_core.exe 某个测试 2>&1 | head`，也只有退出码（失败的个数），没有 QTest 的任何文字（2026-10-11）。
- **原因**：这台机器上 QTest 的默认输出没有进到管道里（`-functions` 这类直接 stdout 的输出倒是能看到）；具体在 Qt 哪一层没查。
- **做法**：让 QTest 自己写文件：`tst_core.exe 测试名 -o 输出.txt,txt`，再读那个文件。跑前把 `C:\Qt\<版本>\msvc2022_64\bin` 加进 PATH（ctest 替你加）。

### 搜索后列表下方留着旧行，几行字叠在一起（GitHub #43）

- **现象**：剪贴板页打字搜索，底栏说“3 条”，前 3 行正常，空一段后，列表下方还画着搜索前的几行，有的位置两三行的字叠在一起。改窗口或列表大小后消失。0.8.0（8c937e9 加了移动过渡）起就有。
- **原因**（Qt 6.12，dev 分支相同）：列表声明了 `move`/`moveDisplaced`，视图就有 transitioner（`enabled: false` 也一样）。每处理一段删除，`applyRemovalChange` 都给它后面的每一行排上“删除让位”过渡，不看有没有这种过渡。筛选一次删掉好几段不相连的行，同一批布局里，第二段及以后删掉的行早被前一段排上了过渡；`releaseItem` 把它们放进复用池时调 `setVisible(false)`，`QQuickItemViewFxItem::setVisible(false)` 见“已排过渡”直接返回，过渡对象随后被删，再没有人把它剔除。池里的行仍挂在 contentItem 下、`visible` 为真、停在旧的 y 上，不计入 count，直到被复用或池被清空；ListView 只在自身尺寸变化或关掉 `reuseItems` 时清池。只删一段连续的行、不复用、不声明过渡都不出现；关掉系统动画没用。
- **做法**：`ClipRow.qml` 里 `ListView.onPooled: row.visible = false`、`ListView.onReused: row.visible = true`。用 `visible` 不用 `opacity`：池里的行也不再接悬停和点击（index 是 -1，`Clipboard::targets(-1)` 取的是已选条目）。模型的增删信号是对的，不改：按连续段一次删也还是几段；reset 或“删掉尾部再插回”会让留下的行换委托、缩略图重新加载闪一下；关掉 `reuseItems` 滚动时一直建、删委托。以后给带 `reuseItems` 的列表加过渡，委托也要这样处理。
- **验证**（2026-10-19）：qml.exe 离屏最小复现（60 行 ListModel，每行一条只属于自己的竖条，按 `setRows` 的方式从下往上逐行删）：一步删到 3 行时画出旧第 10–13 行；分三步删时同一位置叠着 2–3 行；过渡 `enabled: false`、按段删除照样出现；不复用、不声明过渡、只删一段都没有；加上 onPooled/onReused 后只画 3 行，再放宽时复用的行照常出现。真机（同日，提权 SendInput 打字）：`tauri` 4 行“4 条”，`qq` 11 条、`tupian` 27 条都没有残留行，清空后列表补满，`zqxwvj` 空列表“0 条”。

### 倒过来排的列表：短列表一滚就跳到顶上，滚轮停下后又挪一个像素

- **现象**（2026-10-19，GitHub #38 让剪贴板列表开在光标上方时用 `ListView.BottomToTop`，先在 `tst_scrollalignment` 里复现）：条目不满一屏时，滚轮一格就把它们从底部推到顶上（`contentY` −484 → −184），五种缩放都是；条目多时，125%、150%、175% 下滚轮停下后，过一会儿行又挪一个设备像素；`positionViewAtBeginning()` 之后第一行贴着底边，下面 4 px 的留白滚出去了。
- **原因**：倒过来的列表停在底部，静止位置是 `originY + contentHeight + bottomMargin - height`，不是 `originY - topMargin`。内容比视图矮时两端是反的（底部那端比顶部那端小），能停的只有底部那端；`ListWheelScroll` 按“顶部那端是最小值”夹，短列表就被夹到顶上。设备像素的网格、行的滚动位移（`ScrollPixelAlignment`）都从顶部数，底部那端不在网格上时，终点和绘制差半个像素，到尾段才取整过去。`positionViewAtBeginning()` 和 `Contain` 都不管留白（正着排时顶上的 4 px 也一样，`selectFirst` 本来就补）。
- **做法**：`ListWheelScroll`、`ScrollPixelAlignment` 认出 `BottomToTop`（`bottomUp`）：两端以底部那端为准（短列表停在底部），网格和位移都从静止的那端（`restY`）数；“静止那端的行增删了就取消滚动”也换到底部那端。回到第一行时把 `contentY` 设成 `restY()`（`ClipboardPage`）。`tst_scrollalignment` 的 `bottomUp*` 用例在五种缩放下覆盖这些；正着排的列表算法不变。

## 内存占用

### 量出来的“内存”和任务管理器对不上

- **现象**：`Get-Process` 的 `WorkingSet64` 和 `PrivateMemorySize64` 都是 332 MB，任务管理器“内存”一栏却是 227 MB（2026-10-12）。两者比较后，误以为新功能多占了 150 MB，实际只多了约 42 MB。
- **原因**：任务管理器显示的是专用工作集，不含共享的 DLL 页面和映射文件的页面。`WorkingSet64` 是整个工作集，含共享部分；`PrivateMemorySize64` 是提交大小，不管页面在不在内存里。
- **做法**：用计数器 `\Process(WinShun)\Working Set - Private`，或在程序里读 `PROCESS_MEMORY_COUNTERS_EX2.PrivateWorkingSetSize`。

### 空闲时内存一直不降

- **现象**：Win顺 什么都不做也一直显示二百多 MB。
- **原因**：内存不紧张时，Windows 不会回收空闲进程的工作集，碰过的页面（文件索引、窗口图形）就一直算在里面。实测把工作集清空后，3 分钟内只涨回约 5 MB，空闲时真正在用的只有这么多。
- **做法**：`platform/IdleTrim`：程序失去焦点 20 秒后、索引不忙时，在后台线程调用 `SetProcessWorkingSetSizeEx(-1, -1)`（约 10 ms / 100 MB）。页面进入备用列表，算作可用内存，用到时以软缺页拿回。清空后第一次名称搜索约 10 ms（平时 2.3 ms），所以清空过的话，打开搜索框时另起线程调用 `FileIndex::warm()`，把索引的页面一次拿回来（约 20 ms，和窗口出现同时进行），之后第一次搜索约 3 ms，和平时一样。代价是打开时内存约 150 MB，关掉 20 秒后再清空。`wsbench --trim [--warm] <查询…>` 可以量清空所用时间、预读所用时间和之后第一次搜索。
- **忙完立刻清空**：索引加载、追赶日志、读内容、搬家时不清空，每 15 秒再看一次；`IndexService::stateChanged` 一来（加载完、追赶完）就在 5 秒后清空，不必等下一次检查。实测（`--background` 启动，2026-10-12）：启动 20 秒后 217 → 2 MB。
- **后台工作会把页面带回来**：内容索引启动 30 秒后的第一轮、之后每 15 分钟一轮和文件变动后的一轮，`findWork` 都要走遍整个文件索引，约 90 MB 回到工作集。所以闲着时每 30 秒查一次，超过 32 MB 就再清空（查一次只读一个数）；之前每 2 分钟一次，启动后会有 2 分钟停在约 100 MB。
- **测试时用 `--background` 启动**：不带参数启动 WinShun.exe 会打开搜索框，算在用，不会清空。开机自启和安装程序都带 `--background`。
- **关掉设置窗口后一直不清空（2026-10-22）**
  - **现象**：打开设置再用右上角 X 关掉，任务管理器里的内存停在 190–314 MB，不再下降；要等某个窗口（比如搜索框）激活后再失去焦点，约 20 秒才清到 5 MB 左右。
  - **原因**：Windows 上 Qt 的 `applicationState` 只由焦点事件推出来。关掉有焦点的窗口时，Qt 直接把焦点窗口设为空，没有经过“不活动”，所以状态一直是 `ApplicationActive`，`IdleTrim::inUse()` 一直认为在用。
  - **做法**：`inUse()` 不只看应用状态：还要 `QGuiApplication::focusWindow()` 非空，或前台窗口（`GetForegroundWindow`）属于本进程（原生对话框有前台、没有 Qt 焦点窗口）。设置窗口隐藏时也调一次 `poke()`，5 秒后检查。`poke()` 本身不再先问 `inUse()`（刚关窗口时前台可能还没换），由 `check()` 到时再问。实测关闭后 5 秒内 144 → 2 MB，60 秒内稳定在约 12 MB；搜索框开着的 50 秒里一直是 190 MB，Esc 关掉后 30 秒内清空。

## 权限分离（GitHub #21）

### 普通权限的进程碰不到管理员窗口：键、钩子、消息都过不去

- **现象**：Win顺 不提权运行时，管理员身份的程序（以管理员身份打开的命令提示符、注册表编辑器）在前面时，往里粘贴没反应，剪贴板窗口的键盘接管也没反应。
- **原因**：UIPI。2026-10-14 用 scratchpad `uipi` 探针实测（管理员窗口在前，按键由管理员进程注入）：普通进程收不到键盘和鼠标 Raw Input、收不到低级钩子事件、收不到 `RegisterHotKey` 热键；`AttachThreadInput` 失败（1314），`WM_COPYDATA` 被挡（5），`SendInput` 往里送键不报错但对方收不到。可以的：`OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION)` + `OpenProcessToken(TOKEN_QUERY)` 读对方完整性级别（0x3000）、`SetForegroundWindow`、进程外 WinEvent `EVENT_SYSTEM_FOREGROUND`。
- **做法**：`platform/Integrity` 的 `higherThanOurs(hwnd)`：Win顺 自己提权时总是 false；读不出的当成更高。剪贴板不对这种窗口接管键盘（`App::startKeyRouting` 返回 false，窗口自己拿焦点），粘贴只复制并提示“回到它那里按 Ctrl+V”（`Clipboard::onWritten`），对话框栏不贴这种程序的文件对话框（`DialogJump`）。
- **还没量的**：用户亲手按的键（不是注入的）在管理员窗口在前时，普通进程的低级钩子和 `RegisterHotKey` 收不收得到。探针的对照组没做成（普通进程的窗口抢不到前台），要人按。

### 不提权时启动程序：别再借资源管理器的令牌

- **现象**：提权时用来“以普通权限打开”的 `CreateProcessWithTokenW`（借资源管理器的令牌）在普通进程里失败，错误 1314（缺 `SeImpersonatePrivilege`）。
- **做法**：`integrity::processElevated()` 为 false 时直接 `ShellExecuteExW` / `CreateProcessW`，本来就是用户的权限（`Shell.cpp` 的 `executeUnelevated`、`startUnelevated`，`WinV.cpp` 的 `startShell`）。Ctrl+Shift+Enter 仍走 `runas`，实测启动出的进程是 High（0x3000）。

### 普通权限读不了 MFT 和 USN 日志

- **现象**：普通进程能以访问权限 0 打开 `\\.\C:`，`IOCTL_VOLUME_GET_VOLUME_DISK_EXTENTS`、`IOCTL_STORAGE_QUERY_PROPERTY`（寻道惩罚）都行；但 `FSCTL_GET_NTFS_VOLUME_DATA`、`FSCTL_QUERY_USN_JOURNAL` 失败（1），`GENERIC_READ` 打开卷、打开 `\\?\C:\$MFT` 被拒（5）。
- **做法**：查不到日志时 `IndexService` 本来就把卷当普通卷：遍历建索引，用 `ReadDirectoryChangesW` 跟变化。2026-10-14 实测不提权（`__COMPAT_LAYER=RunAsInvoker`，只留 D 盘）：遍历建好索引，启动后新建的文件马上搜得到，打开的程序是 Medium。要读 MFT 就得有提权的部分（计划中的索引服务）。

### 测不提权的 Win顺：配置在 `<profile>\WinShun\WinShun.ini`

- **现象**：`--profile F:\x` 启动前把 `F:\x\WinShun.ini` 写好排除盘，结果它照样遍历所有盘。
- **原因**：`--profile` 把 APPDATA 和 LOCALAPPDATA 都指到那个文件夹，设置文件在 `F:\x\WinShun\WinShun.ini`，索引在 `F:\x\index.bin`。
- **另外**：`ExplorerMenu` 默认开，测试副本一启动就把右键“在 Win顺 里搜索”改指向它自己；测试时在 `[Launcher]` 写 `ExplorerMenu=false`，测完让 dist 最后一个启动。管理员副本的单实例互斥量 `Local\WinShun.Instance` 普通进程能打开（得到 `ERROR_ALREADY_EXISTS`），所以两者不会同时跑；但第二个副本发给管理员副本的 `WM_COPYDATA` 被挡，命令（显示、--query）会丢。

### 索引服务：`DisconnectNamedPipe` 会丢掉客户端还没读走的数据

- **现象**：服务给客户端发完 `VolumeLost` 就断开连接，客户端有时收不到它，只看到管道断了，当成“服务没了”去重连。
- **原因**：`DisconnectNamedPipe` 把管道里还没被读走的数据丢掉。
- **做法**：断开前先 `FlushFileBuffers`（对服务端的管道句柄它会等到客户端把数据读完），再 `DisconnectNamedPipe`。代价：客户端一直不读，这个线程就挂在那里（不占卷句柄，只占这个进程的连接额度）。

### 几个线程同时连索引服务：`ERROR_PIPE_BUSY`，一个盘悄悄退回遍历

- **现象**：不提权的 `wsbench --service` 走服务建索引，C:、E:、F: 都从 MFT 读了，D: 却是遍历的；日志里只有一句 `cannot read the MFT of D:: cannot open the pipe (error 231)`——而且 Qt 的警告在没有控制台窗口的进程里默认走 `OutputDebugString`，`2>&1` 看不到，要设 `QT_FORCE_STDERR_LOGGING=1`。
- **原因**：`syncWithDisk` 给每块盘一条线程，四条线程同一毫秒各开一条连接；服务端 `PIPE_UNLIMITED_INSTANCES`，但任一时刻只有一个实例在听，接受一个之后才建下一个。客户端原来只试两次：`CreateFile` 忙 → `WaitNamedPipe` 说有空 → 再 `CreateFile` 又被别的线程抢走 → 放弃。
- **做法**：客户端在 2 秒期限内反复 `WaitNamedPipe` + `CreateFile`（`connectSession`），忙是瞬时的，四条连接都能进去。测试里的 `RawClient` 同样要处理。

### 索引服务的测试在 C: 上跑：等“有新记录”的读马上就回来

- **现象**：`tst_service` 原先假设 `start(usn, true)` 之后 300 ms 内没动静、杀掉服务后第一次 `finish()` 一定是空的，在 C: 上全不成立。
- **原因**：C: 上别的程序一直在写（浏览器、日志、Windows 自己），`FSCTL_READ_USN_JOURNAL` 带 `BytesToWaitFor=1` 立刻就完成；两次读 MFT 之间也有几条记录变了。
- **做法**：测试按“循环读，直到出现我写的那个名字”写（`readUntil`），MFT 对比允许万分之一的记录不同（实测 C: 两次读相隔 2.5 秒差 4 条，D: 差 0 条）；`WINSHUN_TEST_VOLUME=D:` 指定一个安静的盘。

### 卷不靠特权打开：去掉全部特权照样读 MFT

- **现象 / 做法**：`WinShunService.exe --console --no-privileges` 先用 `AdjustTokenPrivileges(SE_PRIVILEGE_REMOVED)` 去掉除 `SeChangeNotifyPrivilege` 外的全部特权再服务；对它跑 `tst_service`（D:，2026-10-14）：MFT 430080 条记录和直读逐条一致，日志照常跟踪。所以服务的 `SERVICE_CONFIG_REQUIRED_PRIVILEGES_INFO` 只留 `SeChangeNotifyPrivilege`。实测是以管理员身份，走的是卷 DACL 里 Administrators 那条；装成服务后是 SYSTEM，走 SYSTEM 那条，同理。

### 同名的 `Read`：`ntfs::Read` 和 `volproto::Read`

- **现象**：在 `namespace ws::ntfs` 里 `using namespace volproto` 之后写 `Read read {usn, wait}`，编译器选了 Ntfs.h 里 MFT 的 `Read`（一次读的偏移和长度），报一长串 `encode` 不匹配。
- **做法**：写全 `volproto::Read`。

### 复用 `Writer` 发第二条消息，前一条的字段还在里面

- **现象**：客户端报 “the index service sent a malformed end”：`MftDone` 解不出来。
- **原因**：服务端用发 `MftInfo` 的那个 `Writer` 直接 `encode` 了 `MftDone`，没有 `reset()`。
- **做法**：`reset()`，或者新建一个。解码端的长度检查把它当畸形帧拒掉了，正是它该做的。

### `wsbench --mft` 从 Temp 下运行：“test folder missing from the index”

- **现象**：放在 scratchpad（`%LOCALAPPDATA%\Temp` 下）里的 wsbench 跑 `--mft`，末尾的变更日志自测报 test folder missing。
- **原因**：自测用 exe 旁边的 `usn-selftest` 文件夹，而 Temp 在排除规则里。重构前后一样，和改动无关；从别的目录运行就好。

### 普通权限改不了“以最高权限运行”的计划任务，连改回普通都不行

- **现象**：界面改成 `asInvoker` 后，自启动任务的运行级别要按模式写（管理员直读 `HIGHEST`，其余 `LUA`）；普通权限的进程写 `HIGHEST` 被拒，连把提权时注册的旧任务改成 `LUA` 或删掉也被拒（都是 `E_ACCESSDENIED`，2026-10-14 用 `Register-ScheduledTask`/`Unregister-ScheduledTask` 实测）。读得到（`RunLevel=Highest`）。普通进程给自己注册交互式、只在自己登录时触发的 `LUA` 任务可以，删自己注册的也可以。另外 `schtasks /Create /SC ONLOGON` 不带 `/RU` 时对普通进程连 `LIMITED` 都拒绝（任何人登录都触发，算管理员的事），COM 的 `ILogonTrigger::put_UserId` 指定自己就行。
- **做法**：运行级别只由提权的那份写（`App::reconcileAutostart`：不提权就不碰，提权的那份启动时按模式改，登录时正是旧的 `HIGHEST` 任务把它提权起来，改完下次登录就是普通权限）。安装程序 `PrepareToInstall`（提权）先删掉指向本安装目录的旧任务——报告者的场景里它是答 UAC 的那个管理员注册的，普通用户读不了也盖不掉——再以原始用户（`runasoriginaluser`）跑 `--take-autostart --had-autostart` 重新注册一个普通权限的。

### 开机自启关不掉：设置里的开关点不动，托盘菜单也没反应（GitHub #67）

- **现象**：“管理员直读”模式下（任务以最高权限运行），Win顺 以普通权限运行时（双击、开始菜单、升级后由安装程序启动），设置里的“开机时自动启动”点了弹回“开”，托盘那一项也一直打勾。另外两种不一致：设置窗口开着时从托盘关掉，窗口还显示“开”，再点也不动；新打开的设置窗口显示的是缓存，任务在外面改过就不对。
- **原因**：上一条的删除被拒，而 `writeTask(false)` 不看 `DeleteTask` 的结果，任务还在，读回来还是“开”。显示不一致是因为 `SettingsEditor::setAutostart` 拿缓存（`autostart::isEnabled`）和想要的值比，相同就直接返回、不发 `autostartChanged`；`ToggleSwitch` 自己从不改 `checked`，不发通知它就停在原处。托盘和 AI 设置直接调 `autostart::setEnabled`，设置窗口不知道。还有：任务在、但普通权限盖不掉时，打开开关会走 #52 的退路多写一个 Run 值，登录时启动两次。
- **验证时的坑**：用 `Register-ScheduledTask -Xml … -Force` 把已有任务改成 `HighestAvailable` 复现不出来——覆盖已有任务时保留了原来的安全描述，普通权限照样删得掉。要像 Win顺 那样用 COM `RegisterTaskDefinition`（用户名 `DOMAIN\user`、`TASK_LOGON_INTERACTIVE_TOKEN`、`TASK_RUNLEVEL_HIGHEST`）由提权进程新注册，普通权限删除才会 `E_ACCESSDENIED`。
- **做法**：开关只发 `autostartRequested`，由 `App::setAutostart` 写；关闭后仍是“开”且自己没提权，就用 `autostart::removeAsAdministrator` 以管理员身份跑 `schtasks /Delete /TN WinShun /F`（UAC 问一次，窗口隐藏），拒绝了就发通知说明。写完、托盘菜单读过、每次打开设置窗口都在后台重读一次，然后 `showAutostart()` 通知窗口。`setEnabled(true)` 被拒但本副本的任务已在时不写 Run 值。

### 启动 pass 还没存下卷列表就被打断，所有盘的条目都会被删

- **现象**：`IndexService` 加载完快照、还在规划（现在还要等索引服务最多 30 秒）时来了一次 `Pass::Volumes`（插 U 盘、改设置、服务连上），新 pass 从 `m_volumes`（空）算 `known`，`dropStaleRoots` 把快照里每个盘的根都当成“已拔掉的盘”删掉，整个索引重建。以前这个窗口只有加载快照的那一秒，等服务之后有 30 秒，撞上的概率大得多。
- **做法**：快照一加载完就把 `m_volumes`/`m_journals`（快照里的日志位置）存进成员，再等、再规划；被打断后的 pass 从这些位置规划，走日志追赶，不会重读。`volumesChanged` 改成和 pass 开始时拿到的 `previous` 比（早存之后再和 `m_volumes` 比就永远“没变”）。`tst_service` 的 `indexServiceComesLate`/`indexServiceNeverComes` 盯着这条：服务在窗口内连上不读 MFT、伪造的文件夹还在；没连上就遍历、之后连上读一次 MFT。

### 从 bash 起的 pwsh 里跑 ninja：“F:/??????/src/core”

- **现象**：`cmake --build` 在 Bash 工具里经 `pwsh -Command` 跑，ninja 报 `FindFirstFileExA(F:/??????/src/core): 文件名、目录名或卷标语法不正确`。
- **原因**：ninja 在 Windows 上用 ANSI 版 API，源目录的中文按当时控制台的代码页转换，bash 起的子进程代码页不是 936/65001，中文全成了 `?`。
- **做法**：构建从 PowerShell 工具（或项目的 `scripts/build.ps1`）起，不经 bash。

### 便携版的索引服务装在用户可写的位置 = 本地提权

- **现象 / 原因**：便携版（exe 旁边有 `data` 文件夹）在设置里点“安装索引服务”时，老代码直接拿便携文件夹里 `WinShunService.exe` 的路径 `CreateServiceW` 成 LocalSystem、自动启动的服务。便携文件夹常在下载、桌面、`D:\tools` 这种普通用户能写的地方，任何普通权限的程序都能替换那个 exe，下次服务启动就以 SYSTEM 跑任意代码。装到 `C:\Program Files\WinShun` 的正式版没这个问题（只有管理员能写）。
- **做法**：`--install` 先判断自己在不在“受保护的位置”（`svcloc::pathProtected`，`src/core/ServiceLocation.*`）。判断不靠路径前缀，而是看 exe 本身和每一级上级目录的所有者与 DACL：对非管理员 SID（不是 SYSTEM / Administrators / TrustedInstaller 的，含 Everyone、Users、Authenticated Users、CREATOR OWNER、具名用户）的写入权限。**关键**：exe 自己按全套写权限判（写/追加/删/改 DACL/改所有者），而每一级上级目录只看“能不能换掉里面的子项”——删子项、删 / 改名 / 重设该目录本身、改它的所有者；**不含**“能新建兄弟文件 / 子目录”，否则 `C:\` 默认给 Authenticated Users 的“新建文件夹”（`0x4`）会把所有路径都判成不安全，连正式版都误报。**但 exe 所在的那一级文件夹例外**：它也不许非管理员新建文件或子目录——加载器找 DLL 先找程序自己的文件夹、后找 System32（`wtsapi32.dll` 不是 KnownDLL），往 exe 旁边放一个同名 DLL 就是 SYSTEM 代码执行。所以一共三种角色（`svcloc::Role`）：exe 本身（Object）、它所在的文件夹（Parent，再加“新建文件 / 子目录”）、更上级（Ancestor，只看能否换掉子项）。典型漏网：管理员把便携版放进 `C:\ProgramData\某文件夹`，exe 只继承到 Users 只读，看着安全，可那个文件夹继承了 ProgramData 给 Users 的“新建文件”（`(CI)(WD,AD,WEA,WA)`）。判断目录本身（专用目录、日志目录）用 `folderProtected`：目录按 Object、上面全按 Ancestor。不在受保护位置就先把自己复制到 `%ProgramFiles%\WinShun Service`（`SHGetKnownFolderPath(FOLDERID_ProgramFiles)`，不用会被伪造的 `%ProgramFiles%` 环境变量；不放进安装器的 `{app}`，否则卸载正式版会连便携版的服务一起删），注册那一份。目录要么新建、并显式设成 Administrators 当所有者、只有 Administrators/SYSTEM 可写、Users 只读、不继承；要么已存在就用 `folderProtected` 验证、不安全就拒绝报错（不信任别人事先建好的目录）。复制走“把源读进内存 → 同目录 `CREATE_NEW` 临时名写出 → `MoveFileEx(REPLACE_EXISTING)` 换上 → 复验目标 DACL 安全、内容逐字节等于源”；替换前先按名停服务（跑着的 exe 换不了）。服务报 STOPPED 后，它的映像（或杀毒软件在扫旧的、新写的文件）还会占着目标一会儿，这时替换报 ACCESS_DENIED（5）而不是 SHARING_VIOLATION，所以对 5 / 32 / 33 重试几秒（`copyExeInto`）。测试里也栽过：比对内容时用 `QFile` 打开的目标没关（Qt 不带 `FILE_SHARE_DELETE`），紧接着的“升级替换”就是错误 5。`--uninstall` 删服务后，注册的那份在专用目录里才删 exe 和目录（删不掉 `MoveFileEx` DELAY_UNTIL_REBOOT）；正式版（ImagePath 在 `{app}`）不碰，由安装器收。安装器（`RemoveServiceFolder`，`ssDone`）在 `[Run] --install` 把服务改指 `{app}` 之后删掉便携版留下的 `WinShun Service`，**但先看服务的 ImagePath**：正式版装进别人能改的自定义文件夹（例如 `D:\Apps\WinShun`，非系统盘根目录默认给 Authenticated Users 修改权、往下继承）时，`--install` 同样会把服务复制进专用目录并从那里跑，这时删了它，服务重启后就起不来。
- **存量**（旧版便携已经把服务装在可写位置）：服务启动时自检 `pathProtected(自己)`，不安全就写 `service.log` 和事件日志，但**不拒绝运行**——拒绝只会让用户搜索突然变慢，真被攻击者替换的 exe 也不会自检。界面读注册的服务 ImagePath（`QueryServiceConfigW`，普通用户就能读，是 SCM 真正启动的那条路径，权威），用同一套 `pathProtected` 判断（`ServiceVolumeAccess::registeredInUnsafeLocation`），在“便携版的索引服务”那行显示大白话警告和“修复”按钮（= 提权重走 `--install` 的复制）。因此不用改管道协议。
- **顺带（日志目录也是坑）**：`%ProgramData%\WinShun` 默认允许普通用户在 ProgramData 下新建文件夹，用户能抢先建好 `WinShun\` 并占为己有，再在 `service.log` 上放个联接点，把 SYSTEM 的写变成任意文件写 / 删。做法：建目录时设成只有管理员 / SYSTEM 可写（`svcloc::createProtectedDir`，普通用户进程——如 `tst_diagnostics`——写自己的临时目录时退回普通目录，不是提权边界）、开日志文件带 `FILE_FLAG_OPEN_REPARSE_POINT`（联接点不跟随）、安装和服务启动时各把已存在的弱目录收紧一次（`svcloc::secureLogFolder`，SYSTEM 是属主时凭 WRITE_DAC 就能改，降权后也行）。
- **测**：`tst_core` 的 `protectedLocationJudgment` 用 SDDL 构造安全 / 不安全的安全描述符逐一验 `writableByNonAdmin`（Users 可写、Authenticated Users 可改、所有者是普通用户、CREATOR OWNER、只读、拒绝 ACE、仅继承 ACE、`C:\` 式默认按上级安全 / 按对象和父目录不安全、ProgramData 式按上级安全 / 按父目录不安全、父目录 Users 可新建文件、NULL DACL），外加真实路径（`System32\cmd.exe` 和 `System32` 安全、用户临时文件和目录不安全）；`tst_service` 的 `placingInAProtectedFolder`（仅提权跑）在 Program Files 下一个一次性目录上真跑 `ensureProtectedDir` + `copyExeInto` 并复验，再给这个目录单加一条“Users 可新建文件”，验证复制好的 exe 因所在文件夹变成不受保护（`why` 指向目录）、目录被拒，**不碰真服务、不注册真名**，测完删净。

## 双击空白处的文件夹菜单（GitHub #22）

### 鼠标的 Raw Input 一个进程只能登记一份

- **现象**：另开一个窗口登记鼠标 Raw Input，下一次按 Ctrl 后双击 Ctrl 的监听把它顶掉，过一会儿又整个撤掉，双击检测时灵时不灵。
- **原因**：`RegisterRawInputDevices` 对同一个用途（鼠标）是按进程登记的，换一个 `hwndTarget` 再登记就是替换，`RIDEV_REMOVE` 就全撤了。
- **做法**：只让 `KeyListener` 登记鼠标：文件夹菜单开着时一直登记，双击 Ctrl 那边“撤掉鼠标”变成空操作；同一条 `WM_INPUT` 分给 `DoubleTapDetector` 和 `DoubleClickDetector`。改设置时整个 `KeyListener` 重建，旧的先撤。

### 资源管理器分组视图里，组和组标题的 MSAA 角色一样

- **现象**：按“修改日期”分组的文件夹，行间的缝和组标题条 `accHitTest` 都落在 GROUPING 上，分不出哪个是空白。UIA 的 `ElementFromIAccessible` 对这些 DirectUI 对象一律失败（E_INVALIDARG），拿不到 `UIGroupItem` / `UIGroupHeader` 类名。
- **做法**：看它的第一个子对象（`AccessibleChildren(obj, 0, 1)`）：整组的第一个子对象是同名的 GROUPING（它的标题条），标题条的第一个子对象是“折叠组”按钮。前者算空白。不分组的视图和桌面的空处落在列表自己（`CHILDID_SELF`，LIST）上。2026-10-14 实测：空白处 3–7 ms 判完，双击文件夹条目不弹、照常打开。

### 菜单在屏幕下边缘没往上翻，伸到屏幕外

- **现象**：在窗口靠下的空白处双击，菜单从光标往下伸出屏幕，下半截看不见。
- **原因**：条目刚换，`Column` 的 `implicitHeight` 要到下一次布局（polish）才更新，弹出时按旧高度判断放不放得下。
- **做法**：弹出前先 `content.forceLayout()`，再按高度翻转、夹进工作区。

### “最近的文件夹”第一次读要一百多毫秒

- **现象**：菜单从双击到出现要 120–170 ms，其中判空白 3–7 ms、问资源管理器当前文件夹 3–7 ms、别的窗口的文件夹约 6 ms，其余全在读 Windows 最近项目里的快捷方式。
- **做法**：最近的文件夹在启动时和每次菜单弹出之后在后台读好，菜单用上一次读到的。

### Directory Opus 从外面看不出双击落在了空白处

- **现象**：2026-10-14 用户反馈：在 Directory Opus 里双击空白处没有文件夹菜单（当时只做了资源管理器和桌面）。
- **原因**：Opus 13 的文件列表（`dopus.lister` › `dopus.filedisplaycontainer` › `dopus.filedisplay`）不向辅助功能透露行：MSAA `accHitTest` 在列表里任何位置都是 `CHILDID_SELF`（CLIENT），UIA `ElementFromPoint` 一律是这个 Pane，只有列标题有子对象。脚本的 `OnDoubleClick` 只在双击文件或文件夹时触发。Listary 也做不到，它对 Opus 的这一项是灰的。按行高和条目数推算会被视图模式、分组、滚动打乱，不做。
- **做法**：让 Opus 自己判断。它的“首选项 › 文件显示栏 › 鼠标 › 背景事件”（中文界面的叫法；英文 Preferences › File Displays › Mouse › Background Events）可以给双击空白处设一条命令（“左键双击”下的“默认”一行），默认没启用（`prefs.oxc` 里 `<bg_commands><event command="Go UP BACK" enable="no"/>`，那只是预填的例子）。设成 `"…\WinShunSearch.exe" --folder-menu "{sourcepath$|noterm}"`（设置页的“自动设置”替用户写，见下一条；认不出文件时照设置页的步骤手填）：`$` 让非文件夹（收藏集、FTP）不运行，`|noterm` 去掉结尾的 `\`，免得 `"D:\"` 的 `\"` 被当成引号（`WinShunSearch.exe` 再把 `D:` 补成 `D:\`）。它把文件夹经 `WinShun.SearchHere` 窗口交给 Win顺（`WM_COPYDATA`，标记 `WSFM`），Win顺 按光标所在的 lister 弹菜单；选中后 `dopusrt /acmd Go "<文件夹>"`（不带 `NEWTAB`），最近用过的那个 lister 的当前标签过去。实测（Opus 13.25 中文界面）：按设置页的步骤在 Opus 里填好，真的双击空白处，199 ms 菜单出来，当前文件夹正确，Opus 没有回上一级；Esc 关闭；选中后同一个标签跳过去；`C:` 补成了 `C:\`。没测双击条目（那由 Opus 自己分开）。
- **不要给 `dopusrt /info` 传文档以外的类型**：手册只列了 `paths`、`list`、`listsel`、`open`、`select`。2026-10-14 研究时传了一个 `selected`，`dopus.exe` 当场崩溃（0xC0000005，线程 `dopus.worker`），之后的 `/info` 每次都卡 30 秒，直到 Opus 重启。
- **替用户写 `prefs.oxc`**（用户：“能不能不让用户设置？我们帮用户做好”）。实测 Opus 13.25（2026-10-14）：运行中的 Opus **不重读**外面改过的 `prefs.oxc`（改完双击空白处没反应）；`dopusrt /cmd Close PROGRAM` 退出时**不重写**它（修改时间不变，改动还在）；再启动就生效（菜单 213–245 ms），原来开着的 lister 和文件夹都会恢复；`<dblclk>` 里没有 `<bg_commands>` 时，插一段 `<bg_commands><event …/></bg_commands>` 到 `<mouse_hook_delay>` 前面也认。但在 Opus 的首选项里点“确定”会把内存里的整份设置写回去，所以**不能趁 Opus 运行时改文件**：`filemanager::editOpusPrefs` 先关 Opus（等进程退出，15 秒不退就放弃，比如有复制任务在问），再改，再启动。只换 `<dblclk>` 里 `id="0"` 的那个 `<event>`，别的字节原样（`pathtext::withOpusBlankClick`，有单测）；原来那条存在 `[Launcher] FolderMenuOpusBefore`，“取消设置”原样放回（实测还原后整个文件哈希一致）；第一次改之前把整份文件备份到 `%LOCALAPPDATA%\WinShun\opus-prefs.oxc.bak`。用户自己启用了别的命令时不自动换，设置页显示那条命令，用户点“换成 Win顺 的菜单”才换，取消时换回。设置窗口每次到前台都重读一次状态（用户可能在 Opus 里改了）。卸载时 `WinShunSearch.exe --opus-menu-off` 把指向本副本的那条改成 `enable="no"`，免得卸载后双击空白处 Opus 报找不到程序（2026-10-14 用 ISCC 编的测试包真装真卸过：只变了这一行，重启 Opus 后双击空白处什么也不发生）。只在托盘里跑、没开窗口的 Opus，重新启动时会开出默认 lister，`dopus.exe` 没有不开窗口的命令行开关（只有它首选项里的“启动时不打开 lister”）：原来没窗口的，等新窗口出来就 `WM_CLOSE` 掉（实测约 0.5 秒后关掉，仍在托盘）。Opus 15 秒不退就放弃；之后用户才在 Opus 的提问里点“退出”的话，Opus 会关掉且不会再被启动。

### 用快捷键弹出（GitHub #25）：想看菜单做了什么，日志里没有

- **现象**：实测快捷键时想从 `WinShun.log` 读“菜单是关了还是丢了这一次”，文件里一行都没有：`qInfo` 只进调试输出，日志文件只记警告和错误。
- **做法**：环境变量 `WINSHUN_LOG_INFO` 设了就连 info 行一起写（`main.cpp` 的 `logToFile`），测试脚本启动 Win顺 前设上，用 `--profile` 的那份日志读状态；平时不设，日志不变。别为了测试把 info 改成 warning。

### 快捷键在 Win顺 自己的窗口上按：菜单出来了，键一个都到不了它

- **现象**（2026-10-15 合并版实测）：对话框旁搜索框有焦点时按文件夹菜单的快捷键，菜单弹出来，按 Z 进了搜索框的输入法候选，Enter 没反应；启动器在前时同样，Esc 关掉的是启动器。
- **原因**：进程登记了键盘 Raw Input（双击 Ctrl），自己的窗口在前台时 Windows 不调用本进程的低级键盘钩子（见上文“数字键”一条），`KeyRouter` 什么也收不到。
- **做法**：不要让自己的窗口在前台时开这个菜单。搜索框有焦点 → 先把前台还给它旁边的对话框（`DialogJump::isCompanion`，`SetForegroundWindow` 是自己的前台所以允许），菜单按对话框来（`FolderMenu::setDialogOf`；`show()` 的前台核对也认“对话框的同伴在前”，前台切换有一点点滞后）；启动器在前 → 先把启动器收起（和 Esc 一样），等它滑出去（最多 20 × 30 ms）再按新前台弹菜单（`App::folderMenuHotkey`）。实测：搜索框里按，对话框回到前台、菜单的文件夹是对话框的，Z+Enter 对话框转过去；启动器上按，启动器收起，菜单对着后面的窗口。

### “Total Commander、XYplorer 开着中文输入法，Down 和主键盘 Enter 到不了菜单”——是测试脚本发了 VK 0

- **现象**（2026-10-15）：菜单在 TC / XYplorer 上弹出，Z 能跳、Esc 能关、小键盘 Enter 能选，Down 和主键盘 Enter 没反应；装一个最新的低级钩子探针，这两个键探针也看不到；把 XYplorer 线程的布局换成 en-US 它们又“好了”。一度断定是微信输入法在 IMM32 程序那一层吃掉了它们，为此让菜单在这两种窗口上激活、自己收键（`takesKeyboard`），窗口子类化里还按 `VK_PROCESSKEY` 做了换算——菜单激活了，Down / Enter 还是一个 `WM_KEYDOWN` 都没有。
- **原因**：脚本的 `MenuOver` 里写了 `$vk = [int][char]$letter`，PowerShell 变量名**不分大小写**，这个局部 `$vk` 把脚本级的键码表 `$VK` 挡住了，`$VK.Down`、`$VK.Enter` 成了 `(90).Down` → `$null` → `[int]` 0：`SendInput` 发的是 **VK 0、扫描码 0**，Windows 直接丢掉，当然到不了任何钩子、任何窗口。Z 用的是那个局部变量所以对；Esc 是在 case 主体里用的 `$VK.Esc`，那里没有局部 `$vk` 挡着；小键盘 Enter 是字面量 `KeyEx(0x0D, …)`；换布局那段 `LayoutTest` 也没有局部 `$vk`，它的 Enter 本来就是真的——所以恰好只有 `MenuOver` 里的 Down 和主键盘 Enter “坏了”。`DismissTcNag` 里同样的 `$vk` 让 `$VK.Alt` 也成了 0，TC 的提示窗时灵时不灵也是它。给每个键记下 `SendInput` 的返回和实际发出的 `vk/scan` 一眼就看出来了（`tap Down: … vk 0 scan 0`）；在这之前的几轮探针只打印了白名单里的 VK，VK 0 的事件被过滤掉了，所以“探针没看到”也是假象。
- **做法**：`takesKeyboard` 整个撤掉，TC / XYplorer 和别处一样是不抢焦点的 `KeyRouter` 模型（修好脚本后实测：微信输入法中文状态下，菜单在 XYplorer 上 Z / Down / Z / Enter 过去了，TC 同样；菜单激活模型下也测过 Down、Up、Home、PageDown、Left、Right、Space、Tab、Enter 全到，Z 之后 165–900 ms 内的 Down 也到——都没有输入法吃键这回事）。脚本：键码表不要叫 `$VK` 又在函数里用 `$vk`，改名 `$letterVk`、`$digitVk`；每次 `SendInput` 记返回值和发出的 `vk/scan/flags`；探针别按白名单过滤，全部打印。没测的：真按物理键（这台机器只能注入），微软拼音。

### 快捷键弹出的位置：光标不在那个窗口上时落到焦点控件，差 1 像素是换算

- **现象**：150% 下菜单该在焦点控件左上角 (834,454) 往里 24 物理像素即 (858,478)，窗口实际在 (858,477)。
- **原因**：锚点是物理像素，交给 Qt 要换成逻辑像素（478 / 1.5 = 318.67），Qt 放窗口时再换回去，分数缩放下来回各舍一次。
- **做法**：和双击落点一样接受 1 像素；核对时按“同一套单位、容差 1 像素”比。光标在窗口上时直接用光标点，没有这个误差。屏幕下边放不下时菜单照样往上翻（`other`、`explorer-pointer` 两例都翻了，x 不变）。

### 实测脚本（Windows PowerShell 5.1）的几个坑

- 变量名不分大小写：脚本级的 `$VK = @{ Down = 0x28; … }` 和函数里的局部 `$vk = …` 是同一个名字，局部一赋值，`$VK.Down` 就成了 `$null`（见上面“……是测试脚本发了 VK 0”一条，为此错判了一下午）。表和局部变量用不同的词，每个 `SendInput` 记下返回值和实际的 `vk/scan`。
- `[ushort]` 不是 5.1 的类型加速器（pwsh 7 才有），`Add-Type` 之后 `[ushort[]]@(...)` 报 “Unable to find type”，提权窗口里又看不见：先不提权跑一遍脚本看编译错误，数组写 `[uint16[]]`。
- 在“别的窗口”上选一个已经有资源管理器窗口开着的文件夹来验证“用文件管理器打开”，数不到新窗口：外壳打开文件夹时，已有窗口显示着它就只把那个窗口提到前台（前台确实换成了它）。要验证就选没开着的（最近的文件夹），再数新开的 `CabinetWClass` 窗口。
- 记日志的函数里用 `Write-Output` 写一行，这一行就成了调用它的函数的返回值的一部分：`Recorder` 返回的成了 `Object[]`，`MenuWindows` 只剩一项时又被拆成单个对象，`.Count` 是空。日志用 `Write-Host`，返回数组写 `return ,$m`，调用处套 `@()`。

## 内容索引直读（bulk，第一次建立不逐个打开文件）

做法和取舍见 [architecture.md](architecture.md) 的“内容索引第一次建立时直读磁盘”（`src/core/NtfsBulk.cpp`、`ContentIndexer::pass` 的 feeder、`src/service/Volumes.cpp` 的 `serveOpenBulkRead`）。下面是量出来的、光看资料想不到的几条。

### 只读属性和安全描述符的打开，Defender 不扫

- **现象**：设计时拿不准“服务取每个文件的安全描述符要不要再打开一次，打开会不会又被扫”。
- **量法**：`wsbench --open-probe --sample 12000`（不必提权），同一批候选文件三遍、每遍换 seed（Defender 记得扫过的文件）：`GENERIC_READ`+读 64 KB、`FILE_READ_ATTRIBUTES|READ_CONTROL`+`GetSecurityInfo`、`GetFileAttributesExW`；看整机 CPU 减本进程 CPU（钱花在 MsMpEng 里）。
- **结果**（2026-10-14）：读数据 6.24 ms/文件别的进程的 CPU，只读属性 0.06 ms，按路径取属性 0.06 ms。所以访问检查用 `OpenFileById(READ_CONTROL)` 取描述符，不解析 `$Secure`。

### 卷直读看到的是磁盘，不是缓存；记录约 200 ms 后才落盘

- **现象**：`wsbench --dirty-window`：新建的文件写完立刻直读，记录“不在用”或还是别的文件的；`FlushFileBuffers` 之后记录也要约 200 ms 才在磁盘上出现（数据倒是先到）。原地覆盖一个常驻文件（数据在记录里），先前打开的卷句柄读到的是旧内容，新开一个卷句柄读到新的——“打开卷句柄会让 NTFS 刷一次”只是观察，没有单独证明，**不要依赖它**。`FSCTL_GET_NTFS_FILE_RECORD` 对这次常驻覆盖也答了旧记录（LSN 没变），所以它也不是“缓存里的最新”。
- **做法**：三道保险都在界面侧：变更日志给的修改时间 5 分钟内的文件不直读（`kBulkRecent`）；会话前取 `ContentIndex::changeSequence()` 作 `since`，会话期间被写的文件入库时直接标脏（`m_changedWhileReading`）；之后任何写事件照常让它重读。服务每个会话新开卷句柄（便宜）。`FSCTL_GET_NTFS_FILE_RECORD` 返回的记录**不带** USA（fixup 已应用）：`parseFileRecord` 对它会失败，`BulkReader::readRecordCached` 两种都接。

### `$DATA` 的 VDL 之后、末簇的尾巴，盘上是垃圾

- **现象**：原型只按 VDL 决定读多少簇，最后一簇里 VDL 到 want 之间的字节直接交出去；`--raw-read --verify` 之前没撞上，是审查发现的。
- **做法**：`readAll` 的 `deliver()` 把 `initialized` 到 `want` 之间清零（`ReadFile` 给的就是零）；`--raw-read --verify` 2 万个样本逐字节一致（含 `valid data length < size` 这一组）。

### 记录头 0x12 的“硬链接数”把 8.3 短名也算一个

- **现象**：`--raw-read` 的“hard linked”组 2 万个里 1.5 万个。
- **原因**：链接数是 `$FILE_NAME` 属性的个数，DOS 命名空间（2）的短名别名也算。
- **做法**：数命名空间 ≠ DOS 的 `$FILE_NAME`（`FileInfo::names`）。

### `small` 是 Windows 头文件里的宏

- **现象**：测试里 `const QByteArray small = …` 报“`QByteArray` 后面接 `char` 是非法的”。
- **原因**：`rpcndr.h`（经 `windows.h`）`#define small char`。
- **做法**：别用 `small` 当标识符（`tiny`）。

### Qt 测试程序在 PowerShell 工具里一行输出都没有

- **现象**：`tst_core.exe … 2>&1 | Select-Object -Last 8` 什么都不打印，只有退出码。
- **做法**：加 `-o 文件,txt` 让 QTest 把结果写进文件再看；退出码非零再查文件里的 `FAIL!`。

### XYplorer：`WM_COPYDATA` 的 `cbData` 多算了结尾的 0，脚本报错、消息不返回

- **现象**：2026-10-14 给 XYplorer 27.20 发脚本（`dwData` `0x00400001`），回答都收到了，`SendMessageTimeout` 却等到超时才回来；之后再发的脚本一律立刻返回、什么都不做。
- **原因**：`lpData` 是 UTF-16，XYplorer 把 `cbData` 内的每个字节都当脚本，结尾的 `\0` 成了多出来的一行 `·;`，脚本跑完后弹出“'·' is not a valid script command”的模态窗口（后面还跟一个“Error 400: Form already displayed”），消息要等那个窗口关掉才返回，脚本引擎也一直被它占着。VB6 程序，隐藏的错误窗口在截图里才看得见。
- **做法**：`cbData` 只算字符的字节数，不含结尾的 0（`filemanager::askXYplorer`）。它回答时也一样：`cbData` 是字节数、没有结尾的 0，收到后按 `cbData / 2` 个字符取。测试时弹出的脚本错误窗口（`XYplorer - Scripting`，属主是主窗体）发 `WM_CLOSE` 就取消了脚本。

### XYplorer 的标签路径可能是“特殊路径”，盘根没有反斜杠

- **现象**：`get('tabs')` 给出 `此电脑|文档|young`，`get('path')` 给出 `C:`；拿去 `GetFileAttributes`，`文档`、`young` 查不到，`C:` 查的是 C 盘的当前目录。
- **原因**：XYplorer 的标签里存的是它的特殊路径（`文档` 是文档文件夹，用户名代表用户文件夹，`桌面`），`get('path')`、`pathreal()` 的约定是不带结尾的反斜杠，盘根就只剩 `C:`。
- **做法**：脚本里对每个标签 `pathreal()`（`文档` → `C:\Users\young\Documents`，`此电脑` 原样返回，解析时扔掉），`C:` 补成 `C:\` 再查（`pathtext::xyplorerTabs`）。`foreach` 要加 `e` 标志跳过空项：`pathreal('')` 默认是当前聚焦的文件，会混进一个文件路径。

### XYplorer 的主窗口有属主：VB6 运行时的 `ThunderRT6Main`

- **现象**：想用“类名 `ThunderRT6FormDC`、可见、没有属主”认主窗口，一个都找不到。
- **原因**：VB6 程序的每个窗体都是 `ThunderRT6FormDC`，属主是运行时的隐藏窗口 `ThunderRT6Main`（标题是程序名 `XYplorer`，`IsWindowVisible` 为真但 0×0）；它自己的对话框（试用版提示、设置、脚本错误）的属主才是主窗体。别的 VB6 程序的窗体类名一样。
- **做法**：类名 `ThunderRT6FormDC`，属主为空或类名是 `ThunderRT6Main`，再查窗口进程的 exe 是 `XYplorer.exe`（`filemanager::kindOf`）。找“有没有 XYplorer 在跑”（含在托盘里的）用 `FindWindowEx(ThunderRT6Main, "XYplorer")` 再核对 exe。它第一天第一次启动弹“XYplorer Trial Version”（有 Continue Evaluation 按钮，不是许可协议），按钮是 `ThunderRT6CommandButton`，`BM_CLICK` 就行；之后再启动不弹。

### Pulse：刚换的文件夹，会话文件里还是上一个（GitHub #41）

- **现象**：2026-10-21（Pulse 1.0.56）在 Pulse 里换了文件夹马上读 `%LOCALAPPDATA%\Pulse\session.json`，读到的还是上一个文件夹，0.9–3.3 秒后才变；窗口标题约 70 毫秒就变了。
- **原因**：Pulse 自己画窗口，没有对外查询的接口（UI Automation 只有顶层窗口，单实例的 `WM_COPYDATA` 只收“打开”），能读的只有会话文件；它在界面计时器里每 3 秒最多存一次（有变化才写，鼠标拖动中不写）。
- **做法**：只开一个窗口时，标题是“有焦点那一格的文件夹名 — Pulse”（盘根 `C:`，`pathtext::pulseTitle`），和会话里的对不上就等文件的修改时间变（最多 3.5 秒）再读；同一版等过一次还对不上（标题不按这个规则出），记住这一版，不再为它等（`filemanager::pulseSession`）。两个以上窗口时标题是窗口的名字（各窗格共同的文件夹，或用户起的），核对不了，接受最多 3 秒左右的旧数据。会话里的路径是 `\\?\F:\…`、`\\?\UNC\…`，`""` 是此电脑，`pulse:…` 是它自己的地方（`pathtext::pulseWindows` 换回普通路径、扔掉后两种）。

### Pulse：多个窗口按位置对，位置被缩放了

- **现象**：用 `GetWindowPlacement` 取 Pulse 窗口的 `rcNormalPosition` 去对会话里的 `left/top/right/bottom`，150% 缩放下取到 1067×640，会话里是 1600×960。
- **原因**：取位置的线程没有 DPI 感知（当时是 PowerShell），系统按缩放换算了；Pulse 是 Per-Monitor v2，存的是它自己看到的值。
- **做法**：取之前把线程的 DPI 感知设成窗口自己的（`SetThreadDpiAwarenessContext(GetWindowDpiAwarenessContext(窗口))`），取完还原（`filemanager::samePlace`），之后完全一致。

### Pulse：读它的会话文件时，它那一次保存会失败（看源码）

- **现象**：没有实测；Pulse 保存会话是先写临时文件，再 `MoveFileExW(REPLACE_EXISTING | WRITE_THROUGH)` 换过去。
- **原因**：别的进程开着这个文件而没给 `FILE_SHARE_DELETE` 时，替换会因共享冲突失败；`QFile` 打开时不给这个共享。退出时的最后一次保存也一样。
- **做法**：自己 `CreateFileW`，共享给 `FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE`，同一个句柄上 `GetFileTime` 记下这一版（`filemanager::readShared`）。

### Pulse：从 Win顺 打开的文件夹，开在了新窗口里（看源码）

- **现象**：文件夹菜单在 Pulse 上选了一个文件夹，Pulse 不在原窗口里转过去，而是开一个新窗口。
- **原因**：Pulse 只接受“打开”请求（`pulse.exe 路径`，第二个实例转交给在运行的那个），开在哪儿按它的设置“从其他程序打开的文件夹”：默认“智能”——已有窗口显示那个文件夹就切过去；最近用的窗口最小化或在托盘里就在它里面开标签；正忙（改名、菜单、拖动）或 8 秒内在它里面按过键、点过鼠标就开新窗口；否则开标签。快捷键是在 Pulse 里按的，选的时候多半还在 8 秒内。没有“让这个窗口转过去”的接口。
- **做法**：不绕（不模拟按键去改它的地址栏），交给 Pulse，使用说明里写明：要在原窗口开标签，把 Pulse 的这项设置改成“标签页”。Pulse 在新窗口里会弹提示，可以一键改回标签。

### 模拟按键的 `INPUT` 结构在 x64 上是 40 字节

- **现象**：2026-10-14 测试脚本里 `SendInput` 返回 0，回车没按下去，看起来像启动器没响应。
- **原因**：C# 里把 `INPUT` 写成 `uint type; KEYBDINPUT ki; long pad1; long pad2;`，算出 48 字节；真实大小是 40（`type` 4 + 对齐 4 + 联合体 32，以 `MOUSEINPUT` 为准），大小不对 `SendInput` 整批拒绝。
- **做法**：`uint type; KEYBDINPUT ki; long pad1;`，并把 `SendInput` 的返回值记进日志，不要只看“发了”。

### 测试脚本里 `$r`、`$pid`、`$profile` 会撞上内置变量

- **现象**：PowerShell 测试脚本报“无法覆盖变量 PID，因为它是只读变量”、“Collection was of a fixed size”。
- **原因**：`$pid` 是只读的自动变量；变量名不分大小写，`$r = … -split ','` 把报告用的列表 `$R` 换成了定长数组；`$profile` 也是自动变量。
- **做法**：起名避开自动变量（`$sp`、`$rect`、`$prof`），`Get-Variable` 能查。

### 设置窗口加了 `required property`，离屏预览就整页出不来

- **现象**：2026-10-15 `WinShun.exe --preview-settings` 退出码 3，一张图也没有，日志只有“Required property backup was not initialized”和两条 `Cannot read property 'busy' of null`。
- **原因**：设置备份（#17）给 `SettingsWindow.qml` 加了 `required property Backup backup`，`App` 建窗口时传了，`Preview.cpp` 的 `createWithInitialProperties` 没跟着加；缺了必需属性，组件直接建不出来。
- **做法**：给 `SettingsWindow` 加必需属性时同时改 `Preview.cpp`（预览里给一个不设文件夹、不会真备份的对象），改完跑一次 `--preview-settings` 看退出码是不是 0。

## 结果列表的系统右键菜单（GitHub #26）

### 外壳菜单的“发送到”“打开方式”展开是空的

- **现象**：`IContextMenu::QueryContextMenu` 填好的菜单弹出来了，“发送到”“打开方式”这类子菜单展开没有项（或根本展不开）。
- **原因**：这些子菜单在 `WM_INITMENUPOPUP` 时才由处理程序（`IContextMenu2/3::HandleMenuMsg`）填进去，自绘的项还要 `WM_MEASUREITEM` / `WM_DRAWITEM`。这些消息发给菜单的属主窗口；Qt 的窗口过程不认识它们，`shell::popupMenu` 那样带 `TPM_NONOTIFY` 的调用更是根本不发。
- **做法**：不传 `TPM_NONOTIFY`；菜单开着期间 `SetWindowSubclass` 启动器窗口，把 `WM_INITMENUPOPUP` / `WM_MEASUREITEM` / `WM_DRAWITEM` / `WM_MENUCHAR` 交给 `HandleMenuMsg2`（返回它给的 `LRESULT`），`TrackPopupMenuEx` 一回来就 `RemoveWindowSubclass`（`platform/ShellContextMenu.cpp`）。

### 菜单的模态循环里，Qt 照常运转

- **现象**：`TrackPopupMenuEx` 在界面线程上阻塞着，排队的搜索结果照样到、行照样被重写、定时器照样响。
- **原因**：菜单循环自己 `DispatchMessage`，Qt 的定时器和排队事件都是普通窗口消息（`WM_TIMER`、`WM_QT_SENDPOSTEDEVENTS`），一样被派发。托盘菜单、`SHDoDragDrop` 早就是这样。
- **做法**：菜单用 `QueuedConnection` 从事件循环里打开，不在 QML 点击的处理函数里嵌套（行的代理可能在循环期间被回收）；要操作的东西按路径先复制下来，不按行号；菜单关了再按路径找行（`Launcher::forgetGone`）。

### 命令弹的对话框别挂在启动器名下

- **现象**（按机制推断，没有用启动器当属主试过）：`CMINVOKECOMMANDINFOEX.hwnd` 给启动器窗口时，删除确认这类模态对话框一拿到焦点，启动器因失去激活而收起，`ShowWindow(SW_HIDE)` 连带隐藏它名下的窗口，对话框不见了，`InvokeCommand` 还阻塞在里面等人回答；“打开方式”之类我们本来就先收起启动器再执行的命令，对话框更是一出来就没了。
- **做法**：给一个专门的隐藏顶层窗口（`WinShun.MenuOwner`，`WS_POPUP`，从不显示也从不隐藏）当对话框的属主；`TrackPopupMenuEx` 的属主仍是启动器窗口（菜单要前台线程的可见窗口做属主，点在外面才会关）。

### “删除”的进度窗口在 `InvokeCommand` 返回之后才出来，启动器被它挤掉

- **现象**（2026-10-15 实测）：外壳菜单里点“删除”，文件进了回收站，启动器却收起了，前台成了那个隐藏的 `WinShun.MenuOwner`。命令运行期间不让启动器收起也没用。
- **原因**：Windows 11 的删除动词把操作交给复制引擎，`InvokeCommand` 立刻返回；进度窗口（哪怕只闪一下，40 毫秒一轮的轮询都抓不到）之后才出来，属主是 `MenuOwner`，它一激活启动器就失去激活、照规矩收起；它关掉后激活回到属主——一个隐藏窗口。
- **做法**：保留启动器的命令（`shellcontext::keepsLauncher`：复制、剪切、删除、固定……）执行后 `Launcher::m_systemCommand` 一直开着：这期间启动器失去激活、而前台窗口属于本进程（那些对话框、`MenuOwner`）就不收起，前台是别的程序才收起并结束这段；`MenuOwner` 的窗口过程收到 `WM_ACTIVATE` 时把前台交回启动器（它还显示着的话）；启动器再次激活或被隐藏时结束（`App` 的 `activeChanged`、`handleHidden`）。实测：删除一个和两个文件后启动器都还在前台，行已去掉。

### 中文输入法开着时，Shift+Menu 键到不了 QML

- **现象**：Shift+Menu 应弹外壳菜单；微软拼音在中文状态时弹的是 Win顺 自己的菜单，切到英文（单击一下 Shift）就对了。日志里中文状态下 `handleKey` 一行都没有，只有一个带 Shift 的 `QContextMenuEvent`。
- **原因**：输入法把 `WM_KEYDOWN` 的 `wParam` 换成 `VK_PROCESSKEY`，Qt 的键盘映射直接吞掉（“IME will process these keys”），不发按键事件、也不记录这个键；抬起时没有记录，交给 `DefWindowProc`，它才发 `WM_CONTEXTMENU`（Menu 键和 Shift+F10 都这样）。这个事件不说是哪个键，`GetKeyState` 也分不出（到它这时键都抬起了）。`ImmGetVirtualKey` 对 TSF 输入法（微软拼音）给回的还是 `VK_PROCESSKEY`。
- **做法**：`shellcontext::watchKeys` 用 `SetWindowSubclass` 在启动器窗口上记下最后一次 `WM_KEYDOWN` 的键：`VK_PROCESSKEY` 时按 `lParam` 里的扫描码（输入法不改它）`MapVirtualKey(…, MAPVK_VSC_TO_VK_EX)` 还原（Apps 键是扩展键，要带 `0xE000`）。`App::eventFilter` 收到键盘的 `QContextMenuEvent` 时：Shift 按着且那个键是 `VK_APPS`，或 Ctrl+Shift 且是 `VK_F10`，就是外壳菜单，否则 Win顺 的。Qt 的 `QAbstractNativeEventFilter` 收不到按键消息（`isInputMessage`），所以要子类化。英文状态下按键照常到 QML（`Main.qml` 的 `Key_Menu` / `Key_F10` 分支先处理，排队的系统菜单让随后的 `QContextMenuEvent` 被 `systemMenuBusy` 忽略）。

### 测试：原生菜单的项 UI Automation 读不到，字母加速键会撞车

- **现象**：`AutomationElement.FromHandle(#32768)` 下 `FindAll(MenuItem)`（Children、Descendants 都试了）一个项都没有；按 `N` 想展开“发送到(N)”，光标却在“以 Notepad++ 编辑”和它之间跳。
- **做法**：按位置：从截图量出项的 y（这台机器上 Shift 菜单里“发送到”在菜单顶边下 736 物理像素），把鼠标移上去（`WindowFromPoint` 的根是菜单窗口才算），子菜单 `#32768` 自己展开；或 End 再 ↑ 若干下，一步步截图核对（2026-10-15 实测差了一项）。证据用截图和窗口数（子菜单是第二个 `#32768`）。

### 不同文件夹的几个文件，外壳给不出一张菜单

- **现象**（2026-10-15 实测）：多选里的两个文件分别在两个文件夹，`SHCreateShellItemArrayFromIDLists` + `BindToHandler(BHID_SFUIObject)` 照样成功，但菜单只是第一个文件的（“压缩为 zqinner1.zip”），“删除”只删它。换成桌面文件夹的 `GetUIObjectOf`，把完整 ID 列表当它的子项（拖出去的数据对象就是这样做的），拿到的是桌面自己那张短菜单（打开、管理、剪切、复制、删除、属性），“删除”对文件什么也不做。
- **做法**：同一文件夹的多选整组弹菜单；跨文件夹时只对当前行（菜单是在它上面要的）弹，`showSystemMenu` 把它排在路径列表最前。要覆盖几个文件夹得自己实现 `IShellFolder` 或 `CDefFolderMenu_Create2` 的回调，不值得。

### `CMF_CANRENAME` 的“重命名”在这里什么也不做

- **原因**：重命名是让宿主的 `IShellView` 进入编辑状态，不是外壳自己改名；没有视图的宿主点了没反应。
- **做法**：不传 `CMF_CANRENAME`，菜单里就没有这一项。用 `CMF_ITEMMENU`（资源管理器对条目也这样传），Shift 按着时加 `CMF_EXTENDEDVERBS`。

### 测试：PowerShell 5.1 里 `(管道).Count` 对单个对象是空

- **现象**：`(Get-Windows | Where-Object { … }).Count -gt 0` 恒为假，等窗口出现一直等到超时，窗口明明在。
- **原因**：Windows PowerShell 5.1 里 `PSCustomObject` 没有 `Count`；管道只剩一个结果时给出的是那个对象本身。pwsh 7 不受影响，以前的脚本是在 pwsh 下跑的。
- **做法**：`@(…).Count`；接函数返回的列表也用 `$m = @(Wait-Menus 1)`。

### 测试脚本模拟按键不带扫描码，Shift+Menu 看起来弹错了菜单

- **现象**：2026-10-15 合并 #26 后复测，脚本发 Shift+Menu，弹出的是 Win顺 自己的菜单而不是系统菜单；子代理用自己的脚本测是好的，鼠标 Shift+右键也是好的。
- **原因**：中文输入法开着时 Menu 键到窗口是 `VK_PROCESSKEY`，Win顺 靠扫描码认出它（见上面 #26 一节）；脚本的 `SendInput` 只填了虚拟键码，扫描码是 0，认不出来，就当成普通的 Menu 键。真键盘总带扫描码，用户那里没有这个问题。
- **做法**：测试脚本的 `KEYBDINPUT.wScan` 一律填 `MapVirtualKey(vk, MAPVK_VK_TO_VSC)`，Menu、方向键、Insert/Delete 等再加 `KEYEVENTF_EXTENDEDKEY`。结果和真键盘对不上时，先查脚本发的键。

## 在资源管理器里直接打字搜索（论坛 #215、#157、#190）

### 钩子里的键是真键码，不是 `VK_PROCESSKEY`

- **现象**：担心中文输入法开着时低级钩子看到的也是 `VK_PROCESSKEY`，要像结果列表的 Menu 键那样靠扫描码辨认。
- **实际**：`WH_KEYBOARD_LL` 在输入法之前，`vkCode` 就是字母本身（实测微软拼音中文状态下打 `ndbg`，钩子记到的是 `0x4e`）；`VK_PROCESSKEY` 是窗口 `TranslateMessage` 之后才有的。存着的键用 `SendInput` 重发进 Win顺 的框，照常经 Win顺 线程的输入法组字：框里显示 `n'd'b'g`，候选“你打不过、年度报告”。所以这里不用扫描码那一套；`Esc` 要按两次（先取消组字）。

### 焦点在哪，按窗口类一条规则就够

- **做法**：`GetGUIThreadInfo` 取前台线程的焦点窗口，必须是 `SHELLDLL_DefView` 下的 `DirectUIHWND`（`folderview::classify`）。实测（Windows 11 25H2）各处的焦点链：文件列表 `DirectUIHWND<SHELLDLL_DefView<CabinetWClass`；按 `F2` 重命名 `Edit<CtrlNotifySink<CabinetWClass`；`Alt+D` 地址栏是 XAML 的 `InputSiteWindowClass<Microsoft.UI.Content.DesktopChildSiteBridge<CabinetWClass`。后两种自然不匹配，不用列黑名单。
- `SHELLDLL_DefView` 不是标签页的直接子窗口：`CabinetWClass › ShellTabWindowClass › DUIViewWndClassName › DirectUIHWND › CtrlNotifySink › SHELLDLL_DefView › DirectUIHWND`。测试脚本用 `FindWindowEx` 只找直接子窗口，报“没有 DefView”；要 `EnumChildWindows` 按类名找后代。

### 第一次问资源管理器要 160 毫秒，之后 4–9 毫秒

- **现象**：日志里第一次 `Explorer typing: folder "F:\wsexptest" 159 ms`，后面每次 4–9 ms。
- **原因**：`ComWorker` 线程第一次用 `IShellWindows`（建套间、连资源管理器）。键在钩子里存着，用户看不出来，不用预热。

### 提权的测试脚本开出来的资源管理器是提权的

- **现象**：提权的 PowerShell 里 `Start-Process explorer.exe F:\wsexptest`，窗口所在的 `explorer.exe` 是新进程、提权的；功能有意不理提权的资源管理器（`integrity::higherThanOurs`），什么都不发生。
- **做法**：`runas /trustlevel:0x20000 "explorer.exe \"路径\""` 以基本用户令牌启动，窗口是普通权限的（这台机器上文件夹窗口开在单独的 `explorer.exe` 进程里，pid 不是外壳的那个，正常）。用 `GetTokenInformation(TokenElevation)` 记下窗口进程是否提权，别猜。

### 第二个键打下去时启动器已经在前台了

- **现象**：脚本按“前台必须是资源管理器”守卫每一个键，第二个键（间隔 70 ms）常常被中止：第一个键截下后启动器 0–47 ms 就到了前台。头一次碰巧过了。
- **做法**：打字的守卫同时认资源管理器和 Win顺 两个进程（本来就是这样：后面的键要么被钩子存着，要么直接进了启动器）。别把守卫放宽到“任何进程”。

### 用户在用电脑时，脚本的 `SetForegroundWindow` 不起作用，点击也会落到别的窗口

- **现象**：一次运行里 `BringFront` 返回 False，随后点文件列表空白处的守卫报“点落在 Chrome_RenderWidgetHostHWND（VS Code）上”，脚本中止、恢复现场。
- **做法**：这正是守卫的用处；等一会儿再跑，不要去掉守卫或改成强行点击。

### PowerShell 的 `-replace` 不分大小写

- **现象**：用 `-replace 'typetosearch::', 'searchtyping::'` 改命名空间，把类名 `TypeToSearch::` 也全改了。
- **做法**：改代码用 `-creplace`，或者用编辑工具逐处改；改完 `Select-String` 核对一遍。

### 读 `WinShun.log` 里的中文路径成了乱码

- **现象**：测试日志里 `Explorer typing: folder "F:\wsexptest\ä¸­æ–‡ç›®å½•"`，Win顺 的框里却是对的。
- **原因**：日志是 UTF-8，脚本 `Get-Content -Tail` 没加 `-Encoding UTF8`（PowerShell 5.1 默认按系统代码页读）。只是读错，不是功能的问题。

### 文件夹空白处的右键动词不能带 `MultiSelectModel=Single`

- **现象**：0.8.0 的「用 Win顺 搜索此处」在选中的文件夹、盘符上有，在文件夹空白处右键没有（论坛 #209、#215）。用户把 `Directory\Background\shell\WinShun.SearchHere` 下的 `MultiSelectModel` 删掉就好了。
- **原因**：三个键统一写了 `MultiSelectModel=Single`。空白处没有选中项，外壳按“恰好选中一项”去比，就把动词藏了。
- **做法**：`ShellMenu.cpp` 只给 `Directory`、`Drive` 写 `Single`，`Background` 不写，已有的删掉（老用户启动新版即修好）。验证用 `IShellFolder::CreateViewObject(IID_IContextMenu)` 取空白处菜单、`GetUIObjectOf` 取文件夹本身的菜单，列出菜单项对比，不必点界面。

### 经典右键菜单开着时，焦点还在文件列表上（GitHub #37）

- **现象**：Shift+右键弹出经典菜单，按 R 想点“属性(R)”，弹出来的是 Win顺 的启动器。
- **原因**：经典菜单（`#32768`）是资源管理器线程自己的弹出菜单，焦点不挪：`GetGUIThreadInfo` 报的焦点仍是 `DirectUIHWND<SHELLDLL_DefView<CabinetWClass`（捕获和菜单所有者是 `SHELLDLL_DefView`），只按焦点判断就截下了。同一次调用的 `flags` 实测 `0x14`（`GUI_INMENUMODE | GUI_POPUPMENUMODE`），没开菜单、按 `Esc` 关掉后都是 0。
- **做法**：`flags` 带 `0x4`、`0x8`、`0x10` 任一位就放过（`searchtyping::focusOf`，有单测），还是那一次 `GetGUIThreadInfo`，不多花。只测了经典菜单和新式菜单；Menu 键、Shift+F10、Alt 的按键提示、命令栏的下拉、导航窗格的右键没有逐个测。

### Windows 11 的新式右键菜单：焦点进了 XAML 岛，`flags` 是 0

- **现象**：担心新式右键菜单开着时也截键，想加一个“有没有弹出窗口”的检查。
- **实际**：探测（Windows 11 26200）：菜单开着时 `flags` 是 0，焦点在 `InputSiteWindowClass<Microsoft.UI.Content.DesktopChildSiteBridge<CabinetWClass`（和 `Alt+D` 地址栏同一类），焦点规则本来就不匹配，键给菜单，不用另加检查。菜单本身是资源管理器进程里的顶层窗口 `Microsoft.UI.Content.PopupWindowSiteBridge`（标题“主机弹出窗口”，一次两个）。
- **测试注意**：新式菜单里 R 同时是“共享对象”和“属性”的访问键，按 R 是在两项之间切换、不直接打开属性（Windows 自己的行为）。验收“按 R 出属性”要用经典菜单。

### 截下的键重发时，紧跟着打的键夹进中间，“qz”成了“zq”（GitHub #39）

- **现象**：在资源管理器里打 `qzleveltwo`（脚本键间隔 60 ms），框里成了 `zqleveltwo`，没有结果。先是在跳进子文件夹后马上再打字时看到的；复现时普通的第一次打字也出了（`F:\wstyping\ zqlevelone`），和跳转无关。
- **原因**：钩子截下第一个键 q，等启动器拿到键盘再用 `SendInput` 重发。原来的 `typeHeld` 先结束持键（`m_holding = false`，启动器在前台时连钩子也卸了）再重发；重发的键要走完整条钩子链才进窗口，这期间打的 z 不再被截，直接走，夹进了重发的键中间。脚本自己的低级钩子记到的顺序是 `q 按下（Win顺 重发）· z 按下 · z 松开 · q 松开（Win顺 重发）`：q 的按下和松开被 z 隔开，框里出来是“zq”。启动器约 70–75 ms 拿到键盘，正好撞上 60 ms 后的第二个键，所以脚本里常见；手打得快时一样会撞上。不是工作线程排队：问文件夹只要 12 ms，跳转早就结束了。
- **做法**：持键一直持续到重发的最后一个键走完钩子链。`keyboardProc` 在 `CallNextHookEx` 返回后对 Win顺 自己的键调 `ownKeyPassed`，按发出的顺序逐个对上（`searchtyping::Retyped`，有单测；Win顺 别的键不算），对上最后一个时给钩子线程投一条消息（`kPassedMessage`）。消息到之前进来的键照样截下、排在后面，消息到了就把它们作为下一批发出去，没有了才结束持键（`heldPassed`）。投递的消息排在已经在等的钩子调用后面，不会漏掉键。重发的键要是被链上更前面的钩子吞了，1 秒后照样结束（`kPassTimeoutMs`）。不靠调时间。
- **怎么看到的**：只看框里的结果，分不出是哪一层乱的。测试脚本自己装一个 `WH_KEYBOARD_LL`，比 Win顺 的钩子装得早，所以在链上排在它后面；按 `dwExtraInfo == kOwnInput` 区分 Win顺 重发的键和脚本发的键，记下每个键经过的时刻和前台进程。

### 开始菜单打字：启动器“激活了”，前台却还是开始菜单，重发的键进了 Windows 搜索（GitHub #66）

- **现象**：用户报“打开开始菜单马上输入文字，顺序会错乱，baidu 可能变成 dbaiu”。2026-10-24 实测（本机 25H2，自己的 `--profile` 开 StartMenuTyping，提权脚本 `SendInput`：按 Win，抬起后 0–150 ms 开始打 `baidu`，键间隔 30–80 ms），“dbaiu”这种乱序一次也没出现：脚本自己的钩子记到 Win顺 重发的键每次都是 `b a i d u` 原样的顺序（开始菜单热着 35 次，其中 8 次按 Win 前在前台的是开着打字搜索的资源管理器；CPU 跑满 6 次；先结束 SearchHost 再按 Win 的冷启动 9 次）。复现出来的是另外三种丢字、错位：
  - 开始菜单还在打开时（冷启动，或热着但 `Esc` 来得太早），Win顺 的 `Esc` 没关掉它；启动器拿到前台几毫秒后开始菜单又抢回去（实测 8 ms），或者 Qt 已经算启动器激活了、前台其实还是 SearchHost。这时重发的键全进了 Windows 搜索（热着 12 次里 1 次全部进去；冷启动 9 次里 4 次有键进去）。用户看到的是 Windows 搜索里有字、Win顺 框里缺字或没字。
  - 冷启动时开始菜单要约 400 ms 才到前台，这之前打的字进了按 Win 之前的那个窗口（关掉 Win顺 的这个功能也一样，是 Windows 自己的行为）。
  - 抬起 Win 后 1 ms 内就按下第一个键：前台事件是异步的，钩子还没装上，这个键进了 Windows 搜索（d0=0 时 4 次里 3 次丢了 `b`）。手打几乎碰不到。
- **原因**：`deliver` 只看 Qt 说启动器激活了（`activeChanged`），重发前不再看前台是谁。开始菜单在打开动画里不理 `Esc`，`closeShellFlyout` 等满 500 ms 后硬切前台，开始菜单随后又把前台抢回去。
- **做法**：重发前在钩子线程上看 `GetForegroundWindow` 是不是 Win顺 自己的窗口（`TypeToSearch::waitForFront`，判断在 `searchtyping::delivery`，有单测）。不是就接着持键：每个前台事件和每 20 ms 再看一次；开始菜单又在前台时，离上次请求满 600 ms 就再请求一次启动器（它的 `Esc` 再关一次开始菜单），最多两次，这一回已经有键打进框里了就不再请求（再显示会全选框里的字，后面的键会把它们换掉）；都不行就等持键的 3 秒超时丢掉（不再打进 Windows 搜索）。还给资源管理器的键（`giveBack`）不受这一条限制。改后：热着 24 次（其中 4 次开着任务栏搜索框）里 20 次全对，另 4 次是 d0=0 丢第一个键（上面第三条），没有一次进 Windows 搜索；冷启动 19 次没有一个重发的键进 Windows 搜索；日志里看得到重发时前台是 SearchHost、再请求一次、约 250 ms 后启动器到前台才发出去。
- **没解决的**：“dbaiu”的乱序本机没复现，可能和用户那边的 Windows 版本（23H2 的开始菜单和搜索是两个进程）、第三方输入法或真键盘的按键重叠有关；问用户要 Windows 版本和输入法再查。冷启动时进了前一个窗口的字，要在按 Win 时就提前装钩子才接得住（`KeyListener` 的 Raw Input 看得到 Win 抬起），这次没做。
- **怎么查**：只看框里的字分不出键去了哪里。测试脚本自己的 `WH_KEYBOARD_LL` 记每个键的来源（`dwExtraInfo == kOwnInput` 是 Win顺 重发的）和当时的前台进程，再加 WinEvent 记前台变化；“前一个窗口”用一个**不提权**的测试窗口（提权的窗口在前台时，不提权的钩子根本收不到键，看起来像是 Win顺 漏键）。开着微软拼音中文模式时 `--dump` 的 `query` 是空的（字在组字里），要截图看。

### 从 PowerShell 7 用 `Start-Process` 起的 Windows PowerShell 5.1 找不到 `Get-FileHash`

- **现象**：隐藏窗口里跑的 5.1 验收脚本，收尾时报 `The term 'Get-FileHash' is not recognized`：用户那份 Win顺 已经重新启动了，最后一行现场检查（在不在跑、是哪个 exe、提没提权）没记下来，只好事后手查。
- **原因**：`Start-Process powershell.exe` 把 PowerShell 7 的 `PSModulePath`（`Documents\PowerShell\Modules`、`Program Files\PowerShell\Modules` 在前）原样传了下去，5.1 自动加载 `Microsoft.PowerShell.Utility` 里的脚本函数失败。`Add-Type`、`Get-Date` 这些内置的照常能用，所以前面都没事。直接在 7 里敲 `powershell.exe …` 时 7 会修好这个变量，没有这个问题（实测）。
- **做法**：脚本开头重设 `$env:PSModulePath`，或者不用 `Get-FileHash`（比文件用 `[Security.Cryptography.SHA256]` 或逐字节比）。收尾检查放进各自的 `try`，一处出错不要连带跳过现场还原。

### 给有两个标签页的资源管理器窗口发 `WM_CLOSE`，窗口还在

- **现象**：测试脚本收尾时 `PostMessage(WM_CLOSE)` 关测试窗口，三次都剩下一个窗口，里面只有第一个标签页（`F:\wstyping`）。
- **原因**：每次关掉的都是当时在前面的第二个标签页，窗口本身没关（Windows 11 带标签页的资源管理器）。
- **做法**：收尾时查一遍还剩哪些窗口的所有标签页都在测试目录下，再关一次，直到窗口没了；或者逐个标签页 `IWebBrowser2.Quit`。

### 后台打开的文件夹窗口，点到前台时不发 `EVENT_SYSTEM_FOREGROUND`

- **现象**：截图脚本刚打开的资源管理器窗口里打字，第一次不触发，第二次才行（场景里先打个 `z` 绕过去）。用户的正常打开方式（Win+R 输入文件夹、桌面上双击，窗口自己到前台）不复现：第一个键就截下（2026-10-24 实测）。
- **原因**：文件夹窗口由没有前台权限的程序打开（后台的脚本、工具；这里是提权脚本经桌面 `ShellExecute`）时，窗口出来了却留在后面，资源管理器这时发一个 `EVENT_SYSTEM_FOREGROUND`，事件里是它自己（`CabinetWClass`），`GetForegroundWindow()` 却还是原来的窗口（Chrome、计算器），所以没装钩子。之后无论是真鼠标点进窗口，还是 `AttachThreadInput` + `SetForegroundWindow`，窗口到了前台，**一个前台事件都没有**（测试脚本自己的 WinEvent 钩子同时记着，没有收到），钩子一直不装，第一、第二个键都给了资源管理器；脚本里“第二次才行”是别的窗口凑巧发了事件、或者先开关了一次启动器（前台换走又换回来）。用户也会碰上：别的程序在后台打开文件夹（任务栏按钮闪），点进去打字。
- **为什么没有事件**：这个窗口在它自己线程里已经是激活窗口（`GetGUIThreadInfo` 的 `hwndActive` 就是它，实测一直为真），只是它的线程不在前台；后来到前台时激活窗口没变，系统就不发前台事件。窗口正常被切走时线程的激活窗口会被清掉，切回来照常有事件，所以只有这种“后台打开”的窗口这样。
- **做法**：事件里的窗口是资源管理器文件夹窗口、前台却不是它时，记下它（`m_explorerBehind`），钩子照样装着；之后的前台事件（启动器开了又关、切到别的程序）里，只要它还是自己线程的激活窗口（`activeBehind`）就继续记着，真到了前台或者关掉才不再记；第一个能打出字的键再用 `GetForegroundWindow` 看前台（`searchtyping::typingWindow`，有单测），是它（或事件时在前台的那个）才截，别的窗口在前台就放过。这一条也挡住了反过来的情况：资源管理器在前台时，另一个同样“悄悄”到前台的窗口里打的字不会被当成资源管理器的。代价：这样的窗口留在后面期间，钩子装着，别处打的能打出字的键多一次 `GetForegroundWindow` 比较（微秒级）。只记一个开着的“后台”窗口（最近发事件的那个）。
- **怎么看到的**：测试脚本自己装 `SetWinEventHook(EVENT_SYSTEM_FOREGROUND)`，每个事件记下事件里的窗口和当时 `GetForegroundWindow()` 的窗口及其焦点链（`GetGUIThreadInfo`）；只看启动器有没有出来，分不出是钩子没装、焦点不对还是启动器慢。启动器等满 3.5 秒再判“没出来”：存键最多 3 秒，等短了会把“第一个键截下、启动器慢”误判成“第一次不触发、第二次才行”。
## 预览（Alt+P）

### 发布包里的 Qt 读不了 JPEG

- **现象**：预览照片，`QImage` 读出来是空的；开发机上从构建目录跑却正常。
- **原因**：`build.ps1` 部署时跳过了 Qt 的 `imageformats` 插件（省体积），发布包里的 Qt 只认内置的 PNG、BMP、PPM、XBM、XPM。
- **做法**：预览的图片一律用 WIC（Windows 图像组件）解码（`FilePreview.cpp` 的 `decodePicture`），Windows 认的格式都能读（装了扩展的 HEIC、WebP 也行），顺带按 EXIF 方向转正、解码时就缩小。别为了预览把 `imageformats` 加回来。

### 预览程序的窗口挂在 Qt 窗口下，一卡启动器的键也卡

- **现象**：（设计时按文档和实测确认）把 `IPreviewHandler` 的父窗口设成预览窗口（Qt 的，GUI 线程），或者让弹出窗口以预览窗口为所有者，预览程序一卡住，启动器的方向键、`Esc` 也没反应。
- **原因**：跨线程的父子窗口、所有者窗口会把两个线程的输入队列连在一起（`AttachThreadInput` 的效果），一个线程不取消息，另一个的键也排着。
- **做法**：预览程序画在一个专门 STA 线程上的弹出窗口里，这个窗口没有父窗口、没有所有者，只是摆在预览窗口内容区的位置上、z 序紧挨着它（`PreviewHost`）。GUI 线程对它只用异步的调用：`ShowWindowAsync`、`PostMessage`，不用 `SetWindowPos`、`SendMessage`。

### 没装 Outlook 时 .msg 的预览程序永远不返回

- **现象**：.msg 交给它的预览程序（MAPI，`{53BEDF0B-…}`），`CreateInstance` 一直不回来；超时后对它 `CoCancelCall` 能把调用拿回来，但再 `Release` 又卡住。
- **做法**：超时（10 秒）就把整个线程丢掉，不 `Release`、不等，下一个文件开新线程；这个预览程序这次运行里不再用（`PreviewHost::handlerFor` 返回空，显示缩略图和信息）。不去杀 `prevhost.exe`：它是别的程序（资源管理器的预览窗格）也在用的共享进程。

### Office 的预览程序只认文件路径；Edge 的 PDF 一调就返回

- **现象**：只试 `IInitializeWithStream` 时 Word、Excel、PowerPoint、Edge 的 PDF 预览程序都初始化失败；Edge 的 PDF `DoPreview` 2 毫秒就返回，这时窗口里还是空的。
- **做法**：依次试 Stream、File、Item，前一个失败就试下一个。显示弹出窗口的时机不是 `DoPreview` 返回，而是它下面出现了预览程序的子窗口（最多等 3 秒），之前预览窗口里写“正在读取…”。Word 第一次要启动自己（3–6 秒），类工厂留着（`LockServer`），一分钟没有预览才放。

### 带 `\\?\` 前缀的路径不能用正斜杠

- **现象**：`filetext::readHead(QDir::tempPath())` 在单元测试里报“打不开”，不是“文件夹”。
- **原因**：`readHead` 用 `win32::longPath` 加上 `\\?\` 前缀，有这个前缀时 Windows 不再把 `/` 换成 `\`；`QDir::tempPath()` 给的是 `C:/Users/...`。
- **做法**：交给 `win32::longPath` 的路径先 `QDir::toNativeSeparators`（预览里的路径本来就换过）。

### PowerShell 里 `[IO.File]` 用相对路径，写到了别处

- **现象**：在 `F:\wswt\wspreview` 下用 `[IO.File]::WriteAllText('src\…', …)` 改文件，什么也没改到，也不报错。
- **原因**：.NET 的相对路径按进程的当前目录算，不是 PowerShell 的 `$PWD`（`Set-Location` 不改进程的当前目录）。
- **做法**：`[IO.File]` 一律用绝对路径。

### Excel 的预览程序冷启动时抢前台，启动器跟着关了

- **现象**：预览 .xlsx，Excel 还没运行时，前台变成一个类名 `XLMAIN`、标题“Excel”的窗口，启动器失去焦点就关掉了，预览也跟着关。Word、Edge 的 PDF 没有这个问题。
- **原因**：Excel 以 `-Embedding` 启动时自己激活了一下窗口。这时预览程序的子窗口还没出来，按“子窗口所在进程”认不出它是预览的。
- **做法**：给出文件时按 CLSID 的 `LocalServer32`（没有就是 AppID 的 `DllSurrogate`）记下服务它的程序名，8 秒内前台换成这个程序的窗口算预览自己的，启动器不关、把前台拿回来（`PreviewHost::owns`）。

### 英文“非 Unicode 程序语言”的电脑上，GBK 的 .txt 成了乱码

- **现象**：预览 GBK 编码的 .txt，标题下写“ANSI 1252”，内容是 `ÕâÒ»ÐÐ` 这类乱码。单元测试的 GBK 一段因为 `legacyCodePage() == 936` 不成立被跳过了，没发现。
- **原因**：这台机器的系统区域是 en-US，`GetACP()` 是 1252；内容搜索和预览都按它解。
- **做法**：预览（`filetext::decodeHead`）在系统代码页不是中日韩双字节时，再试严格的 GBK（`MB_ERR_INVALID_CHARS`），ASCII 以外的字全是中文才认 GBK；西文（`café.`）过不了。单元测试在 1252 和 936 上都跑这一段。内容搜索没动。

### `Qt::UniqueConnection` 连 lambda 不生效

- **现象**：启动器挪动、展开成搜索面板时，预览窗口不跟着动；代码里明明连了 `QWindow::xChanged` 等信号。
- **原因**：`connect(…, lambda, Qt::UniqueConnection)` 只支持成员函数，连 lambda 时直接失败（只在调试输出里有一句警告）。
- **做法**：只连一次的地方不要加 `Qt::UniqueConnection`；真要防重复连接，就记下 `QMetaObject::Connection`。

### 预览程序的弹出窗口每次挪动都要重新排 z 序

- **现象**：预览窗口压到搜索面板上面时（`HWND_TOPMOST`），Word 的内容不见了，只剩空白的预览窗口；另一种情形里弹出窗口跑到了启动器上面。
- **原因**：弹出窗口只在开始时插到预览窗口上面，之后挪动用了 `SWP_NOZORDER`；预览窗口自己换到最前面后，弹出窗口就被压在下面。另外“预览窗口上面那个窗口就是弹出窗口自己”时误用了 `HWND_TOPMOST`，把它提到了所有窗口最前面。
- **做法**：每次挪动都按 `GetWindow(预览窗口, GW_HWNDPREV)` 重新插（已经紧挨着就 `SWP_NOZORDER`），预览窗口每次摆好后也让弹出窗口跟一次（`FilePreview::showWindow` 末尾的 `placeHandler`）。
## 网页搜索建议和 SVG 图标

### 百度的 JSONP 不是 JSON，而且是 GBK

- **现象**：`suggestion.baidu.com/su?wd=显卡` 回来 `window.baidu.sug({q:"显卡",p:false,s:["显卡性能天梯图",…]});`，去掉外壳后 `QJsonDocument::fromJson` 报错；按 UTF-8 解出来的是乱码。
- **原因**：键没加引号（`q:`、`s:`），是 JavaScript 对象字面量；Content-Type 是 `text/javascript; charset=gbk`。加 `&action=opensearch` 的那个也是 GBK。
- **做法**：`websuggest::parse` 先按严格 JSON，失败再放宽（给标识符键加引号、单引号字符串转双引号）；编码先看 charset。B 站的接口根本不带 Content-Type，按“是合法 UTF-8 就当 UTF-8，否则 GB18030”猜。

### Qt 不认 GBK

- **现象**：`QStringDecoder("GBK")` 无效（`isValid()` 是 false），解出来是空的。
- **原因**：这份 Qt 没带 ICU，`QStringConverter` 只有 UTF 系列和 Latin-1。
- **做法**：GBK / GB2312 / GB18030 用 `MultiByteToWideChar(54936)`（GB18030 是 GBK 的超集），Big5 用 950。

### WinHTTP 的超时是每一步各算的，不是整个请求

- **现象**：接口填死地址 `https://10.255.255.1/…`，`WinHttpSetTimeouts` 设了 1500 ms，请求约 4 s 才返回 12002；实测里迟到的失败（12029、12002）在 9 s 后才回来。
- **原因**：解析、连接、发送、接收各有各的超时，连接还会重试。
- **做法**：界面不靠 WinHTTP 的超时：`WebSuggester` 自己起 1.5 s 的计时器，到点就算没有；请求在自己的线程上，回来时代号不对只进缓存（成功的）或丢掉。不要在界面线程上等它。

### SVG 能引用外面的东西

- **现象**：iconfont 复制来的 SVG 无害，但 SVG 本身可以 `<image href="C:\…">`、`<image href="https://…">`、`<use href="别的文件#id">`、CSS `url(…)`、`@import`、`<!ENTITY … SYSTEM>`。
- **原因**：QtSvg 支持 `<image>`（会按路径读本地图片）和 `<use>`；粘贴来的、网上取来的 SVG 都不是用户写的。
- **做法**：画之前整段检查（`svgicon::render` 的 `External`），只放行 `#片段` 和 `data:image/png|jpeg|gif`，其余整段拒绝并说明，不悄悄删掉再画。不设 `QtSvg::AssumeTrustedSource`。脚本不用管：QtSvg 不执行，存的是画好的 PNG。

### 按路径删结果行会删错

- **现象**：建议从 5 条变 4 条时，按路径找建议行删掉，偶尔删掉的是刚原位改写的那一行。
- **原因**：原位改写是逐行 `update`，改到一半时新旧两行可能暂时是同一个路径（同一条建议从第 3 行挪到第 2 行）。
- **做法**：按位置改、按位置删（`ResultModel::removeRange`），从后往前。

### 测试：普通权限的脚本看不到提权进程的路径

- **现象**：会话脚本按 `ExecutablePath` 判断用户的 Win顺 有没有在跑，判成“没在跑”，没让它退出就启动了测试的那份；测试那份把参数转给已在跑的那份就退了，代理脚本报“staged copy not running”。
- **原因**：用户那份那时是被别的测试以管理员身份重启的，普通权限的 `Get-CimInstance` / `Get-Process` 拿不到提权进程的路径（空）。
- **做法**：路径为空的 `WinShun.exe` 当作用户那份，先 `--quit` 再启动自己的；恢复时用普通权限 `Start-Process`（这才是正常的运行方式）。

### 测试：`--settings --settings-at` 第一次打开时不跳到那一页

- **现象**：设置窗口没开着时 `--settings --settings-at 5,0` 打开的是第一页；窗口已开着时才跳。
- **原因**：`App` 只在 `m_settingsWindow` 已存在时调 `showPlace`。
- **做法**：测试里点侧栏进页面；表单在窗口外面时最大化设置窗口再点，别指望 Tab：设置表单的输入框不在 Tab 链里（从网址框按 Tab 跳到了侧栏和“查找设置”）。

## 优先显示的文件类型（论坛 #104、#116）

### 只靠加分，`curl` 还是文件夹和 curl.h 在前

- **现象**：论坛 #116（0.7.0 之后）：搜 `curl` 仍是文件夹 curl、curl.h 在 curl.exe 前面；给程序加个十来分也翻不过来。
- **原因**：三个都算“和输入完全一样”（`isExactName` 认不带扩展名的），`arrange` 把这一组提到最前、组内按分数排，不分文件和文件夹。分数差得多：文件夹整名 +60、覆盖 25 满分、文件夹 +2，共 117；curl.exe 不带扩展名同名只 +45，扩展名把覆盖分摊薄到 12，共 87；curl.h 91。真正的 curl.exe 多半在 System32 或 Program Files，再扣 35。加分要 30 多分才翻得过来，那就把“系统和程序文件夹靠后”抵消了。
- **做法**：在同名组里直接分先后（`exactFirst`：优先的类型在前，彼此按分数）；别的匹配只加 12 分（`PreferredTypes::kBonus`）；完全同名的优先类型把程序文件夹扣的 35 分还回来，保证它进得了前 N 名。查排序问题先算一遍各项的分数（`score()` 的各项、`staticBonus`、深度），再看 `arrange` 怎么分组，别只盯着加分。

### 先按“最多能加多少”问 `mayAccept` 反而更慢

- **现象**：想省掉每个匹配都查一次扩展名：先拿“分数 + 最多能加的分”问 `heap.mayAccept`，过了才查类型。单字母 `a` 从 12.4 ms 变成 14.4 ms，比每个都查还慢。
- **原因**：堆是每段 16384 项各一个（`kSliceSize`），段内的前 N 名门槛低；最多能加的分对程序文件夹里的文件是 47 分，放进来的多了很多，每个都要读深度、进堆。
- **做法**：类型照样每个匹配查一次，但查得便宜：`PreferredTypes` 是定长表，先看长度、首字节各一位（大小写两位都记，首字节不用折叠），再比两个 64 位的键；名字和扩展名长度用 `FileIndex::fileName(e, extLength)` 一次读出。单字母查询多约 1 ms，一般的词测不出差别。

### PowerShell 里 `[IO.File]` 的相对路径不是当前目录

- **现象**：在 `cd F:\wswt\<工作区>` 之后用 `[IO.File]::ReadAllText('tests/tst_core.cpp')` 读、改、写回，改的却是主工作区 `F:\电脑便捷工具` 里的同名文件（2026-10-16，`git status` 才发现）。
- **原因**：`cd`（`Set-Location`）只改 PowerShell 的位置；.NET 的相对路径按进程的当前目录（`[Environment]::CurrentDirectory`）解析，那是工具启动时的目录。
- **做法**：.NET 方法一律传绝对路径（或 `(Resolve-Path x).Path`）；改文件优先用编辑工具。改完看一眼 `git status`，主工作区和别人的工作区也看。

### 留空的类型列表存成 `@Invalid()`，重启后仍是空的

- **现象**：设置里把“优先显示的文件类型”清空，INI 里写的是 `PreferredTypes=@Invalid()`，看着像坏值，担心下次启动被当成没设、又回到默认的五种。
- **原因**：`QSettings` 把空的 `QStringList` 存成 `@Invalid()`；`readOrDefault` 只看 `contains(key)`，键在，就读出空列表，不回默认。
- **做法**：已实测（2026-10-16，清空后重启测试那份，`curl` 仍是文件夹在前）。要改成“空即默认”的读法时记得这一条会把用户关掉的功能又打开。

### 测试：`tail -f` 盯着的日志，提权的代理写不进去

- **现象**：会话脚本拿到锁、提权代理一启动就以退出码 1 结束，日志里一行它自己的都没有。
- **原因**：Git Bash 的 `tail -f` 开着日志文件，代理里 `Add-Content` 写同一个文件失败，`catch` 里再写日志又失败，异常一路抛出脚本。
- **做法**：代理写自己的日志（`[IO.File]::AppendAllText` 带重试），监视用循环按行数读新增的部分，不用 `tail -f`；结束写一个 done.flag。

## 长文件名：完整名字提示和调宽（论坛 #188）

### 自己缩短的名字，`Text.truncated` 是假的

- **现象**（写代码时看出来的）：结果行的名字明明显示成“2026年度华东…第二版.xlsx”，只按 `title.truncated` 判断“放不下才出提示”的话，这样的行永远没有提示。
- **原因**：名字是 C++ 按宽度缩好的（`elidedHtml`，带高亮的富文本），交给 `Text` 时已经放得下，`Text` 自己没截，`truncated` 当然是假。只有交给 `Text.elide` 的那部分（比如高亮的词本身就比一行宽）才会是真。
- **做法**：两个都看：`truncated`，或显示的字里“…”比原文多（原文自己带“…”的名字不会误判）。富文本先去掉标签再数（`cut()` 在三个行组件里各一份）。

### 列表滚不动时转滚轮，提示不走

- **现象**：结果只有几行、列表滚不动，鼠标停出提示后转滚轮，提示一直在（实测 2026-10-16：`dump` 前后都是 `visible=true`）。资源管理器的提示一转滚轮就没了。
- **原因**：只在列表的 `contentY` 变了时收起提示；滚不动就没有变化。
- **做法**：行的 `MouseArea` 加 `onWheel`：收起提示，再 `wheel.accepted = false` 让列表照常滚。`MouseArea` 只有连了 `onWheel` 才碰滚轮，不接受就往下传。

### 新加的测试程序，ctest 报 `0xc0000135`

- **现象**：`tst_elide` 直接运行（PATH 里有 Qt）全过，`./scripts/build.ps1` 里的 ctest 却是 `Exit code 0xc0000135`（找不到 DLL），一行输出都没有。
- **原因**：ctest 靠 `tests/CMakeLists.txt` 末尾的 `set_tests_properties(... ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:...")` 把 Qt 的 bin 放进 PATH；新测试没加进那张表。
- **做法**：加 `add_test` 时同时把名字加进那一行。

### 测试：杀掉测试副本后马上启动用户的副本，它立刻退出

- **现象**：会话脚本收尾时 `--quit` 测试副本 4 秒没退，`Stop-Process` 杀掉，接着 `Start-Process WinShun.exe --background` 启动用户的那份，日志里 `installed running again: False`，用户的 Win顺 一度没在运行（2026-10-16，手动补启动）。
- **原因**：`Stop-Process` 不等进程真正结束；新进程启动时单实例互斥量 `Local\WinShun.Instance` 还在，它当成“已经在运行”，带 `--background` 就直接返回 0。
- **做法**：杀掉后等测试副本的进程都没了再启动；启动后等几秒确认它还在，不在就再启动一次（最多三次），最后把结果写进日志。

### 按键收起提示后，鼠标没动，它又出来了

- **现象**：对话框旁的列表里，提示开着时按 ↓，当前行移到鼠标下的这一行，提示收起后 400 ms 又出来了，鼠标根本没动（实测 2026-10-16，`wins` 读到提示窗口还在，大小也变了）。
- **原因**：当前行显示“Ctrl+Enter 直接打开”（启动器里是一排按钮），鼠标下面的东西变了，Qt 会在原地补发悬停移动，`MouseArea` 照样发 `positionChanged`，行就又 `offer` 一次，等完悬停时间就出来了。滚动后行从鼠标下面滑过也一样。
- **做法**：每次 `offer` 带上鼠标的全局位置（`area.mapToGlobal(mouseX, mouseY)`），`NameTip` 记住最后一次的（`lastAt`）；`dismiss()` 时把它记成 `quietAt`，之后同一位置来的 `offer` 都不理，鼠标真动了才清掉。别用“dismiss 之后第一次 offer 的位置”当基准：打完字一下子把鼠标甩到某一行上，可能只来一次 offer，被吞掉后提示就不出了（也实测过）。

### 提示里很长的英文名字折成“…FINAL_v3.”和“docx”两行

- **现象**：对话框旁列表的提示，宽度按行的文字宽 + 160 定，`Quarterly_Financial_Report_…_FINAL_v3.docx` 这种没有空格的长名字先在“zqlong”后面的空格折一次，剩下的放不下又在任意处折，`.docx` 单独落在第三行。
- **原因**：`Text.Wrap` 是“能在词边界折就在词边界折，不行就任意处折”；名字里只有一个空格。
- **做法**：提示的最大宽度至少是名字一行的宽度（`TextMetrics`），只有比屏幕还宽时才折；文件夹按这个宽度折。

### 测试：量悬停时间别忘了 `--dump` 本身要 270 ms

- **现象**：鼠标移到一行、150 ms 后 `--dump`，提示已经是显示的，像是没等悬停时间。
- **原因**：一是 `--dump` 要起一个新进程、发消息、写文件，读到的状态比命令晚约 270 ms；二是鼠标一步步滑过去时先经过别的行，路上的行已经开始计时。
- **做法**：光标一步跳到目标行（`hover dx dy 1`），立刻 `--dump`（约 270 ms，应还没出），再等 400 ms `--dump`（应已出）；再一步跳到下一行立刻 `--dump`，量“换行立即出”。

### 提示里的中文文件夹名会在字中间折行

- **现象**：完整路径太长要折行时，“…二级目录也比较长的文件夹”和“名字”分在两行。
- **原因**：每个 `\` 后面插了零宽空格，想让它在文件夹之间折；但 Qt 对中日韩文字本来就是每个字之间都能折，`Text.Wrap` 只在“这一行放满”时找最近的折点，不会优先选零宽空格。
- **做法**：没改（要自己排每一行），读起来问题不大；英文文件夹名在空格或 `\` 处折。真要按文件夹折，用 `TextLayout` 自己按 `\` 分段排。

### 切换圆角只更新 QML，会漏掉原生窗口

- **现象**（代码路径检查）：预览处理器页由独立线程创建 HWND，任务栏搜索框是 Explorer 下面的分层子窗口；它们不在 `QGuiApplication::allWindows()` 中。
- **原因**：预览页自己设置 DWM 的 ROUNDSMALL，任务栏框用 Direct2D 绘制，二者都不会自动跟随 QML 的 Theme 绑定。
- **做法**：预览页与普通窗口共用 HWND 圆角登记，切换时枚举本进程已登记的顶层窗口；任务栏框通过现有 Look 消息传偏好并重画。直角时外扩阴影/光环的半径也必须保持 0，不能只把基础半径清零后又加上 out。

### 圆角改变后，任务栏框仍拿到旧偏好

- **现象**（调用顺序检查）：`applySettings()` 原先先调用 `updateSearchBoxLook()`，再调用 `applyAppearance()`；只在 Look 中读取全局圆角状态会晚一次。
- **原因**：此时 `SystemTheme` 尚未收到新设置，而且语言未改变时 `applyAppearance()` 会提前返回，不会走末尾的任务栏文字刷新。
- **做法**：在 `applyAppearance()` 写入新圆角偏好后立即刷新 Look，不能依赖主题或语言发生变化时的刷新。

### Windows 头文件把参数名 small 展开成 char

- **现象**：`setWindowCorners(HWND, bool small)` 在部分翻译单元编译报 C2632，“bool 后面的 char 非法”。
- **原因**：Windows RPC 头文件定义了 `small` 宏，是否触发取决于头文件包含顺序。
- **做法**：参数命名为 `smallCorners`，不使用这个宏名。

## 截图（阶段 0–2）

阶段 0 的数字在 `docs/design-capture.md` §6，实现见 architecture.md 的“截图”。

### 全屏遮罩差 1 个像素，冻结的图整张错位

- **现象**（阶段 0，150%，2560×1600）：遮罩窗口按 `QScreen::geometry()` 放，客户区是 2561×1601；图片按“物理 ÷ DPR”给大小时，全屏噪声图有 8956 个像素不对；让图填满窗口则 300 万个像素不对。
- **原因**：1707×1067 逻辑单位 × 1.5 不是整数，Qt 的缓冲区按 `round(logical × dpr)` 取整，真实缩放是 `round(logical×dpr)/logical`，不是 DPR。
- **做法**：原生 `SetWindowPos` 把窗口放到显示器的精确物理矩形；图和所有坐标按真实缓冲区缩放换算（`bufferScale`，`CaptureScreen::scaleX/scaleY`），图片 `smooth: false`。这样 0 像素差。

### 文字光标指示器盖在遮罩上，被当成窗口选中

- **现象**（阶段 0 设计时查到，阶段 1 未在开着指示器的机器上实测）：开了“文本光标指示器”时，它是一个置顶的顶层窗口，会出现在冻结的遮罩上面，也会在窗口列表里被当成一个光标大小的“窗口”框出来。
- **原因**：它是一个顶层可见窗口（类名 `CursorVisualClass`），在 Z 序最上面。
- **做法**：`WindowSnapshot` 跳过这个类名，以及 `WS_EX_TRANSPARENT` 和透明度为 0 的分层窗口。

### 改了 PrtScn 的注册表值不广播，按 PrtScn 整屏进了剪贴板

- **现象**（阶段 0 实测）：把 `PrintScreenKeyForSnippingEnabled` 设成 0 后注册 PrtScn，按下去收不到 `WM_HOTKEY`，截图工具也不出来，整个屏幕被复制进剪贴板（和 Windows 剪贴板历史）。
- **原因**：Windows 只在键盘设置变化的通知里重读这个值；没通知时它按“不给截图工具”处理，PrtScn 退回老行为。
- **做法**：每次写值后广播 `WM_SETTINGCHANGE`，lParam 为 `"Control Panel\Keyboard"`（`printscreen::write`）。接管期间 Win顺 没在运行时，PrtScn 仍是整屏复制，使用说明里写明。

### `RegisterHotKey` 成功不等于收得到这个键

- **现象**：值不存在（默认）时 `RegisterHotKey(VK_SNAPSHOT)` 返回成功，按 PrtScn 却弹出“选择应用”（OpenWith），收不到 `WM_HOTKEY`。
- **原因**：PrtScn 交给截图工具时，系统在热键之前就把它拿走了；注册本身不冲突。
- **做法**：测热键要真按一次看 `WM_HOTKEY` 到没到，不能只看返回值；PrtScn 先改值并广播再注册。

### `CreateForMonitor` 慢，每次截图都建会多 20 多 ms

- **现象**：WGC 每次新建 capture item 37.7 ms，其中 `CreateForMonitor` 22–24 ms；留着 item 只要 11.4 ms。
- **做法**：截图开着时 D3D 设备和每台显示器的 item 一直留着，显示器变化时重建；关掉截图才释放。

### D3D11 的 commit 在窗口关掉后不回落

- **现象**（阶段 0，贴图实验）：20 个 d3d11 窗口关掉后显存归零、工作集也能清掉，commit 仍比开始多约 163 MB；第二轮不再继续涨。
- **原因**：驱动和 D3D 运行时留着分配过的堆，不还给系统；只有进程退出才还。
- **做法**：遮罩窗口建一次、用完隐藏复用，不每次销毁重建；关掉截图时整个 `Capture` 销毁。量内存看 `PrivateWorkingSetSize` 和 commit 两个数。

### 带 DPR 的 QImage，`implicitWidth` 是物理像素数

- **现象**（阶段 0）：给 QImage `setDevicePixelRatio(1.5)` 后让 Item 用 implicit 大小，600×400 的图画成 600×400 逻辑单位（放大 1.5 倍），24 万个像素几乎全错。
- **做法**：图的 Item 大小显式给逻辑值（物理 × scale），不靠 implicit 大小；`CaptureImage` 根本不设 DPR。

### 25H2 开始菜单在前时，截图遮罩显示不出来，菜单也被拍进去

- **现象**（阶段 0）：开始菜单开着按热键，热键 3–4 ms 就到，但这时截的图里有开始菜单（44% 的屏幕）；自己的置顶窗口加 `SetForegroundWindow` 关不掉它（返回 0，1 秒后还在前台）。
- **原因**：开始菜单（`SearchHost.exe` 的 `Windows.UI.Core.CoreWindow`）占着前台，不让普通程序抢；它只认 Esc 或点别处。
- **做法**：热键到时如果前台是开始菜单或任务栏弹出面板（`win::isShellFlyout`），先截图（用户按热键时看到的就是要截的），再 `SendInput` 一个带 `kOwnInput` 标记的 Esc（不让“开始菜单打字”接管把它吃掉），每 10 ms 看一次前台，最多 300 ms 后再显示遮罩（实测约 200 ms 走开）。

### 设置窗口的副本把接管前的 PrtScn 值冲掉

- **现象**（代码检查）：打开接管时 App 把原值写进 `[Capture] PrintScreenBefore`，但设置窗口的 `SettingsEditor` 拿着自己的 `Settings` 副本，下一次改任何设置都会把空值 commit 回来；App 以为还没接管，再读一次值（这时已经是 0）当成原值，关掉后就恢复不回去了。
- **做法**：App 自己改了 `m_settings` 之后立刻 `m_settingsEditor->setSettings(m_settings)`（`App::applyCapture`）。以后 App 再有自己写回设置的值，也照这样同步。

### 测试程序链接 Qt6::Qml，ctest 构建失败在 `qml_imports`

- **现象**：`tst_capture` 只为 `QML_ELEMENT` 宏链接 `Qt6::Qml`，构建报 `FAILED: tests/.qt/qml_imports/tst_capture_build.cmake`。
- **原因**：`qt_add_executable` 链接了 Qml 就会扫描 QML import，测试程序没有 QML 文件，扫描失败。
- **做法**：给这类测试设 `QT_QML_MODULE_NO_IMPORT_SCAN TRUE`（和 `tst_numberhints` 一样）。

### QML 里读不到 QImage 的 width：放大镜从来不显示

- **现象**（阶段 1 实测）：放大镜 `visible: cursorHere && screenModel.image.width > 0`，截图时一次都没出现；按 C 复制颜色却正常（C++ 那边的状态都对）。
- **原因**：`QImage` 属性到了 QML 里是一个不透明的 variant，没有 `width`；`undefined > 0` 是 false，不报警告。
- **做法**：要在 QML 里判断图片有没有，就在 C++ 加一个 bool 属性（`CaptureScreen::hasImage`）；QImage 只直接交给 C++ 的 Item（`CaptureImage.image`）。实测脚本里加一项“放大镜那块的像素变了没有”，别只看最终结果。

### 自定义属性叫 left / top / right / bottom，遮罩窗口建不出来

- **现象**（阶段 1 实测）：开着截图启动时日志 “Screen capture: no overlay window: CaptureOverlay.qml:48:9: Cannot override FINAL property”，按热键什么都不出。编译、单元测试和离屏设置预览都过了，因为遮罩只在打开截图时才建。
- **原因**：`Item` 自己有 `left`、`top`、`right`、`bottom`（锚线，FINAL），子 Item 里 `readonly property real left` 算覆盖。
- **做法**：换名字（`edgeLeft` 等）。新 QML 文件只在某个开关打开时才加载的，部署后要把开关打开看一次日志；这个路径下 qmllint 打不开中文路径的文件，靠不住。

### 遮罩和冻结的屏幕在任务栏上差几百个像素

- **现象**（阶段 1 实测，150%）：遮罩显示时整屏截图和按热键前的截图比，除了画上去的框、标签、放大镜，只剩任务栏底部 1272..1762, 1587..1593 一条有 164 个像素不同，最大差 18；拿到剪贴板和保存的图与屏幕逐像素比 0 差。
- **原因**：WGC 拍到的任务栏（亚克力材质）和 BitBlt 读到的合成结果本来就有这点差（阶段 0：BitBlt 对 WGC 243 个像素，最大 16，同一块地方）；不是遮罩错位。
- **做法**：比对时把它当成已知差异，不要为它改缩放。

### 开始菜单开着时，悬停框落在菜单后面的窗口上

- **现象**（阶段 1 遗留，2026-10-19）：开着开始菜单按热键，冻结的图里有菜单，鼠标放在菜单上，框住的却是后面的窗口。
- **原因**：25H2 的开始菜单不在 `EnumWindows` 里，也不在 `GetWindow` 链里（另一个 Z 序带，任务栏这时也一起消失）；前台窗口是 SearchHost 的 CoreWindow“搜索”（矩形只有搜索那块），看得见的菜单属于 StartMenuExperienceHost 的“开始”，矩形是整个工作区；没有子窗口，UI 自动化只给里面的元素。
- **做法**：菜单关掉、前台变了以后重新截那台显示器，和冻结的图比出变了的区域（`changedRegion`，去掉只变暗的阴影行），当作最前面的窗口（`Capture::findFlyout`）；任务栏的矩形另外补进窗口列表。这样连手机连接面板一起框住。

### 开始菜单丢了焦点还在屏幕上：马上重截只差出一小块

- **现象**（阶段 2 实测）：前台一变就重截，比出的区域只有 34×100，悬停框还是落在后面的窗口上；拿事先存好的“开着 / 关着”两张图离线算是对的。
- **原因**：菜单丢了焦点以后还要播关闭动画，前台变化那一刻它大半还在屏幕上。
- **做法**：每 15 ms 再截一次，连续两次比出同一个矩形（且够大）才算，最多 500 ms；实测 6 次、约 260 ms 得到 1675×1299。代价是开始菜单开着时遮罩 500 ms 左右才出来（以前 260 ms 但框是错的）。离线用截好的图验证过的算法，还要在真实时序里再测一次。

### 分层窗口的像素从截图来，alpha 是 0

- **现象**（阶段 2 写贴图时查到）：`UpdateLayeredWindow` 按每像素 alpha 合成，BitBlt 读来的 32 位像素 alpha 可能是 0，整张贴图就是透明的。
- **做法**：拷进 DIB 时把 alpha 一律设成 0xFF（`copyOpaque`），透明度只用 `SourceConstantAlpha`。

### `tst_service` 被跳过时删掉了当前目录

- **现象**：不提权跑单元测试，构建目录 `build/capture` 整个空了两次。
- **原因**：没提权时 `initTestCase` 直接 QSKIP，`m_scratch` 还是空串，`cleanupTestCase` 里 `QDir("").removeRecursively()` 删的是当前目录，也就是测试程序所在的构建目录。
- **做法**：只在 `m_scratch` 非空时删。跑测试时当前目录放在构建目录以外。

### 测试脚本：托盘菜单不认发来的方向键，通知盖住菜单

- **现象**（阶段 2 实测）：右键托盘图标弹出菜单，`SendInput` 发方向键和回车没有反应（贴图的右键菜单却可以）；另存为之后的系统通知正好盖在托盘菜单下半截上，点击的守卫查到是 ShellExperienceHost。
- **做法**：测试里用 `MN_GETHMENU` + `GetMenuString` + `GetMenuItemRect` 按文字找菜单项、用鼠标点；托盘那一步放在会弹通知的保存之前。托盘菜单键盘不响应的原因还没查（不是这次加的）。

### 混合 DPI 下的贴图（未实测）

- **约定**：贴图保持物理大小（1:1 是这个功能的意义），`WM_DPICHANGED` 只按新 DPI 重算边框和阴影，返回 0、不用系统建议的矩形（那个矩形会按 DPI 比例缩放窗口）。作者只有一台 150% 的显示器，拖到另一个 DPI 的显示器上的情况还没测过。

### lupdate 把 QML 字符串里的反斜杠吃掉

- **现象**（阶段 3 跑 `update_translations`）：`CaptureCard.qml` 里 `qsTr("……“图片\\屏幕截图”……")` 进 .ts 后成了“图片屏幕截图”，原来的译文被当成过时删掉，新条目没有译文；运行时 `qsTr` 查的是带反斜杠的原文，英文界面就显示中文。
- **做法**：界面文字里不写反斜杠：这句改成“图片 › 屏幕截图”（英文 Pictures › Screenshots）。跑完 lupdate 和跑之前的 .ts 比一下被删掉的 `<source>`，别的这种情况也能看出来。

### MCP 参数：`Args::integer` 超出范围是夹住，不是报错

- **现象**：测试里给 `take_screenshot` 传 `monitor: 99`，以为会直接报错，结果被夹成 16 去问了用户。
- **做法**：范围外的数当成边界值是这一层的约定（和 `limit` 一样）；真正“没有这台显示器”由 App 那边回错误。测“参数不对”要传类型不对的值（字符串）。

### `ToolHost::cancel` 的 id 是 host 自己的，不是 JSON-RPC 的

- **现象**：测试里 `host->cancel(6)` 取消不掉 JSON-RPC id 为 6 的调用，`pendingCalls()` 还是 1。
- **原因**：`McpServer` 给每个调用另发一个 host 的 id，`notifications/cancelled` 由它换算。
- **做法**：要取消就像客户端那样经服务器发 `notifications/cancelled`。

### SQLite：绑定空 `QString()` 是 NULL

- **现象**：测试里往 `text TEXT NOT NULL` 插 `QString()`，`exec` 失败（NOT NULL constraint failed）。
- **做法**：要空串就绑 `u""_s`（非 null 的空串）；`QString()` 绑进去是 SQL 的 NULL。

### 窗口按创建时的高度居中，换行的文字之后才把它撑高
- **现象**：AI 截屏的确认窗口（`ScreenshotConfirm.qml`，高度绑在几段 `wrapMode: Text.Wrap` 的文字上）`create()` 后按 `height()` 放到工作区正中，实测整体偏下 32 像素：显示时又长高了 65 像素，只往下长。
- **原因**：换行文字的高度要等第一次排版才定，`createWithInitialProperties` 返回时绑定算出来的还是不换行的高度。
- **做法**：居中的函数在 `QWindow::heightChanged` 上再调一次（`App::confirmScreenshot`）。别的按内容定高、又要居中的窗口同理；测时读窗口矩形和工作区中心比。

### 搜 “vscode” 找不到 Visual Studio Code
- **现象**：全部范围里输 `vscode`，应用里没有 Visual Studio Code，Windows 任务栏搜索却把它排第一（2026-10-19）。
- **原因**：`NameMatcher` 只做子串（汉字另有拼音），首字母匹配只认整个查询是名字各词首字母的前缀（`vsc`）。`vscode` 是“首字母 + 整词”混写（v|s|code），两条路都够不着，分数 −1 被丢掉。
- **做法**：`searchApps` 里加“词首连缀”匹配（`wordSpanScore`）：名字按空格 / 标点 / 字母数字交界 / camelCase 切词（`AppInfo::words`），查询要能切成连续几个词的非空前缀，不跳词、至少两个词、至少 3 个字符（`vcode` 跳过 Studio 不算）；多个纯 ASCII 词（`vs code`）拼起来再试。分数 30，从第一个词起 +6，最后一段是整词 +6。不放进 `NameMatcher::score`：那是文件搜索对几百万个名字的热路径，改了会动文件的排序和速度。

### 搜“微信”“ps”“cmd”，要的应用不在第一
- **现象**（本机实测，2026-10-19）：`wx`、`weixin`、`微信` 第一行是微信输入法，微信本身在第 6 行（16 分）；`ps` 没有 Photoshop，`ppt` 没有 PowerPoint；`cmd` 前 7 行全是 VS 开发者命令提示符、Qt 和 Node.js 的命令行（都是 109 分），命令提示符排不上来；`python` 把 Module Docs 和 Python 本身并列 109。
- **原因**：
  - 应用只按开始菜单里的名字和程序文件名找，快捷方式的名字却是安装包或用户起的：这台电脑的微信是便携版，快捷方式叫“启动微信”，只有“微信”两个字落在名字中间。企业微信的 WXWork.exe 里倒有“wx”。
  - `ps`、`ppt` 这种简写既不是名字的一段，也不是各词的首字母。
  - 开发者命令提示符是 `cmd.exe /k vcvars64.bat`，程序文件名是 cmd，可它不是命令提示符；mmc、rundll32、explorer、chrome_proxy 的快捷方式也一样，程序文件名说的是宿主，不是它打开的东西。
- **做法**（`src/core/apps.txt`，`AppAliases`，`AppInfo::prepare`）：
  - 别名表按程序文件名认应用，不按显示的名字：`program = WeChat; Weixin` 认出“启动微信”。程序名要整个相同，所以 WXWork 不算。几个程序同名时写路径结尾：Chromium 系浏览器大多叫 chrome.exe，Chrome 写 `Google\Chrome\Application\chrome`，否则 Helium 也会被当成 Chrome。商店应用写 AppUserModelID 或包系列名；显示的名字（`title`）只在都用不了时写。Update、launcher 这种许多程序共用的名字不写。
  - 别名（`aliases`）和名字一样匹配，拼音自动可用（写“微信”就有 wx、weixin，用不着写拼音）；完整输入算 115，和名字完全相同一样，显示的仍是开始菜单里的名字。
  - 简写（`short`）只认完整输入，单独一档 112 分：低于名字完全相同（115），高于程序文件名（109）和 Windows PowerShell 的首字母 `wps`（45）。只认完整的，是因为“p”“s”“a”出现在太多名字里，按前缀算的话每次输一个字母都会带出 Adobe 全家。简写别和常用的拼音撞：“an”在本机对上 76 个设置（安装、安全、按……），Animate 就没写；“ai”只对上 7 个，多是“AI 组件”这类，Illustrator 照写。完整输入的另一种语言的名字、别名和简写都算完全命中（`AppHit::exact`，插件的结果排在它后面），以前只有开始菜单里的名字算。
  - 快捷方式的参数从 `PKEY_Link_Arguments` 读，AppsFolder 的属性里本来就有，不用另开 .lnk，读应用的时间测不出差别（本机 328 个应用约 0.3 秒）。只管启动的宿主（rundll32、mmc、explorer、mshta、chrome_proxy……）有参数就算经宿主运行别的东西（`AppInfo::hosted`）；cmd、powershell、pwsh、python、java 这些 shell 和解释器自己也常被直接打开，参数里有运行开关（`/k`、`-Command`、`-m`、`-jar`……）、脚本或路径才算。PowerShell 7 的快捷方式是 `pwsh -WorkingDirectory ~`，按“有参数就算”会把它也弄丢。算了的不按宿主的程序名匹配，也不拿它查别名表；不带参数的 cmd、explorer 照旧按程序名找到。
  - 别名表里的段对没对上，用 `wsbench --apps` 在真机上看，单元测试只能证明规则。本机没有 WPS、飞书、Illustrator 等，这些段还没在真机上对过。

### 搜“qq”，设置里冒出“登录前启动放大镜”（查过，没改）
- **现象**：`qq` 除了 QQ，还对上 11 个设置，**全部** 里列出更改设备安装设置、登录前启动放大镜、登录前启动讲述人、查看上周哪些应用请求拨打电话……；`word` 除了 Word，还有管理网络密码、创建密码重置盘、管理 Web 凭据（30 分）。
- **原因**：
  - 两个字母碰巧是名字中间两个字的首字母：前启、请求、取器。
  - 更改设备安装设置的关键词里有“气球”，整个关键词的首字母是 qq。
  - 管理网络密码那 3 个的关键词表里本来就有“word”这个词（还有“pass-word”）；不是从“password”中间找到的，关键词里的词本来就要从词首算起。
- **试过、不行的**（676 个两字母查询，`wsbench --places`，前后对比）：
  - 两个字母的拼音只在名字开头或词首才算：命中从 17636 降到 11832，33 个查询变空，前 5 名里 127 个 13 分以上的结果没了，其中有不少真词：dn → 此电脑，gn → 可选功能、程序和功能，xg → 视觉效果，sq → 更改时区，mb → 控制面板，ly → 启用蓝牙。汉字挨着汉字不算词首，中文名字里的“词首”就只剩第一个字，从第二个词起的真词也跟着没了。现有测试 `wk` → 当我看向别处时（我看）是有意留下的同一类命中。
  - 再让两个字母不认 Windows 关键词的首字母（places.txt 的照认）：降到 8684，63 个查询变空，还丢了 mm → 更改密码（靠关键词“密码”）、ly → 飞行模式设置（蓝牙）。
- **做法**：匹配不动。要压这类杂讯，得在排序或合并上想办法，比如应用完全命中时把设置排在后面。

## 截图标注（阶段 4a，2026-10-21）

### 大于 64 像素的字，换了一个字号画过之后轮廓会偏
- **现象**：同一个字形，先画过一个带小数偏移的小字，再画大字，轮廓整体偏 1/64 像素；瓦片拼起来和整张画差 1 级灰阶（单测“瓦片 == 整张”随种子时过时不过）。
- **原因**：Qt 的 FreeType 引擎让各字号共用一个 `FT_Face`，小字从字形缓存画出来时，最后一个亚像素偏移留在这个 face 上不复位；大于 64 像素的字走轮廓，加载时带着这个偏移。
- **做法**：大字的每段字形用字体的新副本（`QRawFont::setPixelSize(px + 1/4096)`，26.6 下尺寸不变），新引擎创建时偏移归零。回归测试 `tilesTextHistory`。

### GDI、DirectWrite 往 ARGB32 / RGB32 的 QImage 上画字有彩边
- **现象**：软件渲染模式（用 GDI）或 DirectWrite 下，标注文字有 ClearType 的彩色边；`QFont::NoSubpixelAntialias` 不起作用。
- **原因**：Qt 按目标图像格式挑字形格式，目标是 ARGB32 / RGB32 且系统开着 ClearType 就是彩色抗锯齿。
- **做法**：文字和序号先画进 RGBA8888 的对象层（灰度抗锯齿），再转成预乘的 ARGB32，瓦片在整像素位置贴上。

### moc 看到原始字符串里的 `"#`，丢掉整个 Q_OBJECT 类
- **现象**：`R"({"color":"#1E88E5"})"` 这样的原始字符串所在的文件，moc 报 “No relevant classes found”，链接时 LNK2001 metaObject。
- **做法**：这类 JSON 样例放进 `tests/data/`，或不用原始字符串。

### Qt Quick 的双击：第二下先是按下，再是 doubleClicked
- **现象**：双击序号想改号，结果截图被复制、关掉了。
- **原因**：`MouseArea` 的第二下先发 `pressed`（标注手势已经开始），再发 `doubleClicked`；会话在 `Annotating` 阶段不认双击，落到“选区里双击 = 复制”。
- **做法**：`CaptureSession::doubleClick` 先把进行中的手势取消，再问模型；开着工具或点在任何对象上，双击都不复制。单测 `doubleClickOnAnObjectNeverCopies`。

### 输入法拼音时的 Esc
- **做法**：遮罩的根按键 Item 不接受输入法，所以工具键直接到会话；只有文字框（`TextEdit`）接受输入法。拼音中按 `Esc` 由输入法自己取消拼音串（到不了我们），文字框留着；再按一次才提交文字。测试里 `Esc` 要按两次，不是一次。

### 贴图窗口收键盘时，输入法的候选窗会冒出来
- **现象**：输入法在中文状态时对着贴图按字母键，屏幕左上角冒出候选窗（这台电脑是微信输入法：`wetype_candidate` 窗口 “1 啊 2 阿 3 哎…”，外加本进程的组字窗 `CiceroUIWndFrame`）。
- **原因**：贴图是自己建的原生窗口（`WinShun.Pin`），用的是线程默认的输入上下文。Qt 的窗口由 Qt 按焦点开关输入法（焦点不收文字时 `ImmAssociateContext(hwnd, nullptr)`），原生窗口没人管。贴图没有光标位置，候选窗就落在左上角。
- **做法**：建好贴图窗口就 `ImmAssociateContextEx(hwnd, nullptr, 0)`（`PinWindow.cpp`）。遮罩是 Qt 窗口，文字工具的输入法不受影响。2026-10-21 验证（测法见下一条）：修前把贴图的上下文切到中文，按 a 出候选窗；修后同样去切，读回什么也没变（贴图没有输入上下文），按 a、b 没有新窗口，一次 `Esc` 就关掉贴图；遮罩的文字框里按 a 照样出候选窗。

### 测试：输入法是英文状态时，候选窗测不出来
- **现象**：第一次复现上一条，按了 a 什么也没出来，差点得出“没这个问题”。
- **原因**：用户当时的输入法是英文状态（贴图上下文的打开状态是 0）。输入法关着，哪个窗口都不会出候选窗。
- **做法**：测试脚本给目标线程的默认 IME 窗口（`ImmGetDefaultIMEWnd`）发 `WM_IME_CONTROL`：先用 `IMC_GETOPENSTATUS`、`IMC_GETCONVERSIONMODE` 记下状态，再 `IMC_SETOPENSTATUS` 设 1、转换模式加上 `IME_CMODE_NATIVE`，读回确认变了再按键。测完趁目标还有焦点放回（有组字时先按一次 `Esc` 结束组字，再放回，再 `Esc` 关窗口）。读回“没变”本身就说明那个窗口没有输入上下文。

### 测试：把贴图当成遮罩，拖走了它
- **现象**：测试脚本用“前台窗口属于 Win顺 且标题里有 Win”判断遮罩开着，实际前台是上一次留下的贴图（标题“Win顺 贴图”），后面的拖选把贴图拖到屏幕另一头，冻结的屏幕里还带着贴图，下一次“没有标注”的截图和上一张逐像素一样（同像素去重，条目只移到最前）。
- **做法**：按窗口类（`Qt…`）认遮罩；每次测试前先关掉所有贴图（点一下它再 `Esc`）；`WindowFromPoint` 的守卫只管进程，管不了这种情形，要查类名。

### 测试：QtTest 的输出在 PowerShell 里抓不到
- **做法**：`tst_xxx.exe -o 文件,txt`，看文件里的 `Totals`。

### 测试：`while ($h = [T]::FindWindowEx(...))` 永远不结束
- **原因**：`[IntPtr]::Zero` 是个对象，PowerShell 里是真值。
- **做法**：显式和 `[IntPtr]::Zero` 比，并给循环加次数上限；托盘菜单开着时脚本卡住，菜单会一直开在屏幕上。

### 测试：重启后的前两次热键，遮罩出来了却没拿到前台
- **现象**：阶段 4a 测试时，刚换了 exe 重启，脚本发的前两次热键，日志里 “overlay up” 了，前台却还是别的程序；第三次起都正常。
- **原因**：复现不出来，没找到程序上的原因，证据指向当时的测试脚本。2026-10-21 重启 6 次（5 次 `--quit` 后，其中 4 次前一个进程退出时崩了，见下面“每次退出都崩溃”；1 次强杀后），每次重启后最先的 1–4 次热键，共 16 次（人的节奏和一次发完的 6 个事件；前台是测试窗、Edge、VS Code；重启后 6–40 秒），遮罩次次拿到前台：一次发完时按下后 47–73 ms，按人的节奏时从第一个键按下算 223–230 ms（组合键本身要按约 140 ms）；`GetGUIThreadInfo` 的 active 和 focus 都是遮罩，方向键立刻生效，`bringToFront` 一次也没报 “Could not bring the window to the front”。16 次里 12 次跑的是 15:24 的 `build\release` 版本，4 次是 16:10、16:27 的 `build\capture` 版本；最早出问题的 13:36 版本没有再测。当时的脚本在用户自己的 VS Code 在前台时从 PowerShell 一次发出整个热键、发前不看前台，前台不对还会“再按一次”（热键是开关，再按就是取消），多半是脚本自己造成的。
- **做法**：显示遮罩照启动器、剪贴板的办法（`win::bringToFront` 再 `requestActivate`），不改。加了一条警告：遮罩第一帧画出来时不在前台，日志写 “Screen capture: overlay up, but <类名> (<程序>) is in front”，真遇到时知道是谁拿着前台（修后打开 8 次，一次也没出现）。测试脚本发热键前确认前台是自己的测试窗，按人的节奏发；先试 `RegisterHotKey`，能注册成功说明 Win顺 没占着这个键，就不要发（键会落到前台窗口）。

### 测试：拖动时的帧间隔，量的是输入的节奏
- **现象**：56 个对象（含 5 个马赛克）时拖一个大矩形，日志 “gesture of … frames” 的 p95 在 17.6–32 ms 之间跳，像掉帧。
- **原因**：`Capture::frameDrawn` 量的是手势期间两帧之间的时间，而帧是鼠标移动引起的：没有新的移动就没有新帧。旧脚本每步 `Sleep`，受系统计时器精度限制约 15.6 ms 一步、偶尔 31 ms，120 次移动正好 119 帧，量到的就是输入的间隔。2026-10-21 同一时刻对比（240 Hz 屏，D3D11，threaded 渲染循环，每个窗口一条 `QSGRenderThread`，两轮）：
  - 每步 15.6 ms：我们 p50 15.6–15.7、p95 17.1–17.5 ms；空的 QQuickWindow 15.6、17.6–17.8 ms，一样。
  - 每步 2 ms（忙等）：我们 p50 4.1–4.2（一次刷新）、p95 7.8–7.9、最大 8.7–9.1 ms；空窗口 4.2、4.5–4.9、28.6–31.2 ms。p95 多出的一次刷新是我们的，离 16.7 ms 还远。
  - 同一手势的 “Annotation canvas” 一行：每帧画 12–13 块瓦片（最多 24），界面线程上画图 p50 0.7–0.9、p95 1.2、最大 2.2 ms；同步 p95 0.04 ms；马赛克一次也没重算（缓存键没变）。
- **做法**：量流畅度要用比刷新快的输入，同一时刻跑空窗口基线；看 “Annotation canvas” 一行分清是界面线程（画瓦片）还是别处。还没量的是渲染线程：每块重画的瓦片每帧都建新节点、新纹理再上传，p95 多出的那次刷新可能在这里。

### 每次重画瓦片都先整块复制一遍
- **现象**：每步 2 ms 拖大矩形，3637 次重画瓦片里 3601 次先把整块图（256×256，256 KB）复制了一遍。
- **原因**：上一帧用这块图建的纹理（`createTextureFromImage`）还拿着它（QImage 共享）。往里画时 QImage 先分离（深拷贝），而合成器的 `copyBase` 随后把每个像素重写一遍，拷贝白做。
- **做法**：画之前图还被共享（`!isDetached()`）就先放掉，让合成器新分配一张（`AnnotationCanvas::updatePolish`）。画图时间没有看得出的变化（p50 0.72 → 0.87 ms、p95 1.17 → 1.17 ms，各量一次，在噪声以内），帧间隔也没变：这份拷贝本来就不贵。

### 测试：重启 Win顺 时剪贴板历史按默认的 300 条修剪，调大 MaxItems 挡不住
- **现象**：测试前把 `[Clipboard] MaxItems` 调到 310，免得测试新增的条目挤掉用户最旧的条目；几次重启后，最旧的 5 条还是没了，图片文件也删了。
- **原因**：`ClipStore::open` 载入后立刻用默认限制（300 条、30 天）修剪一次，用户的设置要等后面 `App::applyClipboard()` 才 `setLimits`（2ee7c71 起，已发布的版本都这样）。所以每次启动都按 300 条、30 天删，和设置无关：设了“不限”、多于 300 条或多于 30 天的用户，每次启动都会丢掉超出的部分。
- **做法**：已改（2026-10-21）：`App.cpp` 里 `setLimits` 挪到 `open` 之前（没打开时只记下限制，不修剪），`tst_clipboard` 的 `limitsBeforeOpenKeepTheHistory` 盯着这一点（400 条加 3 条旧的：`{0,0}` 全留，`{0,30}` 只去旧的，默认 300 条）。测试里尽量不重启；备份历史时把 `clipboard\images`、`clipboard\projects` 一起备份。2026-10-21 这次被挤掉的两条从备份库放回，其中一张图片是从 C 盘的卷影副本（`Win32_ShadowCopy`，提权只读）找回的。

### 测试：用户在用电脑时，测试窗抢不到前台
- **现象**：脚本把自己的测试窗切到前台失败而中止，前台是 Windows 搜索，或另一个程序“未响应”的幽灵窗口（`Ghost`，dwm.exe）。
- **做法**：开测前用 `GetLastInputInfo` 等到 10 秒没人碰键鼠，每步之间再看一次；前台不是普通窗口就不测。脚本中止时不发任何键，这是对的。

### 测试：PowerShell 的变量名不分大小写
- **现象**：脚本的参数 `[string]$Steps`，又写了 `$steps = $Steps -split ','`，结果一步都没跑；`"no $Backup: …"` 直接是语法错误。
- **原因**：`$steps` 和 `$Steps` 是同一个变量，带类型的参数把数组又转回了字符串；`$名字:` 被当成带驱动器名的变量。
- **做法**：局部变量另起名字；字符串里变量后面紧跟冒号时写 `${名字}`。改完的脚本先用 `[Parser]::ParseFile` 检查一遍。

### 每次退出都崩溃：build\release 里的 main.cpp 没跟着头文件重编
- **现象**：2026-10-21 14:27–16:20 在 dist 里跑的两个版本（14:03、15:24 两次构建，都出自 `build\release`），7 次退出全部崩溃，`%LOCALAPPDATA%\CrashDumps\WinShun.exe.*.dmp`，0xc0000409、参数 2（栈 cookie 被改写）。`build\capture` 出的版本（13:36、16:10、16:27）退出都正常。
- **原因**：崩在主线程 `main.cpp` 的 `restart = application.restartArguments();`。这是 `App.h` 里的内联函数，代码编在 main.cpp 里；它去复制 `m_restartArguments` 的地方，放的其实是前一个成员 `m_translator`（读到 QTranslator 的虚表），写地址 2 访问冲突，分派异常时又查出 main 的栈 cookie 被改写。main.cpp.obj 和 App.cpp.obj 对 `App` 的布局差 24 字节：`build\release\.ninja_deps` 里 main.cpp.obj 的头文件列表是空的（10-21 09:05 那次编译记下的），之后 `Settings.h` 改过几次（App 里有 `Settings m_settings`），ninja 也不重编 main.cpp；App.cpp.obj 14:03 重编过。main 给栈上的 `App` 留的地方小了 24 字节，App 自己的代码写过了界；退出时 `App` 的析构（存索引等）也没跑到。09:05 那次编译的 26 个源文件（main、Clipboard、ClipStore、Query、Backup、Migration、PluginSettings、Preview……）头文件列表都是空的；`build\capture` 的 main.cpp.obj 记着 262 个。
- **做法**：发版（`release.ps1` → `build.ps1` → `build\release`）前清掉重编（`cmake --build --preset release --clean-first`，或删掉 `build\release`），装好后退出一次，`CrashDumps` 里不该有新的 dump。列表为空多半是那次编译时 cl 的 `/showIncludes` 输出和配置时记进 `rules.ninja` 的前缀（`msvc_deps_prefix = 注意: 包含文件:`）对不上，比如那个 shell 设了 `VSLANG=1033`（英文是 “Note: including file:”）或代码页不同；共用的构建目录，编译时的语言环境要和配置时一样（是哪次、什么环境没查到）。查法：读 `.ninja_deps`（v4），看各 `.cpp.obj` 记下的输入是不是 0 个；只含系统头文件的（如 `src/stub/main.cpp`）本来就是 0。

### 缩放和 HDR 下的截图实测（2026-10-21，2560×1600，100/125/150/175% 和 150% + HDR）
- **结果**：五种情况下，遮罩冻结的图和冻结前的屏幕逐像素 0 差，选区导出的 PNG、F3 贴图都和屏幕上的 0 差，遮罩窗口矩形都是 0,0,2560,1600，选区尺寸都是“拖动距离 + 1”（1200×700 拖出 1201×701），放大镜格子 100/125/150/175% 是 120/150/180/210 像素（整块含边框和文字 122×197、151×245、183×294、213×343），工具条高 42/53/62/73 像素、右对齐在选区下面。HDR（`active_mode=HDR`，日志 “1 by WGC”，没有退到 BitBlt）下纯白（#FFFFFF）区域导出后是 255,255,255，字的最深像素是 0，和 SDR 逐像素相同；Win顺 自己的识别（PaddleOcr）读出 “WinShun HDR OCR test 2026 / Pure white background, black text”，Windows 自带识别把 HDR 读成 “I-IDR”（SDR 下也一样，不是 HDR 的问题）。没测：SDR 白电平不在 80 尼特时（`Hdr.cpp` 按它除）。
- **改缩放、开 HDR 的做法**：`DisplayConfigSetDeviceInfo`，类型 -4（设缩放）、-3（取）：值是相对“推荐档”的下标，档位 100,125,150,175,200,225,250,300…；HDR 用类型 10（`SET_ADVANCED_COLOR_STATE`）就够，开关后用类型 15（`ADVANCED_COLOR_INFO_2` 的 `activeColorMode`）确认真是 HDR，类型 9 的 `advancedColorEnabled` 在 WCG 下也为真。改缩放后等 `GetDpiForMonitor` 读到新值再多等 3 秒。测完要在外层 `finally` 里改回，因为脚本被杀时里面的不会跑。
- **现象**：175% 那一轮冻结图、导出图、贴图里多出 4 个紫红色的小块（28×62 像素，每处 867 个像素不同），和屏幕上的位置对不上。
- **原因**：Windows 的“文本光标指示器”（`CursorVisualClass`，分层窗口）。用户开着它；WGC 的帧里有它，`CopyFromScreen`（不带 `CAPTUREBLT`）抓不到分层窗口，所以逐像素比时出现。位置跟着别的程序（VS Code）的光标走，每次不一样。不是 Win顺 的问题：冻结的图本来就该和用户按热键时看到的一样。
- **做法**：比对前后各记一次它的窗口矩形，比较时把矩形外扩 70 像素排除（`matrix.ps1` 的 `Ind`、`cmp.py`）。
- **测试脚本的坑**：用户在用电脑时 VS Code 会抢走前台，发热键前要先 `AttachThreadInput` + `SetForegroundWindow` 把测试窗拉回来再确认前台，拉不回就不发；`FindWindow(null, 标题)` 在 PowerShell 里要传 `[NullString]::Value`，传 `$null` 变成空串找不到。导出的 PNG 比选区小一格会错位，所以先看尺寸，再按“拖动距离 + 1”去屏幕上截同样大小的块。
- **未解释**：125% 那轮第一次热键到遮罩出来 689 毫秒（其余各次 36–85 毫秒），只有这一次；前一刻刚改了缩放，日志里没有警告。

### 编辑窗口（阶段 4b，2026-10-21）

#### 软件渲染的单测里，移动了的画布不重画
- **现象**：`tst_annotmodel` 里画布放在 20000 像素高的 Item 上，`setY(-10000)` 再换视口，`grabWindow()` 抓到的还是旧画面（背景色），可 `tilesShown()` 是对的。
- **原因**：软件场景图只重画它认为变了的区域，新建的瓦片节点挂在一个刚挪过的父节点下，脏区算在旧位置上。真窗口（D3D）没有这个问题。
- **做法**：只用 `tilesShown()` 验证“换视口后留下哪些瓦片”；要比像素的位置，另开一个窗口和画布，一开始就在那里（`limitedCanvasKeepsTheView`）。

#### 100% 不是 1/DPR：缓冲比例每个轴不一样，窗口一改大小就变
- **现象**：窗口宽 1001 逻辑 px、150% 时缓冲是 1502 px，比例 1.5005，不是 1.5；按 DPR 摆图，右边差几个像素，鼠标命中也偏。
- **做法**：编辑窗口每个轴各用 `bufferScale(窗口逻辑大小, dpr)`（`CaptureEditor::setMetrics`），窗口大小、屏幕、`devicePixelRatioChanged` 一变就重算；图的左上角和视口都先取整到设备像素，QML 里 `content.x = contentOrigin − view.x`，不用 Flickable（它的偏移是小数）。鼠标逻辑位置用 `toPhysical(逻辑, dpr)`，不是缓冲比例（Qt 的逻辑位置是原生位置除以 dpr）。

#### 命中容差和手柄距离按图像 px 定，缩放后就错了
- **做法**：它们是屏幕尺寸（4 / 6 / 3 逻辑 px），`AnnotationModel::setView(设备像素/逻辑像素, 缩放)` 换成图像 px；手柄在 QML 里按设备像素画（`unitX`），不再通过 `px()` 乘图像 px 数。25% 时点线要 4 倍的图像 px 才碰得到，800% 时不到 1 像素。

#### 长图的瓦片：全可见时有几百块
- **现象/风险**：1080×30000 缩到适合窗口时，590 块瓦片都在视野里，一次画完要卡一下，CPU 图 150 MB 加纹理。
- **做法**：`fitZoom` 在整张会小于 25% 时改按短边适合（宽度）；`limited` 画布每次 polish 最多画约 8 ms 的瓦片（离中心近的先画），最多 480 块；缩小到更小只是预览。D3D11 上一帧最多新建 8 个纹理（`kUploadsPerFrame`），其余下一帧接着来；软件渲染不限（见上面软件渲染那条，后补的节点显示不出来）。真机实测（150%，1080×30000）：滚动帧间隔 p95 约 17 ms，滚轮到出画面 p95 4.3 ms；缩到 15%（视野加一圈 160 块）帧间隔最大 15.4 ms（见下面“缩放连发”一条）；**10% 和 480 块上限没测透**。

#### 裁剪手柄和对象抢同一个按下
- **现象**：画在图边上的矩形，点它的边变成了裁剪。
- **做法**：`cropHandleAt`：在裁剪框里面时，有工具开着或按在对象上都不抓裁剪手柄；有工具时手柄只从框外面抓。单测 `cropByTheEdges`。

#### 窗口自己的关闭按钮绕过控制器
- **现象**：标题栏的关闭按钮（和 Alt+F4）直接把 `Window` 隐藏，模型和被藏起来的贴图都还留着；隐藏后的窗口不再画，画布的瓦片和纹理也放不掉。
- **做法**：`TitleBar` 加了 `closeIntercepted` 属性和 `closeRequested()` 信号，编辑窗口的标题栏设成拦截，关闭按钮调 `editor.close()`（cloak、画两帧空的、隐藏）；Alt+F4 由控制器的按键处理走同一个 `close()`；`CaptureEditor::attach` 另接 `visibleChanged(false)` 兜底。`~App` 删窗口前先断开控制器对它的连接，免得销毁时的隐藏又走一遍 `close()`。
- **不要用 `onClosing` 里 `close.accepted = false`**：Qt 退出时会给隐藏着的窗口也发关闭事件，拒绝了整个 `QGuiApplication::quit()` 就取消，`WinShun.exe --quit` 退不掉（实测过）。

#### 开编辑窗口前要先收起剪贴板窗口
- **现象**：剪贴板窗口开着时有按键路由（`KeyRouter`）把键送给它；编辑窗口在它还开着时抢到前台，按键就进了剪贴板窗口。
- **做法**：`Clipboard::trigger(EditImage)` 先 `dismissRequested`（同步隐藏并还回按键）再 `editRequested`；启动器同理，先 `dismissRequested` 再开。

#### `QImage::convertToFormat(RGB32)` 会把透明像素变成黑的
- **做法**：不是截图的图片当底图前先铺白再画上去（`CaptureEditor` 的 `flatten`）；合成器和导出都是不透明的 RGB32。

#### 贴靠（Snap）过的窗口再显示时大小不对
- **现象**：编辑窗口贴靠到半屏后关掉，下次打开 Windows 把它还原成贴靠前的大小和位置，不是我们算好的。
- **做法**：`CaptureEditor` 记下想要的大小和位置（`m_wantedSize/m_wantedPosition`），`present()` 和 `settle()` 里重设一次；显示用 `showNormal()`，不要用 `show()`。

#### Qt 把 Alt+滚轮变成横向滚动
- **现象**：编辑窗口里 Alt+滚轮（改粗细）没有反应，滚轮事件的竖直分量是 0，数值在横向分量里。
- **做法**：`CaptureEditor::wheeled` 收竖直和横向两个分量，Alt 时竖直为 0 就用横向的；`EditWindow.qml` 两个分量都传进来。

#### 测试：缩放连发时的“慢帧”，是测试代理两条命令之间的空档
- **现象**：Ctrl+滚轮连发缩放时，日志里有 40–80 ms 的 “Editor: slow frame”，看着像 8 格 32 ms 连发让界面线程停了 40–67 ms。
- **原因**：帧只在有变化时才画，慢帧量到的是没有输入的空闲。测试代理（`agent.ps1`）的每条 `wheel` 命令先 `Guard-Mouse`、再 `MoveTo` 等 40 ms 才发格子，几条命令连着发，命令之间就空出 60 ms 上下；慢帧都落在这些空档上，不在一条命令的 8 格里面。旧日志：19:20:34 的 43.2、67.2、39.6 ms 和 19:29 的 61–71 ms 慢帧，时间全在 “from the last swap to the sync”（同步 ≤ 0.1 ms、渲染 ≤ 0.4 ms）；19:29 那段 32 格，每格处理最多 1.1 ms、滚轮格到出画面最多 8.3 ms，“Annotation canvas” 是 30 次 polish、0 块瓦片、0 个纹理：界面线程闲着在等下一格。代码上也没有按格做的重活：每格只改缩放和视口（`zoomTo` → `relayout` → 绑定），已有的瓦片不重画也不重传（缩放只改节点矩形）；`polish()`、`update()` 只是登记，一帧最多合成、上传一次，只画新进视野的瓦片。
- **实测**（2026-10-21，同一版本、同一张 1080×30000 的图，240 Hz 屏，一次刷新 4.2 ms）：
  - 8 格在一条命令里 28.6 ms 内发完（100% → 15%）：17 帧，帧间隔 p50 8.1、最大 15.4 ms，没有慢帧，滚轮格到出画面最多 7.7 ms。p50 8.1 ms 是两次刷新出一帧：15% 时视野加一圈要 160 块瓦片（关窗时的 “kept at most 160 tiles”），多半是新进来的瓦片每帧最多画 8 ms、建 8 个纹理（这次 polish 不到 8 次，没打出 “Annotation canvas” 一行，没量到）。再连发 8 格放大回 100%：没有慢帧，到出画面最多 7.3 ms（不到 8 帧，日志不记帧间隔）。
  - 同样 8 格分成 4 条命令（每条 2 格，命令相隔 61–65 ms）：3 个慢帧 66.4、57.2、60.3 ms，各自在下一条命令的格子发出后几毫秒内结束；每格处理最多 0.2 ms，到出画面最多 3.6 ms。
- **做法**：不用改。测连发要在一条命令里发完（代理的 `wheel x y 增量 次数` 会记下每格的发出时刻），看 “from a wheel notch to the frame that shows it” 和慢帧里各段的时间，不要只看帧间隔。

#### “D3D11 上有一个像素显示成黑色”：屏幕上是对的，是抓图丢了这个颜色
- **现象**：1080×30000 的图在 100%、D3D11 下，图像坐标 (353,1037) 那一个像素在测试的抓图里是黑的；CPU 合成器、导出、取色器、软件画布的抓图都对。
- **原因**：这个像素是 (13,11,12)，整张图里只有它是这个颜色，正是 GDI+ 抓图读不出的那个值（见下一条）；之前 5 张 100% 抓图和原图逐像素比（每张 1080×1172），不同的都只有它。2026-10-21 编辑窗口开着时读同一个屏幕点 (1093,1249)：DXGI 桌面复制、普通 GDI `BitBlt` 到 DIB 都是 (13,11,12)；`CopyFromScreen` 到新位图是 (0,0,0)，到先涂成 (200,100,50) 的位图还是 (200,100,50)。旁边的 (354,1037) = (13,12,17) 四种读法都对。
- **做法**：不用改，也不用去试“去掉 `TextureIsOpaque`、换 RHI 后端”。单测 `tilesOfAPlainPicture` / `onePixelAreas` 照旧锁住 CPU 一侧。

#### 测试：GDI+ 的抓图读不出 (13,11,12)（#0D0B0C）
- **现象**：矩形颜色是 #0D0B0C 时，叠加层和贴图窗口在抓图里是 (0,0,0)，保存的 PNG 和剪贴板图里是 (13,11,12)；#212121、#E53935 都是准的。
- **原因**：屏幕没错，错在测试的抓图。`lib.ps1` 的 `Shot` 用 .NET 的 `Graphics.CopyFromScreen`，它在 GDI+ 位图的 `GetHdc` 上 `BitBlt`，再 `ReleaseHdc`。位图上的 `GetHdc` 给的是一张填了哨兵图案的新位图，`ReleaseHdc` 只把和哨兵不同的像素拷回原位图（微软 KB 311221）。文档没说哨兵是什么颜色，量出来这台机器上是 (13,11,12)：屏幕上恰好是这个颜色的像素被当成“没画过”，不拷回，位图里留着原来的值（新位图是 0，先涂过色的还是那个色）。和 Win顺 无关，普通 WinForms 窗口画的色块一样。2026-10-21（150%、SDR）一个色块窗口的读法：
  - GDI+（`CopyFromScreen`，或 `Graphics.FromImage` + `GetHdc` + `BitBlt` + `ReleaseHdc`，带不带 `CAPTUREBLT` 一样）：只有 (13,11,12) 读成 (0,0,0)；(13,11,13)、(12,11,12)、(14,11,12)、(13,12,12)、(12,11,13)、(0,0,0)、(1,1,1)、(16,16,16)、#212121、#E53935 都对。
  - DXGI 桌面复制、普通 GDI `BitBlt` 到 32 位 DIB：全对，包括 (13,11,12)。
- **做法**：查像素用 DXGI 桌面复制或普通 GDI `BitBlt` 到 DIB，或者以保存出来的文件为准，不拿 GDI+ 的抓图当真值；GDI+ 抓图里有单个像素是 0 或“没变”，先看原色是不是 (13,11,12)。

#### 测试会挤掉 Windows 剪贴板历史
- **现象**：Windows 剪贴板历史满 25 条，WinShun 每复制一次就挤掉最旧的一条。
- **做法**：测试前把最旧的 10 条存下来（WinRT `Clipboard.GetHistoryItemsAsync`），测完删掉测试产生的、再按原顺序补回；对 `ExcludeClipboardContentFromMonitorProcessing` / `CanIncludeInClipboardHistory=0` 的数据不会进历史，测试用的复制尽量带上这两个标记。

### 序号写法、序号旁的字、识别文字动作、工具条按钮、穿透贴图（GitHub #57 #59，2026-10-22）

#### 穿透的贴图按住 Ctrl 能操作：Ctrl+滚轮是调透明度，不是缩放
- **现象**：穿透的贴图靠按住 Ctrl 才够得着，可贴图自己的 Ctrl+滚轮是调透明度；按住 Ctrl 时滚轮缩放不了。
- **做法**：不改（Ctrl 是够得着它的唯一条件）。要缩放先在右键菜单里取消穿透。使用说明里写明。
- **实现要点**：不装键盘钩子（它会看到系统里每一个键，和 `KeyListener` 的双击 Ctrl 抢），而是只在有穿透的贴图时开一个 40 ms 的 `QTimer`，读 `GetAsyncKeyState(VK_CONTROL)`；没有穿透的贴图时没有定时器。切换 `WS_EX_TRANSPARENT` 要 `SetWindowLongPtr` 加 `SWP_FRAMECHANGED`（`applyClickThrough`）。拖动中和右键菜单开着时不能收回能操作的状态，否则松开 Ctrl 的瞬间拖动和菜单（菜单开着时本进程的消息循环仍在跑定时器）就断了。实测留给真实输入。

#### 放下序号后马上输入文字：单元测试里的 Number 工具点击不再只是放下一个序号
- **现象**：`tst_annotmodel` 里用序号工具点一下，随后 `selectedId` 是 0、`canUndo` 为假。
- **原因**：放下序号（`finishGesture`）后立刻进入“序号旁的字”的输入（`m_editing->words`），选中被清掉，`AddItems` 还没进撤销栈（它留在 `Private::placing`，写完字才推进去，这样放加打字是一步撤销）。
- **做法**：测试里点完序号要 `commitPending()` 再查选中和撤销；用户这一侧，下一次点击先写完字再放下一个序号（`pressFirst` 返回 false，不吃掉这次按下），所以连续点几下仍是一下一个序号。输入框里按下仍归输入框。

#### 放序号的同时改它的样式：撤销栈里先后顺序错了
- **现象**：序号旁的字还在输入时，从面板改颜色，撤销要按两次，第二次什么也看不见；重做回来的序号是改色之前的颜色。
- **原因**：放下的序号（`Private::placing`）还没进撤销栈，面板的 `Restyle` 经 `push()` 先进去了。
- **做法**：`AnnotationModel::push()` 开头先把 `placing` 推进栈（此后字写完是单独的一步 `Retext`）。单测 `restylingANumberWhileItsWordsAreTypedUndoesInOrder`。

#### 编辑窗口：序号工具的样式面板比默认窗口还宽
- **现象**（2026-10-22 实测，150%）：从贴图进编辑窗口按 N，面板（颜色、字号、起始、五个写法按钮、层级按钮，约 917 逻辑 px）压在右下角的“1201 × 761”和缩放百分比上，最右的按钮被窗口边切掉；窗口默认只有 908 逻辑 px 宽。
- **做法**：`EditWindow.qml` 的 `minimumWidth` 在面板显示时再加上面板和右边状态行（`status`）的宽度，窗口会长到放得下。面板宽度随选中的对象变（选中后多出层级按钮），所以最小宽度也跟着变。

#### ini 里手写的“undo,pin”读出来是空的
- **现象**（2026-10-22 实测）：手工在 `[Capture]` 里写 `ToolbarHidden=undo,pin`，工具条上什么也没藏。
- **原因**：QSettings 读 INI 时，没有引号、含逗号的值会被当成字符串列表，`value().value<QString>()` 得到空串；程序自己写的值带引号，读得回来。
- **做法**：这一项读到列表就用逗号接回去（`Settings::load`）。以后 ini 里用逗号分隔的字符串项都要这样读，并用手写的无引号形式测一遍。

#### 前台是管理员窗口时，按住 Ctrl 够不着穿透的贴图
- **现象**（2026-10-22 实测）：测试窗口是提权的（从提权脚本里启动的子进程），它在前台时按住 Ctrl，贴图一直不“可操作”；改成普通权限的测试窗口就好了。
- **原因**：UIPI：前台窗口完整性级别比 Win顺 高时，`GetAsyncKeyState` 在 Win顺 里读不到按键。用户平时前台是普通权限的程序，不受影响；前台是管理员窗口（任务管理器之类）时这个办法用不了，先点一下别的窗口。
- **测试**：从提权的测试脚本启动测试窗口要先降权（在普通权限的 shell 里启动它，提权脚本只按标题找窗口）。

#### 测试：让 Win顺 自己的复制不进 Windows 剪贴板历史
- **做法**：环境变量 `WINSHUN_TEST_NO_CLIP_HISTORY` 设了以后，`ClipboardWatcher` 写系统剪贴板时加 `ExcludeClipboardContentFromMonitorProcessing` 和 `CanIncludeInClipboardHistory=0`（Windows 的历史满 25 条时每复制一次就挤掉最旧的一条）。只给测试用，正常运行不设。同一次测试里 `WINSHUN_WEB_DRYRUN` 让网页搜索只写日志。
- **备份**：历史库 `clipboard.db` 满 300 条时，每多一条会按上限删最旧的行和它的图片文件；备份要连 `clipboard\images` 一起备份（这次只备份了库，被删的一张图是从卷影副本找回来的）。

#### 另起一组后选序号写法，改到了上一组
- **现象**（2026-10-22 实测）：放几个序号，点“另起一组”，再点“A”按钮：新一组还是 1 2 3，上一组变成了 A B。
- **原因**：写法按“选中的序号所在的组”改，刚放的最后一个序号还选着，它在旧组里。
- **做法**：`newSeries()` 先放掉选中（`m_picked.clear()`），面板就对着新的一组。

#### 序号工具下点一下序号：先选中，不是马上打开它的字
- **现象**：第一版点一下序号就进了字的输入框，选中的轮廓没了，Delete、方向键、`[` `]` 都落进输入框。
- **做法**：点一下只选中；已经是唯一选中的那个序号再点一下，或者点在它的字上，才打开字（`Private::reopenWords`）。单测 `clickSelectsANumberBeforeItOpensItsWords`。

#### 识别文字并复制 / 搜索 / 网页搜索：没有单独的“默认搜索引擎”
- **做法**：Win顺 没有“默认网页搜索”这个设置。`defaultWebSearchUrl` 取网页搜索列表里第一个带 `%s` 的网站（默认百度，用户调整顺序就换了），列表里没有再用必应。这三个动作用单独的 `CaptureOcr`，不碰 OCR 窗口里的字。`WINSHUN_WEB_DRYRUN` 时只写日志不打开网页，测试用。

## 智能打码（阶段 5，2026-10-22）

### 读字模型给的字位置只准到约四分之一个字高
- **现象**：照 CTC 时间步算出的框，左右边缘能偏 3–10 px（24 px 的字、150% 时更多），盖到“电话”两个字或漏掉头一个数字。
- **原因**：识别模型每 8 px（行图高 48 px）出一个时间步，加上四边形外扩（unclip），字的位置本来就只有这个精度。
- **做法**：`SmartRedact` 把边缘吸到扫描图上最近的墨迹起止（`atTheInk`）。吸附有两条规矩：空白里的边缘往匹配这一侧走，只削空白；落在字里的边缘看这个字离首尾字还是邻字的中点近，近首尾就包进来，近邻字就退到邻字前面。第一版“取最近的墨迹起止”会在边缘落在第一个数字中间时跳到第二个数字，把第一个数字露出来。单测 `snappingNeverCutsAGlyph` 和 `tst_ocr` 的 16 种字体、字号、缩放。

### 涂抹抹平沿着长笔迹慢得离谱
- **现象**：2560×1600 上一笔画满屏的涂抹（笔宽 36），抹平要十几秒。
- **原因**：方向填充每个点往四个方向找最近的可读像素；沿着笔迹的方向最近的可读像素可能在一屏之外。
- **做法**：涂抹的扫描最远 64 px，只算笔迹内的像素，笔迹覆盖只算一次。同一笔现在 2.7 s（抹平）、0.2 s（模糊）、0.1 s（马赛克）；正常的一笔（几百 px）是几十 ms。画的过程中每次移动整笔重算，长笔的抹平会卡，以后可以拖动时先用马赛克代替。

### 单个 `std::vector<double> middle(std::size_t(n));` 被当成函数声明
- **现象**：MSVC 报“下标要求数组或指针类型”，指向后面用到这个变量的地方。
- **做法**：最令人烦的解析；用 `static_cast<std::size_t>(n), 0.0` 两个参数的构造，或花括号。

### Shift+B 本来是“选打码工具”
- **现象**：`tst_annotmodel::toolKeysToggle` 的“Shift+字母关掉数字键开的工具”对打码不成立了。
- **做法**：Shift+B 现在是智能打码，工具键只有 B 和 9；测试里打码单独处理。

### 手写不加引号的逗号列表，QSettings 读回来是字符串列表
- **做法**：`[Capture] SmartWords=a,b` 同 `ToolbarHidden`，`Settings::load` 先看是不是 `QStringList` 再 join；单测 `tst_settings` 有手写的情形。

### 测试：别人的窗口一直抢前台
- **现象**：这台电脑上 FlClash（`FLUTTER_RUNNER_WIN32_WINDOW`，管理员权限）一直是前台，我的测试页被它盖住；非提权的脚本发的热键和鼠标到不了它，也切不走前台。WinShun 日志里有“Could not bring the window to the front over FLUTTER_RUNNER_WIN32_WINDOW”。
- **做法**：用提权的 pwsh（UAC 在最低，不弹窗）跑测试脚本，里面 `AttachThreadInput` 把测试窗口调到前台；测试窗口放在不被它盖的地方（它在 433,264 到 2126,1462，把测试页排成一条横幅放在上面 y < 264 处）；每个键、每次点击前仍然检查前台。
- **测试窗口别用 Edge**：`msedge --user-data-dir=<新目录>` 会登上系统账号、同步扩展，弹出 Tampermonkey、Distill 的页面抢前台；要用就加 `--disable-sync --disable-extensions`。WinForms 的测试窗口由 `powershell.exe` 开，控制台窗口要在脚本里 `ShowWindow(GetConsoleWindow(), 0)` 藏掉，否则它盖在窗口上被一起截进去。
- **Windows 剪贴板历史**：测试只用 Ctrl+S（只记 Win顺 自己的历史，不动系统剪贴板），Windows 的历史 25 条前后 id 一样。

### 测试里编的密钥被 GitHub 当成泄露的真密钥
- **现象**：推送后 GitHub 的密钥扫描（secret scanning）报警 #1：`tests/tst_smartredact.cpp` 里有 Google API Key（2026-10-24 用户看到后以为把密钥传上去了）。
- **原因**：识别密钥的单测要用各家格式的样例，`AIzaSyA-AbCdEf…`、`ghp_…`、`AKIA…`、`-----BEGIN RSA PRIVATE KEY-----` 完整写在源码里，格式对得上就报，扫描器不管值是不是编的。查过全部历史（`git log -p --all` 按各家格式 grep），只有这些编的样例，没有真密钥；`tests/data/ditto-real.db` 里也只是专门复制的 5 条测试内容（文件路径里带用户名 `young`）。
- **做法**：样例在前缀处断开拼接（`u"AIza"_s + u"SyA-…"_s`），源码里没有完整格式，测试的文本不变。GitHub 上的警报用 API 关成“仅用于测试”：`gh api -X PATCH repos/LingCore/WinShun/secret-scanning/alerts/<n> -f state=resolved -f resolution=used_in_tests`。以后写这类样例照此办理；真密钥（语义搜索的 SiliconFlow 等）只在 `%APPDATA%` 的设置里，从不进仓库。

## 系统开关（docs/design-system-switches.md，2026-10-19）

### 只写 Personalize 注册表，窗口和任务栏都不变

- **现象**：把 `AppsUseLightTheme`、`SystemUsesLightTheme` 写成浅色后，约 1.6–2 秒内本进程的窗口一条 WM_SETTINGCHANGE 都没收到，Qt 不变；任务栏时钟和大部分托盘图标也不变（只有输入法指示器的“中”变了色）。
- **原因**：Windows 设置改深浅色时会广播 WM_SETTINGCHANGE（"ImmersiveColorSet"），各程序靠它重读。只写注册表没人通知。和 pitfalls“改了 PrtScn 的注册表值不广播”是同一类。
- **做法**：写完要自己广播 "ImmersiveColorSet"（`platform/SettingBroadcast`）。Win顺 自己的 Qt 窗口收到后 3–4 ms 发出 `colorSchemeChanged`，之后走原来的 `applyTheme`（Mica 的修正见“切换深浅色后，Mica 还是旧主题的颜色”）。

### 广播 ImmersiveColorSet 要两三秒，自己的窗口最后才轮到

- **现象**：`SendMessageTimeout(HWND_BROADCAST, WM_SETTINGCHANGE, 0, "ImmersiveColorSet", SMTO_ABORTIFHUNG, 1000)` 四次分别 2764、3821、2501、3108 ms 才返回；广播开始后 1.4 秒的截图里任务栏的字已经变了，本进程的 Qt 窗口却要等 1.5–2.8 秒才收到消息。在界面线程上发，界面就卡住几秒。
- **原因**：广播按顶层窗口一个一个发，每个最多等 1000 ms。别的程序先变，自己最后。广播也发给 Win顺 自己的窗口：在界面线程上等广播，或退出时 `join` 发送线程，界面线程都不处理消息，广播就卡在发给自己窗口的那一步直到超时，Win顺 有好几个顶层窗口，退出会卡好几秒。
- **做法**：
  - 先直接、同步地通知本线程上自己的窗口（`SendMessageTimeoutW` 发给每个本线程的顶层窗口），Qt 和托盘的单色图标马上换。
  - 其余放在单独的线程上发，同一时刻最多一条，发的过程中又来请求只记一个标志、发完补发。
  - 这条线程是“用 `std::jthread`”的例外：用 detach 的 `std::thread`，退出时**不等**，只按值捕获，不碰退出时会析构的对象。
  - 测试程序改了设置后，用 `settingbroadcast::busy()` 等它结束再退出，否则别的程序可能听不到最后一次改动。

### 在 STA 线程上同步广播，测量循环一次没跑，得出“刷新不了”的假结论

- **现象**：探针在界面（STA）线程上同步发 "ShellState" 广播（3709 ms 才返回），返回之前测量窗口有没有变的循环一次都没跑，当时得出“这样刷新不了”的结论。
- **原因**：量的线程和发的线程是同一条，发送没返回，量也没开始。
- **做法**：要量一个广播的效果，把广播放到别的线程上发，量的线程不阻塞。

### 同值写入静默成功：没有回调，返回值也各不一样

- **现象**：同值 `IAudioEndpointVolume::SetMute` 返回 `S_FALSE`（成功码），没有回调；同值 `SetMasterVolumeLevelScalar` 返回 `S_OK`，也没有回调；无线电同值 `SetStateAsync` 返回 Allowed，不报错。
- **原因**：这些接口把“已经是了”当成功，也不通知，所以不能靠返回值或回调知道有没有真的改。
- **做法**：写之前自己先比较（音量按四舍五入后的整数），相等就不写、直接回“已经是了”（底栏说一声）；判断成功用 `SUCCEEDED`，不要把 `S_FALSE` 当失败。

### 只写 Explorer\Advanced，资源管理器窗口不刷新

- **现象**：把 `Hidden`（1 显示 / 2 不显示）或 `HideFileExt` 写进注册表，3 秒内已打开的资源管理器窗口不变。再加 `SHChangeNotify(SHCNE_ASSOCCHANGED)`（调用 163 ms）窗口照样不变。
- **原因**：只改注册表不会让窗口重读；`SHChangeNotify(SHCNE_ASSOCCHANGED)` 也不是它要的通知。
- **做法**：改用 `SHGetSetSettings(&ss, SSF_SHOWALLOBJECTS 或 SSF_SHOWEXTENSIONS, TRUE)`：调用 1.6–1.8 ms，一次写好注册表、带出 "ShellState" 广播（调用后约 4 ms 本进程窗口就收到）、让打开的窗口自己刷新（隐藏文件 358 ms，扩展名 742 ms）。它失败时才退回“直接写注册表 + 在别的线程上广播 ShellState”（实测可行：95 ms 后刷新，广播本身 0.8–3.7 秒才返回）。`IShellWindows` 逐个 `Refresh` 能刷（62 ms）但要碰用户的每个窗口，不用。

### `SHELLSTATE` 的位域读回来，“真”是 -1

- **现象**：读回 `SHELLSTATE` 里的位域，应当为真的位是 -1，`== 1` 判断永远不成立。
- **原因**：有符号的 1 位位域，置位读出来是 -1。
- **做法**：判断一律用 `!= 0`。

### `CoCreateInstance(ShellBrowserWindow)` 在 explorer 里留下看不见的窗口

- **现象**：测试里用它找资源管理器窗口，在这台电脑上返回 E_FAIL，却在 explorer 里留下两个看不见的“库”窗口。
- **原因**：没有查清；调用失败了，窗口却已经建在 explorer.exe 里。
- **做法**：测试要开自己的资源管理器窗口，用 `ShellExecuteEx` 的 `opennewwindow` 动词；按路径找到窗口后，核对它的 HWND 不是用户原有的框架（Win11 的标签页共用一个 HWND）；误留下的窗口用 `Quit()` 关掉，最后核对只剩用户原来的窗口。

### TranslucentTB 开着时，取任务栏的颜色量不出深浅色的变化

- **现象**：想用任务栏背景的颜色判断深浅色是否切过去了，取到的是透明背后的壁纸，两个主题下一样。
- **原因**：TranslucentTB 把任务栏做成了透明。
- **做法**：看时钟和图标的字色，不看背景。

### `rundll32 shell32.dll,Options_RunDLL 7` 的窗口属于 explorer.exe

- **现象**：用它打开“文件资源管理器选项”（直接是“查看”页）后，按 rundll32 的进程号找不到这个窗口，rundll32 很快就退出了。
- **原因**：对话框开在 explorer.exe 里，rundll32 只是个发起者。
- **做法**：不要按 rundll32 的进程号找这个窗口；它属于 explorer.exe，按窗口本身（标题、类）去找。

### 重开 WLAN 的无线电可能自动连网

- **现象**：关了再开 WLAN，Windows 可能自动连上设为“自动连接”的已存网络，包括用户之前手动断开的那个。
- **原因**：Windows 本身的行为，和在快速设置里点一下一样。
- **做法**：WLAN 的翻转测试要用户在场；关 WLAN 当前正靠它上网时，底栏写“（会断网）”，不弹确认，再按一次就开回来。

### 无线电和音频的回调不在调用的线程上，落在哪个线程也不固定

- **现象**：`Radio.StateChanged` 两次落在两个不同的线程上；`IAudioEndpointVolumeCallback::OnNotify` 4 次回调落在 2 个线程上。`StateChanged` 还比 `SetStateAsync` 完成早约 1 ms。
- **原因**：实测如此，它们不在调用的线程上，落在哪个线程也不固定。
- **做法**：回调里不碰界面的东西，一律带上下文对象 `QMetaObject::invokeMethod(…, Qt::QueuedConnection)` 转回界面线程（`SwitchRelay`）；`IMMNotificationClient` 的回调里不调 MMDevice，把事情投给开关线程再重新绑定。见 pitfalls“不带上下文对象 connect 引擎的信号，槽在引擎的工作线程上跑”。

### Win顺 自己的 `WinShun.exe.bak-*`：测试脚本 ShellExecute 改过名的 exe，弹出 Windows 的“打开方式”

- **现象**：测试脚本用 `ShellExecute` 去启动被改名备份过的 exe（`WinShun.exe.bak-…`），弹出 Windows 的“打开方式”对话框。
- **原因**：改名后的文件不再是 `.exe`，shell 不知道怎么打开它。
- **做法**：只启动准确的 `WinShun.exe` 路径；要回退旧版，先改回原名再启动，不要直接启动 `.bak-…` 文件。

### UI Automation 的命中测试到任务栏窗口就停了，到不了托盘图标

- **现象**：用 UI Automation 按坐标找托盘里的 Win顺 图标，命中的总是任务栏窗口本身，找不到图标按钮。
- **原因**：UI Automation 的命中测试在任务栏窗口停住，到不了里面的托盘图标。
- **做法**：用 `Shell_NotifyIconGetRect` 或位置去找托盘按钮，不靠 UIA 的命中测试。

## 打开应用（2026-10-20）

### 从 VS Code 里启动的 Win顺，打不开 VS Code 和别的 Electron 应用

- **现象**：搜 vscode 按 Enter，什么都不出来：没有新窗口，开着的 VS Code 也不到前面，日志里也没有失败。记事本这类原生程序照常打开。从资源管理器启动同一个 `shell:AppsFolder\Microsoft.VisualStudioCode` 却没问题。
- **原因**：Win顺 是从 VS Code 里启动的（终端，或扩展里跑的工具重启了它），继承了扩展宿主的 `ELECTRON_RUN_AS_NODE=1`。Win顺 用 `ShellExecuteEx` 启动应用，环境变量原样传下去；Electron 应用（VS Code、Cherry Studio……）看到它就当普通 Node.js 运行，不开窗口直接退出。看起来很像前台权限问题，其实不是：测试窗口一直留在前台也一样打不开，去掉这个变量后马上就好。
- **做法**：`main()` 里 `qunsetenv("ELECTRON_RUN_AS_NODE")`，和 `QT_QPA_PLATFORM` 一样不传给启动的程序。测试脚本自己也是从 VS Code 里跑的，排查“某个应用打不开”时，先看看 `env | grep ELECTRON`。

## 文字清晰（2026-10-21）

### 13 px 的字在 150% 缩放下发虚

- **现象**：拾穗计划页底部的致谢和说明（13 px）比上面那行 14 px 的说明明显糊，横笔画像是摊成了两行半亮的像素。逐行比对截图，文字的竖直位置已经在整像素上了，把位置再对齐一次，像素一个都没变。
- **原因**：13 px × 1.5 = 19.5 个物理像素，字号本身不是整数，FreeType 的竖直微调对不齐笔画。用 text_stats 量，半调像素比例 13 px 是 57.7%/54.7%，改成 14 px（21 个物理像素）后是 51.5%/49.7%，跟原本清楚的那行（47.8%）接近。
- **做法**：字号用 Theme 里的档位（fontCaption 14 等，150% 下都是整数物理像素），不要随手写 13、11 这种奇数 px。怀疑字发虚时，先算“字号 × 缩放”是不是整数，再去查位置。

## 普通用户模式（GitHub #52，2026-10-21）

### 本机装着服务，测不到“没装 / 停用 / 装不了”

- **现象**：开发机上服务一直装着、在跑，`installed()` 永远为真，“没有安装索引服务”“装不了”的文案和启动时不等服务的路径都碰不到；真去 `sc delete` 又会弄坏本机的环境。
- **做法**：环境变量 `WINSHUN_SERVICE=none|disabled|stopped` 让 `ServiceVolumeAccess::state()` 假装 SCM 的回答（写法同 `WINSHUN_SPINNING`），`installed()` 跟着变；这三种状态下界面不去连管道，和真的没服务一样。`classify()` 是纯函数（`tst_core` 的 `serviceStateFromScm`）。要连管道一起假装，另设 `WINSHUN_PIPE=<不存在的名字>`。
- **注意**：SCM 拒绝查询（ACCESS_DENIED）时是 `Unknown`，按老办法由管道探测决定；“启动类型停用但还在跑”算 `Running`，由管道决定。

### 登录任务注册不上：改写 HKCU Run

- **现象**：任务名 `WinShun` 在全机是唯一的，另一个账户已经注册过，或策略禁止任务计划，`RegisterTaskDefinition` 失败，该用户就没有开机自启。
- **做法**：失败时写 `HKCU\Software\Microsoft\Windows\CurrentVersion\Run` 的 `WinShun` = `"<exe>" --background`（`AutostartRun.h`）。“开机启动”读取时任务和 Run 值都认，关闭时两个都删，任务注册成功时顺手删掉 Run 值，不会开机起两份。安装器卸载时也删指向本副本的 Run 值。限制：Run 值起不来需要提权的启动（管理员直读模式），那个模式本来就只有管理员账户用。
- **MCP 中继**：`schtasks /Run /TN WinShun` 失败时，直接启动中继旁边（找不到再用 Run 值指的）`WinShun.exe --background`；`WinShunSearch.exe`（stub）本来就有这个回退。

### 盯着一个文件夹，它的上级就改不了名

- **现象**：只监视 `T\a\b`（为 ReadDirectoryChangesW 打开它的目录句柄，带 `FILE_SHARE_DELETE`）时，`T\a` 改名或移动失败，ERROR_ACCESS_DENIED（5）；资源管理器里是“文件夹正在使用”，删到回收站也一样。监视 `T` 或卷根则不挡下面任何文件夹改名（设计 7.2，2026-10-21 实测）。
- **原因**：下面有打开的句柄时，NTFS 不许给目录改名（那些句柄的路径会变）；`FILE_SHARE_DELETE` 只放行改这个文件夹自己。
- **做法**：选中的文件夹照旧经卷根监视（每卷一个句柄，范围外的事件在 `changesInScope` 里丢掉）。只有卷根打不开（这个账户不能列它的内容）时，才逐个监视选中的文件夹（`ChangeWatcher::Fallback`，在监视线程里决定：根打得开就不开文件夹）；代价是 Win顺 运行时它们的上级改不了名。`tst_core` 的 `watchedFoldersRename` 每次都把结果打出来。

### 监视超过 62 个，监视线程悄悄退出

- **现象**：`ChangeWatcher` 用一次 `WaitForMultipleObjects` 等所有监视的事件，再加它自己的两个。句柄超过 64 个时返回 `WAIT_FAILED`，循环 `break`，线程结束：所有监视都没了，也没有日志。卷根最多 26 个碰不到；逐个文件夹监视、网络共享很多时会碰到。
- **做法**：`ChangeWatcher::kMaxWatches` = 62，按请求的顺序开：先是各个根，再是代替打不开的根的文件夹，超出的不开，`roots()` 里也没有。`IndexService` 据此把没监视的选中文件夹标成“没有实时更新”（只靠定时核对），`ShareIndexer` 把共享标成没在监视。`tst_core` 的 `changeWatcherCap`。要去掉上限，可以改用 IOCP。

### ReadDirectoryChangesW 的缓冲区：网络上不能超过 64 KB

- **现象**：缓冲区大于 64 KB 时，监视网络共享（或其实指向网络的盘符，比如 subst 了一个共享）的 `ReadDirectoryChangesW` 直接失败，ERROR_INVALID_PARAMETER（文档说是底层文件共享协议的包大小限制）。本地卷根一直用 64 KB，又容易溢出：解压一个包、编译一次就丢事件，而只索引选中文件夹时，溢出一次就要把卷上的选中文件夹重走一遍。
- **做法**：缓冲区按根分（`ChangeWatcher::bufferBytes`）：本地卷根（`X:`，且 `GetDriveTypeW` 不是 `DRIVE_REMOTE`：只读本进程的盘符表，不碰盘）1 MB；共享、文件夹、插件目录 64 KB。1 MB 失败时（ERROR_INVALID_PARAMETER，或别的原因，比如能锁定的内存不够）退回 64 KB 再试一次，不会比以前差。连续溢出时，选中文件夹的重走按 3 秒 → 30 秒 → 5 分钟退避，10 分钟没再溢出就回到 3 秒（`LostEventsBackoff`）；只有丢了事件才退避，上级文件夹删了又建回来这类变化照旧 3 秒后重走。整盘遍历的卷照旧 3 秒。

### 测试里按子串找设置名：`Index/Folder` 撞上 `Index/Folders`

- **现象**：`tst_aisettings` 的 `neverWritable` 检查“`Index/Folder` 不在 AI 能改的清单里”。加了 `Index/Folders` 以后它失败：清单文字里有 `Index/Folders`，子串 `Index/Folder` 自然也在。
- **做法**：按带引号的整个键找（`"Index/Folder"`）。新加的键是旧键加个后缀时，先看看测试里有没有按子串找的。

### 索引文件夹里加文件，要登记进 IndexFolder

- **现象**：`IndexFolder.h` 写明，索引文件夹里别的文件一概不碰。不登记的附属文件，换索引位置时被丢在原处，删除索引时被留下，判断目标文件夹是不是索引文件夹时也会出错。
- **做法**：`scope.json`（选中文件夹的范围和上次核对）登记进 `IndexFolder.cpp` 的文件清单（`hasIndex`、`size`、`onlyIndexFiles`、`copy`、`remove`），`tst_core` 的 `scopeFileIsAnIndexFile` 守着。以后再加附属文件，照此办理。它只在快照写成功之后写，并记下 `index.bin` 的大小和时间，对不上就当作不知道（多核对一次），所以老版本重写了快照也不会读错。
- **整盘时也要看它**（代码审查）：整盘时对不上的 scope.json 说明快照可能是文件夹模式存的（遍历的盘只剩选中的文件夹）。`readScopeFile` 分出“没有”和“有但对不上 / 读不了”（`stale`）：后者整盘时把遍历的盘照“启动时核对”再走一遍（`staleScopeFileWholeDisks`）。“没有”照旧信快照：老版本和整盘用户都没有它；第一次写就失败的那种补不回来。

### 登录时服务还没起来，范围先把别的盘删了（代码审查）

- **现象**：Scope=folders、服务装着。登录时 Win顺 比服务先起来，这一轮按“只索引选中的文件夹”选卷：没有选中文件夹的盘在等服务（`awaitAccess`）之前就从快照里删掉了；服务一到又是整盘，全部重读 MFT。每次登录一遍。
- **原因**：“只索引选中的文件夹”看的是此刻能不能读日志，服务只是还没到，范围就生效了；选卷、删条目又都在等服务之前。
- **做法**：两层。App 那层（`Settings::foldersInEffect`）：服务装着、没被停用（不是 NotInstalled / Disabled）时一律整盘，暂时没运行也一样；设置里第三项灰掉，下面一行说明原因。IndexService 那层：先列出所有盘、快照条目全留着，等完服务再按实际读法算 `foldersOnly`、剪卷、删条目（`chosenFoldersAwaitService`；`tst_settings` 的 `foldersInEffect`）。两层缺一不可：只改 App，等服务超时后的那一轮仍会按旧的读法剪；只改 IndexService，App 每次服务没起来时还是发范围下来。

### 只忽略 ASCII 大小写的查找：选中文件夹写法不同，每 3 秒重走（代码审查）

- **现象**：选的是 `доки`，盘上是 `ДОКИ`。`prepareScope` 按盘上的写法建条目（`onDisk` 取 `cFileName`），`walkFolders` 却用 `findPath(写法)` 找，找不到，以为“刚出现”，3 秒后再排，永远不就绪。8.3 短名（`LONGFO~1`）同样。
- **原因**：`FileIndex::findPath` / `findChild` 只忽略 ASCII 大小写（NTFS 的大写表是 Unicode 的）。
- **做法**：`findFolder`：先 `findPath`，找不到再逐级把孩子的名字解码后用 `win32::equalsIgnoreCase`（`CompareStringOrdinal`）比。仍找不到、而这一轮刚由 `prepareScope` 放进索引的（`m_placed`），标“读不了”并记一行日志，不再重排；不是这一轮放的（真是刚出现的）才照旧 3 秒后再走。`chosenFolderSpelledOtherwise`。

### 只改大小写的改名，Windows 先单独报一个删除（代码审查，实测）

- **现象**：选中文件夹的上级 `up` 改成 `UP`：ReadDirectoryChangesW 先单独送来一批 `FILE_ACTION_REMOVED …\up`，下一批才是 `RENAMED_OLD_NAME up` / `RENAMED_NEW_NAME UP`。按“上级只看删除”，第一批就把通路和选中文件夹的条目全删了，改名来时已无从挪起，只能等 3 秒后整个重走（`aboveRenamedInCase` 第一次就是这样失败的）。
- **原因**：开着 8.3 名字的卷上，小写名另有一个大写的短名；改成大写后短名和长名合一，那个短名链接被删掉，报成一个删除（推测，现象是实测的）。
- **做法**：上级文件夹说删掉了（不是这一批里配对的改名）时先看盘上（`changesInScope` 的 `stillThere`，`presenceOf`）：还在就不删也不重走；真不在照旧删、重走。两半都在上级的改名整个挪（`moveOnly`：挪不成什么也不加，不会把整个用户文件夹当新文件夹走一遍），只改大小写的不重走；从上级挪进选中文件夹的当作删除＋新增。别以为改名一定是同一批里的一对 OLD/NEW。

### 默认文件夹在第一次读设置时就写死了（代码审查）

- **现象**：第一次读设置就把桌面、文档、下载……写进 `Index/Folders`。以后 OneDrive 接管了“文档”，Win顺 还去找原来的位置；整盘用户也平白多了一份清单。
- **原因**：`readOrDefault` 缺键时把默认值写回文件。
- **做法**：没有这一项时按当时的已知文件夹算、读时不写（`indexFoldersChosen` 为假，`save` 也不写）；第一次真的只索引选中的文件夹时（`App::takeDefaultIndexFolders` → `Settings::chooseDefaultIndexFolders`）原样定下来；设置窗口、AI 改过的立刻算“设过了”（不然内存里的标记和文件不一致，`reloadSettings` 会把自己的保存当成外部改动）；清空了保持空；一个都没有（全在服务器上）时不定，下次启动再算；“恢复默认”回到没设过。`indexFoldersUnset`。

### 一轮结束时先看欠账、后清 m_running：最后一刻的“立即核对”丢了（代码审查）

- **现象**：一轮快结束时点“立即核对”：`verifyFolder` 把文件夹记进 `m_verifyOwed`，看到 `m_running` 还是真，就不起新的一轮；`ranOut` 却是先看欠账（那时还空）、再清 `m_running`。那次核对就丢了，直到下次别的事起一轮。
- **做法**：先清 `m_running`，再看欠账（`passOver`）：要么这一轮看见，要么请求方看到没在跑、自己起一轮。`passOverOrder`。

### “不在”被 scope.json 盖掉；整盘排除的盘显示“不在”（代码审查）

- **现象**：启动时先标了“盘不在”，随后从 scope.json 恢复上次的状态，盘不在的文件夹显示成“就绪”。“不搜索的文件夹”里有 `X:` 时，选卷就去掉了 X 盘，上面的选中文件夹显示“盘不在”，其实是被排除了。
- **做法**：“不在”“整盘收”在恢复 scope.json 之后、等完服务之后才标；盘不在时先问 `isExcludedPath`，是排除就标“已排除”。`offlineOverScopeFile`、`chosenFolderOnExcludedDrive`。

### 1 MB 的监视缓冲挂得上、读却总失败（代码审查）

- **现象**：1 MB 的卷根缓冲只在挂上时失败才退回 64 KB；挂上了、每次完成却失败（能锁定的内存不够之类）时，每次都当作丢了事件 → 重走选中文件夹 → 再失败。
- **做法**：`ChangeWatcher::bufferAfter`：大缓冲的读失败就换 64 KB（`ERROR_NOTIFY_ENUM_DIR` 除外：那是“事件太多”，缓冲没问题）；完成后重新挂失败也先退到 64 KB 再挂一次。`changeWatcherShrinks`。

### 每 5 分钟的服务探测都去盘上查选中文件夹的路径（代码审查）

- **现象**：`probeService` 之后每次都走 `applyIndexScope`，里面 `indexFolderPaths()` 对每个选中文件夹调 `GetLongPathNameW`（碰盘，机械硬盘会被唤醒），哪怕范围根本没变。
- **做法**：`applyIndexScope` 只在“只索引选中的文件夹”真的翻转时才重算路径、下发范围、更新启动器的提示；平时只刷新设置里的状态。文件夹清单改了走 `m_indexOptionsApply`（`setOptions`），不靠这里。

### prepareScope 看盘时不理停止（代码审查）

- **现象**：`prepareScope` 在加锁前逐个看选中文件夹和它们的上级（`presenceOf`、`FindFirstFileExW`），盘慢或在休眠时要一会儿；这期间弹出 U 盘、改设置要等它看完（`stopWorker` 会 join）。
- **做法**：每个选中文件夹看之前查一次 `stop_token`，停了就什么都不做直接返回（还没动索引）；`run` 随即返回，已经放进来、该走的文件夹记进 `m_verifyOwed`，下一轮接着走。

## 剪贴板导入（Ditto、txt、csv，GitHub #56）

### 读正在运行的 Ditto 的数据库：被占着，最近的写还在 -wal 里

- **现象**：Ditto 开着时直接打开它的 `Ditto.db`，可能被锁住，或者读不到刚存的几条。
- **原因**：Ditto 一直开着库，近期的写在 `Ditto.db-wal` 里，没写回主文件。
- **做法**：把 `Ditto.db`（还有 `-wal`、`-shm`，有就复制）复制到临时文件夹，用自己的连接名打开那份副本（`QSqlDatabase` 的连接不能跨线程，也不碰 `ClipStore` 的）。副本是读写打开的，不是只读：只读打开带 WAL 的库要建 `-shm`，临时文件夹里读写反而稳；原文件不动。别人事务里还没提交的行读不到，`dittoRunning` 用例。

### Ditto 的字段不是看名字就能猜的

- **现象**：按“日期是毫秒”“置顶就是不为 0”“不在组里 `lParentID` 是 0”去读，日期差 1000 倍，没置顶的全成了置顶，组里的条目顺序错。
- **原因**：（从 Ditto 源码 `DatabaseUtilities.cpp`、`Clip.cpp`、`Misc.cpp` 看，再用 Ditto 3.25.113.0 自己建出来的库对过）`lDate`、`lastPasteDate` 是 Unix **秒**；没置顶的 `stickyClipOrder`、`stickyClipGroupOrder` 是 `-2147483647`，置顶的比它大，降序排；组是 `bIsGroup=1` 的一行（`mText` 是组名），组里条目的 `lParentID` 是组的 `lID`，不在组里是 `-1`，组可以嵌套；组里的顺序用 `clipGroupOrder`，不在组里用 `clipOrder`。`Data` 里一条一种格式：`CF_UNICODETEXT`（UTF-16，带结尾的 NUL）、`CF_TEXT`（系统代码页）、`CF_HDROP`（`DROPFILES`：第一个 DWORD 是名单起点，偏移 16 的 BOOL 是宽字符，名字间 NUL，最后多一个）、`CF_DIB`、`CF_DIBV5`、`PNG`、`HTML Format`、`Rich Text Format`，同一种格式有几行时 Ditto 取 `Data.lID` 最大的。老版本的库可能缺列。
- **做法**：读之前 `PRAGMA table_info(Main)`，缺的列当没有；按上面的单位和哨兵值读（`ClipImport.cpp` 的 `importDitto`）；Ditto 的组拍平成“父/子”，置顶的只在组里排前面，不放进 WinShun 的“固定”。

### 按空行分段时，正则吃掉了下一段开头的缩进

- **现象**：`  三` 这样带缩进的段落，首行的空格丢了。
- **原因**：分隔符写成“换行，加空白，再换行，加空白”，最后那个“空白”把下一段开头的缩进也吞了。
- **做法**：分隔符是“换行，再一行或几行只有空白的行，每行以换行结束”：`\r?\n(?:[ \t]*\r?\n)+`。`splitting` 用例。

### 中文版 Excel 存的 CSV 按 UTF-8 读全是乱码

- **现象**：导入 Excel 另存的 `.csv`，中文全成了问号或方块。
- **原因**：中文版 Excel 的“CSV”是 GBK，不是 UTF-8，也没有 BOM。
- **做法**：`decodeText` 先看 BOM（UTF-8、UTF-16 LE/BE），没有就严格按 UTF-8 解，出错（`QStringDecoder::hasError`）再用 `MultiByteToWideChar(936)`。`decoding` 用例。

## 长截图（阶段 6，2026-10-22）

### 投递的滚轮排在程序还没处理的按键前面：Word 把 Ctrl+L 的 Ctrl 当成还按着，缩放了

- **现象**：在 Word 上按 Ctrl+L 开始长截图，第一步滚轮让 Word 从 150% 缩到 140%，之后每一帧都对不上（4 分钟后才取消）。
- **原因**：`PostMessage(WM_MOUSEWHEEL)` 进的是投递队列，排在 Word 还没处理的键盘输入（Ctrl 抬起）前面；Word 处理滚轮时用 `GetKeyState` 看 Ctrl，读到的是“按着”，当成 Ctrl+滚轮。
- **做法**：`ScrollInput::keysHeld`：Ctrl、Shift、Alt、Win（SendInput 滚轮还加鼠标键）按着，或全部松开不到 150 ms 时不滚，返回 `ScrollKeysHeld`，引擎停 150 ms 再试；诊断里记 “post wheel keys held”。

### 管理员窗口在前时，不带修饰键的全局热键收不到

- **现象**：目标是管理员程序时，长截图临时注册的 Enter、Esc 按了没反应，Enter 进了那个程序。
- **原因**：（实测 2026-10-22，`hk6listen` / `hk6send`）前台是高完整性级别的窗口时，普通权限进程用 `RegisterHotKey` 注册的热键只有带 Ctrl、Alt、Shift 或 Win 的才触发；单独的 Enter、Esc、F13 都不触发。
- **做法**：这时的提示是“这个程序以管理员身份运行，请手动滚动，滚完点‘完成’”；截图快捷键（带修饰键）照样能结束。别指望单键热键在管理员窗口前生效。

### 自己的通知被当成吸底栏拍进长图

- **现象**：上一张“截图已保存”的通知还在右下角时开始长截图，结果最底下多了一块通知；通知窗口每帧都在同一处，拼接器把它当成吸底栏（诊断里 bottom 164 px）。
- **原因**：通知是系统的窗口，不能排除出捕获。鼠标停在上面时它不消失；这台测试机上只有脚本注入的输入时，一条通知在屏幕上停了 18 分钟。已经退出的进程留下的通知谁都撤不掉：对它的图标 `NIM_MODIFY`（`NIF_INFO`，空文字）和 `NIM_DELETE` 都返回成功，通知不动（实测 2026-10-23）。
- **做法**：开始长截图前 `Callbacks::longStarting` → `MessageWindow::hideNotification` 撤掉本进程的通知（实测开始后 170 ms 内消失）；通知是淡出的，第一帧等画面静下来再定（下面“开始时页面还在平滑滚动”）。测试脚本重启 Win顺 后先用 `toastx` 点掉旧进程留下的通知；拼接测试的区域别压在通知上。

### 重复的行：只共有几行时，任何位移都显得“很密”

- **现象**：Edge 自动滚动的结果比实际短 6880 px：有一步被判成“往回滚 1131 px”。
- **原因**：候选位移里有一类按“票数 / 共有行数”排；1151 行高的区域，往回 1131 px 只共有 20 行，那 20 行里人人都有的东西（空白、同样的边）让它的密度最高。
- **做法**：密度候选只收共有行数至少是可比较高度五分之一的位移；两帧至少共有 16 行（`kMinOverlap`，PgDn 一样大的步子也要能接）。`tst_stitch` 的 `alikeRowsFarApart`；用 `wsbench --scroll-replay` 重放那次的帧确认。

### 页面缩放不是整数倍时，同一行每帧画得不一样

- **现象**：Edge 的测试配置页面缩放约 130%，行距 85.8 物理 px，相邻两帧同一行的像素不全相同，最好的位移分数只有 0.80 左右（位移是对的）。
- **原因**：文字落在不同的亚像素位置，抗锯齿不同；WinUI 重画的字也会差到 8 级亮度。
- **做法**：段签名比平均亮度（容差 3），分数门槛 0.6，不要求逐像素相等；渐显、懒加载这种在重叠区里原地变的，靠“远好于其他位移”（≥ 0.35、≥ 160 票、2 倍）也收。

### 没为高 DPI 适配、又平滑滚动的列表会拼错

- **现象**：管理员测试窗口（PowerShell 的 WinForms 列表，DPI 不感知，150% 下被系统放大）手动滚完，图错位、行重复。
- **原因**：系统把 100% 的画面放大 1.5 倍，平滑滚动每帧移动的是小数个物理像素，两帧之间没有一个整数位移能逐行对上；列表每行又长得一样，投票选错。
- **做法**：已知限制，写进了使用说明（关掉自动滚动、慢慢滚）。现代程序（DPI 感知）不受影响。

### 开始时页面还在平滑滚动

- **现象**：Chrome 按 Ctrl+Home 后马上开始长截图，结果 24991 px，中间一段重复。手动长截图还没滚，面板就说“跟丢了”（2026-10-23，区域压着刚撤掉的通知）。
- **原因**：Chrome 的平滑滚动还在往上走，第一帧拍在半路，之后的帧和它之间多了一段反方向的移动。撤掉的通知要淡出一会儿，第一帧里是半透明的通知，后面的帧那一块都和它不一样。
- **做法**：第一帧也像每一步之后那样等静下来（100 ms 不变；自动滚动最多 2.5 s，手动最多 600 ms，用户可能已经在滚）。测试脚本开始前仍等画面静止（`Wait-Still`：相隔 300 ms 两次截图相同）。

### 测试：BitBlt、DXGI 都拍不到长截图的面板和边框

- **现象**：测试脚本截屏想看面板，什么都没有。
- **原因**：面板和边框都设了 `WDA_EXCLUDEFROMCAPTURE`（就是为了不进长图），所有截屏办法都看不到。
- **做法**：设了 `WINSHUN_SCROLL_DUMP` 时，完成或取消那一刻用 `grabWindow` 把面板存成那次 dump 文件夹里的 `panel.png`。

### 测试：编辑窗口里 Ctrl+S 保存后窗口就关了

- **现象**：脚本 Ctrl+S 之后再按 Esc“关窗口”，Esc 落到了下一个前台窗口。
- **原因**：编辑窗口的 Ctrl+S 是保存并关闭。
- **做法**：Ctrl+S 后等编辑窗口消失；只有它还在、而且在前台时才发 Esc。

### 测试：“自动滚动”开关记着上一次

- **现象**：脚本按固定次数点开关，有一次“手动”测试其实开着自动滚动。
- **原因**：开关是用户的选择，下一次长截图沿用（只在内存里，Win顺 重启回到开）。原先引擎跟丢后转手动也会留到下一次，已改成只算那一次（`m_autoScrollChosen`）。
- **做法**：脚本从开始那行日志（“auto-scroll on/off”）读出现在的状态，再决定点不点。

### 测试：隐藏启动的脚本第一次 ShowWindow 变成 SW_HIDE

- **现象**：提权的测试脚本对 Edge 的窗口调 `ShowWindow(SW_SHOW)`，Edge 被藏了。
- **原因**：脚本进程是隐藏启动的（`STARTF_USESHOWWINDOW` + `SW_HIDE`）；进程第一次调 `ShowWindow` 时，参数被忽略，用的是 STARTUPINFO 里的值，对别的进程的窗口也一样。
- **做法**：不对已经显示的窗口调 `ShowWindow`；只有最小化时才 `ShowWindow(SW_RESTORE)`，拉到前台用 `AttachThreadInput` + `BringWindowToTop` + `SetForegroundWindow`。

### UI Automation 的 ScrollPattern 会把目标拉到前台

- **现象**：探针对 Word、Chrome、Edge、设置调 `ScrollPattern.Scroll` 后，它们都抢过前台。
- **原因**：这些程序收到 UIA 的滚动请求时激活了自己的窗口。
- **做法**：长截图开始时目标本来就被放到前台，UIA 又是阶梯的第三级，影响不大；测试脚本每个按键前都看前台。

### 各个程序的滚动办法（实测 2026-10-22，150%）

- **现象和做法**：
  - Edge、Chrome：探针里（窗口不在前台）投递滚轮 `PostMessage` 成功但页面不动；长截图里窗口在前台，投递滚轮每步都动。UIA 也能滚，但一步 247 px，太粗。
  - VS Code、记事本（Win11 新版）、资源管理器详细信息、Word：投递滚轮管用（记事本一格 92 px，资源管理器一格约 118 px）；Word 要先等 Ctrl 松开（上面那条）。VS Code 没有 ScrollPattern；它的 `scrollBeyondLastLine` 让结尾多出将近一屏空白。
  - 设置（WinUI）：投递滚轮不动，UIA 的 ScrollViewer 也不动；SendInput 滚轮能滚（只在鼠标在区域里时用，否则提示“把鼠标放到要滚动的区域上”）。
  - 管理员程序：`PostMessage` 拒绝访问（5），直接手动。
  - 微信：要手机登录，没测。

### 测试：用户的 VS Code 正在更新时，另开的实例 30 秒就退出

- **现象**：用自己的 `--user-data-dir` 另开一个 VS Code 给测试用，窗口不出来，进程 30 秒后退出。
- **原因**：后台有 `CodeSetup-stable` 更新程序在跑，新实例等它，等不到就退出。
- **做法**：在用户已经开着的 VS Code 里打开测试文件，结束时只给那个窗口发 `WM_CLOSE`；不动用户别的窗口，不结束更新程序。

### 长截图之后内存不回来：驱动留着帧池的纹理

- **现象**：同样先截一张普通截图，再做一次长截图，commit 比只截普通截图多约 67 MB，一直不回。
- **原因**：WGC 帧池（两块整屏大小的纹理）和 staging 纹理放掉后，D3D11 的延迟销毁和驱动的缓存还留着；面板窗口隐藏后它的场景图资源也留着。
- **做法**：流结束时放掉 staging、帧池、session，再 `ClearState` + `Flush` + `IDXGIDevice3::Trim`；面板隐藏后 `releaseResources()`。之后同样的对比只多 11.6 MB（`vm6.ps1` 按分配大小列 private commit）；截 3 万 px 再结束，5 秒内回到开始前 +16.6 MB。另：阶段 5 的“框选后自动识别”在选区时加载 onnxruntime（约 +40 MB 常驻），不是长截图的。

## 截图（阶段 7，2026-10-23）

### 新加第三方目录后 ninja 报 `FindFirstFileExA(../../../??????/…)`（又是从 Bash 编译）

- **现象**：zxing-cpp 放在 `third_party/zxing-cpp/core`，`add_subdirectory(<那个目录> ${CMAKE_BINARY_DIR}/third_party/zxing-cpp)` 之后，ninja 报 `FindFirstFileExA(../../../??????/third_party/zxing-cpp/core/src): 文件名、目录名或卷标语法不正确`，什么也编不了；就算只编 `-n` 也报。
- **原因**：不是编译目录的问题本身。这是前面写过的那个坑（“从 bash 起的 pwsh 里跑 ninja”）：从 Bash 工具里经 `pwsh` 编译，`cl` 的 `/showIncludes` 路径里的中文和 ninja 的当前目录对不上，`.ninja_deps` 里存成了 `../../../电脑便捷工具/...`，下一次再读就报错。换成 VS 自带的 ninja 1.12.1 也一样（它也没有 UTF-8 清单）。第一个报错的恰好是新加的目录。
- **做法**：编译只从 PowerShell 工具起；坏了就删 `.ninja_deps`、`.ninja_log`，再把 `CMakeFiles\rules.ninja` 里的 `msvc_deps_prefix` 换成 UTF-8 的“注意: 包含文件: ”（重新生成后会被改回 `??: ????: `，每次编译前检查一次，scratchpad 的 `build.ps1` 做了这一步）。`add_subdirectory` 的编译目录也让它镜像源码目录（`${CMAKE_BINARY_DIR}/third_party/zxing-cpp/core`），这一点本身没有出过问题。
- **zxing-cpp 另外两处**：它的 CMake 用预编译头和 `AUTOMOC`（我们的 `qt_standard_project_setup` 全局开着），用 `set_target_properties(ZXing PROPERTIES AUTOMOC OFF DISABLE_PRECOMPILE_HEADERS ON ...)` 关掉；`ZXING_WRITERS` 开着只是为了单测里能自己生成二维码（静态库，不用就不链进来）。写二维码要 `setEncoding(CharacterSet::UTF8)` 并传 `std::string`，默认的 Latin-1 遇到汉字直接抛异常。

### 界面自动化：遮罩盖着，`ElementFromPoint` 命中的是我们自己

- **现象**：想用 `IUIAutomation::ElementFromPoint` 取鼠标下的控件，得到的是 Win顺 的遮罩。
- **做法**：从目标窗口的 `ElementFromHandle` 一层层往下走（`FindAllBuildCache(TreeScope_Children)`，缓存 `BoundingRectangle`、`IsOffscreen`），每层取包含这个点的子元素。

### Chromium（VS Code、Edge、微信）的界面自动化树要有人听事件才会建（已作废，见下一条）

- **现象**：另开的 VS Code 里，窗口的 `ElementFromHandle` 往下只有一个窗格，没有侧栏、编辑区；而测试脚本里用 `AutomationElement.FromPoint` 能看到完整的树（`TabItem`、`StatusBar`……）。
- **原因**：Chromium 只在发现有界面自动化的客户端在听或在问时才建网页内容的辅助功能树；光问外壳，不给内容。
- **做法（2026-10-23 撤销）**：当时让探针线程注册一个什么也不做的 `IUIAutomationFocusChangedEventHandler`（`FocusSink`）把树逼出来。这正是下一条的祸根：不要再这样做，Chromium 窗口根本不该问。

### Chromium 的界面自动化会让 VS Code 进入屏幕阅读器模式（截图悬停控件识别，2026-10-23）

- **现象**：开着截图悬停过 VS Code 之后，VS Code 状态栏出现“已为屏幕阅读器优化”（`editor.accessibilitySupport` 为默认的 `auto` 时），编辑器行为改变（折叠、缩略图、自动换行等被关），要重启 VS Code 才恢复。Chrome、Edge、新版微信等 Chromium 程序同样被打开整套无障碍。
- **原因**：Chromium 收到界面自动化的请求（`WM_GETOBJECT` 的 `UiaRootObjectId`、读元素属性、事件监听）就升级无障碍模式（`legacy_render_widget_host_win.cc` 的 `OnGetObject` → `AXPlatform::SetUiaRequested`；`AXPlatformNodeWin::OnPropertiesUsed` → `BrowserAccessibilityStateImpl::OnPropertiesUsedInWebContent`；扩展属性触发 `DiscoverAssistiveTech`，可加上 `kScreenReader`）。Electron 的 `app.accessibilitySupportEnabled` 看的是 `kAXModeComplete`，VS Code 把它经 `accessibility-support-changed` 转给每个窗口（`isScreenReaderOptimized`：设置为 `auto` 且支持已开）。一旦开了，Chromium 不会自己关。`FindAllBuildCache(子元素)` 还会逐个读子窗口的元素，所以宿主窗口里只要嵌了 Chromium 子窗口（WebView2、CEF、Qt WebEngine）也一样。`WindowFromPoint` 不会：它只在 user32 里，不给别的进程发消息。
- **做法**：Chromium 的窗口不问界面自动化：`WindowSnapshot::isChromiumClass`（`Chrome_WidgetWin_*`、`Chrome_RenderWidgetHostHWND`、`Intermediate D3D Window`、`CefBrowserWindow`）、`hostsChromium`（顶层或任何子窗口，含隐藏的）、`wantsUia` 都先拒绝这些类名；`ScrollInput` 的 `Uia` 一级在区域中心的窗口链上有 Chromium 时直接 `ScrollNoPattern`；删掉 `UiaProbe` 的 `FocusSink`（全局事件监听会让整台机器上所有 Chromium 程序都收到事件，探针去问 Notepad 也一样）。这些窗口只用 Win32 子窗口的矩形。测试：`tst_stage7` 的 `uiaIsNeverAskedOfChromium`。
- **测的时候注意**：这台机器上有常驻的无障碍客户端（输入法或别的，没查出是哪个），任何新开的 VS Code 几秒内（实测约 2.6 s 时 no、5 s 时 yes）就自己变成 `Screen Reader: yes`（`bin\code.cmd --status --user-data-dir …` 读得到），所以“VS Code 有没有进屏幕阅读器模式”在这里区分不出两个版本，要看 Win顺 的日志：有 “UI Automation gave N controls” 还是 “UI Automation skipped for a Chromium window”。另开的测试实例的状态栏在两个版本下都没出现“屏幕阅读器”一项（用户自己的 VS Code 出现过），悬停前后各截一张图也看不出差别。实测（2026-10-23，`WINSHUN_LOG_INFO=1`）：旧版悬停 VS Code 日志 80 行 “UI Automation gave N controls”（连状态栏、侧栏都框得出），新版只有一行 “skipped”、悬停框是整个窗口，计算器（`ApplicationFrameWindow`）照样框出按钮。`HKCU\Software\Microsoft\Narrator\NoRoam\RunningState` 如果残留为 1（Narrator 没在跑），Chromium 的辅助技术探测会把它当成 Narrator 在跑，也会加上屏幕阅读器模式。
- **同类**：`TextCaret`（剪贴板窗口找输入光标）早就对 Chrome 系和 Firefox 不用 UI Automation。截图探针仍会问 Firefox：它被问到也会打开自己的无障碍支持、之后变慢，但不改变界面。

### Chromium 把内容包在十几层同样大小的窗格里，只取“最后一个包含这个点的子元素”会走进死路

- **现象**：能进到 `Document`，往下一层却是 0 个子元素，答案只有一个窗口大小的矩形（日志 “UI Automation gave 1 controls”）。
- **原因**：窗口下有两个窗格（Win32 的渲染子窗口和网页内容的根），网页内容里又套了七层同样大小的窗格、`Group`、`Document`、`Group`，同一层有两个同样大小的窗格时后一个是死路。
- **做法**：每个包含这个点的子元素都试，留最深的一条（`UiaProbe.cpp` 的 `dive`：深度 32、最多读 400 个元素、总共 150 ms）。链里同样大小的矩形去重（`uiaControls`），所以日志里“N 个控件”比层数少很多。
- **后续修正（2026-10-23，GitHub #59）**：仅按链深度仍会让层层包装的大窗格胜过另一条较浅分支里的小按钮。现在优先比较链中最小的有效矩形面积，大小相同再比较深度；元素扫描循环也遵守 400 项和 150 ms 的预算。回归测试覆盖“小按钮 vs. 20 层同尺寸包装”，不把辅助功能树的层数当成控件识别精度。

### 测超时：把目标进程挂起

- **做法**：`NtSuspendProcess`（`PROCESS_SUSPEND_RESUME` 0x800）挂起测试用的 VS Code，鼠标移上去：界面自动化的调用等满事务超时（150 ms），答案 0 个控件、超出预算，悬停框是子窗口，鼠标移到别的窗口 54 ms 内悬停框就跟上（说明没在界面线程上等）。`finally` 里一定 `NtResumeProcess`。

### 带鼠标指针：句柄不是你读到的那个

- **现象**：测试脚本在按热键前用 `GetCursorInfo` 读到箭头指针（句柄 65539，热点 1,0），存下的 PNG 里的指针却和用这个句柄 `DrawIconEx` 画的差 423 个像素。
- **原因**：按下热键的那一刻，系统的当前指针是另一个箭头句柄（65561，热点 1,1）；Win顺 画的是那一刻的，没错。
- **做法**：参照图用日志里写的句柄和位置（`Screen capture: mouse cursor drawn (handle N) at x,y`）来画；整张图 0 像素差。


### 截图悬停框认不到 Win顺自己的设置窗口

- **现象**：2026-10-23 用户反馈“窗口识别不准，比如 Win顺的设置窗口就识别不到”。鼠标放在设置窗口上，悬停框框到了它后面的 VS Code 控件（1162×29），不是设置窗口。
- **原因**：`WindowSnapshot` 的 `skipped()` 把同一进程（`GetWindowThreadProcessId == GetCurrentProcessId()`）的窗口全部跳过，本意只是排除截图遮罩和贴图，结果设置窗口、编辑窗口等 Win顺的普通窗口也一起没了。
- **做法**：只排除截图自己的东西：遮罩按句柄（`WindowSnapshot::take(overlays)`，`Capture::start` 传入），贴图和倒计时窗口按类名（`WinShun.Pin`、`WinShun.Countdown`，`isCaptureSurfaceClass`）。别再按进程号排除。实测：设置窗口上悬停框 1380×1080，正好是它的 `DWMWA_EXTENDED_FRAME_BOUNDS`。

### 放大镜右边一条空白（盒子比放大的像素宽）

- **现象**：2026-10-23 用户反馈“光标右下角的放大镜右边什么都没有，像屏幕坏了”。放大的 15×15 像素只占盒子的左边约 120 逻辑像素，右边是一条空的背景。
- **原因**：盒子宽度取了放大图、位置行、颜色行、提示行 `implicitWidth` 里最大的；阶段 7 的颜色格式文字和“C 复制 · Shift+C 换格式”比放大图宽，盒子被撑宽，放大图却是固定宽度。
- **做法**：盒子宽度就等于放大图宽度（`loupe.width + 2`），下面各行 `elide`；放大图每格 8 → 11 逻辑像素（`CaptureOverlay.qml` 的 `magnifier.cell`），好让提示行放得下。实测 150% 下放大图铺满盒子，在屏幕中间和右下角（翻到光标左上）都是完整的 15×15 格。

### 测试脚本

- 原生弹出菜单是自绘的，界面自动化看不到菜单项（一个空的 `Pane`）：用键盘走（第一层从没有选中的状态 `Down` n 次；开子菜单按 `Right`，子菜单里第一项已经选中，再 `Down` n−1 次；`Enter`）。
- 方向键要带 `KEYEVENTF_EXTENDEDKEY`（`Shift`+方向键否则变成小键盘键，`Shift` 被吃掉）。
- 启动器和遮罩都是 Qt 窗口：“前台是 WinShun 的 Qt 窗口”不能当成“遮罩出来了”，还要看它是整块屏大小。热键没成功时前台的是启动器，脚本的守卫会挡住后面的键，但要查清为什么。
- 改 `WinShun.ini` 的 `Hotkey=` 要限定在 `[Capture]` 段里：第一个匹配的是 `[Launcher]` 的，把截图热键设给它，截图键就注册不上、按下去打开了启动器。
- PowerShell 的 `@(a + b, c)` 逗号比加号结合得紧，加括号。
- 自绘菜单里“全部置顶”会把贴图落到测试窗（也是置顶的）后面，之后点不到它：测这一项时让贴图有一部分在测试窗外面。
- 翻译：`lupdate` 看不到包了一层的 `tr`（`say("...")` 之类），直接写 `QCoreApplication::translate("上下文", "...")`；`wscapture` 库里的文件要加进 `src/app/CMakeLists.txt` 的 `qt_add_lupdate(... SOURCES ...)`。

## 拖放圆盘（2026-10-23）

### 怎么知道“正在拖文件”：看来源线程的捕获窗口是不是 `CLIPBRDWNDCLASS`

- **现象**：只看“按住左键在动”，框选、拖窗口、拖滑块、在文字里拖选也都算，晃一晃就会误弹。
- **原因**：`DoDragDrop` 期间 OLE 在来源线程上建一个 `CLIPBRDWNDCLASS` 类的隐藏窗口并用它捕获鼠标；别的左键拖动捕获的是程序自己的窗口，或者没有捕获。
- **做法**：晃动成立时取按下点 `WindowFromPoint` 的线程（取不到用前台线程，`GetGUIThreadInfo(0)`）的 `GetGUIThreadInfo().hwndCapture`，类名是 `CLIPBRDWNDCLASS` 才弹。它只说明“有 OLE 拖动”，拖的是不是文件要等 `DragEnter` 看数据。Total Commander 是例外，见下一条。

### Total Commander 在自己窗口里拖文件不是 OLE 拖放，晃了不弹

- **现象**（2026-10-23 实测，TC 11.58）：在 TC 的文件列表里按住文件、就在 TC 窗口里晃，圆盘不出来；拖出窗口再晃才出来。资源管理器、Directory Opus、XYplorer 没有这个问题。
- **原因**：TC 在自己窗口里拖时是自己捕获鼠标（捕获窗口是主窗口，类名 `TTOTAL_CMD`）、自己画、自己判断放在哪个面板；指针到了别的程序的窗口上才调 `DoDragDrop`，捕获才变成 `CLIPBRDWNDCLASS`。
- **做法**：按下点那个线程的捕获是 `TTOTAL_CMD` 时也当作候选（`KeyListener` 的 `onDrag`，通用的 `CLIPBRDWNDCLASS` 判断不动；`GetGUIThreadInfo` 的 `flags` 有 `GUI_INMOVESIZE` 或菜单模式时都不算，拖 TC 的标题栏时捕获也是 `TTOTAL_CMD`），照常建圆盘。圆盘窗口就在指针下面，TC 下一次鼠标移动就转成 OLE 拖放，`DragEnter` 随后就到（实测在显示后 30 ms 内），复制、移动都成功。等不到 `DragEnter`（比如 TC 拖的是按钮栏上的按钮）就按上一条“不动的指针”的规则收起，什么也不做。

### Qt 的窗口已经注册了放下目标，自己的 `RegisterDragDrop` 失败

- **现象**：`RegisterDragDrop` 返回 `DRAGDROP_E_ALREADYREGISTERED`，拖过来时进的是 Qt 的 `QWindowsOleDropTarget`。
- **原因**：Qt 建平台窗口时自己注册一个放下目标；以后只有窗口重建（句柄变了）才会再注册。
- **做法**：`RevokeDragDrop` 掉 Qt 的再注册自己的（`DropPie::takeDropSite`）；每次显示前比一下 `winId()`，变了就重新接管。析构时窗口还在就 `RevokeDragDrop`，并让放下目标与控制器断开（`detach`），晚到的回调什么也不做。

### 透明窗口的透明像素照样接鼠标和拖放

- **现象**：圆盘窗口比圆环大（四周留着提示和底栏的地方），拖到圆环外面的透明区域，放下还是落到我们窗口上，下面的资源管理器收不到。
- **原因**：Qt Quick 的透明窗口是整块矩形参与命中测试的，不按像素的透明度。
- **做法**：指针一离开圆环（外圈外 12 像素）就立刻藏起窗口并返回 `DROPEFFECT_NONE`，OLE 下一次移动就会找到下面的窗口；窗口本身也只占这一台显示器上必要的大小。

### 拖动图像是 `IDropTargetHelper` 的 SysDragImage 窗口画的

- **现象**：自己实现 `IDropTarget` 后，拖到圆盘上文件的缩略图不见了，或者圆盘收起后缩略图停在原地。
- **原因**：拖动图像由 `CLSID_DragDropHelper` 管的一个 SysDragImage 窗口画；目标不把 `DragEnter`、`DragOver`、`DragLeave`、`Drop` 转给它，它就不画或不收。我们提前收起（指针离开圆环、`Esc`）时 OLE 还没发 `DragLeave`。
- **做法**：四个回调都在处理完后转给 helper；提前收起时自己先给 helper 一次 `DragLeave`（`DropTarget::leaveEarly`），之后 OLE 真的 `DragLeave` 来了不再转（不然重复）。

### 窗口出现在不动的指针下面，OLE 不会对它 `DragEnter`（设计时的考虑，未实测）

- **担心**：晃完手停住，圆盘窗口已经在指针下面，可是等不到 `DragEnter`，300 ms 后圆盘以为拖动没过来就收起了。
- **原因**：`DoDragDrop` 只在有鼠标或键盘输入时重新找指针下的目标（文档里的说法，没有单独测过）。
- **做法**：显示后 300 ms 没有 `DragEnter`：指针没动（4 像素内）就接着等，每 100 ms 看一次，最多 1.5 秒；动了还没来才收起。真机试时看日志里 “hidden, the drag did not come over it” 出现得是否太早。

### 管理员程序在前台时可能收不到 Raw Input（未实测）

- **担心**：从以管理员身份运行的程序里拖文件出来晃，圆盘不出来。
- **原因**：UIPI 下，完整性级别高的窗口在前台时，普通权限进程的 `RIDEV_INPUTSINK` 可能收不到鼠标输入。拖放本身不受影响：管理员进程拖给普通程序 UIPI 不挡（见“把结果拖出去”一节的实测）。
- **做法**：收到晃动就照常弹出（日志写明来源是管理员程序），不显示“接不到”之类的说明。真机试时确认一次。

### 移动时不对来源返回 `DROPEFFECT_MOVE`（设计时的考虑，未实测）

- **担心**：放下时我们自己用 `IFileOperation` 移动了文件，如果还向来源报告 `DROPEFFECT_MOVE`，没有做“优化移动”的来源会在 `DoDragDrop` 返回后自己去删原文件，和还在进行的文件操作撞上。
- **做法**：真实文件由我们的 `IFileOperation` 在自己的线程上完成，`Drop` 一律返回 `DROPEFFECT_NONE`；只有转给文件夹放下目标的虚拟项目才返回它给的效果。

### 测试：临时副本用新的 `--profile` 启动，把开机自启的 Run 值改成了自己

- **现象**：用 `F:\wsstage\pie\WinShun.exe --profile <新文件夹>` 测完，`HKCU\...\Run` 的 `WinShun` 变成了 `"F:\wsstage\pie\WinShun.exe" --background`；重启用户那份并不会改回来。
- **原因**：新配置的开机自启默认开着；普通权限注册不了计划任务（0x80070005），就退回写 Run 值，写的是自己的路径（`Shell.cpp` 的 `writeRunValue`）。
- **做法**：测完把 Run 值改回 `"F:\电脑便捷工具\dist\WinShun\WinShun.exe" --background`（和资源管理器右键菜单一样要检查）；或者测试配置里先关掉开机自启。

### 靠屏幕边弹出时圆环被切掉；把指针移到圆心，拖放跟得上

- **现象**（2026-10-23 实测，150%）：离工作区右边 60 逻辑像素晃，圆环右边被切掉约 53 像素。原先 `placeCentre` 只许圆心离指针不超过内圈半径，好让指针落在中间或内圈。
- **做法**：整个圆环放进指针所在显示器的工作区；圆心离指针超过“内圈外沿 − 6”时用 `SetCursorPos` 把指针移到圆心，等 `DragEnter` 确认是文件之后才移（`DropPie::dragEnter`；拖文字也会过晃动这一关，那时指针不能跳）。实测：OLE 的拖放循环跟着新位置走，拖动图像（SysDragImage）立刻跟过去，后面的 `DragOver`、放下都按新位置算，复制成功；`SetCursorPos` 不产生 Raw Input，不会被当成又一次晃动。
- **测试时的坑**：测试脚本用绝对坐标的 `SendInput`（`MOUSEEVENTF_ABSOLUTE`）晃，晃动成立时后面几步还在输入队列里，它们把指针又放回脚本以为的位置，看起来像“没有移过去”。真鼠标和触摸板发的是相对移动，从圆心接着走；测这件事要用相对移动晃（`pielive\ex-cases.ps1` 的 `c14r`）。数位板、远程桌面这类发绝对坐标的，晃完手还在动时指针会回到手的位置，那时就是原来的效果。

### 拖动一结束来源就删的数据：回收站、压缩软件的临时副本

- **现象**（代码审查，2026-10-24）：放下时立刻对来源返回、过后才在自己的线程上按 `CF_HDROP` 的路径复制。压缩软件（7-Zip、WinRAR、Bandizip）给的是解压到 `%TEMP%` 的副本，`DoDragDrop` 一返回它就删；回收站给的是 `$R…` 名字。
- **做法**：`DragEnter` 时有 `CFSTR_SHELLIDLIST` 就只读它（不碰 `CF_HDROP`，压缩软件可能在那次调用里才解压）；不全是“长久的文件”（没有文件系统路径、在回收站里、在临时文件夹下）就趁来源还拿着，交给目标文件夹自己的放下目标。长久的文件照旧自己复制；来源是异步的（资源管理器）时 `StartOperation`，复制完 `EndOperation`。

### 关掉圆盘时界面卡住：复制线程归圆盘，析构时在界面线程上等

- **现象**（代码审查，2026-10-24）：复制正等着“替换还是跳过”时在设置里关掉圆盘，`~FileOperations` 在界面线程上等那条线程，Win顺 一直卡到对话框被回答。
- **做法**：`FileOperations` 归 `App`，活到进程结束；圆盘随时可删。回调不碰圆盘（只写日志、经 `App` 弹通知）。

### 不提权的 Win顺 在第二块盘上给严格插件的数据文件夹打不上低完整性标签（“数据文件夹不能用”）

- **现象**（2026-10-24，0.13.0 截图）：截图配置在 `F:\电脑便捷工具\tools\screenshots\work\profile`，按意思找显示“语义索引停下了：数据文件夹不能用”；`plugins.log` 里有“无法给 …\winshun.semantic.data 打低完整性标签”，`winshun.tools` 也一样。0.12.0 的截图没事，是因为那时截图工具用提权的 Win顺。
- **原因**：设置强制完整性标签要 `WRITE_OWNER`。Windows 给新格式化的盘根目录的默认 ACL 是 Authenticated Users“修改”（没有 `WRITE_OWNER`），Administrators、SYSTEM 完全控制；不提权的进程在那里建的文件夹虽然归自己，也打不上标签（`icacls /setintegritylevel` 同样“拒绝访问”）。`%LOCALAPPDATA%` 里用户自己是完全控制，所以装好的 Win顺 一直没事；便携版放在 D:、F: 这类盘上（不提权是默认）、或 `--profile` 指向那里时就会中。标签没打上，低完整性的插件写不进去（不能往上写），`Store::open` 建不了 `semantic` 子文件夹。另外 `dataDirFor` 只在新建文件夹时打标签，失败一次以后永远不再试。
- **做法**：`labelLow` 遇到拒绝访问时先给当前用户加一条可继承的完全控制（文件夹的主人隐含 `WRITE_DAC`，能改 DACL），再打一次标签；`dataDirFor` 对已有但没有低标签的严格插件文件夹也补打（先用 `isLabelledLow` 读，不每次都重打——重打会把标签传到里面每个文件）。`tst_plugin::labelLowWithoutWriteOwner` 用复刻盘根默认 ACL 的文件夹测。

## 命令行 ws（2026-10-24）

### 拷回来的源文件带着旧的修改时间，ninja 不重编它

- **现象**：计时对比时把 `src/cli/main.cpp` 临时换成另一种写法、编好，再用 `Copy-Item` 把原文件拷回来重编：main.cpp 没有重新编译，ws.exe 和临时那版的哈希一模一样。
- **原因**：`Copy-Item`（`CopyFile`）保留源文件的修改时间。拷回来的文件比临时版编出的 .obj 旧，ninja 按时间认为它没变。
- **做法**：拷回之后把修改时间改成现在（`(Get-Item 文件).LastWriteTime = Get-Date`），或者用编辑工具写回；编完核对产物（哈希，或 `Select-String` 查只有临时版才有的字符串）。

### `//` 注释以反斜杠结尾，下一行也成了注释

- **现象**（编译前检查出来的）：`src/cli/Connect.h` 里一个成员的注释写成 `// … without \\.\pipe\`，下一行的成员声明会被并进这条注释，悄悄消失，编译错误会报在用到它的地方。
- **原因**：续行在去掉注释之前处理（翻译阶段 2 在 3 之前）：行尾的 `\` 把下一行接上来，`//` 注释里也一样。
- **做法**：注释里的路径不以 `\` 结尾（写 `D:\…`、加引号，或换个说法）；写完用 `rg '\\$' src/cli` 查一遍。

### 测试替 ws 加的参数放在后面：遇到 `--` 就成了查询词，连到用户真的管道

- **现象**（写 `tst_cli` 时核对代码发现，没真跑出来）：测试的辅助函数把 `--pipe <测试管道>` 追加在用例参数的后面。`ws -- recent` 这类用例里，`--` 之后全是查询词，`--pipe` 不再算参数，ws 连的是默认管道：用户正开着的 Win顺。
- **做法**：测试自己的参数（`--pipe`、`--instance`）一律放在最前面。另外用 `WS_TEST_START`（写文件代替拉起 Win顺）、`WS_TEST_OPEN`（写文件代替打开）、临时的 `CLAUDE_CONFIG_DIR` 兜底，万一连错了也不会动到用户的东西。

### `recent_files` 在 AppData 下的文件夹里一个也不给

- **现象**：`tst_cli` 在 `QTemporaryDir`（`%TEMP%`，在 AppData 下）里建文件，`ws recent --in <那个文件夹>` 退出码 1，什么也没有。
- **原因**：`recent_files` 默认不列隐藏的项和“系统、程序、工具文件夹”里的项（`EntryFlag::Hidden`、`LowPriority`，都往下继承），要 `include_hidden`、`include_system` 才列。AppData 本身是隐藏文件夹，在 Win顺 的设置里又算工具文件夹，所以它下面的全被滤掉。
- **做法**：ws 对 `--in` 的文件夹第一次问不到时，带上这两个再问一次，并在 stderr 说明（`docs/design-cli.md` 第 3 节）。不一开始就带：`ws recent --in D:\项目` 会被 node_modules 里的改动淹没。

### 注册表读不出来不等于“没有这个值”：PATH 被整个换成一项（H1，2026-10-24 复查）

- **现象**：用户的 `Path` 如果是 `REG_BINARY` 之类（或两次读之间变长了），“启用命令行”把它当成空，写回 `<bin>` 一项：用户原来的整个 PATH 丢了。“从 PATH 移除”读不出时照样删掉记录、报成功，PATH 里的那一项还在。安装程序里同样的写法（`RegQueryStringValue` 失败就当没有，`Path := Folder`）会改系统 PATH。
- **原因**：`readValue` / `RegQueryStringValue` 失败只返回“没读到”，不分“没有这个值”和“有但读不了”。
- **做法**：只有 `ERROR_FILE_NOT_FOUND`（安装程序里是 `RegValueExists` 为假）才是没有；类型不对、没权限、一直变长都是错误，原样返回，不写、不动记录，界面写明原因；`ERROR_MORE_DATA` 整个重读。写之前再读一遍对比，变了就从头来，别拿旧快照覆盖别的程序刚改的 PATH。测试用 `REG_BINARY` / `REG_DWORD` / `REG_MULTI_SZ` 的值，在两次读之间的钩子里把值写长。

### PATH 里一项带 `;`：按 `;` 切就认不出它，每点一次追加一次

- **现象**：`bin` 的路径里有 `;` 时，原来不加引号追加，之后“已经在 PATH 里吗”按 `;` 切成两半，永远认不出，每点一次“启用命令行”多一项；Windows 自己读 PATH 时把 `"C:\a;b\bin"` 当一项。
- **做法**：写成带引号的（`entryOf`），所有切 PATH 的地方（`hasEntry`、`analyse`、移除）按引号切（`splitPath`，拼回去一个字节不差）；记录里存写进去的原样文字，移除时只拿掉和它**一模一样**的那一项（原来拿掉所有写法，包括用户自己写的）。

### `GetLongPathNameW` 对 PATH 里的每一项都问，共享上的项要等很多秒

- **现象**：比较 PATH 项时对每一项调 `GetLongPathNameW`（为了展开 `PROGRA~1`）；PATH 里有一个断开的共享，设置页就卡住，窗口激活一次读一次。
- **做法**：只有带 `~` 的才问文件系统（`normalized`）；测试里用保留给文档的 `192.0.2.1` 的共享做回归（不该碰它，所以不等）。`ws` 里同理：`absolutePath` 只对带 `~` 的存在的路径展开。

### 拼音匹配把英文字母当首字母：“ws”匹配每个 “Windows”

- **现象**：设置里搜 “ws” 出 21 项，大半是写着 Windows 的项，“ws 命令” 那几行混在里面。
- **原因**：两层。`findWord` 允许词中间的匹配（`Windows` 的 `ws` 在词尾，分数 80）；拼音匹配器把文字里的英文字母当成它自己的首字母，`ws` 又在 `Windows` 的末两个字母上匹配成“首字母”（68 分）。
- **做法**：1 到 2 个 ASCII 字符的词只在词首或整词匹配（`Term::atWordStart`，3 个字母以上照旧可以在词中）；拼音匹配里一个全是 ASCII 的片段不算拼音匹配（只是找错了位置的词）。回车去的是得分最高的那一行（`ws 命令`，标题以这个词开头，100 分）；各行仍按页面里的先后排列，不按分数重排。

### 一个变量设在注册表里，Win顺 起的子进程却看不见（`CLAUDE_CONFIG_DIR`）

- **现象**：用户 `setx CLAUDE_CONFIG_DIR` 之后，设置页的“装 Skill”还是装到老地方；Win顺 以管理员运行时，子进程拿到的是管理员的环境。
- **原因**：`CreateProcessW(…, lpEnvironment = nullptr)` 继承 Win顺 启动时的环境；提权时用的 `CreateProcessWithTokenW` 不传环境块，也不是明确“用这个令牌的用户的环境”。
- **做法**：`CreateEnvironmentBlock`：不提权时 `bInherit = TRUE`（用户的变量按注册表现在的样子盖在自己的环境上，父进程设的仍在）；提权时用桌面 shell 的令牌、`bInherit = FALSE`，加 `CREATE_UNICODE_ENVIRONMENT`。要链 `userenv`。“已装”以 ws 打印的路径为准，不用 Win顺 自己猜的。

### 第二次 `--settings --settings-at` 转交给运行中的 Win顺 时位置丢了

- **现象**：`WinShun.exe --settings --settings-at 9,0` 对已经开着的 Win顺 只打开设置窗口的第一页。
- **原因**：`main.cpp` 把第二次启动的参数转成命令字符串，`--settings` 只转成 `settings`，`--settings-at` 没人转；只有 Win顺 自己第一次启动时（`StartOptions::settingsAt`）才认。
- **做法**：命令改成 `settings:<位置>`（`SettingsCommand.h`），`App::handleCommand` 解开后调 `showPlace`。位置的格式 `<页>,<标签>`（从 0 数）或 `indexAccess`，写在 architecture.md 的命令行参数一段。没在真机上对着运行中的 Win顺 试过（那会动用户的窗口）：解析有单元测试，`showPlace` 本身由 `--preview-settings` 的搜索探针走过。

### 开关打开后上面多出几行，把刚点的那一节顶出窗口

- **现象**：设置 → AI 助手，点“启用命令行”，开关打开，上面多出权限那几行、页面多出标签，“命令行”一节被顶到窗口下面看不见。
- **原因**：行是带动画出来的：只在改变那一刻补偿滚动位置不够，补偿完它们还在长。
- **做法**：`SettingsWindow.holdInView(item, change)`：记下那一节在窗口里的位置，做改变，之后页面每次变高（`implicitHeight`）就把滚动位置补回去，守 0.8 秒或用户一滚动就放开。`--preview-settings … --hold-demo` 前后各抓一张并比较位置（150% 下相差 1 个设备像素以内）。

### 管道名里只有用户的 SID：谁都能先建一根

- **现象**（推演，没有真实的攻击样本）：同一用户下的任何程序可以先建 `\\.\pipe\WinShun.Mcp.<SID>`，ws 连上去的是它；`ws open` 会照它回答的路径打开文件。
- **做法**：`ws open` 在发任何东西之前，用 `GetNamedPipeServerProcessId` 看管道另一头是不是安装位置里的 `WinShun.exe`；连接用 `SECURITY_IMPERSONATION`，服务端读到数据后才能模拟，所以先查再写就连模拟的机会也没有。别的命令只读，不查。

### 测试文件里 `#include <shlobj.h>` 之后变量名 `small` 变成了 `char`

- **现象**：`tst_cli.cpp` 里 `const bool small = …` 报“语法错误：意外的 char”，指着完全无关的一行。
- **原因**：`rpcndr.h` 有 `#define small char`，`shlobj.h` 带进来了。
- **做法**：换名字（`single`）；这类名字：`small`、`near`、`far`、`min`、`max`、`interface`。

### 设置搜索：两个字母的拼音首字母在关键字当中也匹配（“ws”命中“允许 AI 修改设置”）

- **现象**：设置里搜 `ws`，“允许 AI 修改设置”排在前面，它和命令行 ws 毫无关系。
- **原因**：它的关键字“帮我设置”读成首字母是 b-w-s-z，`ws` 正好是中间的“我设”。`findWord` 早就让短词（一两个字母；拼音匹配器要两个字母起）只在词首或整词匹配（`Term::atWordStart`），`findPinyin` 没有这条：中文没有空格，“跟在另一个汉字后面”也算词首，所以拼音那条路把它放了进来。
- **做法**：`findPinyin` 对 `atWordStart` 的词，匹配只能从文本开头或分隔符（`separates`，字段文字没有像查询那样先 NFKC，全角的“，”“；”按它们变成的 `,` `;` 算）后面开始；Matcher 只给第一处，被拒了就从下一个位置接着找，后面的词里还有就还能命中（“帮我设置 我设”仍命中 `ws`）。三个字母以上照旧在哪里都能匹配。`tst_core` 的 `settingsMatch` 用那一行的真实关键字测。

### 状态文字多出一行，把下面的一栏连同按钮顶下去

- **现象**：设置 → AI 助手，ws 第一次被调用后，“允许 AI 助手和命令行连接”那一行多了一行“命令行：N 次调用…”，下面的“命令行”一栏和它的按钮往下挪了约 35 像素；正要点按钮的人点偏了。
- **原因**：调用次数是那一行 `description` 的第二行，它在后台的 ws 第一次运行时才出现，行高跟着变，下面所有东西都得挪。
- **做法**：会在没人操作时冒出来的文字，不放在别的行的文字里，也不放在别的东西上面。次数放在“命令行”一栏自己的最后一行（`SettingsEditor::cliTally`，没有调用时写说明），开关开着时一直在：第一次调用只换字，上面不动，这一栏也不变高。`--preview-settings` 看 150% 下的样子；`--no-cli-tally` 是还没有调用的一行。
