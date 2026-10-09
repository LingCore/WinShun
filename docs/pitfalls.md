# 踩坑记录

开发中遇到过、光看文档或代码想不到的问题。每条写现象、原因和现在的做法；改相关代码之前先看一眼。新遇到的坑补在对应分类里。

设计上的取舍（为什么读 MFT、为什么用 Raw Input、为什么要 D3D11 + FreeType 等）见 README 的“几个设计取舍”。

## 先想清楚：屏幕缩放和分辨率

下面很多坑的根源都在这里。设计和改动窗口、拖动、布局、图片、命中测试时，先过一遍：

- **每个数字都要标明单位**：逻辑像素还是物理像素。Win32 消息（`WM_MOVING`、`WM_NCHITTEST`、`GetWindowRect`）用物理像素，Qt 和 QML 用逻辑像素。在两者交界的地方换算一次，别的地方不要混用。
- **距离和阈值按逻辑像素定**，并想一想在 100%、150%、200% 下各是什么手感。吸附距离 12 逻辑像素，在 150% 下就是 18 物理像素。
- **小数缩放会有取整误差**：125%、150%、175% 下，逻辑像素换成物理像素可能差 1 像素。比较位置时，两边要用同一种单位，或者允许 1 像素的误差。
- **其他情况也要考虑**：多台显示器缩放不同、窗口在显示器之间移动、工作区比整个屏幕小（任务栏占掉一块）、低分辨率的小屏幕，以及程序运行中改了分辨率或缩放。

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

- **现象**：托盘右键菜单第一次打开比之后慢。改之前，第一次要 40 ms 才出现、55 ms 才画好；之后 4 ms 出现、20 ms 画好。
- **怎么量**：
  - 提权脚本向消息窗口投递托盘回调（`WM_APP + 1`，`lParam` 低位是 `WM_CONTEXTMENU`）来打开菜单。
  - 两个时刻：一是等 `#32768` 窗口可见，二是连续截取菜单区域，直到画面不再变化。只量到窗口可见，会漏掉第一次绘制。
  - 用 `Get-Process -Module` 对比第一次打开菜单前后，看进程多加载了哪些 DLL。
- **原因**：
  - **输入法**：弹托盘菜单前必须 `SetForegroundWindow`。线程第一次获得焦点时，Windows 会把当前输入法加载进进程。这台机器用的是微信输入法，会带进来 `wetype_tip.dll`、`CrashRpt1500.dll`、`d2d1.dll`、`DWrite.dll`、`textinputframework.dll` 等十几个 DLL。
  - **任务计划程序**：原来每次弹菜单前，都要同步查询它来决定“开机自动启动”是否打勾。在新进程里第一次查要 23–33 ms。
  - **Windows 自身**：进程第一次成为前台约 11–14 ms（之后 0.01 ms），第一次创建菜单约 18 ms（之后 5–7 ms）。这两项程序这边去不掉。
- **做法**：
  - 启动 1 秒后的预热里（`App::prewarmLauncher`），在主线程上创建并激活 `ITfThreadMgr`（`win::prepareTextInput`），输入法就提前加载好了。这个对象永不释放，因为退出时 COM 已经卸载。
    - 不对 TSF 文档管理器调用 `SetFocus`：多省约 3 ms，但可能干扰搜索框里的中文输入。
  - “开机自动启动”的状态改为缓存。启动时由 `adoptIfOrphaned` 填好；`setEnabled` 改完后重新读一次；每次菜单关闭后，在后台线程刷新一次（`autostart::refresh`）。用一个计数防止旧的刷新结果覆盖新设置的值。
  - 结果：第一次 26–30 ms 出现、40–46 ms 画好（之后不变）。
- **试过、没用的办法**：
  - 预加载菜单主题（`OpenThemeData`），或者先用菜单字体量一遍中文：没效果。
  - `LoadLibrary` 预加载 `TextShaping`、`CoreUIComponents`、`threadpoolwinrt`：DLL 确实提前载入了，但耗时没变。
  - 启动时偷偷弹一个菜单，在 `EVENT_SYSTEM_MENUPOPUPSTART` 时 `EndMenu`：之后的菜单出现得快了，却要 165 ms 才画好（出现了淡入动画），更慢。
- **日志**：从点击到菜单出现超过 100 ms 时，`WinShun.log` 里会写一行分段耗时：等主线程处理、构建菜单、Windows 显示，各用了多久（`App::showTrayMenu`）。
  - “点击时刻”取自 `GetLastInputInfo`。等待期间鼠标一动，这个时刻就会刷新，等待时间会算少。
  - 直接投递消息的测试没有真实点击，等待会算得很大，测完要从日志里删掉这些行。
- **还没解决**：2026-10-08 用户真实右键时，日志记到过一次 “313 ms for Windows to show it”（`TrackPopupMenuEx` 到菜单出现）。投递消息的模拟里从没超过 63 ms，所以真实点击还有模拟没覆盖到的情况。再出现就照这一行查。

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
- **会冲突的程序**：PowerToys 的“查找鼠标”默认就是双击左 Ctrl，Listary 默认也是双击 Ctrl。两边都会响应。

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


### 列表滚动时整行在抖

- **现象**：结果列表滚动时（尤其是快停下的那段），行里的文字和图标轮流上下跳 1 像素，像在抖。
- **怎么量**：提权脚本用滚轮滚动列表，或按住拖动后甩出去，连续截屏（约 10 ms 一帧），逐帧找每一行图标上边缘和标题上边缘的位置，统计两者之差。
  - 选中行和悬停行的浅色背景会让文字顶上的抗锯齿多算一行，那是测量误差，要按行背景分开统计。
  - 结果：改之前约 20% 的帧差 1 像素。0.2.3 的做法下滚轮不抖，但拖动甩出时仍有约 18% 的帧错开。现在的做法下，静止、滚轮、拖动甩出全部一致。
- **原因**（150% 缩放，两层）：
  - **列表停在小数位置**：Qt 的 Flickable 滚轮和惯性滚动会停在任意小数位置。文字、按最近邻采样的图标、矩形各按自己的规则对齐到像素，于是轮流错开。Qt 的 `pixelAligned` 只对齐到逻辑像素，150% 下一半的位置仍在半个设备像素上。
  - **文字顶点在 .5 处随机取整**：列表对齐到设备像素后，文字还在跳。原生文字的着色器（`textmask.vert`）把每个字形四边形的顶点各自按 `floor(x × dpr + 0.5)` 取整，而且用单精度浮点。居中排版常让文字落在正好 .5 个设备像素上，这时往哪边取整由浮点误差决定，而误差随滚动位置变化，结果字形整体错开或被拉高 1 像素（标题高度在 20 和 21 之间变）。
- **做法**：
  - **只修正画面，不改滚动位置**。每一行加一个 `Translate`，把它在窗口里的设备像素位置补成整数，补的量不到 1 像素（`Main.qml` 的委托）。`contentY` 保持 Flickable 自己算的值，所以滚轮、拖动、惯性滚动都用 Qt 原生的，每一帧都对齐。浏览器也是这么做的：滚动位置可以是小数，只在画出来时对齐（Firefox 的 `snapped-scrolled-content` 测试，Chromium 合成器）。
    - 列表在窗口里的位置要用 `layout.y + listArea.y + list.y` 这样能通知变化的属性来算，不能用 `mapToItem()`：Column 在稍后的布局阶段才给子项定位，`mapToItem` 的结果不会因此重算，结果刚出来时第二行就落在了半个像素上。
  - **行内会随滚动移动的文字，竖直位置都放到整数设备像素上**：标题与路径所在的 Column、标题行的高度、内容搜索时跟在名字后的文件夹名、右侧的“最近”、徽标里的文字（`ResultRow.qml` 的 `onPixel` / `upToPixel`）。用 `anchors.verticalCenter` 居中的文字很容易落在 .5 上。
  - **设置页（和拾穗计划页）整页滚动，控件太多，不逐个对齐**：只给整页的 Column 加 `Translate`，补到整数设备像素后再往下多偏 0.02 个设备像素（`SettingsWindow.qml` 的 `page`，`GleaningPage.qml` 的 `Snapped`）。页里落在 .5 上的文字、边框、图片都变成 .52，取整方向固定，不再随浮点误差变；0.02 远大于单精度误差（几千像素处约 0.001），又小到看不出来。2026-10-09 实测（150%，剪贴板页，滚轮和小步滚动各上下一次，约 10 ms 一帧，相邻两帧按整数平移对齐后数对不上的像素）：改之前 168 对有变化的帧里 138 对有错开，改之后 96 对全部一致。
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
- **为什么不改用系统边框那条路**：微软的说法（“Apply rounded corners in desktop apps”）是，带 `WS_THICKFRAME` 和 `WS_CAPTION`、或留出 1 像素非客户区边框的窗口，系统自动圆角并画好边框和阴影（VS Code、Windows Terminal 就是保留系统边框、只把标题栏区域划给自己）；没有边框的窗口用 `DWMWCP_ROUND` 申请圆角，这是我们的情况（Qt 的 `FramelessWindowHint` 是 `WS_POPUP`）。走系统边框那条路，启动器和剪贴板会多出可拖的缩放边、贴靠布局和打开关闭动画，Qt 算窗口位置的方式也要跟着改；现在自己画的边已经和系统的一样平滑，没有必要。Qt 6.9 起的 `Qt::ExpandedClientAreaHint` 在 Windows 上是给普通窗口自绘标题栏用的（Qt 自己画一条带图标和按钮的标题栏），也不合适。原先对话框旁的搜索框用强调色边框，太扎眼，也改成同一套。
- **怎么查**：边框颜色可以从别的进程设（`DwmSetWindowAttribute(hwnd, DWMWA_BORDER_COLOR, …)`，提权的脚本对 Win顺 的窗口也行），不用重新编译就能把几种颜色和“不要边框”挨个截图对比；角上 24×24 像素放大 12 倍看。

### 日志里一直有 `Swapchain says surface has alpha but the window has no alphaBufferSize set`

- **现象**：开启 Mica 后，启动器每打开一次，`WinShun.log` 里就多一行这个警告（两天里 195 行）。
- **原因**：窗口颜色激活时透明、失焦时不透明（见上面“Mica 画成一块平的灰色”）。`QQuickWindow::setColor()` 在颜色的 alpha 变化时会改窗口的格式：变成不透明就把 `alphaBufferSize` 设成 -1。可交换链是按创建时的格式建的，一直带 alpha；Qt 的 D3D11 后端每次改大小（`QD3D11SwapChain::createOrResize`）都检查两者是否一致，不一致就打这个警告。窗口在失焦时改大小，就写一次日志，而且是在显示窗口的路上同步写磁盘。
- **做法**：`prepareBackdrop` 设好 `alphaBufferSize(8)` 后，再连上 `colorChanged`，格式里的 alpha 被去掉就加回来（`App.cpp`）。这样格式和交换链一直一致；交换链将来要是重建，也还是带 alpha 的。
- **实测**（2026-10-09）：启动器开关 5 次，旧版每次 1 行，新版 0 行。

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
- **做法**：Windows 自带应用按 AppUserModelID 记下中英文两个名字（`AppCatalog.cpp` 的 `kKnownApps`，中文名逐个核对过它们 zh-Hans 资源里的字符串），两个都参与匹配（含拼音），用哪个名字找到的就显示哪个：搜“计算器”显示“计算器”，搜“calc”显示“Calculator”。

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

## 文件索引（NTFS）

### 机械硬盘上的盘建索引很慢：MFT 读不了

- **现象**：日志里有 `cannot read the MFT of D:: unsupported record or cluster size`，这些盘只好逐个文件夹遍历，在机械硬盘上要几分钟甚至更久（磁头在各个文件夹之间来回跑）。
- **原因**：读 MFT 的代码假定一个簇装得下整条文件记录，这样记录不会跨区段，遇到簇比记录小的盘就放弃。可是从 FAT32 转换来的分区、很老的分区、手动选了小簇的分区，簇只有 512 字节（记录 1 KB）；`format /L`（4 KB 记录）配 512–2048 字节的簇也一样。这种分区多在用了很多年的机械硬盘上，正好是最慢的盘。
- **做法**：不再挑簇的大小。读到的一块末尾如果是半条记录，把它拷到下一块数据的前面（每块前面留着一条记录的空位），接成整条再解析。`tst_core` 的 `mftReader` 用内存里的卷镜像测：区段是奇数个簇、在“磁盘”上顺序打乱，记录既跨区段也跨 4 MB 的块。真机上用 VHD 按各种簇大小格式化，`wsbench --mft P: Q: …` 读 MFT 并和遍历逐项比对。VHD 要用 `.vhd`：`.vhdx` 的物理扇区是 4 KB，格式化不出比它小的簇。

### MFT 的占用位图不一定在 0 号记录里

- **现象**：读 MFT 时按位图跳过空记录，D、E、F 盘都生效，C 盘一段都没跳，可它 4.4 GB 的 MFT 里明明有 1 GB 是空的。
- **原因**：大的 MFT 的 0 号记录装不下全部属性，改放一份属性列表（`$ATTRIBUTE_LIST`），`$BITMAP` 搬到了扩展记录里（本机 C 盘在 21 号记录）。只在 0 号记录里找，自然找不到。属性多时，属性列表本身也可能不在记录里（非常驻），一个属性的区段表也可能分成几段放在不同记录里。
- **做法**：先看 0 号记录有没有属性列表，按列表里 `$BITMAP` 的每一段（起始 VCN、所在记录）逐段读出区段表、接起来；读不到就照常全读，不会漏记录。`wsbench --mft` 会打印位图在哪、为什么没读到，以及“没有一条在用的块里，位图标了几条在用”（应为 0）。`tst_core` 的 `mftReader` 覆盖了位图在 0 号记录、在别的记录、分两段这几种情况。

## 内容索引

### 索引说“可能含有”的文件太多，内容搜索还是慢

- **现象**：索引建好了（0 个待读），在“内容”里搜一行代码 `const auto lock = index->readLock();` 仍要 2.7 秒才出第一条、6.5 秒才搜完；再搜一遍又是这样。
- **原因**：先量再猜：`wsbench --content-search "一行代码"` 用程序自己的索引走一遍搜索，报出“可能含有”的文件有 1392 个、共 1 GB，真正含有的只有 12 个。片段只记单词内部的三个字母（`con`、`aut`、`loc`），这些在代码里到处都是；几十 MB 的 `*.min.js`、导出的 JSON 几乎含有所有片段，每次都要整个读一遍，读之前还要等 Defender 扫描。
- **做法**：片段带上空格和标点（剩 13 个），按种类给文件大小设上限。改片段规则时，`Collector`（读文件）和 `ofPhrase`（查词）必须按同一套规则拆，并升 `kStateVersion`：旧索引按旧规则记的，用新规则去查会把真含有的文件排除掉。改完跑 `wsbench --content-index --verify`，它把每个被排除的文件都实际读一遍，应当一个都不含。

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

### 重启资源管理器后，任务栏没回来

- **现象**：在设置里点“现在重启资源管理器”，任务栏消失了，十几秒后也没回来；进程里只剩一个开着文件夹窗口的 `explorer.exe`。注册表里 `AutoRestartShell` 是 1。
- **原因**：原来的做法是结束任务栏所在的 `explorer.exe`（退出码非零），等 Winlogon 重新启动它。但 Winlogon 只管它在登录时启动的那个外壳；外壳之前被别的程序或用户重启过的话，结束后就没人再启动它。
- **做法**：结束前先复制这个进程的令牌，3 秒内没有新外壳就自己启动：`CreateProcessWithTokenW`，不行再以用户的 `sihost.exe` 为父进程（`winv::restartExplorer`）。不能用 Win顺 自己的令牌，那样外壳带着管理员权限，任务栏上打开的一切也都是。测完查新外壳令牌的 `TokenElevation`，应该是未提权。用户手动恢复：`Ctrl+Shift+Esc` → 运行新任务 → `explorer`。

### 粘贴偶尔没贴进去（目标窗口刚回到前台）

- **现象**：选好条目按 Enter，剪贴板里的内容是对的，原来的窗口也回到了前面，文本框里却是空的；同样的步骤再做一次又正常。
- **原因**：目标窗口一到前台就发了 `Ctrl+V`。它这时可能还没把焦点放回输入框，按键落在窗口本身，丢了。
- **做法**：到了前台再等 60 ms 才发（`Clipboard.cpp` 的 `kSettleMs`）；改后原来出错的场景连做 5 次都成功。不用 `GetGUIThreadInfo` 查焦点：UWP 程序的输入在另一个进程里，查不准，反而要等满超时。

### 在 Win顺 自己的搜索框里按 Win+V，粘贴不进这个框

- **现象**（用户反馈）：在启动器的搜索框里按 `Win+V`，选一条按 Enter，搜索框里什么也没有，内容贴到了打开启动器之前的那个程序里。对话框旁的搜索框和设置窗口里的输入框也一样，只是提示“没有找到要粘贴进去的窗口”。
- **原因**：粘贴的目标只在启动器从隐藏状态打开时记一次前台窗口。搜索框和剪贴板页是同一个窗口的两页，剪贴板页一出来搜索框就看不见了，目标还是之前的程序；对话框旁的搜索框在启动器到前台时跟着隐藏；设置窗口是 Win顺 自己的，`paste::usableTarget` 不收。何况往自己的窗口发 `Ctrl+V` 也要先切回那一页、等焦点回来。
- **做法**：打开剪贴板页时先看键盘是不是在 Win顺 自己的输入框里，是的话粘贴时回到那个框，文字用 `QInputMethodEvent` 直接给它，不发按键（见 architecture.md 的“粘贴到 Win顺 自己的输入框”）。光标和选区要在剪贴板页拿走焦点**之前**记下：焦点在同一个窗口里移走时，`TextInput` 会丢掉选区（`QQuickTextInputPrivate::handleFocusEvent`，除非 `persistentSelection`）；窗口失去激活（`Qt::ActiveWindowFocusReason`）时不丢。切回搜索页时 `Main.qml` 还会全选一次，所以恢复选区放在它之后。

### 多行文字粘贴进搜索框，灰色的字铺满整个窗口

- **现象**（用户截图）：往启动器的搜索框里 `Ctrl+V` 一大段多行文字，框里只看到最后一行，“0 个结果”；同时一行行灰色的字铺满整个窗口，压在搜索栏和底栏上，左右两边都被截掉。
- **原因**：Qt 的单行 `TextInput` 粘贴时不去掉换行，换行留在 `text` 里，只是显示成空格（`QQuickTextInputPrivate::updateDisplayText`）。查询里带着换行，搜索什么也找不到；空状态的“没有找到“%1””用的是 `Text`，纯文本里的换行照样分行，又没限宽度和行数，就从 80 像素高的区域里上下溢出、左右超出窗口。
- **做法**：源头上，多行文字粘贴进单行的框合成一行（`App::eventFilter` + `textfield::pasteOneLine`，见 architecture.md 的“多行文字粘贴进单行的框合成一行”）。显示上，凡是把用户输入放进 `Text` 的地方，先把空白合成一个空格，再定宽度、`elide`；`ElideMiddle` 只对单行文字有效，所以合并空白这一步不能省。

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

### QML 文件和 C++ 类同名，加载的是 C++ 那个

- **现象**：对话框搜索框的窗口建不出来，日志 `Could not load DialogBar, as the type is uncreatable`。
- **原因**：同一个 QML 模块里，C++ 类 `DialogBar`（`QML_ELEMENT`、`QML_UNCREATABLE`）和 `DialogBar.qml` 都叫 `DialogBar`，`QQmlComponent(engine, "WinShun", "DialogBar")` 拿到的是 C++ 类型。编译和 qmllint 都不报错。
- **做法**：QML 文件换个名字（`DialogBarWindow.qml`），和 `Launcher` / `Main.qml` 一样。

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

## 安装程序（Inno Setup）

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

## 读文档（WinShunExtract.exe）

### 大 PDF 在沙箱里读到一半退出，退出码 0xE0000008

- **现象**：一份 94 MB 的 PDF 在主程序里直接读没问题，在 WinShunExtract.exe 里读却失败；管道断开，进程退出码 `0xE0000008`。
- **原因**：这是 PDFium（Chromium）内存不够时主动终止用的退出码。某一页有巨大的矢量图，读的时候瞬间要 1.2 GB，超过了作业对象当时设的每个进程 1 GB 上限。
- **做法**：上限改为 2 GB（只是上限，不预先占用）。还是超了的文件记为读不出，文件改动后才重读，不会反复重试。排查办法：`wsbench --extract --sandbox` 看哪些文件 failed，设 `QT_FORCE_STDERR_LOGGING=1` 能看到退出码；再用不带 `--sandbox` 的 `wsbench --extract` 看同一文件在本进程里的峰值内存。

## 构建、升级 Qt

### 从 Git Bash 调 `powershell.exe` 编译，构建目录被弄坏

- **现象**：在 Git Bash 里用 `powershell.exe -Command "./scripts/build.ps1"` 编译，报 `ninja: error: FindFirstFileExA(../../../??????/src/app)`。之后在 PowerShell 里正常编译也报同样的错。
- **原因**：项目路径里有中文，经过 Bash 再传给 Windows PowerShell 时编码错了，路径变成问号。ninja 把这个乱码路径写进了依赖缓存 `build\release\.ninja_deps`，以后每次编译都会读到它。根子在控制台的代码页：Bash 里起的控制台是 1252，编译器 `/showIncludes` 输出的中文路径就成了问号；第一次编译不读依赖缓存，所以能过，第二次才报错。
- **做法**：编译只在 PowerShell 里运行（`./scripts/build.ps1`），不要经过 Bash。已经坏了的话，删掉 `.ninja_deps` 再编译（它只是依赖缓存）。脚本里 `Select-String` 只能看到标准输出，要看到编译错误得用 `*>&1 | Out-String -Stream`。
- **新建编译目录也别在 Bash 里配置**：CMake 配置时记下的 `/showIncludes` 前缀（`CMakeFiles\rules.ninja` 里的 `msvc_deps_prefix`，中文版编译器是“注意: 包含文件:”）也会变成问号，ninja 从此认不出依赖，每次都全部重编。2026-10-09 碰到过：删掉 `CMakeCache.txt` 和 `CMakeFiles`，在 PowerShell 里重新配置就好。

### moc 解析不了原始字符串字面量

- **现象**：在测试类（`Q_OBJECT`）的函数里写 `R"({"tag_name": ...})"`，编译报 `AutoMoc ... Parse error at "}"`。
- **原因**：moc 自己的词法分析器不认识 C++11 的原始字符串，里面的引号和大括号打乱了它对类体的解析。
- **做法**：`Q_OBJECT` 类里用普通字符串和转义（`tests/tst_core.cpp`）。

### lrelease 在中文路径下打不开 .ts

- **现象**：`qt_add_translations` 生成的编译步骤报 `lrelease error: Cannot open F:/??????/src/app/i18n/winshun_en.ts`。lupdate 没问题。
- **原因**：lrelease 按 ANSI 代码页读命令行参数，这台机器是 1252（英文系统区域），中文路径变成问号。lupdate 的文件列表是写在 JSON 项目文件里的，所以不受影响。
- **做法**：只用 `qt_add_lupdate`；lrelease 自己写成 `add_custom_command`，在源码目录下用相对路径调用，生成的 .qm 用 `qt_add_resources` 嵌进去（`src/app/CMakeLists.txt`）。

### Qt 6.12：QML 导入扫描在中文路径下失败

- **现象**：编译报 `qmlimportscanner: No such file or directory F:/电脑便捷工具/src/app`。
- **原因**：6.12 起编译时就做 QML 导入扫描，是否扫描在 `qt_add_qml_module` 里就定了，之后再设 `QT_QML_MODULE_NO_IMPORT_SCAN` 目标属性已经晚了。
- **做法**：用共享库版 Qt 时，直接在 `qt_add_qml_module` 里写 `NO_IMPORT_SCAN`（`src/app/CMakeLists.txt`）。部署时由 windeployqt 按 `--qmldir` 扫描。

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

### `qmllint --version` 会弹出消息框

- **现象**：在终端里运行 `qmllint.exe --version`，命令一直不返回；屏幕上弹出一个“qmllint 6.12.0”的消息框，跳到前台，打断了正在进行的界面测试。
- **原因**：Qt 的命令行工具在 Windows 上用消息框显示 `--version` 和 `--help` 的输出（没有控制台时）。
- **做法**：查 Qt 版本看 `C:\Qt` 下的文件夹名或用 `qtpaths`；不要在自动化脚本里运行带 `--version` 的 Qt 工具。检查 QML 直接 `qmllint -I build\<目录>\src\app 文件…`，它没有问题时什么都不输出。

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

## 后台开销和主线程卡顿

### 什么都不做时也有线程在醒

- **Qt 的垂直同步线程**：只要有 D3D11 窗口（藏着也算），Qt 就开一个 `QDxgiVSyncThread` 一直 `WaitForVBlank`，240 Hz 屏幕上每秒醒 240 次，只为了给 `QWindow::requestUpdate()` 对时（`qdxgivsyncservice.cpp`）。Qt Quick 的渲染线程靠呈现（Present）掌握节奏，不靠它；没有它时 `requestUpdate` 用一个 1–5 ms 的定时器，只在真要刷新时才跑。做法：`main.cpp` 里在建 `QGuiApplication` 之前设 `QT_D3D_NO_VBLANK_THREAD=1`。实测（2026-10-09）：空闲 5 秒醒 1209 次 → 这个线程没了。
- **双击 Ctrl 的鼠标监听**：原来键盘和鼠标的原始输入一直都收，鼠标每动一下都唤醒监听线程（游戏鼠标每秒上千次）。鼠标只用来判断连按期间有没有点击、拖动、滚轮，所以改成按下 Ctrl 时才登记鼠标，松开后超过连按间隔、第二下不可能再来时撤掉（`KeyListener::listenToMouse`）。按着 Ctrl 时已经按下的鼠标键，照旧用 `GetAsyncKeyState` 补查。实测：每毫秒挪一次鼠标、共 5 秒（约 2770 次），监听线程从醒 1050 次降到 7 次；双击 Ctrl 的测试（含 Ctrl+滚轮、按住鼠标键、两次 Ctrl+点击）16 项照样全过。
- **每个窗口一套显卡驱动线程**：启动器和剪贴板窗口各有自己的 D3D11 设备（Qt Quick 每个窗口一个渲染线程、一个设备），NVIDIA 驱动给每个设备开约 54 个线程（这台 32 线程的 CPU 上；VS Code 的 GPU 进程也是 54 个），其中一个按系统时钟每秒醒约 65 次。窗口销毁时这些线程随设备一起收回（设置窗口关掉后，它那一份就没了），不会越积越多。`D3D11_CREATE_DEVICE_PREVENT_INTERNAL_THREADING_OPTIMIZATIONS` 对 NVIDIA 驱动没用（单个设备 21 个线程，加了还有 19 个）。常驻的两个窗口要一按就出来，这份开销保留；要省只能让多个窗口共用一个设备，那得改用 basic 渲染循环、在主线程上画，不划算。
- **怎么查**：提权运行的程序隔几秒用 `NtQuerySystemInformation(SystemProcessInformation)` 取两次 Win顺 每个线程的上下文切换次数和 CPU 时间，相减就是这段时间谁醒了多少次；再用 `NtQueryInformationThread(ThreadQuerySetWin32StartAddress)` 和 `GetThreadDescription` 认出线程是谁的（Qt 的线程有名字，驱动线程的起始地址在 `nvwgf2umx.dll` 里，我们的 `std::thread` 起始地址在 `ucrtbase.dll` 里）。普通权限读不到提权进程的线程。

### 主线程在常用操作里卡不卡

- **实测**（2026-10-09）：测试程序每 2 ms 给 Win顺 主线程上的窗口发一次 `WM_NULL`（`SendMessageTimeout`），看多久得到回应，同时走一遍打开启动器、搜文件名、输一个字母出大量结果、切到“内容”搜文字、关闭、打开和关闭设置窗口。约 8500 次里超过 8 ms 的只有：打开设置窗口两次（53、61 ms，每次重新创建窗口），关掉它一次（18–39 ms）。搜索时一次也没有，搜索都在工作线程上。
- **这种探针有盲区**：主线程在等 COM 调用或别的线程时，Windows 会顺带处理别的线程发来的消息，探针会被“插队”回答，看不出这段卡顿。要量某个命令的总耗时，就比第二个进程的寿命：`WinShun.exe --background` 什么都不转交（只是启动，47 ms），`--settings` 要等 Win顺 处理完才退出（95 ms），差值就是处理的时间。
- **剪贴板历史在主线程上存**（`ClipStore` 只在主线程用；读剪贴板、算图片哈希、转 PNG 都在监听线程上）。单独量 `ClipStore::add`：一段普通文字 0.08 ms，100k 字 0.7 ms，截图 3 MB 约 3 ms、照片 12 MB 约 8 ms，2M 字纯文本约 15 ms，100 万字带 4 MB 网页格式约 27 ms；最坏是网页格式和 RTF 各 16 MB（复制一大片 Excel）约 140–160 ms。只有这种极端的复制会卡到感觉得出来，要改就把写库挪到单独的线程。

## 界面实测（模拟真实输入）

改了窗口和鼠标交互，编译通过不等于能用，要在部署好的程序上用模拟的真实输入试一遍。

- **测试程序要以管理员身份运行**：Win顺以管理员身份运行，普通权限进程用 `SendInput` 发的输入会被 UIPI 悄悄丢掉，不报错。
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
- **测别的程序时，开单独的实例**，不碰用户自己开着的窗口和配置：Edge 加 `--user-data-dir=临时文件夹 --no-first-run --new-window`；Firefox 用 `-no-remote -new-instance -profile 临时文件夹`，文件夹里放一个关掉欢迎页的 `user.js`；Windows Terminal 用 `wt -w new --title 名字 --suppressApplicationTitle`（提权启动时标题前面会加“Administrator:”，按包含查找）；经典控制台直接启动 `conhost.exe cmd.exe /k title 名字`。点一下控制台会进入“选择”模式（标题前面出现“选择”），之后的按键算作选择操作。新版记事本别拿来测：用户开着它时，打开文件会变成用户窗口里的一个标签。
- **Windows 11 资源管理器开着几个标签时，`WM_CLOSE` 只关当前那个标签**：测试结束关窗口后，剩下的标签还开着。关之前看一下这个窗口还有几个标签，关完再查一遍。
- **会删除、移动、覆盖文件的测试，动手前先核对目标**：结果列表随时可能变。比如另一个会话重启了 Win顺，列表变成“最近使用”，脚本照原步骤点下去，就会对真实文件下手。2026-10-08 的多选测试就差点把 OneDrive.exe、msinfo32.exe 送进回收站（文件都没事）。做法：
  - 测试文件放在专门的文件夹（如 `C:\wsmulti`；`%TEMP%` 不会被索引，搜不到）。
  - 每一步先用 Ctrl+Shift+C 复制选中项的路径，读剪贴板核对，路径全在测试文件夹里才继续。
  - 每一步前检查 WinShun 进程的启动时间没变，变了立即停止。
  - 测完把测试文件从回收站里彻底删掉。
- **SendInput 发方向键要带 `KEYEVENTF_EXTENDEDKEY`**：不带的话会被当成小键盘方向键。NumLock 开着时，Windows 会在 Shift+小键盘键前后插入假的 Shift 抬起和按下，程序收到的就是不带 Shift 的方向键，Shift+↓ 测出来像“不能多选”。Home、End、PgUp、PgDn 同理。
- **找搜索框窗口要按类名和大小**：WinShun 进程里可能还开着设置窗口（别的会话或用户打开的），只取最大的可见窗口会找错。搜索框的类名是 `Qt6120QWindowToolSaveBits`，宽 760 逻辑像素。
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
  - `$null` 传给 C# 方法的 `string` 参数会变成空字符串：`FindWindowEx(h, 0, '类名', $null)` 实际在找标题为空的窗口，有标题的（如资源管理器的标签页）就找不到。要传 null 的调用写在 C# 里。
  - 刚关掉的资源管理器窗口在 `Shell.Application` 的 `Windows()` 里还会列一会儿。测试新开一个窗口后按路径找它，要排除开之前就有的窗口，否则会拿到正在关闭的旧窗口。
- **测剪贴板时，测试内容也进了 Windows 自带的剪贴板历史**：用户开着 Windows 的剪贴板历史时，测试脚本复制的每一条都会进去。那里最多 25 条，会把用户原来的挤掉，而且没法恢复。测试前告诉用户，测试条目尽量少。
- **别的功能的测试改了剪贴板，也会进 Win顺 自己的剪贴板历史**：比如对话框搜索框的“剪贴板里的路径”要往剪贴板里放路径。在带剪贴板历史的版本上测完，要把这些测试条目从历史里删掉（2026-10-08 删过两条），再把用户原来的剪贴板文字放回去。更省事的办法是一开始就不让它们进去：历史按剪贴板所有者的程序过滤，测试期间把 `pwsh.exe` 和 `WinShun.exe`（Win顺 自己的“复制路径”）临时加进 `WinShun.ini` 的 `[Clipboard] ExcludedApps`；结束时先放回用户的剪贴板、等一下，再还原设置文件。前后用只读方式（`sqlite3` 的 `mode=ro`）数一下 `clipboard.db` 里 `clips` 的条数核对。
- **模拟单击的按下和松开在同一次 `SendInput` 里**：程序处理激活（`WM_ACTIVATE`）时，`GetAsyncKeyState(VK_LBUTTON)` 已经是松开的，按“鼠标是不是按着”来判断“是不是点进来的”在测试里永远不成立，真人点击却成立，测不出问题。要判断就看随后到来的鼠标事件（见“点进搜索框，对话框却跳到了第一个建议”）。
- **提权运行的测试脚本没跑起来时，读到的是上一次的日志**：用 Windows PowerShell 5.1 运行只有 pwsh 7 才能解析的脚本，提权的隐藏窗口里报错谁也看不见，日志文件还是上一次的，结果一字不差。读日志前先看时间、窗口句柄这些每次都变的值，或者先把旧日志改名。
- **Qt 会在窗口标题后面加上程序的显示名**：标题设成“剪贴板”，Windows 里的标题是“剪贴板 - Win顺”（`QPlatformWindow::formatWindowTitle`，标题和显示名相同时不加，所以启动器还是“Win顺”）。按标题找窗口要按开头匹配，并限定 WinShun 进程。
- **Win顺 刚启动时，测试脚本写剪贴板可能失败**：`Set-Clipboard` 报 “Requested Clipboard operation did not succeed”，剪贴板正被别的程序打开着（Win顺 的监听线程在读）。写剪贴板要隔一会儿重试几次。
- **Windows 自带的剪贴板面板不抢前台**：- **Windows 自带的剪贴板面板不抢前台**：按 `Win+V` 后 `GetForegroundWindow()` 还是原来的窗口，判断面板是否打开要截图；前台换了它也不关，要点它的关闭按钮。
- **磁盘弹出和锁定不需要真硬件**：用 diskpart 建一个 VHD，挂上并格式化成 NTFS（挂上的 VHD 算固定磁盘，会被索引）。`FSCTL_LOCK_VOLUME` 模拟格式化、chkdsk 的锁定，`CM_Query_And_Remove_SubTreeW` 模拟弹出。弹出后 `diskpart detach vdisk` 会失败（0x80070057），改用 `Dismount-DiskImage`。
