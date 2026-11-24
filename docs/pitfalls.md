# Win顺 开发踩坑与避雷指南

本页列出跨子系统的通用红线，并按开发领域路由到 6 份专题避雷文档。改动对应模块前先查阅对应专题；记录新坑时直接合入对应专题，保持现行有效，不写按日期堆砌的排障日记。

## 1. 通用红线（高频跨子系统陷阱）

1. **编码与乱码**：所有文本与源码文件一律 UTF-8；含中文的 PowerShell 脚本（`.ps1`/`.psm1`/`.psd1`）必须带 UTF-8 BOM；`.bat`/`.cmd` 仅限 ASCII。C++ 禁止调用 A 后缀 Win32 API；确属系统代码页的例外必须登记到 `tests/tst_encoding.cpp` 并运行 `ctest -R ^tst_encoding$`。详见 [乱码与编码](pitfalls/installer-build-and-testing.md#乱码与编码)。
2. **构建环境与 CMake**：始终在 PowerShell 中通过 `scripts/build.ps1` 或 `scripts/devshell.ps1` 编译，避免经 Git Bash 调用 MSVC 导致环境/路径异常。新增 C++/QML 文件与测试需手工登记到对应 `CMakeLists.txt`（仅主题目录自动扫描）。工程开启了 `QT_NO_CAST_FROM_ASCII`，字符串字面量统一用 `u"..."_s`。
3. **单实例与保护用户环境**：Win顺 为单实例应用，切勿擅自 `--quit` 或覆盖正在运行的用户实例。UI 核验优先用 `--preview-* --renderer d3d11` 离屏出图；需要隔离运行时用 `--profile <dir>` 并在 `[Launcher]` 写入 `ExplorerMenu=false` 与 `UriProtocol=false`，避免把用户 HKCU 右键菜单与协议指向临时目录。
4. **高 DPI 与真实像素核验**：界面必须在 100%、125%、150%、175% 下清晰对齐。1 物理像素线用 `Theme.edge(dpr)`，坐标对齐用 `Theme.snap`；严禁对 `Glyph` 或文本使用 `scale`（字形缩放改用 `Glyph.inkScale`）；`image://` 与 SVG 的 `sourceSize` 按逻辑像素，PNG/JPG 按物理像素。判断像素清晰度必须用 `--renderer d3d11`。
5. **DWM 窗口、圆角与防闪烁**：保留 DWM 原生圆角、描边与阴影，不手工绘制窗口外框；可最大化或标准系统窗口不要设 `DWMWA_TRANSITIONS_FORCEDISABLED`；窗口首帧未就绪前通过 `WindowFrame` 遮罩防白屏或黑框闪烁。
6. **QML / UIA 与列表性能**：QML 文件名严禁与 C++ `QML_ELEMENT` 类名重名；QML/JS 中以 `(` 或 `[` 开头的行会与上一行拼接。自定义可交互控件须正确配置 `Accessible.role` 且不吞掉文本；禁止在 `ListView` delegate 内直接绑定 `Screen.*` 或高开销动态查询。
7. **全局输入与前台切换**：双击触发通过独立线程 Raw Input 实现，禁止常驻 `WH_KEYBOARD_LL`；切前台统一用 `win::bringToFront`（`AllowSetForegroundWindow` + 空鼠标输入），废弃 `AttachThreadInput`；录制热键或临时接管键盘期间须用 `KeyboardRawInputAside` 让位。
8. **后台线程与异常处理**：所有 `std::jthread` 与工作线程入口必须捕获异常（`try/catch (...)`），避免未捕获异常触发 `std::terminate()` 直接闪退；跨线程回调 UI 一律 `QMetaObject::invokeMethod(..., Qt::QueuedConnection)` 并绑定上下文对象。
9. **系统版本兼容**：最低支持 Windows 10 1809；Windows 11 专属 DWM/WinRT API 必须运行时动态解析并提供降级路径，禁止硬链接导致旧版系统无法启动。
10. **安装与升级实测**：任何涉及 `installer/` 或应用内拉起安装程序的改动，提交前必须运行 `tools/verify-installer.ps1` 做真实安装与文件占用升级测试，不可仅凭编译通过判断。

## 2. 领域专题索引（按需阅读）

| 修改领域 | 专题文档 | 典型关键词 |
|---|---|---|
| 界面、QML、DPI 与窗口 | [ui-qml-and-windows.md](pitfalls/ui-qml-and-windows.md) | 高 DPI、清晰度、字体/DirectWrite、DWM 圆角/Mica/阴影、QML 陷阱、UIA 无障碍、列表性能、内嵌预览、拖拽与设置窗 |
| 索引、存储与网络共享 | [indexing-and-storage.md](pitfalls/indexing-and-storage.md) | NTFS MFT/USN、目录链接/Junction、外接盘、内容索引与直读、`WinShunExtract`、SMB/WebDAV/WSL、Everything 兼容 |
| 输入、剪贴板、对话框与资源管理器 | [input-dialogs-and-explorer.md](pitfalls/input-dialogs-and-explorer.md) | 双击触发、`WH_KEYBOARD_LL`、前台切换、剪贴板监听/SQLite/`KeyRouter`/粘贴/UIAccess、打开保存对话框、资源管理器/拖放圆盘、任务栏搜索 |
| 截图、标注、OCR 与录屏 | [capture-ocr-and-recording.md](pitfalls/capture-ocr-and-recording.md) | 多屏/HDR 捕获、遮罩与显卡驱动/TDR、标注与智能打码、贴图、长截图、Media Foundation/WASAPI 录屏、PaddleOCR/WinRT OCR |
| 插件、MCP 与命令行 | [plugins-mcp-and-cli.md](pitfalls/plugins-mcp-and-cli.md) | 插件沙箱与权限、`WinShunPluginHost`、共享 Qt、stdio 管道与缓存、MCP、`ws.exe` CLI、官方插件专项问题 |
| 安装升级、构建、编码与测试 | [installer-build-and-testing.md](pitfalls/installer-build-and-testing.md) | UTF-8/BOM/`tst_encoding`、Inno Setup 占用与回滚、Scoop、`WinShunService`、MSVC/Qt 升级、`IdleTrim`/卡死/崩溃诊断、离屏预览与截图工具 |
