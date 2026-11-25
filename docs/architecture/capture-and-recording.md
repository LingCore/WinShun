# 截图、标注、录屏与演示架构（winshun.capture）

`winshun.capture` 是 Win顺 的官方常驻插件，集中提供截屏框选、矢量标注、智能打码、屏幕贴图、图片再编辑、文字识别与翻译、竖向/横向长截图、区域录屏（MP4/GIF）以及桌面演示白板能力。核心纯逻辑与多媒体管线位于静态库 `src/capture/`（`wscapture`），插件入口、宿主协议、配置与 QML 界面位于 `src/capture-plugin/`。

## 1. 进程与插件结构

### 1.1 双二进制设计与共享宿主 Qt

插件采用**无 Qt 启动桩 + 共享宿主 Qt 的应用 DLL** 两段式结构，避免在插件包内重复分发 Qt 运行时：

- **`WinShunCapture.exe`**（`src/capture-plugin/stub/main.cpp`）：静态链接 CRT、不依赖 Qt。启动时从 `WS_APP_DIR`（或 `--app-dir`）定位主程序目录并校验 `Qt6Core.dll`，调用 `SetDefaultDllDirectories(LOAD_LIBRARY_SEARCH_DEFAULT_DIRS)` 与 `AddDllDirectory(appDir)`（不搜索 `PATH` 与当前目录），随后加载同目录 `WinShunCaptureApp.dll`，按启动参数分派到三个导出入口：
  - `winshunCaptureMain`：常规常驻插件主入口。
  - `winshunCapturePreview`：`--preview-*` 离屏与独立验证入口。
  - `winshunCaptureWorker`：`--recording-worker` 录屏隔离工作进程入口。
- **`WinShunCaptureApp.dll`**：静态链接 `src/capture/`（`wscapture`），编译内置 `src/capture-plugin/qml/` 与复用的 `src/app/qml` 基础控件为插件自有 `WinShun` QML 字节码模块。

### 1.2 宿主协作、配置与热键

- **插件生命周期与协议（`Protocol` / `CaptureApp`）**：`package/plugin.json` 声明为 `resident` + `eager`、`normal` 沙箱、不空闲退出（`idleExitSec: 0`）、内存限额 2048 MB。宿主与插件经 stdio JSON-RPC 通信：
  - 宿主下发：`appearance`（外观与字体快照）、`trayMenu` / `menu`（托盘“截图”子菜单）、`picture`（剪贴板与启动器图片的“贴到屏幕”“编辑”）、`showSettings`（打开 `CaptureSettingsWindow.qml`）。
  - 插件上报与调用：`winshun/hotkeys`（上报自持快捷键，使主程序让出冲突键），以及 `clipboard_add_image`、`translate_text`、`web_search` 等宿主工具。截图历史（关键词 `jt`）由主程序 `clipboard.db`（`clips.screenshot` 与 `projects/<id>.wsedit`）统一承载。
- **配置持久化（`CaptureSettings` / `CaptureOptions`）**：读写 `%APPDATA%\WinShun\plugins\winshun.capture.data\capture.ini` 的 `[Capture]` 段，涵盖热键、全屏暂停热键、保存目录、工具条隐藏项、鼠标动作、智能打码开关与自定义词、选区尺寸预设（`SizePresets`）、录屏参数与 `Capture/Keys/<id>` 自定义键位（`CaptureKeys`）。
- **独立热键与 PrtScn 接管**：`CaptureApp` 持有独立消息窗口管理全局热键。接管 PrtScn 时修改 `Control Panel\Keyboard` 并广播 `WM_SETTINGCHANGE`，原值记入 `PrintScreenBefore` 以便关闭时恢复，并经 `RegistryWatch` 与 `printscreen::RetakeLimit` 防止外部覆盖引发死循环。
- **运行期健康与内存策略**：
  - **`IdleGraphics`（`releaseIdleGraphics`）**：编辑、OCR、设置、长截图、录屏、演示窗口在隐藏满 20 秒后关闭 `persistentSceneGraph` / `persistentGraphics` 并调用 `releaseResources()` 释放 GPU 设备与交换链，保留原生窗口及 QML/模型状态；截图遮罩始终保持预热以维持低延迟唤起。
  - 配合 `IdleTrim`、`HotPages`、`UiStallWatch`、`CaptureDiagnostics`、`GraphicsLoss`（统计渲染线程 `sceneGraphInvalidated` / TDR）与 `DisplayPulse`（独立线程追踪 D3DKMT 垂直同步与 `DwmFlush` 间隔）定位卡顿与驱动异常。

## 2. 框选遮罩与多屏捕获

### 2.1 多屏捕获与 HDR 处理（`Grabber` / `Hdr`）

捕获由独立 MTA 工作线程执行：

- **SDR 与 HDR 路径**：普通 SDR 单张截图直接走 `BitBlt(CAPTUREBLT)`，不常驻 WGC/D3D11 设备。检测到 HDR 显示器（`ADVANCED_COLOR_INFO_2`）时，预建 D3D11 设备（BGRA），以 `CreateFreeThreaded` 帧池（`IsBorderRequired(false)`、`IsCursorCaptureEnabled(false)`）抓首帧（超时 1 s）经 staging 纹理回读 CPU。帧尺寸不符、超时或任一失败均回退 `BitBlt(CAPTUREBLT)`；全部失败时重建设备。
- **HDR 色调映射与 `.jxr` 旁路保存**：HDR 帧取 `R16G16B16A16Float` scRGB，除以 `DISPLAYCONFIG_SDR_WHITE_LEVEL`，超过 `0.8` 高光按扩展 Reinhard 压缩到最大亮度再编 sRGB（`Hdr.cpp`）。开启 `[Capture] HdrFile` 时当轮保留 `Shot::hdrPixels`（结束即释），保存时 `paintSdrOver` 仅把指针与标注差异像素覆写入半精度缓冲，经 WIC 无损输出同名 `.jxr`。设 `WINSHUN_TEST_HDR_CAPTURE` 可在 SDR 屏按 80 尼特白电平演练完整路径。
- **鼠标指针合成**：开启包含光标时，`Capture::start` 在遮罩显示前经 `GetCursorInfo` + `DrawIconEx` 绘入对应屏幕冻结图。

### 2.2 遮罩渲染、整像素对齐与弹出面板处理（`CaptureOverlay`）

- **每屏独立遮罩与整像素对齐**：每台显示器预热一个无边框置顶 `QQuickWindow`，用原生 `SetWindowPos` 定位到精确物理矩形；显示前先 DWM cloak，待首次 `frameSwapped` 排队回调后 uncloak。`CaptureScreen` 以物理像素存状态，QML 经 `scaleX` / `scaleY` 映射回逻辑坐标。显示器增减或 DPI 变化经 300 ms 去抖重建遮罩。
- **冻结图、指针与放大镜**：`CaptureImage`（`QSGImageNode`，Nearest）1:1 渲染；`CapturePointer` 按 DPR 缓存黑白 RGBA 十字光标（子 `MouseArea` 必须 `cursorShape: undefined`）；`CaptureLoupe`（`QQuickPaintedItem`）按需刷新 15×15 放大像素，支持 `hex` / `rgb` / `hsl`（单按 `Shift` 切换）。`CaptureScreen::dimmed` 压暗选区外四块区域及其余非活动屏幕。
- **开始菜单与任务栏弹出层**：检测到弹出面板在前台时先抓图，发带 `kOwnInput` 的 Esc 等前台切换（最多 300 ms）再显遮罩；随后 `Capture::findFlyout` 重抓该屏与冻结图做帧差，将弹出面板与任务栏矩形插入悬停候选前端。
- **商店应用全屏（媒体播放器等）**：前台为 `ApplicationFrameWindow` 且 `GetWindowBand` 不是桌面带（1）、窗口盖满所在显示器时，先抓图，等热键修饰键松开（最多 1.5 s）后发带 `kOwnInput` 的 `Win+Shift+Enter` 让它退出全屏，等其回到桌面带（最多 1 s）再显遮罩，并把它的全屏矩形插入悬停候选前端（`Capture::fullScreenTick`）；以复制、保存或取消结束时，待它重获前台、键松开 150 ms 后再发一次切回全屏，400 ms 未回去重发一次（`Capture::putBackTick`）；贴图、识别文字、录屏、长截图结束不切回。切不出全屏则结束截图，通知说明原因并记日志与诊断 `capture-full-screen-app`。

### 2.3 窗口探测与 UIA 元素识别（`WindowSnapshot` / `UiaProbe`）

- **Win32 窗口链**：冻结时 `EnumWindows` 按 Z 序收集可见窗口（跳过最小化、cloaked、本进程、`WS_EX_TRANSPARENT`、0 透明分层窗与 `CursorVisualClass`），以 `DWMWA_EXTENDED_FRAME_BOUNDS` 剔除阴影；子控件首次悬停时才 `EnumChildWindows` 并缓存至本轮结束。滚轮切换层级，按 `Tab` 切换 `windowsOnly`（只框整窗）。
- **UIA 识别与硬防护不变量**：
  - **绝不探测 Chromium 系窗口**：`WindowSnapshot::hostsChromium` 检查顶层及所有子窗是否含 `Chrome_WidgetWin_*`、`Chrome_RenderWidgetHostHWND`、`Intermediate D3D Window`、`CefBrowserWindow`（覆盖 Chrome、Edge、Electron/VS Code、CEF、WebView2、Qt WebEngine）。命中即跳过 UIA 且**绝不注册 UIA 事件监听**，避免 Chromium 永久开启无障碍树并迫使 VS Code 进入屏幕阅读器模式。
  - **独立线程与硬预算**：仅对 `wantsUia` 通过且未被 `UiaGate` 拉黑的窗口，在按需创建的 MTA 探针线程（`CUIAutomation8`，截图结束即释）中逐层 `FindAllBuildCache` 下探。重叠分支优先最小可选控件、同尺寸比链深。硬预算：深度 ≤32、元素 ≤400、单次总耗时 ≤150 ms；超时丢弃并将该窗本轮拉黑，连续 6 次无有效控件不再询问。

### 2.4 状态机、尺寸预设、选区记忆与画面历史

- **状态机（`CaptureSession`）**：在 `Hover` → `Pressed` → `Dragging` → `Selected` / `Adjusting` / `Annotating` 间流转，支持 `WASD` 1/10 物理像素微移指针、方向键微调选区、`KeyHintPolicy` 停顿提示卡（`CaptureKeyHints.qml`）以及按 `F` 编辑物理尺寸/管理最多 12 个预设（`CaptureSizeBox.qml` / `SizePresets.h`）。
- **最近选区与画面历史（`CaptureHistory`）**：内存记最近 10 个虚拟桌面选区（`R` / `Shift+R` 恢复）；每次出图后在串行 `std::jthread` 上原子写入各屏冻结图、选区及 `doc.json` 到 `<插件数据目录>\capture-history\<ms>\`（默认保留 10 份），遮罩中用 `,` / `.` 翻回历史整屏继续编辑，未出图退出前先 `restoreLive`。
- **延时、复制并贴图与保存**：延时由原生 `CountdownWindow`（`WS_EX_NOACTIVATE`）倒数并在销毁后 `DwmFlush` 两次再抓图；`CopyPin` / `PinOnCopy` 共用同一合成图与标注项目创建贴图并复制，历史只记一条；配置 `SaveFolder` 时直接落盘，否则弹原生 `IFileSaveDialog`（`ChoosingSave` 期间冻结重复热键并推迟屏重建）。

## 3. 标注、智能打码、贴图与编辑器

### 3.1 矢量文档、命令栈与瓦片合成（`Annotation*`）

- **数据模型与命令栈**：坐标统一为图像物理像素，支持 9 种 `Kind`（线型、荧光笔模式、序号格式与原地旁注文字）。文档变更均经 `AnnotationCommands` 产出脏区 `Change`，`CommandStack` 支持 200 步差异撤销与 700 ms 同批合并。
- **唯一合成管线与瓦片画布（`AnnotationRender.*` / `AnnotationCanvas` / `LiveStroke`）**：`Compositor::render(area)` 同时服务屏幕瓦片与最终导出（保证所见即导出），对象缓存上限 32 MB、会话结束即清；文字与序号用灰度抗锯齿缓冲整像素合成，避免 ClearType 彩边。`AnnotationCanvas` 按 256×256 分块（遮罩用 `sparse`，编辑窗用 `limited`、单次 `updatePolish` ≤8 ms 且总块数 ≤480）；活跃自由画笔由 `LiveStroke` 分块（`kChunk = 128`）直交 GPU 三角带，落笔回瓦片。
- **项目格式（`AnnotationFormat.*`）**：`.wsedit` 为 zip（`doc.json` deflate + `base.png` store），当前格式 v2（兼容 v1 自动迁移，拒读未知高版本）。未导出退出时内存留 10 分钟草稿（`Capture::m_draft`）。

### 3.2 智能打码（`SmartRedact`）

- 选区稳定 300 ms 后，`SmartHooks::scan` 将共享冻结图与选区交由 `CaptureOcr` 线程裁剪、灰度化并调 `PaddleOcr::readPieces`，按票号丢弃过期结果。
- `findSensitive` 在 GUI 线程规整全角/形近字，匹配手机号、身份证、银行卡（Luhn）、邮箱、常见密钥、`key=value`、本机用户名、可选 IP/MAC 与自定义词，结合灰度图将框边吸附至墨迹边缘；`Shift+B` 作为单步 `AddItems` 批量落框。
- 马赛克、模糊、纯色、`Smooth` 采样严格限定于**所有打码对象真实形状并集 U 之外**，随机噪声种子随项目保存；`RedactShape::Brush` 无抗锯齿判定、四向最远扫描 64 px。

### 3.3 原生分层贴图（`PinManager` / `PinWindow` / `PinTools` / `PinImage`）

- 每张贴图是独立原生窗 `WinShun.Pin`（`WS_EX_LAYERED | WS_EX_TOOLWINDOW | WS_EX_TOPMOST`），经 32 位预乘 DIB + `UpdateLayeredWindow` 提交（不建 `QQuickWindow`，避免 D3D 持久 commit 堆积），PMv2 下保持物理像素 1:1。
- 滚轮 / `+`/`-` 定点缩放（上限 `kZoomBudget = 24,000,000` 像素，整数倍最近邻），拖角自由缩放，支持无滤波 90° 旋转与翻转；收起为缩略小块（`Tile`）或顶边小条（`Edge`）并在后台 PNG 编码后释放大位图。
- 穿透贴图仅在存在实例时由 `PinManager` 40 ms 轮询 `GetAsyncKeyState(VK_CONTROL)` + 指针位置，按住 `Ctrl` 临时移除 `WS_EX_TRANSPARENT`（不装键盘钩子）。支持剪贴板文本贴图（`PinImage::renderText`，留 256K 原文）与 `PinAutoOcr` 后台静默识字供直接拖选或按 `T` 框选复制。

### 3.4 图片编辑窗口（`EditWindow.qml` / `CaptureEditor` / `EditView`）

- 首次使用时创建单个 `EditWindow` 并隐藏复用（绝不 `deleteLater`）。`EditView` 按设备像素算视口（100% Nearest、小于 100% Linear，超长图按短边适配防低于 25%）；支持多选、无损自由裁剪（`SetCrop` 保留原 `base.png`）、吸管（`I`）及贴图原地覆盖编辑。关闭时 `model.end()` → cloak → 画两空帧释瓦片 → 隐藏 → `releaseResources()` → `IdleTrim::poke()`。

### 3.5 OCR、表格复制、二维码与原地翻译

- **OCR 与表格（`CaptureOcr` / `OcrPanel.qml` / `OcrBubble.qml`）**：单低优先级线程运行 `PaddleOcr`（`clamp(核数/2, 1, 4)` 线程，空闲 60 秒卸载模型）；屏 DPR < 1.5 时预放大（最多 2× 且宽 ≤2000 px）。同时生成制表符分隔表格（`CaptureOcr::table`）供直接粘贴进 Excel/WPS；快捷识字弹轻量 `OcrBubble.qml`。
- **二维码（`QrCode`）**：静态链接 `zxing-cpp`（仅 QRCode/MicroQRCode，最多 8 个；未检出时 2×/3× 重试且不超过 1200 万像素）。
- **原地贴图翻译（`TranslateOverlay`）**：`ocrBlocks` 按行高、对齐与段宽合段（跳过倾角 >3°），经宿主 `translate_text` 调 `winshun.translate`。`translatedPicture` 按底色中位数、最远像素字色、字重门槛（深字 `0.085`、浅字深底 `0.105`）与居中边距覆盖重排，经 `PinWindow::setTranslation` 支持 `Tab` 翻面切换；行数不匹配且 ≤24 块时逐块降级重试，超限或未配置服务时新建翻译贴图回退至 OCR 窗口译文区；`translate_text` 答复 `service` 时带 `message`（服务拒绝密钥的 `setup` 行标题），OCR 窗口把它写在“设置翻译服务…”上方，旧宿主没有 `message` 则退回原来的措辞（#444）。

## 4. 长截图

`LongCapture` 按需创建；进入前交还目标窗口前台并撤除自身悬浮通知。屏幕显示四条点击穿透 `Theme.accent` 边框、不抢焦点控制条 `LongCapturePanel.qml`（含活动状态指示、方向切换与慢速/标准/快速三档自动滚动速度）与可收起竖向/横向自适应预览窗（禁用 DWM 淡出动画），以临时全局 `Enter`/`Esc` 控制完成与取消；工作线程入口包裹异常捕获防止未捕获异常终止进程。

- **捕获排除策略（`CaptureExclusion.h`）**：仅在**正在取帧期间**（含点击完成后等待在途帧）且窗口矩形**外扩 DWM 阴影半径**（面板 64 逻辑 px，边框 0）与选区相交时设 `WDA_EXCLUDEFROMCAPTURE`；不依赖 `IsWindowVisible`（规避 `visibleChanged` 早于原生显现竞态）。仅在 Windows 10 2004（Build 19041）及以上启用，旧版设该标志会退化成黑块。
- **取帧、连续流式自动滚动与输入路由（`ScrollGrabber` / `ScrollInput` / `ScrollAutoEngine`）**：
  - `ScrollGrabber` 按需开 `CreateFreeThreaded` WGC 流（GPU 裁剪后按排除滚动条区域判变，避免覆盖式滚动条淡入淡出误触发变化；不可用时以复用 DIB 缓冲的 `BitBlt(SRCCOPY)` 16 ms 轮询，不带 `CAPTUREBLT` 防指针闪烁），结束即 `ClearState`/`Flush`/`Trim`。
  - `ScrollAutoEngine` 为纯状态机闭环控制器：以连续节律细粒度滚轮/UIA 脉冲驱动平滑匀速滚动（自动探测老旧 Win32 控件是否支持亚格滚轮并在不支持时回落整格），按三档速度、滚动窗口实测步长与在途背压自适应节拍；顺延用户已建立的手动滚动方向（支持聊天记录向上自动滚动）；遇瞬时跟丢先停顿并尝试轻度回滚接回再降级手动；用户反向滚动或移走鼠标立即让权。
  - `ScrollInput` 绕开 Chromium 的 `Intermediate D3D Window` 投递至 `Chrome_RenderWidgetHostHWND`；识别 WinUI/UWP `InputSite` 直走 `SendInput` 滚轮；必要且无遮挡时临时停靠指针于选区安全点并在结束时还原，一旦检测到用户挪动鼠标立即放弃停靠不抢手；管理员窗拒访提示手动；修饰键/鼠标键按住或松开不足 150 ms 暂停防误缩放。
- **双向与横向拼接（`ScrollStitcher`）**：每行 16 段亮度+边缘签名，以无堆分配平铺 CSR 桶索引并兼顾深色模式稀疏边缘与 ±1 活跃度容差；结合锚点关键帧防连续小步亚像素累积漂移，精确匹配失败时以纳入 `dy=0` 的对称 Birchfield-Tomasi 垂向亚像素包络兜底；结合行级共识抗聊天动图/视频原地动画与带动画滚动干扰，仅认贴边侧边栏并按真实滚动累计位移确认滚出后吸顶/吸底栏；`m_prev` 失配时自动重匹配 `m_far`/`m_near` 并用轻量全画布行签名重定位（快速回甩立即接回）；以 `deque` 管理 256 行 `VirtualAlloc` 块支持双向扩展与弹性回弹裁切（上限 30,000 px 高或 6,000 万像素）；横向长截图通过转置复用同一拼接核。`ScrollDiagnostics` 维护 2000 条纯元数据环形日志（去重连续瞬态等待），`WINSHUN_SCROLL_DUMP` 可落盘供 `wsbench --scroll-replay` 离线重放。

## 5. 区域录屏

### 5.1 隔离工作进程与监督机制（`Recorder` / `RecordingWorker`）

媒体控制线程拉起并监督独立子进程 **`WinShunCapture.exe --recording-worker`**（挂入独立生命周期 Job），通过有界 stdio JSON 行传递控制与输入事件，双方共享 QPC 时钟：

- 连续 10 秒无进展（暂停不计）或停止后 10 秒未退出时强杀子进程。
- 硬件编码启动失败或首秒内卡住且尚未落盘时，自动回退一次系统软件编码。
- 首帧前不进入“正在录制”，首帧前取消不调 `Finalize` 并删 UUID 暂存文件；中途失败尽量封装，否则保留 `.partial.mp4` 并报告路径。
- 录制期间禁止 `IdleTrim` 收缩工作集，防页换出导致低级钩子超时。

### 5.2 采集、编码、音频与 GIF 管线（`src/capture/Recording` / `GifExport`）

- **零额外多媒体分发依赖**：Media Foundation DLL 延迟加载，缺媒体组件时仅禁用录屏。
- **屏幕采集与 Win10 1809 GDI 回退**：优先 WGC `CreateFreeThreaded`；在缺少 `IGraphicsCaptureItemInterop` 的 Windows 10 1809（LTSC 2019）或任一 WGC 启动失败时自动回退 GDI（`ScreenDcSource`：逐帧 `BitBlt` 不带 `CAPTUREBLT`、`DrawIconEx` 补光标、Alpha 补 255 上传至同类 BGRA 纹理）。结束 `uninit_apartment` 前必须清 C++/WinRT 工厂缓存。
- **编码与落盘**：SDR 在 GPU 完成裁剪、奇数补偶与 RGB→NV12（MF 分配器 ≤12 样本，硬编零 CPU 回读）；HDR 复用 `Hdr.cpp` 映射（需 CPU 回读）。H.264 High 优先质量导向 VBR，画质四档对应编码质量 70/80/90/100（`kQualities`：小体积、标准、高清、超清）；编码器不接受质量模式时退回按档位递增的码率（`videoBitrate`，每档翻倍，上限 60 Mbps）。帧率与画质分开选（MP4：15/30/60 fps）：帧按序号计时（`n × 1 s / fps`），循环用高精度可等待定时器（`Pacer`）等到下一帧；编码跟不上时跳到当前应到的帧号，保存后由 `fallenShortFps` 提示实际帧率。同目录 UUID 暂存成功后原子改名且不覆盖已有文件。
- **2 倍尺寸（`sharpScale` / `Doubler` / `Encoder`）**：H.264 的 4:2:0 每 2×2 像素只存一个颜色，原尺寸下彩色小字和 ClearType 字边的颜色被抹掉（还原度约 30 dB，与编码质量无关）。“超清”在选区不超出 H.264 Level 5.2（每帧 ≤ 36,864 宏块、每秒 ≤ 2,073,600 宏块，单边 ≤ 4096）时把每个像素用 Direct2D 最近邻放大成 2×2 块再编码，色度无损（约 47 dB），体积约 2 倍。依次尝试“硬件 2 倍 → 硬件原尺寸 → 软件 2 倍 → 软件原尺寸”（软件编码只对不超过 1920×1080 的选区放大；无 D3D11 视频设备时不放大），建不起来或第一帧被拒就换下一种；`Result::scale` 记实际倍数，`Result::refused` 记每一种被拒的阶段和错误码并写进日志。监督进程因首秒卡死而重试时固定用“软件、原尺寸”。没按所选的 2 倍保存时，保存后的面板会说明（`Recorder::advice`），不静默降级。
- **按录制帧率出帧（Windows 11 24H2 起）**：`IGraphicsCaptureSession5::MinUpdateInterval` 设为一帧的间隔，系统不再按显示器刷新率（144 / 240 Hz）送帧；240 Hz 屏上录 60 帧时录制进程 CPU 约减半。更早的系统没有这个接口，照常录；日志的 `screen` 带 `paced` 表示已启用。
- **GIF（`GifExport`）**：录制阶段共用 MP4 时间轴（不采音频）；停止后 MF 解码（处理负步幅/填充行/显示孔径）+ WIC 量化写帧（最多保留两帧及变化矩形，四档 10/15/20/20 fps、最长边 960/1280/1920/原尺寸，不放大），成功即删中间 MP4。GIF 不缩小时（输出尺寸等于选区）中间 MP4 同样按 2 倍尺寸录，导出时按 `GifOptions::sourceScale` 缩回选区尺寸，颜色取自每个像素自己（彩色小字还原度约 27 → 39 dB）；中间 MP4 一律用最高编码质量。
- **WASAPI 共享音频**：系统回环与麦克风默认关闭，开启后重采样至 48 kHz 双声道 PCM 按 QPC 对齐混音编 AAC 128 kbps；设备不可用时明确报错而非静默无声。

### 5.3 点击高亮、鼠标拖尾与按键提示

- **点击高亮（`RecordingPointer` / `RawClicks`）**：录制期间订阅鼠标 Raw Input，按物理坐标与消息时钟送 `ClickStream`（≤16 待处理、≤8 个 0.5 s 脉冲，滤除选区外与控制条），Direct2D 合成至独立 BGRA 纹理。`PointerOverlay` 自下而上合成：拖尾 → 点击脉冲 → 自绘指针 → 键帽。
- **鼠标拖尾（`CursorTrail` / `CursorArt` / `PointerTrail`）**：仅在同时录指针时生效（面板里指针关着时拖尾显示为关，点开拖尾会一并打开录指针，不做置灰的死按钮）。工作进程每轮主循环（≥60 次/秒）取 `GetCursorInfo` + `GetPhysicalCursorPos`，移动 ≥0.5 px 才记采样，采样存活 0.5 s；以相邻采样中点为端点、采样为控制点的二次曲线平滑成折线，每点带 life（指针处 1）与 place（尾 0），宽度 = 指针尺寸 × 6/32 × life、不透明度 = life，停下后尾部先到期、整条线缩回指针。拖尾先以 `D2D1_PRIMITIVE_BLEND_COPY` 画进按需增长的透明中间位图（新段覆盖旧段，自交处不叠色，指针端平头），再叠到帧上。拖尾必须在指针之下：WGC 关 `IsCursorCaptureEnabled`（1903–1909 无该接口时指针仍在帧里，拖尾只能盖在其上：不再自绘，记一条日志）、GDI 源不画指针，由 `CursorShapes` 按 Windows 实际显示重画——标准指针（与 `LoadCursorW(IDC_*)` 句柄比对）按 `CursorBaseSize`（缺省 32）× 显示器缩放取尺寸（`GetIconInfo` 只给系统 DPI 的默认尺寸，放大指针时会小一截），自行解析 `HKCU\Control Panel\Cursors` 方案文件（`cursorFileFrames`）：.cur 取恰为该尺寸的那一帧原样（经 `CreateIconFromResourceEx` 按原尺寸解码，与 DWM 所画逐像素一致；`LoadImageW` 按尺寸加载会改从更大帧重采样，轮廓差约 1 px），无此尺寸取次大帧平滑缩小；.ani 按 `anih`/`rate`/`seq ` 步进各帧；解析失败才退回 `LoadImageW`，无文件则平滑缩放句柄图；程序自带指针保持原尺寸，动画经 user32 导出的 `GetCursorFrameInfo` 按帧时长步进；读不出的指针该帧不画，每个句柄记一条日志（每次录制至多 8 条）；单色/无 Alpha 指针按 AND/XOR 拆成覆盖图与反色掩码，反色部分用 `D2D1_COMPOSITE_MODE_MASK_INVERT`。颜色取自箭头指针的饱和色（按色相分桶，取最多的 ≤4 种，亮的在尾、指针端亮度夹在 0.11–0.29），黑白指针用中性灰。暂停时清空。
- **按键提示（`KeyHook` / `keycaps::Model` / `KeycapRender`）**：仅于录制中、未暂停、前台非本进程窗口时在独立高优先级线程挂载 `WH_KEYBOARD_LL`/`WH_MOUSE_LL`（先 `CallNextHookEx`），避免与自身 Raw Input 冲突；250 ms 定时器对比 `GetLastInputInfo` 与回调时间戳超过 500 ms 时自动重装防 `LowLevelHooksTimeout` 静默卸载；`KeyBook` 用 `GetAsyncKeyState` 纠偏遗漏松开的修饰键。
- **隐私硬约束**：按键只渲染进视频帧，绝不写入日志、诊断或磁盘；内存仅存屏幕上最多 3 行；录制一结束立刻清空队列（早于 GIF 导出）。

## 6. 演示与白板（`Presenter` / `PresenterHost` / `LaserTrail`）

- **状态与多屏模型（`Presenter` / `PresenterState.h`）**：支持 `Closed`、`Draw`、`Through`（穿透，开白/黑板时禁止穿透）。`Esc`/右键阶梯回退而不清墨迹。每屏持有桌面透明墨迹 `desk` 与按需板面模型 `boardModel`（`W` 白板、`K` 黑板），复用 `AnnotationModel` 与透明合成（禁用打码与吸管），跨屏共享撤销栈，状态存 `presenter.ini`。
- **窗口与防遮挡（`PresenterHost`）**：每屏 8 位 Alpha 透明画布 `PresenterOverlay.qml` + 浮动不抢焦点 `PresenterToolbar.qml`（按 `inputRects` 同步输入掩码），监听前景/重排 WinEvent 自动抬升，隐藏 20 s 走 `releaseIdleGraphics`。
- **激光笔（`LaserTrail`）**：按 `X` 切换，`kLifeMs = 600` ms、最多 512 点，闭合轮廓按绕数填充避免自交叠色，不进撤销栈。

## 7. 测试与离屏预览入口

| 类别 | 入口与覆盖范围 |
|---|---|
| 截屏与状态机 | `tst_capture`、`tst_captureui`、`tst_capturekeys`、`tst_capturesettings`、`tst_printscreen`、`tst_numberhints` |
| 标注与打码 | `tst_annotate`、`tst_annotmodel`、`tst_smartredact` |
| 编辑、贴图与 UIA | `tst_editor`、`tst_pinwindow`、`tst_pinclipboard`、`tst_stage7` |
| 长截图与录屏 | `tst_stitch`、`tst_scrollalignment`、`tst_recording`、`tst_keycaps` |
| OCR 与演示 | `tst_ocr`、`tst_presenter`、`tst_lasertrail`、`tst_displaypulse`、`tst_graphicsloss` |
| 独立离屏预览 | `WinShunCapture.exe --app-dir <Win顺目录>` 配 `--preview-capture-tools` / `--preview-capture-settings` / `--preview-recording` / `--preview-long-capture` / `--preview-ocr` / `--preview-presenter`（退出码为 QML 警告数，校验失败返 4） |
| 专用探针 | `recording-probe`、`wsbench --scroll-replay <目录>` |
