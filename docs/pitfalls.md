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
- **做法**：`KeyListener` 同时用 Raw Input 收鼠标（`RIDEV_INPUTSINK`）。Ctrl 按住期间，或者一次敲击之后，只要有鼠标按键按下，就不算敲击（`DoubleTapDetector::mouseButtonDown`）。鼠标移动的消息只看一下按键标志就返回。Raw Input 不是钩子，不会拖慢鼠标。

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
- **弃用的做法**（0.2.3）：自己用 `WheelHandler` 加 `NumberAnimation` 接管滚轮，并在每次 `contentY` 变化时把它对齐到设备像素。问题有两个：触屏和拖动甩出走的是 Flickable 自己的惯性滚动，惯性过程中不能改 `contentY`（会重置它的 timeline），所以仍然会抖；而且滚动手感也不再是原生的。
- **注意**：Windows 上 Qt 的滚轮事件 `pixelDelta` 永远是 0，精密触摸板也一样（`qwindowspointerhandler.cpp` 里传的是 `QPoint()`），不能靠它区分触摸板和鼠标滚轮。
- **其他办法**：`Text.QtRendering` 或 `Text.CurveRendering` 不走 `textmask.vert` 的取整，但会失去 FreeType 的垂直 hinting，中文小字会变软。`QT_SCALE_FACTOR_ROUNDING_POLICY=Round` 会把 150% 变成 100% 或 200%。都没采用。

## 窗口材质（Mica）

设置窗口和搜索面板在 Windows 11 22H2 及以上、用“显卡加速”绘制时，背后是 Mica（`DWMWA_SYSTEMBACKDROP_TYPE` = `DWMSBT_MAINWINDOW`），窗口自己的背景色设成透明。“外观”里可选关 / 开 / 自动，默认“自动”：内存不超过 16 GB 时关闭（同界面绘制方式的“自动”，`Settings::lowMemory`）；Windows 10 没有这个效果，那一行也不显示。

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

### 补充的关键词要加在页面本身那一条上

- **现象**：搜“壁纸”出来的是“视差背景”。
- **原因**：一个设置页面有十几条任务，打开命令都一样。关键词加到了每一条上，同分时取了名字最短的那条。
- **做法**：有页面本身那一条（Filename 是 `AAA_<页面 ID>`）就只加在它上面。

### 循环里现建 `QRegularExpression` 很慢

- **现象**：读系统入口要 1.9 秒，其中解析资源字符串只占 0.3 秒。
- **原因**：`split(QRegularExpression(...))` 每次调用都重新编译正则，约 3000 次就是 1.5 秒。
- **做法**：用 `static const` 的正则，或者手写切分（`splitKeywords`）。改完约 0.45 秒。

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

### 以 `(` 开头的一行会接到上一行

- **现象**：在搜索结果上点右键，菜单不出来，日志里有 `Main.qml:103: TypeError: true is not a function`。
- **原因**：`menuLoader.active = true` 的下一行是 `(menuLoader.item as ContextMenu).popup(...)`。JavaScript 不会在 `(` 前面自动补分号，两行连成了 `true(...)`。
- **做法**：不要让一行以 `(`、`[` 或模板字符串开头；先存进一个变量再调用（`Main.qml`）。

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

## 构建、升级 Qt

### 从 Git Bash 调 `powershell.exe` 编译，构建目录被弄坏

- **现象**：在 Git Bash 里用 `powershell.exe -Command "./scripts/build.ps1"` 编译，报 `ninja: error: FindFirstFileExA(../../../??????/src/app)`。之后在 PowerShell 里正常编译也报同样的错。
- **原因**：项目路径里有中文，经过 Bash 再传给 Windows PowerShell 时编码错了，路径变成问号。ninja 把这个乱码路径写进了依赖缓存 `build\release\.ninja_deps`，以后每次编译都会读到它。
- **做法**：编译只在 PowerShell 里运行（`./scripts/build.ps1`），不要经过 Bash。已经坏了的话，删掉 `.ninja_deps` 再编译（它只是依赖缓存）。脚本里 `Select-String` 只能看到标准输出，要看到编译错误得用 `*>&1 | Out-String -Stream`。

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
  - 变量名不区分大小写：`$seq` 和参数 `$Seq` 是同一个变量。参数声明了 `[string]` 时，给它赋一个数组会被转回一个字符串，`foreach` 只循环一次。局部变量换个名字。
- **磁盘弹出和锁定不需要真硬件**：用 diskpart 建一个 VHD，挂上并格式化成 NTFS（挂上的 VHD 算固定磁盘，会被索引）。`FSCTL_LOCK_VOLUME` 模拟格式化、chkdsk 的锁定，`CM_Query_And_Remove_SubTreeW` 模拟弹出。弹出后 `diskpart detach vdisk` 会失败（0x80070057），改用 `Dismount-DiskImage`。
