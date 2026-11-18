# 资源管理器右键「用 Win顺 搜索此处」设计（GitHub #20）

**2026-10-14 定下并已实现：只做经典菜单。** 没有 DLL 和系统包：HKCU 下三个经典动词（`Directory`、`Directory\Background`、`Drive` 的 `shell\WinShun.SearchHere`）运行 `WinShunSearch.exe --search-here "%V"`，小程序把文件夹交给 `MessageWindow` 里只收文件夹路径的 `WinShun.SearchHere` 窗口（见 `platform/ShellMenu.cpp`、`src/stub/main.cpp`、pitfalls「资源管理器的右键动词不能直接指向 WinShun.exe」）。Win11 上在「显示更多选项」里。设置「系统整合 → 文件管理器 → 资源管理器右键」可关，卸载时安装程序删掉指向本份的动词。

下面是当初比较过的完整方案（含 Win11 顶层菜单的未签名稀疏包），留作以后想上顶层菜单时参考。

2026-10-13 在本机（Win11 Pro 26200）做过原型实验。

## 约束

- WinShun.exe 是 `requireAdministrator`：Explorer 动词不能直接指向它（每次右键都弹 UAC，单实例转交来不及跑）。
- 运行中的实例收 `WM_COPYDATA`（`dwData = 0x5753484E`，`L"query:<文本>"`），但 `MessageWindow.cpp` 只对中完整性放行了 TaskbarCreated：从 Explorer / DllHost 发会被 UIPI 拦（实测 GetLastError=5）。
- 搜索框已支持 `D:\目录\ 词` 限定路径，右键要做的就是打开启动器、预填 `D:\目录\ `。

## 方案

一个纯 Win32 的 COM DLL（`WinShunShell.dll`，IExplorerCommand + IObjectWithSite，/MT，无 Qt），两套注册，注册全由 WinShun.exe 自己做（它总是提权）：

| 系统 | 顶层菜单 | 「显示更多选项」、驱动器 |
|---|---|---|
| Win11 ≥ 22000 | 未签名稀疏包（external location），`Directory` + `Directory\Background` | HKCU 经典注册 `Directory`、`Directory\Background`、`Drive`（`ExplorerCommandHandler`） |
| Win10 | — | HKCU 经典注册（就是顶层） |

- 实测：未签名稀疏包含 COM 类时，普通权限 `Add-AppxPackage -AllowUnsigned` 被拒（0x80073D2B），**提权注册成功**（开发者模式关着也行，按用户注册，约 250 ms，Explorer 不用重启）。微软文档说这是「测试用途，勿广泛分发」：政策风险在于哪天收紧后 Win11 顶层项消失，经典菜单不受影响。备选：自签证书 + `.cer` 导入 `Cert:\CurrentUser\TrustedPeople`（未实测；.pfx 不能进公开仓库）。
- 两套互不重复：新菜单只列包注册的，经典菜单只列注册表动词。`Drive` 进不了包（schema 不允许），盘根只在经典菜单里。
- DLL 按哈希暂存到 `%ProgramData%\WinShun\shell\<sha256 前缀>\`（照 0.11 及以前给任务栏留位置的 DLL 那样按哈希暂存、只读保护；那段代码已删，见 git 历史里的 `TaskbarStartGap::stage()` + `protect()`），升级时安装目录里没有被 Explorer 锁住的文件。包里的类跑在 DllHost.exe，经典动词跑在 explorer.exe，都是中完整性。
- `Invoke`：`FindWindow(L"WinShun.MessageWindow")` → `AllowSetForegroundWindow(pid)` → `SendMessageTimeout(WM_COPYDATA)`；找不到实例就 `ShellExecute(<home>\WinShunSearch.exe --query "<文本>")`（给 stub 加这个选项：先走登录任务免 UAC 启动，否则启动 WinShun.exe 弹一次 UAC，等消息窗口出现再发）。
- `GetState`：多选、非文件系统项、zip 文件夹、`<home>` 不存在（便携版被删）→ `ECS_HIDDEN`；UNC 路径在 `okToBeSlow == FALSE` 时返回 `E_PENDING`。背景右键经 `SID_SFolderView → IFolderView::GetFolder` 取当前文件夹。
- 标题要短（新菜单会截断且无省略号）：「用 Win顺 搜索此处」/「Search here with WinShun」。语言由 WinShun 写进 `HKCU\Software\WinShun\Shell\Language`。图标：DLL 资源 `"<dll>,-1"`，经典注册还要写 `Icon` 值。

## WinShun 本体要改的

1. `MessageWindow` 对 `WM_COPYDATA` 调 `ChangeWindowMessageFilterEx(MSGFLT_ALLOW)`。暴露面：同用户的普通进程能发 quit/settings/toggle。缓解：发送方在 wParam 带自己的 HWND，接收方查它的进程是否提权，未提权的只接受 `query:` 和 `show`。**和 #21（权限分离）方向相关，先对齐。**
2. 外部来的查询打开后光标放末尾，不全选（`Main.qml onShown → focusAndSelect()` 现在会全选，一打字路径就没了；`--query` 今天也这样）。
3. ~~内容搜索 `looksLikeCode` 被 `(x86)` 这类目录名误伤~~ — 已修（`splitContentQuery` 先切出开头的路径）。

## 注册 / 反注册

- 应用侧 `platform/ShellMenu`：`stage()`、`registerClassic(dll)`（CLSID\InprocServer32 + 三个动词键 + `Software\WinShun\Shell` 的 Home/Language + `SHChangeNotify(SHCNE_ASSOCCHANGED)`）、`registerPackage(dllDir)`（仅 Win11；C++/WinRT `PackageManager`，先 Remove 再 Add 避免 0x80073CF9，`AddPackageOptions.AllowUnsigned(true)`）、`unregister()`、`ensure()`（只在开关刚打开或版本变化时重装，免得用户在「设置 → 应用」里删掉后又被装回）。
- 设置开关「添加到资源管理器右键菜单」，命令行 `--shell-menu on|off`（安装器 [Run] 和卸载时调用）。
- CMake：`src/shell`（DLL + `makeappx pack /o /nv` 出 `WinShunShell.msix`；找不到 makeappx 就只做经典注册）。

## 未测

Win10 实机、22000/22621、桌面背景右键、端到端（等过滤器改完）、标准用户用管理员凭据提权时注册落在管理员的 HKCU（整个应用本就如此）。
