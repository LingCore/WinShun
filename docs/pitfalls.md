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

## 界面实测（模拟真实输入）

改了窗口和鼠标交互，编译通过不等于能用，要在部署好的程序上用模拟的真实输入试一遍。

- **测试程序要以管理员身份运行**：Win顺以管理员身份运行，普通权限进程用 `SendInput` 发的输入会被 UIPI 悄悄丢掉，不报错。
- **坐标按物理像素**：测试程序先调 `SetThreadDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2)`，否则拿到的窗口位置是按缩放换算过的。`SendInput` 的绝对坐标加 `MOUSEEVENTF_VIRTUALDESK`，按整个虚拟桌面归一化到 0–65535。
- **检查拖动区不用动鼠标**：`SendMessage(hwnd, WM_NCHITTEST, 0, 屏幕坐标)` 直接问窗口某一点是拖动区、按钮还是边框。
- **从 `dist\WinShun` 测**，不要从 `build\release` 启动：那里没有 Qt 插件，会报 “no Qt platform plugin”。替换前保留旧版（只换 exe 时把旧的改名为 `.bak`，整个文件夹要换时把旧文件夹改名），新版起不来就自动换回去。
- 部署和测试放在同一次提权运行里，只弹一次 UAC；测试期间不要碰鼠标。
- 贴靠布局的浮层没有出现在自动测试的截图里（最大化按钮的悬停高亮有），它是否正常弹出要手动确认。
- **PowerShell 的坑**：
  - 函数名别和内置别名重名：别名优先于函数，如 `r`（Invoke-History）、`sp`（Set-ItemProperty）。
  - `@(...)` 里逗号比 `+`、`-` 结合得更紧：`@($a + 1, $b)` 要写成 `@(($a + 1), $b)`。
  - pwsh 7 的 `Start-Process -Wait` 会等所有子孙进程，测试脚本里启动了 Win顺就会一直卡住；改用 `-PassThru` 再 `.WaitForExit()`。
  - 要用 `System.Drawing` 截图量像素时，用 Windows PowerShell 5.1。
- **磁盘弹出和锁定不需要真硬件**：用 diskpart 建一个 VHD，挂上并格式化成 NTFS（挂上的 VHD 算固定磁盘，会被索引）。`FSCTL_LOCK_VOLUME` 模拟格式化、chkdsk 的锁定，`CM_Query_And_Remove_SubTreeW` 模拟弹出。弹出后 `diskpart detach vdisk` 会失败（0x80070057），改用 `Dismount-DiskImage`。
