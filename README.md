<p align="center">
  <img src="docs/images/icon.png" width="128" height="128" alt="Win顺 app icon">
</p>

<h1 align="center">Win顺 · WinShun</h1>

<p align="center">
  <b>双击 Ctrl，搜遍整台电脑</b> —— 免费开源的 Windows 快速搜索启动器<br>
  <b>Press Ctrl twice, search your whole PC</b> — a free, open-source search launcher for Windows
</p>

<p align="center">
  <a href="https://github.com/LingCore/WinShun/releases/latest"><img src="https://img.shields.io/github/v/release/LingCore/WinShun?label=%E4%B8%8B%E8%BD%BD%20Download" alt="Download"></a>
  <img src="https://img.shields.io/badge/Windows-10%20%7C%2011-0078D4" alt="Windows 10 | 11">
  <img src="https://img.shields.io/badge/x64-64--bit-blue" alt="x64">
  <a href="LICENSE"><img src="https://img.shields.io/badge/license-MIT-green" alt="MIT"></a>
  <a href="https://linux.do"><img src="https://img.shields.io/badge/LINUX%20DO-%E7%A4%BE%E5%8C%BA%20Community-1f1f1f" alt="LINUX DO 社区"></a>
</p>

<p align="center">
  <a href="#中文">中文</a> · <a href="#english">English</a>
</p>

<p align="center">
  <picture>
    <source media="(prefers-color-scheme: dark)" srcset="docs/images/search-dark.png">
    <img src="docs/images/search-light.png" width="720" alt="Win顺 搜索框：输入拼音 ndbg 找到“年度报告” / WinShun search bar: pinyin initials find 年度报告">
  </picture>
</p>

---

<a id="中文"></a>

## 中文

**Win顺 是什么？** 一个 Windows 上的快速搜索启动器。连按两下 Ctrl，屏幕上弹出搜索框，边打字边出结果：按文件名或拼音找文件和文件夹，打开已安装的应用，也能找文本文件里的文字。不用装 Everything，也不用写任何配置。

它直接读 NTFS 的主文件表建索引，三百多万个文件十秒左右就能建好，之后文件的增删改名都实时跟上。它常驻在任务栏右下角的托盘里，除了到 GitHub 检查新版本以外不联网，不需要账号，完全免费。界面有简体中文和英文两种，默认跟随 Windows 的显示语言，也可以在设置里随时切换。

### 功能

#### 🔍 文件搜索

- **连按两下 Ctrl** 打开搜索框，再按一次或按 Esc 关闭。三百多万个文件里搜一次，一般只要几毫秒。
- **支持拼音和首字母**：`bg`、`baogao`、`baog` 都能找到“报告”，`ndbg` 找到“年度报告”；多音字按常用读音（“银行”用 `yh` 或 `yinhang`）。
- **搜整台电脑**：所有本地磁盘；U 盘和移动硬盘可以在设置里打开。
- **排序懂你**：自己的文件排在前面，系统目录、程序目录、`node_modules`、`.git` 排在后面；最近打开过的排得更前，不输入时直接列出最近打开的。
- **索引实时更新**：文件改名、新建、删除马上就能搜到。插上新磁盘自动收录，拔掉的自动移除；“安全删除硬件”时 Win顺 会先放开这块磁盘，不会提示“设备正在使用”。
- **搜索语法**：

| 写法 | 含义 |
|---|---|
| `报告 2024` | 文件名同时包含“报告”和“2024”（空格分隔，顺序不限，不区分大小写） |
| `ndbg`、`niandubaogao` | 拼音：每个汉字用读音或读音开头的几个字母，可以混用（如 `ndbaog`）；字母和数字照常匹配（`v2bg` 找“v2报告”） |
| `"年度 报告"` | 引号内作为一个整体，包括空格 |
| `!草稿` | 排除文件名含“草稿”的 |
| `!node_modules\` | 排除名字含 `node_modules` 的文件夹里的所有内容；`!临时\*.log` 只排除这种文件夹里的 `.log` |
| `*.pdf`、`报告?.docx` | 通配符：`*` 任意多个字符，`?` 一个字符 |
| `ext:pdf,docx` | 只要这些扩展名的文件 |
| `项目\readme` | 文件名含 `readme`，并且在某个名字含“项目”的文件夹下（文件夹名也可以用拼音：`xm\readme`） |

#### 📄 文字内容搜索

- 切到 **内容** 范围，找文本文件里的文字，结果里直接显示命中的那一行和行号。
- 默认搜 txt、md、log、csv、ini、json、xml、html 和常见源代码等纯文本格式，可以在设置里增删扩展名。Word、Excel、PDF 不是纯文本，搜不到。
- **自动识别编码**：UTF-8（含 BOM）、UTF-16、GBK 等本地编码都能正确读出。
- **有内容索引**：搜中文，或三个以上连在一起的英文字母、数字时，先用索引筛掉不可能含有它的文件，只打开剩下的，几十万个文件也很快。
- 在 **全部** 范围里，停止输入后也会搜文件内容，排在文件名结果后面。默认跳过系统、程序目录和 `node_modules` 等。

<p align="center">
  <picture>
    <source media="(prefers-color-scheme: dark)" srcset="docs/images/content-dark.png">
    <img src="docs/images/content-light.png" width="720" alt="Win顺 内容搜索：显示命中的那一行和行号">
  </picture>
</p>

#### 🚀 打开应用

- 已安装的应用和开始菜单的“所有应用”一致：桌面程序和 Microsoft Store 应用都有。
- 按名称、拼音、英文首字母（`vsc` 找 Visual Studio Code）或程序文件名（`winword` 找 Word）搜索；不输入时先列最近打开的。
- 在 **全部** 范围里依次是：应用和系统设置、文件、文件夹、文件内容。**文件** 范围只有文件和文件夹（文件在前），**内容** 范围只搜文件里的文字。
- 应用像在开始菜单里一样以普通权限打开；需要时按 `Ctrl+Shift+Enter` 以管理员身份运行。

<p align="center">
  <picture>
    <source media="(prefers-color-scheme: dark)" srcset="docs/images/apps-dark.png">
    <img src="docs/images/apps-light.png" width="720" alt="Win顺 应用搜索：输入拼音首字母找到已安装的应用">
  </picture>
</p>

#### 🧭 直达系统设置

- 输入“适配器”“网卡”或 `ncpa.cpl` 都能打开“网络连接”：Windows 设置里的页面、控制面板里的项目和任务（约 1200 个）按名称和关键词都能搜到，拼音也行，名称和关键词随 Windows 的显示语言。
- 关键词来自 Windows 自带的设置检索清单（开始菜单搜设置用的就是它）；Win顺 又补了一批平时的叫法（网卡、梯子、开机启动、显示隐藏文件……），以及开始菜单里没有的工具和文件夹（磁盘管理、组策略、本地用户和组、启动文件夹、hosts 所在文件夹、AppData……）。
- 在 **全部** 范围里和应用排在一起，标着“系统”，最多列 5 个；同样以普通权限打开，需要管理员权限的由 Windows 自己弹出确认。

#### ⌨️ 全键盘操作

| 按键 | 作用 |
|---|---|
| 双击 `Ctrl` | 打开 / 关闭搜索框（另可在设置里加一个组合键，如 `Alt+Space`） |
| `↑` `↓` `PgUp` `PgDn` | 选择结果 |
| `Shift+↑` `Shift+↓` | 选中多项（也可以按住 `Ctrl` 点击逐个选，按住 `Shift` 点击选一段） |
| `Enter` | 打开 |
| `Ctrl+Enter` | 打开所在文件夹并选中 |
| `Ctrl+Shift+Enter` | 以管理员身份运行 |
| `Ctrl+C` | 复制文件（可以直接到资源管理器里粘贴）；搜索框里有选中文字时复制文字 |
| `Ctrl+Shift+C` | 复制完整路径 |
| `Tab` / `Shift+Tab` | 切换搜索范围（全部 / 文件 / 内容） |
| 菜单键 / `Shift+F10` / 右键 | 更多操作 |
| `Esc` | 关闭（选中了多项时先取消选择） |

鼠标也能用：选中或悬停的结果右边有四个按钮，分别是打开所在位置、复制、复制路径和删除（点两次才删，移到回收站）。选中了多项时，打开、复制、删除等操作都对全部选中项生效。

#### 🎨 像 Windows 11 自带的一样

- 浅色 / 深色主题可以跟随系统，也可以固定一种；跟随系统强调色，Windows 11 圆角窗口。在设置的 **外观** 里点主题预览图就能切换，整个界面淡入淡出地换过去，不用重启。
- 图标按显示缩放取原生尺寸，对齐物理像素，150% 缩放下也不糊；Store 应用用它为当前尺寸和主题准备的图标。
- 界面字体随程序附带（阿里巴巴普惠体），中文清晰。

#### ⚙️ 设置

托盘图标右键 → **设置…**，修改后自动保存、立即生效：

- **打开 Win顺**：双击 Ctrl 开关、另设一个组合键、开机自动启动。
- **外观**：主题（跟随系统 / 浅色 / 深色）、界面语言（跟随系统 / 简体中文 / English），切换即时生效。
- **搜索范围**：不搜索的文件夹、任何位置都跳过的文件夹名称（如 `node_modules`）、是否包括 U 盘和移动硬盘。
- **文件内容搜索**：要搜索内容的文件类型、文件大小上限、是否也搜系统和程序文件夹、是否建立内容索引。
- **高级**：检查更新、是否自动检查更新、界面绘制方式（省内存 / 显卡加速 / 自动）、恢复默认设置。

#### 🔔 新版本提醒

- 每次启动和之后每隔 12 小时到 GitHub 看一眼有没有新版本，有的话在托盘弹出提示，打开就能看到这一版改了什么。
- 点 **去下载** 会在浏览器里打开这个版本的下载页，下载新的安装程序运行就行，设置和索引都会保留。Win顺 不会自己下载、替换程序文件。
- 当前版本显示在设置窗口左下角和托盘菜单里；托盘菜单里可以随时 **检查更新…**，设置 → 高级里可以关掉自动检查。

<p align="center">
  <picture>
    <source media="(prefers-color-scheme: dark)" srcset="docs/images/settings-dark.png">
    <img src="docs/images/settings-light.png" width="720" alt="Win顺 设置窗口">
  </picture>
</p>

### 下载安装

1. 到 [Releases 页面](https://github.com/LingCore/WinShun/releases/latest) 下载 `WinShun-版本号-x64-setup.exe`，双击安装。如果 Windows 提示“Windows 已保护你的电脑”，点 **更多信息 → 仍要运行**。这是因为作者还没有购买代码签名证书，不是程序有问题。
2. 安装程序会装到 `C:\Program Files\WinShun`，在开始菜单里放一个 Win顺（桌面快捷方式可选）。装完勾着 **运行 Win顺** 点完成，托盘里出现 Win顺 的图标，就说明在运行了。第一次建立索引只要几秒到十几秒。
3. 连按两下 Ctrl，开始搜索。第一次运行会设成开机自动启动（以后开机不会弹 UAC），不想要的话在托盘图标的右键菜单里取消 **开机自动启动**。

**升级**：有新版本时 Win顺 会提示，下载新的安装程序运行就行，它会先关掉正在运行的 Win顺，设置和索引都会保留。**卸载**：在 Windows 的“设置 → 应用”里找到 Win顺。

系统要求：Windows 10 或 Windows 11，64 位。

### 为什么需要管理员权限

| 用来做什么 | 好处 |
|---|---|
| 直接读 NTFS 主文件表（MFT） | 一次顺序读完整块磁盘的文件名单，三百多万个文件十秒左右建好索引 |
| 读 NTFS 变更日志（USN 日志） | 启动时只补读关机期间的改动，不用重新扫描磁盘；运行中改动实时跟上 |
| 以最高权限的计划任务开机自启 | 开机不弹 UAC |

Win顺 自己以管理员身份运行，但 **你从它打开的文件和应用仍是普通权限**（交给资源管理器代为打开），只有按 `Ctrl+Shift+Enter` 才会以管理员身份运行。

### 常见问题

**和 Everything、Listary 有什么区别？**
Win顺 自己建索引，不需要另装 Everything。文件名、拼音、应用和文字内容在同一个搜索框里搜，默认就针对中文做了优化。它没有 Listary 那种嵌在资源管理器和“打开 / 保存”对话框里的功能。

**能搜 Word、Excel、PDF 里的文字吗？**
不能，内容搜索只读纯文本文件（txt、md、csv、json、代码等）。

**双击 Ctrl 和别的软件冲突怎么办？**
在设置的“打开 Win顺”页关掉双击 Ctrl，另设一个组合键（如 `Alt+Space`）。

**占多少内存？**
文件名索引每 100 万个文件约 30 MB。整个程序在任务管理器里一般是 120～140 MB。

**收费吗？会上传我的数据吗？**
完全免费，源代码公开。Win顺 只在检查新版本时访问 GitHub 的公开接口，不发送任何个人信息（设置 → 高级里可以关掉）；索引和最近使用记录只保存在你自己的电脑上（`%LOCALAPPDATA%\WinShun`）。最近使用记录可以在设置 → 打开 Win顺里清除或关掉，也可以在搜索框里右键单独移除某一项。

**怎么卸载？**
在 Windows 的“设置 → 应用”里卸载 Win顺，它会去掉开机自动启动，并问你要不要删除设置和索引。0.2.0 及更早的版本是压缩包，没有卸载程序：在托盘图标的右键菜单里取消 **开机自动启动**，再选 **退出**，然后删除程序所在的文件夹；如果还想清除设置和索引，再删掉 `%APPDATA%\WinShun` 和 `%LOCALAPPDATA%\WinShun`。

### 反馈

遇到问题或有建议，欢迎在 [Issues](https://github.com/LingCore/WinShun/issues) 里提出。

### 从源码编译

需要 Visual Studio 2022（“使用 C++ 的桌面开发”）、Qt 6.8 或更新版本（推荐 6.12，MSVC 2022 64 位套件）、CMake 3.25+ 和 Ninja。

```powershell
./scripts/build.ps1                                # 编译 + 运行单元测试
./scripts/build.ps1 -QtDir C:\Qt\6.12.0\msvc2022_64 # 指定 Qt（默认用 QTDIR 或 C:\Qt 下最新的版本）
./scripts/build.ps1 -Deploy                        # 生成可分发的文件夹 dist\WinShun
./scripts/release.ps1                              # 打包安装程序和 SHA256SUMS.txt（需要 Inno Setup 6.5+）；加 -Publish 发布到 GitHub
```

也可以直接用 Qt Creator 或 VS Code（CMake Tools）打开项目目录，预设见 `CMakePresets.json`（需要环境变量 `QTDIR` 指向 Qt 套件目录）。

源码结构、设计取舍和性能数据见 [docs/architecture.md](docs/architecture.md)，开发中踩过的坑见 [docs/pitfalls.md](docs/pitfalls.md)。

---

<a id="english"></a>

## English

**What is WinShun?** WinShun (Win顺, "Windows made smooth") is a fast search launcher for Windows. Press Ctrl twice and a search bar pops up with results as you type: find files and folders by name or by pinyin, launch installed apps, and search the text inside text files. No Everything install and no configuration needed.

It builds its index by reading the NTFS master file table directly — over three million files in about ten seconds — and keeps up with every create, rename and delete in real time. It lives in the notification area, goes online only to ask GitHub about new versions, needs no account, and is completely free.

The interface comes in **English and Simplified Chinese**. It follows the Windows display language by default; switch any time under Settings → **Appearance**.

### Features

#### 🔍 File search

- **Press Ctrl twice** to open the search bar; press again or Esc to close. A search over three million files usually takes a few milliseconds.
- **Pinyin search** for Chinese names: `bg`, `baogao` and `baog` all find 报告 (“report”); `ndbg` finds 年度报告.
- **Your whole PC**: all local drives; USB and external drives can be turned on in Settings.
- **Sensible ranking**: your own files first; system and program folders, `node_modules` and `.git` last. Recently opened items rank higher and are listed when the box is empty.
- **Always up to date**: renamed, new and deleted files show up immediately. New drives are added when plugged in and removed when unplugged; “Safely Remove Hardware” works, because WinShun lets go of the drive first.
- **Query syntax**: words separated by spaces must all match; `"exact phrase"`; `!word` excludes; `!node_modules\` excludes everything in such folders; wildcards `*` and `?`; `ext:pdf,docx`; `folder\name` limits matches to folders whose name contains `folder`.

#### 📄 Text content search

- Switch to the **内容 (Contents)** scope to find text inside files, with the matching line and line number in the results.
- Searches plain-text formats by default — txt, md, log, csv, ini, json, xml, html and common source code — and you can add or remove extensions. Word, Excel and PDF are not plain text and are not searched.
- **Detects the encoding**: UTF-8 (with or without BOM), UTF-16 and local code pages such as GBK.
- **Content index**: for Chinese text, or three or more letters and digits in a row, an index rules out files that cannot contain it, so only a few files are opened.
- In the **全部 (All)** scope, content matches follow the file name matches once you stop typing.

#### 🚀 App launcher

- Installed apps are the same as “All apps” in the Start menu: desktop programs and Microsoft Store apps.
- Search by name, pinyin, initials (`vsc` finds Visual Studio Code) or program file name (`winword` finds Word). With an empty box, recently opened apps come first.
- The **全部 (All)** scope lists apps and Windows settings, then files, then folders, then file contents. **文件 (Files)** has files and then folders; **内容 (Content)** searches the text inside files.
- Apps open with normal rights, as from the Start menu; `Ctrl+Shift+Enter` runs one as administrator.

#### 🧭 Straight to Windows settings

- 适配器 (adapter), 网卡 (network card) or `ncpa.cpl` all open Network Connections: pages of Settings and Control Panel items and tasks (about 1,200) are found by name and by keyword, pinyin included, in the language Windows displays.
- The keywords come from the settings index that ships with Windows (what the Start menu searches). WinShun adds everyday words for them and tools and folders the Start menu lacks: Disk Management, Group Policy, Local Users and Groups, the Startup folder, the folder of the hosts file, AppData and more.
- In **全部 (All)** they sit with the apps, tagged “系统 (System)”, five at most, and open with normal rights; Windows asks for administrator rights itself where needed.

#### ⌨️ Keyboard first

`Enter` opens, `Ctrl+Enter` shows the item in its folder, `Ctrl+Shift+Enter` runs as administrator, `Ctrl+C` copies the file, `Ctrl+Shift+C` copies its path, `Tab` / `Shift+Tab` switch scopes, the menu key or right-click shows more actions, `Esc` closes. The selected row also has buttons to show in folder, copy, copy path and delete (to the Recycle Bin, after a second click). Select several results with `Shift+↑` / `Shift+↓`, `Ctrl`+click or `Shift`+click: opening, copying and deleting then act on all of them, and `Esc` first clears the selection.

#### 🎨 Feels like part of Windows 11

Light or dark theme, following Windows or fixed to one, with the system accent color and rounded Windows 11 corners. Pick a theme from the previews under Settings → **Appearance**: the whole window cross-fades to it, no restart. Icons are drawn at their native size for your display scaling, so they stay sharp at 150%.

#### 🔔 New version reminders

At every start and every 12 hours WinShun asks GitHub whether a new version is out, and announces it from the tray with what changed. **Download** opens that version's page in your browser: run the new installer, and your settings and index carry over. WinShun never downloads or replaces its own files. The current version is shown in the corner of the settings window and in the tray menu, which also has **Check for updates…**; automatic checks can be turned off in Settings → Advanced.

### Download and install

1. Download `WinShun-<version>-x64-setup.exe` from the [Releases page](https://github.com/LingCore/WinShun/releases/latest) and run it. If Windows says “Windows protected your PC”, click **More info → Run anyway**: the app is not yet signed with a paid code-signing certificate.
2. It installs to `C:\Program Files\WinShun` with a Start menu shortcut (a desktop one is optional). Leave **Run WinShun** checked and click Finish; when the WinShun icon appears in the notification area, it is running. The first index takes a few seconds.
3. Press Ctrl twice and start typing. The first run turns on **Start with Windows** (no UAC prompt at login); uncheck it in the tray icon's menu if you don't want it.

**Upgrading**: WinShun tells you about new versions; download the new installer and run it. It closes the running WinShun first and keeps your settings and index. **Uninstalling**: find WinShun in Windows Settings → Apps.

Requires Windows 10 or 11, 64-bit.

### Why administrator rights

| What | Why |
|---|---|
| Read the NTFS master file table (MFT) directly | One sequential read lists every file on the drive: three million files indexed in about ten seconds |
| Read the NTFS change journal (USN journal) | At startup only the changes made while it was off are read, no rescan; changes are followed live |
| Start at login as a scheduled task with highest privileges | No UAC prompt at login |

WinShun runs as administrator, but **files and apps you open from it run with your normal rights** (Explorer opens them); only `Ctrl+Shift+Enter` runs something elevated.

### FAQ

**How is it different from Everything or Listary?**
WinShun builds its own index, so Everything is not needed. File names, pinyin, apps and file contents are searched from one box, tuned for Chinese by default. It does not embed itself in Explorer or in Open / Save dialogs the way Listary does.

**Can it search inside Word, Excel or PDF files?**
No. Content search reads plain-text files only.

**How much memory does it use?**
About 30 MB of index per million files; the whole app usually shows 120–140 MB in Task Manager.

**Is it free? Does it collect data?**
Free and open source. WinShun goes online only to ask GitHub's public API about new versions, sending nothing personal (you can turn that off in Settings → Advanced); the index and history stay on your PC (`%LOCALAPPDATA%\WinShun`). Clear or turn off the history in Settings → Open WinShun, or right-click an item in the search window to remove just that one.

**How do I uninstall it?**
Uncheck **开机自动启动** in the tray menu, choose **退出 (Quit)**, and delete the app's folder. To remove settings and index too, delete `%APPDATA%\WinShun` and `%LOCALAPPDATA%\WinShun`.

### Feedback

Bug reports and suggestions are welcome in [Issues](https://github.com/LingCore/WinShun/issues).

### Build from source

Needs Visual Studio 2022 (Desktop development with C++), Qt 6.8 or later (6.12 recommended, MSVC 2022 64-bit kit), CMake 3.25+ and Ninja.

```powershell
./scripts/build.ps1                                # build and run the unit tests
./scripts/build.ps1 -QtDir C:\Qt\6.12.0\msvc2022_64 # pick a Qt kit (default: QTDIR or the newest under C:\Qt)
./scripts/build.ps1 -Deploy                        # make the distributable folder dist\WinShun
./scripts/release.ps1                              # package the installer and SHA256SUMS.txt (needs Inno Setup 6.5+); -Publish makes the GitHub release
```

Architecture, design notes and benchmarks (in Chinese) are in [docs/architecture.md](docs/architecture.md).

---

## LINUX DO

本项目积极参与并认可 [LINUX DO 社区](https://linux.do)。

WinShun is proud to be part of the [LINUX DO community](https://linux.do).

## 许可证 · License

Win顺 以 [MIT 许可证](LICENSE) 发布。Copyright © 2026 LingCore.

WinShun is released under the [MIT License](LICENSE). Copyright © 2026 LingCore.

随附的第三方内容按各自的许可使用，不在 MIT 许可范围内 · Bundled third-party content keeps its own license:

- `resources/fonts/` 阿里巴巴普惠体 3.0 · Alibaba PuHuiTi 3.0 — © Alibaba Group，按其免费商用授权随程序附带 · redistributed under its free commercial-use terms.
- 拼音读音数据 · Pinyin data — [pinyin-data](https://github.com/mozillazg/pinyin-data)，MIT，见 · see `tools/data/pinyin-data-LICENSE.txt`。常用字的读音以其中的《通用规范汉字字典》（2013）数据为准。

Windows 是微软公司的商标。本项目与微软没有任何关联。
Windows is a trademark of Microsoft Corporation. This project is not affiliated with Microsoft.
