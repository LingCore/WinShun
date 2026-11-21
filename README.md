<p align="center">
  <img src="docs/images/icon.png" width="128" height="128" alt="Win顺 app icon">
</p>

<h1 align="center">Win顺 · WinShun</h1>

<p align="center">
  <b>双击 Ctrl，搜遍整台电脑</b><br>
  免费开源的 Windows 文件搜索和快速启动器<br>
  <sub>Press Ctrl twice, search your whole PC — a free, open-source file search and launcher for Windows</sub>
</p>

<p align="center">
  <a href="https://github.com/LingCore/WinShun/releases/latest"><img src="https://img.shields.io/github/v/release/LingCore/WinShun?label=%E4%B8%8B%E8%BD%BD%20Download" alt="Download"></a>
  <img src="https://img.shields.io/badge/Windows-10%20%7C%2011-0078D4" alt="Windows 10 | 11">
  <img src="https://img.shields.io/badge/x64-64--bit-blue" alt="x64">
  <a href="LICENSE"><img src="https://img.shields.io/badge/license-MIT-green" alt="MIT"></a>
  <a href="https://linux.do"><img src="https://img.shields.io/badge/LINUX%20DO-%E7%A4%BE%E5%8C%BA%20Community-1f1f1f" alt="LINUX DO 社区"></a>
  <a href="https://deepwiki.com/LingCore/WinShun"><img src="https://devin.ai/assets/askdeepwiki.png" alt="Ask DeepWiki" height="20"></a>
</p>

<a id="screenshots"></a>

<p align="center">
  <a href="https://lcstp.com/projects/winshun#downloads"><b>⬇️ 国内下载</b></a> ·
  <a href="https://cnb.cool/lcstp/WinShun/-/releases"><b>CNB 镜像</b></a> ·
  <a href="https://github.com/LingCore/WinShun/releases/latest"><b>GitHub 下载 Download</b></a> ·
  <a href="docs/使用说明.md">📖 使用说明</a> ·
  <a href="docs/截图.md">🖼️ 截图 Screenshots</a> ·
  <a href="https://github.com/LingCore/WinShun-plugins">🧩 插件库 Plugins</a> ·
  <a href="#english">🌐 English</a>
</p>

<p align="center">
  <img src="https://raw.githubusercontent.com/LingCore/WinShun/51d1a60cf02b30509206116878c3d2e375b33e6d/docs/images/cover.jpg" width="640" alt="Win顺 WinShun：让你的电脑更好用，13 合 1，双击 Ctrl 全搞定，免费开源 / a free Windows search launcher, thirteen tools in one, press Ctrl twice">
</p>

---

<a id="中文"></a>

## 中文

### Win顺 是什么

连按两下 `Ctrl`，屏幕上弹出一个搜索框，边打字边出结果，按 `Enter` 打开。文件、文件里的字、应用、系统设置、剪贴板历史，都在这一个框里找。

- ⚡ **快**：直接读 NTFS 主文件表建索引，三百多万个文件几秒建好，搜一次只要几毫秒；新建、改名、删除马上搜得到。不用装 Everything。
- 🀄 **懂中文**：拼音全拼、首字母都能搜，`ndbg` 就能找到“年度报告”；打错同音字也找得到。
- 🔒 **免费、不上传**：MIT 开源，不要账号，索引和历史只存在你的电脑上。

### 三步上手

1. 到[官网下载页](https://lcstp.com/projects/winshun#downloads)（国内加速线路）、[CNB 镜像](https://cnb.cool/lcstp/WinShun/-/releases)（腾讯云，国内快）或 [GitHub Releases](https://github.com/LingCore/WinShun/releases/latest) 下载 `WinShun-版本号-x64-setup.exe`，双击安装。Windows 提示“已保护你的电脑”时，点 **更多信息 → 仍要运行**（还没有代码签名证书）。
2. 第一次启动有两页简单设置：内容索引、剪贴板历史、截图要不要开，用哪个键打开 Win顺。选好点 **开始使用**；不想选就点 **稍后设置**，以后在设置里随时改。
3. 连按两下 `Ctrl`，打字，按 `Enter` 打开。再按两下 `Ctrl` 或按 `Esc` 关掉。

不想安装，或者用 Scoop、WinGet 管理软件？见下面的[下载安装](#下载安装)。

### 能做什么

| | 功能 | 一句话说明 |
|:-:|---|---|
| 🔍 | [搜文件](docs/使用说明.md#搜文件) | 按名字、拼音、首字母找；本机磁盘、U 盘、NAS 和共享文件夹、WSL 都能搜 |
| 📄 | [搜文件里的字](docs/使用说明.md#搜文件里的文字) | Word、Excel、PPT、PDF、WPS 和文本、代码，结果显示命中的那一行；可以用正则表达式 |
| 🗂️ | [搜索面板](docs/使用说明.md#搜索面板筛选和排序) | `Ctrl+E` 展开：按类型、时间、大小、位置筛选，常用条件可存成一点就用的快捷筛选，按列排序，大图浏览 |
| 👀 | [预览](docs/使用说明.md#预览文件) | `Alt+P` 看一眼图片、文档、PDF，不用打开 |
| 🚀 | [应用和系统设置](docs/使用说明.md#打开应用和系统设置) | 开始菜单里的应用和约 1200 个设置页，`vsc`、`网卡` 都能找到；可以加一个把应用排成图标网格的“应用”标签，还能切换已打开的窗口 |
| 📋 | [剪贴板历史](docs/使用说明.md#剪贴板历史) ¹ | 代替 `Win+V`，保存文字、图片和文件，支持分组查找与悬停预览 |
| 📂 | [“打开”“另存为”对话框](docs/使用说明.md#打开另存为对话框) ² | 对话框旁多一个搜索框；`Ctrl+G` 转到文件管理器正开着的文件夹 |
| 🖱️ | [在资源管理器里](docs/使用说明.md#在资源管理器里) ² | 双击空白处弹出常用文件夹，可一层层走进子文件夹；在文件列表里直接打字就搜 |
| ⭐ | [收藏夹](docs/使用说明.md#收藏夹) | 给常用的文件、文件夹、应用起个关键词，一打就开；点列头按名称、日期、类型、大小排好；还能自定义命令，也可放进文件的右键菜单 |
| 🔎 | [任务栏搜索框](docs/使用说明.md#任务栏搜索框) ² | 在 Windows 11 任务栏上放一个 Win顺 的搜索框 |
| 🤖 | [AI 助手和命令行](docs/使用说明.md#给-ai-助手用mcp) ² | 让 Claude、Cursor 等 AI 搜你的文件（默认只读）；终端里用 `ws` 命令搜 |
| 🎨 | [外观和备份](docs/使用说明.md#外观设置和备份) | 四种界面风格（含液态玻璃），自选字体与界面大小；设置自动备份，换电脑一键迁移，也可以用云备份插件传到 WebDAV 或 GitHub 私有仓库 |

¹ 在第一次启动的引导里选，或以后在设置里打开。　² 默认关着，用的话在设置里打开。

每个功能的截图见 **[截图](docs/截图.md)**，详细用法见 **[使用说明](docs/使用说明.md)**。

### 插件

有些插件随 Win顺 装好，其余的要用时到 **设置 › 插件 › 插件库** 里装上。

| 插件 | 怎么用 | 从哪来 |
|---|---|---|
| 小工具 | `= 1+2*3` 计算（只打 `=` 列出算过的账），`12 inch to cm` 换算，`#ff8800` 颜色，还有时间戳、UUID、Base64、哈希 | 随包 |
| 结束进程 | `kill 进程名` | 随包 |
| [系统开关](docs/使用说明.md#系统开关) | 深色模式、蓝牙、WLAN、音量等一键切换：`关蓝牙`、`静音`、`音量 30`、`保持唤醒 1小时` | 随包，默认关 |
| [截图与录屏](docs/使用说明.md#截图) | 截图、标注、打码、长截图、贴图、截图翻译、录屏和 GIF、演示白板 | 插件库 |
| [图片文字搜索](docs/使用说明.md#图片文字搜索) | 搜图片和扫描件里的字，或 `ocr 关键词` | 插件库 |
| 按意思找 | `? 去年报销的规定` | 插件库 |
| 翻译 | `tr 句子` 翻译，`dict 单词` 查词典；截图翻译也用它 | 插件库 |
| [网页搜索](docs/使用说明.md#网页搜索) | `baidu 天气`、`zhihu 显卡` | 插件库 |
| 浏览器书签 | `bm 关键词`，搜 Chrome、Edge 的书签，拼音也行；打开开关后 `bh 关键词` 搜浏览历史 | 插件库 |
| 局域网传输 | `传`，和开着 [LocalSend](https://localsend.org) 的手机、电脑互传文件 | 插件库 |
| [剪贴板同步](src/sync-plugin/package/README.md) | 在自己的几台 Windows 电脑和 Mac（[Mac顺](https://github.com/LingCore/MacShun)）之间同步剪贴板：这边复制，那边直接粘贴文字、图片、文件；端到端加密，不用账号；`同步` 看设备、加新设备 | 插件库 |
| 休息提醒 | 定时提醒休息，`休息` 看还有多久 | 插件库 |
| [鼠标手势](src/gestures-plugin/package/README.md) | 按住右键画一下：后退、关窗口、切标签页；选中文字再画或按快捷键：复制、搜索、翻译；画圈、屏幕角落、屏幕边、右键+滚轮、按程序设置，关机重启先倒数；`手势` 暂停 | 插件库 |
| 缩写展开 | 在任何地方打 `cmd` 再按空格就换成 `command`，缩写和内容自己设，紧接着按退格撤回；`缩写` 列出、暂停 | 插件库 |
| 资源管理器标签页 | 新开的资源管理器窗口并成标签页，还能在下次登录后打开上次的标签页（Windows 11 22H2 起） | 插件库 |
| [云备份](docs/使用说明.md#外观设置和备份) | 设置自动备份到 WebDAV（坚果云、NAS）或 GitHub 私有仓库，`cloud` 立即备份或恢复 | 插件库 |
| 开发工具 | `json` 格式化、压缩、转义剪贴板里的 JSON，`Enter` 打开树形窗口查看、搜索键名和值；直接粘贴 JSON 也认 | 插件库 |

想自己写插件：零代码的 `plugin.json`、任意语言的脚本都行，附 Python / PowerShell / C++ SDK，见 [插件文档](docs/plugins.md)。

交给 AI 助手：`npx skills add LingCore/WinShun` 给 Claude Code、Codex、Cursor 这类助手装上 Win顺 的 Skill，一共三个：`winshun`（用 `ws` 命令搜文件、装插件）、`winshun-plugin-dev`（写插件）、`winshun-dev`（改 Win顺 本身）。装插件也可以让它来：`ws plugin search 翻译`、`ws plugin install winshun.translate`，要你点头的地方（插件要的权限等）Win顺 会弹窗问你，见 [命令行 ws](docs/使用说明.md#命令行-ws)。

### 常用按键

| 按键 | 作用 |
|---|---|
| 双击 `Ctrl` | 打开 / 关闭搜索框（可以改成双击 `Alt`、`Shift`，或 `Alt+Space` 这类组合键） |
| `Enter` | 打开 |
| `Ctrl+Enter` | 打开所在文件夹 |
| `Ctrl+Shift+Enter` | 以管理员身份运行 |
| `Ctrl+1`～`Ctrl+9` | 打开第几项 |
| `Tab` | 切换范围：全部 / 文件 / 内容（打开了“应用”标签时还有应用） |
| `Ctrl+E` | 展开成搜索面板 |
| `Alt+P` | 预览 |
| `Ctrl+S` | 排序：最匹配 / 最新 / 最早 |
| `Ctrl+C` / `Ctrl+Shift+C` | 复制文件 / 复制路径 |
| `Ctrl+G`（在对话框里） | 转到文件管理器正开着的文件夹 |
| `Ctrl+,` | 打开设置 |

全部按键见 [快捷键总表](docs/使用说明.md#快捷键总表)。

### 搜索小技巧

| 这样打 | 意思 |
|---|---|
| `报告 2024` | 名字里同时有“报告”和“2024”，顺序不限 |
| `ndbg`、`niandubaogao` | 拼音首字母、全拼 |
| `"年度 报告"` | 引号里的当成一个整体 |
| `!草稿` | 不要名字里有“草稿”的 |
| `*.pdf`、`ext:pdf,docx` | 按扩展名找 |
| `报告 \| 总结` | 有“报告”或有“总结” |
| `项目\readme` | 在名字含“项目”的文件夹里找 readme |
| `D:\My Files\` | 直接进到这个文件夹，列出里面的东西 |
| `size:>10mb`、`dm:7d` | 大于 10 MB、7 天内改过 |

更多写法（正则、类型筛选等）见 [搜索语法](docs/使用说明.md#搜索语法)。

### 下载安装

到[官网下载页](https://lcstp.com/projects/winshun#downloads)（走国内加速线路）、[CNB 镜像](https://cnb.cool/lcstp/WinShun/-/releases)（腾讯云，国内快）或 [GitHub Releases]（三处是同一份文件）(https://github.com/LingCore/WinShun/releases/latest) 下载。系统要求：Windows 10 或 11，64 位。

| 下载哪个 | 适合谁 |
|---|---|
| **安装版** `WinShun-版本号-x64-setup.exe` | 大多数人。“为所有用户安装”带索引服务，最快；“只为我安装”不要管理员权限，只索引你选的文件夹 |
| **便携版** `WinShun-版本号-x64-portable.zip` | 不想安装。解压后运行 `WinShun.exe`，设置和索引都在旁边的 `data` 文件夹，可以整个带走 |
| **离线安装版** `WinShun-版本号-x64-offline-setup.exe` | 没网的电脑。自带增强文字识别组件 |
| **Scoop** | `scoop bucket add winshun https://github.com/LingCore/WinShun`，再 `scoop install winshun` |
| **WinGet** | `winget install LingCore.WinShun` |

**更新**：有新版本时托盘会提醒，点“下载并安装”，Win顺 自己下载、核对 SHA-256 后装好并重新打开，设置和索引都保留。卸载在 Windows 的“设置 → 应用”里。

<details>
<summary><b>安装细节</b>：便携版、Scoop、WinGet、增强文字识别、AI 工具连着时升级</summary>

AI 工具（Claude Code、Cursor 等）正连着 Win顺 时，安装程序会先问一声：现在装，它会断开，装好后在它里面重新连接 Win顺 或重启它；也可以等它用完再装。

不想安装的话，下载 `WinShun-版本号-x64-portable.zip`，解压到任意文件夹，运行里面的 `WinShun.exe` 就行。设置、记录、剪贴板历史和索引都存在它旁边的 `data` 文件夹里，整个文件夹可以拷到 U 盘或别的电脑；第一次启动时，这台电脑上原来的设置和数据（`%APPDATA%\WinShun`、`%LOCALAPPDATA%\WinShun`）会复制过来，原处的不动。升级时把新版解压到同一个位置覆盖，`data` 留着。删掉 `data` 文件夹（先退出 Win顺），就和安装版一样存在原来的地方。

用 [Scoop](https://scoop.sh) 管理软件的话，两行命令装好：

```powershell
scoop bucket add winshun https://github.com/LingCore/WinShun
scoop install winshun
```

装的就是上面的便携版（设置和索引存在 Scoop 的 `persist\winshun\data` 里），开始菜单里有 “WinShun”，终端里直接能用 `ws`。更新：Win顺 弹出更新提醒时点“用 Scoop 更新”（会打开一个窗口运行 `scoop update`，完成后自动重新打开），或自己运行 `scoop update winshun`。卸载用 `scoop uninstall winshun`，会先取消开机自启、右键菜单等 Win顺 登记的项目，设置和索引保留；加 `-p` 才连同数据一起删除。Scoop 不会装索引服务，需要索引整个磁盘时到 **设置 › 搜索范围 › 磁盘 › 索引方式** 里开启。

用 [WinGet](https://learn.microsoft.com/windows/package-manager/) 的话一条命令：

```powershell
winget install LingCore.WinShun
```

装的是**离线安装版**（自带增强文字识别），按“为所有用户安装”装进 `C:\Program Files\WinShun`，带索引服务，Windows 问一次管理员权限。包在 [microsoft/winget-pkgs](https://github.com/microsoft/winget-pkgs/tree/master/manifests/l/LingCore/WinShun)：新版本发布后才有对应的更新提交，审核合并后才上架，所以常常比 Releases 晚一两天，`winget show LingCore.WinShun` 里的版本就是能装到的版本。更新用 `winget upgrade LingCore.WinShun`；托盘里的“下载并安装”照常能用，装完就是最新版，这时 WinGet 记的版本比实际装的旧，`winget upgrade` 要等包追上才有动作（不会把更新的版本装回去）。卸载用 `winget uninstall LingCore.WinShun`，运行的就是“设置 → 应用”里那个卸载程序。

标准安装版和便携版按需安装增强文字识别：到 **设置 › 剪贴板 › 记录 › 增强文字识别** 下载约 12 MB 的组件，也可以选择本地组件包。安装后剪贴板和截图共用，离线识别，不需要重启。旧版已有的识别文件继续使用。无网络的电脑可选 `WinShun-版本号-x64-offline-setup.exe`，自带完整识别组件；或把 Release 中的 `WinShun-ocr-*.zip` 拷过去从本地安装。截图识别尚未安装时会保留原图，安装后继续识别。 程序更新、插件库和增强识别的 GitHub 文件默认优先通过两个加速镜像下载，连接失败或文件校验不符会自动换源，最后尝试官方原站。

</details>

<details>
<summary><b>管理员权限</b>：平时不用；哪些情况会受影响</summary>

- **平时不用**：Win顺 以普通权限运行。安装版带一个很小的“Win顺 索引服务”，由它读 NTFS 主文件表（MFT）和 USN 变更日志，所以照样几秒建好索引、实时跟上改动。安装时要管理员同意一次，之后登录和打开都不弹 UAC。
- **能看到哪些文件**：和 Everything 一样，索引里有整块盘的文件名，包括这台电脑上其他用户的；没有权限的文件照样打不开。
- **便携版**：默认一个个文件夹走一遍来建索引，第一次慢一些，之后实时跟上改动。在设置 → 搜索范围 → 磁盘 → 索引方式里可以装上索引服务，或以管理员身份重新启动。便携版装索引服务时，会把服务程序复制一份放进 `%ProgramFiles%\WinShun Service`，只有管理员能改，普通程序没法把它换掉；以前装过的，设置里会提醒并有“修复”按钮。
- **没有管理员权限（“只为我安装”）**：安装时选“只为我安装”，不装索引服务，也不用管理员同意。这时 Win顺 只索引你选的文件夹（设置 › 搜索范围 › 文件夹 里改，默认是桌面、文档、下载、图片这些常用位置），可以从资源管理器把文件夹直接拖进去；改动实时跟上，每天再整个核对一遍（也可以改成每小时、每 4 小时或只在启动时）。输入的文件夹不在索引范围里时，搜索框会提示，点“这次也搜它”只搜这一次，点“加入索引”就长期收录。内容搜索照样能用，只是慢一些。
- **管理员直读**：管理员账户可以在设置 → 搜索范围 → 磁盘 → 索引方式里选“管理员直读”，像旧版一样整个程序以管理员身份运行、直接读盘。
- **以管理员身份运行的程序**：Windows 不让普通权限的程序往它们里面打字。往这样的窗口里粘贴时，装了索引服务的由服务替你按一次粘贴键（设置 › 剪贴板 › “粘贴到以管理员身份运行的程序”，默认开启）；没有索引服务或关掉这一项时，Win顺 只复制，提示你按 `Ctrl+V`。它们的“打开”“另存为”对话框旁也有搜索框：装了索引服务的安装版能像别的对话框一样直接转到选中的文件夹，`Ctrl+G` 也管用；便携版和“只为我安装”的版本只能复制选中的路径，要你自己粘贴到路径框里。
- **它们在最前面时双击 Ctrl 没反应**：Windows 也不把发给它们的按键交给普通权限的程序，所以没装索引服务时，这种窗口在最前面，双击 Ctrl（Alt、Shift）没反应；带 Ctrl、Alt、Shift 或 Win 的组合键（比如 `Alt+Space`）照样能用。装了索引服务就没有这个问题（服务的助手听得到），Win顺 自己以管理员身份运行（管理员直读）时也没有。这种程序第一次在最前面停一会儿时，Win顺 会弹一次通知说明：设了组合键就提醒你改按它；便携版（含 Scoop 装的）能装索引服务时，点通知直接到 设置 › 搜索范围 › 磁盘 › 索引方式，在下面一行装上。每个程序只说一次，最多说 3 个，一次运行只说一次。
- **打开的东西是普通权限**：从 Win顺 打开的文件和应用都以普通权限运行，只有 `Ctrl+Shift+Enter` 才以管理员身份运行。常用管理员身份开的程序（比如终端），可以在 **设置 › 结果列表 › 操作习惯** 里打开“记住以管理员身份运行”（默认关闭）：这样运行过一次后，之后按 `Enter` 也以管理员身份运行，这一行会标出“管理员”；右键选“以普通权限运行”改回。

</details>

### 最近更新

**0.44.0**

- ⬅️ **设置窗口能后退、前进**：标题栏箭头、`Alt+←/→`、鼠标侧键都行，回去还在原来的位置；⏬ 滚到底再滚就到下一页；🔗 相关的设置之间能直接跳过去。
- 🖱️ **托盘图标左键、中键自己选**做什么；⌨️ 一个快捷键**打开设置**；📸 Win顺 截图记不记进剪贴板历史**单独设**；🔍 搜不到时提示**排除目录、已跳过的文件里**有没有，点一下这次也搜；🛡️ **以管理员身份在终端中打开**（#425）；🔢 1–9 编号跟着滚动走。
- 🛠️ **改进** 钉住的搜索框在任务栏上有图标了；剪贴板的预览和看大图窗口有了边框和阴影。

详见 [0.44.0 发布说明](docs/release-notes/v0.44.0.md)。

**0.43.0**

- 📌 **钉住的搜索框是个普通窗口了**：任务栏上有按钮，`Alt+Tab` 能切到，能最小化；📋 **贴图里的字直接拖选复制**，`Shift+C` 复制全部文字。
- 🖱️ 结果里右键可以**默认弹出系统菜单**；📁 索引和不搜索的文件夹可以**直接输入路径**（支持 `%USERPROFILE%`）；🗂️ 文件夹菜单可以把收藏夹、最近和已打开的文件夹**直接列在第一层**；✏️ 截图编辑时 `Esc` 先放下工具，方便回去改标注。
- 🛠️ **修复** 索引服务装不上（错误 5）；插件窗口没有边框；Directory Opus 的“此电脑”里弹不出文件夹菜单。

详见 [0.43.0 发布说明](docs/release-notes/v0.43.0.md)。

**0.42.0**

- 🔁 **剪贴板同步**（新插件）：在自己的几台 Windows 电脑和 Mac（Mac顺）之间，这边复制、那边粘贴文字、图片和文件，端到端加密，不用账号。
- 📂 **OneCommander 里直接打字搜索**；🖥️ 截图可以**跨屏框选**；📋 **粘贴为纯文本**的快捷键；🔍 剪贴板**看大图**；🖱️ 鼠标手势**选中文字按快捷键翻译**；🌗 搜索框阴影可以调浅或关掉。
- 🛠️ **改进** 正则搜索快多了；空文件夹的标记挨着名字；Windows 10 1809 上能录屏了；国内可以从 [CNB 镜像](https://cnb.cool/lcstp/WinShun/-/releases)下载。

详见 [0.42.0 发布说明](docs/release-notes/v0.42.0.md)。

**0.41.0**

- 🖱️ **鼠标手势 2.0**：选中文字再画一下就复制、搜索、翻译；画圈、屏幕角落、屏幕边、右键+滚轮、按程序设置，关机重启先倒数 5 秒；新的设置窗口里画出来就能设。
- ⌨️ **缩写展开**（新插件）：打 `cmd` 加空格就换成 `command`；📋 剪贴板可以选打开时显示哪一类；🗂️ 收藏圆盘按 `Tab` 翻页；❤️ 捐赠按钮可以挪到拾穗计划页。
- 🛠️ **修复** 数据文件夹里的插件搜不到；设置换分类更快；长截图后不再留下空框。

详见 [0.41.0 发布说明](docs/release-notes/v0.41.0.md)。

**0.40.0**

- 🖍️ **剪贴板备注可以上色**，七种颜色一眼分开；🔍 不用搜索的话可以把剪贴板的搜索框藏起来。
- 📐 **设置窗口记住大小**，重启后照样，上次最大化的仍然最大化；不想要在 设置 › 外观 › 记住窗口大小 关掉。
- 🛠️ **修复** 管理员身份运行时软件内更新后回到管理员身份；有道词典划词复制的文件、图片不再进剪贴板历史；Everything SDK 程序按路径搜；缩略图没读完就关窗口偶尔会让程序退出。

详见 [0.40.0 发布说明](docs/release-notes/v0.40.0.md)。

**0.39.0**

- 🗂️ **收藏夹点列头排序**：名称、修改日期、类型、大小一点就排好，再点反过来，“恢复默认”随时回到原来的顺序。
- 🧩 **插件页排成卡片**，点开看详情；🕘 插件关键词的搜索（比如算式）也进搜索历史；❤️ 设置窗口多了“捐赠”。
- 🛠️ **修复** 很长很细的图片读字时占几个 GB、让 Win顺 闪退（截图与录屏 1.1.27、图片文字搜索 1.0.24）；万一闪退，下次启动会记下停在哪里。

详见 [0.39.0 发布说明](docs/release-notes/v0.39.0.md)。

**0.38.0**

- 📌 **剪贴板里的文字也能贴到屏幕**（`F3`），⚙️ 搜“截图设置”这样的词**直达插件设置**，Win顺 设置排在“设置”第一行。
- ✂️ 剪贴板可以**只复制、粘贴预览里选中的一部分**，打开位置多了“上次的位置”，菜单的键不开菜单也能按；💻 “在终端中打开”**能选 PowerShell、Windows 终端、Git Bash 等**。
- 🫙 **空文件夹、空文件在结果里标出来**；🔀 快捷筛选可以带自己的排序；📂 文件夹菜单列宽跟着名字收紧，还能列出文件。

详见 [0.38.0 发布说明](docs/release-notes/v0.38.0.md)。

**0.37.0**

- 🧰 **开发工具**（新插件）：`json` 格式化、压缩、转义 JSON，树形查看、搜索键名和值。
- 📊 **截图识别的表格可以“复制为表格”**，粘进 Excel、WPS 一格一格排好；📄 只装了 WPS 也能**预览 Word、Excel、PPT 里的文字**。
- ⚙️ **设置分类更少更清楚**；📋 剪贴板跟着“弹出动画”滑入淡出，带公式的文字按原样显示，复制一个文件时侧边预览和搜索的预览一样；修好剪贴板打开卡住半分钟（#111）。

详见 [0.37.0 发布说明](docs/release-notes/v0.37.0.md)。

**0.36.0**

- 🧭 **新装默认像 Listary**：对话框旁的搜索框、文件夹菜单、资源管理器里打字搜索都默认开着（升级的保持原样）。
- 🧮 **WPS 公式从剪贴板历史粘回去还能编辑**；🖼️ **WebP、HEIC 图片在剪贴板列表里有缩略图**；💬 **悬停提示可以关**。
- ⬇️ 第一次**增量更新**：从 0.35.0 只下载变了的部分；文件夹里打字可以带类型，复制的图片文件也在“图片”分类里。

详见 [0.36.0 发布说明](docs/release-notes/v0.36.0.md)。

**0.35.0**

- 🖱️ **鼠标手势**（新插件）：按住右键画一下就能后退、关窗口、叫出搜索框。
- 🔲 **收藏排成图标**；🎬 **预览里放视频**；📊 **排序：打开最多**；↔️ **长截图可以横着截**（截图与录屏 1.1.23）。
- ⬇️ **增量更新**，下一次更新起只下载变了的部分；网页搜索可以用指定浏览器打开，单击预览，搜索面板记住铺满屏幕，Clover、360 文件夹的标签页里也能用文件夹菜单。

详见 [0.35.0 发布说明](docs/release-notes/v0.35.0.md)。

**0.34.2**

- 🛠️ **修复** 较早的 Windows 10 上托盘图标几乎看不见、深色标题栏不生效、录屏直接失败、截图排除自己窗口时变黑（截图与录屏 1.1.22）。

详见 [0.34.2 发布说明](docs/release-notes/v0.34.2.md)。

**0.34.1**

- 🛠️ **修复** 较早的 Windows 10 上闲着时不归还内存，一直占着两三百 MB（截图与录屏 1.1.21、图片文字搜索 1.0.20 一起修了）。

详见 [0.34.1 发布说明](docs/release-notes/v0.34.1.md)。

**0.34.0**

- 🔤 **字母直达可以换键、换字母**，`Shift`+字母点一行上的按钮（#357）。
- ✖️ **贴图角上的关闭按钮**，鼠标从哪边进来就在哪个角（截图与录屏 1.1.20）；⚡ **可以关掉弹出动画**（#365）。
- 🛠️ **修复** Total Commander 通过 Everything 接口看不到文件夹大小、“遍历”方式的磁盘不收录目录链接（#364）；以管理员身份运行时更新后回到管理员身份（#363）；下拉菜单的滚动条能拖了。

详见 [0.34.0 发布说明](docs/release-notes/v0.34.0.md)。

**0.33.0**

- 🗂️ **文件夹菜单一列列并排展开**，前面的列都留着；✏️ **贴图原地编辑**，工具条直接长在贴图上。
- 🌐 **截图翻译有自己的快捷键**；🪟 **框选前按 `Tab` 只框整个窗口**（截图与录屏 1.1.19）。
- 🔗 **`winshun://` 链接** 从网页、笔记里打开搜索框或设置；📏 **界面大小“密集”**，每条结果一行。
- ↪️ **`Tab` 一层层进文件夹，`Ctrl+Backspace` 退回来**；#️⃣ **文件右键菜单复制 SHA-256**（小工具 1.2.0）。
- 🛠️ **修复** “另存为”旁的搜索框乱跳、对话框最大化时挡住地址栏，现在停在屏幕底部的任务栏上（#362）；搜索面板 `Ctrl+Shift+1…5` 切换标签。

详见 [0.33.0 发布说明](docs/release-notes/v0.33.0.md)。

**0.32.0**

- 🖱️ **右键拖出结果** 按住右键把文件拖到资源管理器，松开时选复制、移动还是创建快捷方式。
- 📂 **对话框旁的搜索框** 回车直接打开搜到的文件，`Ctrl+Enter` 只转到它所在的文件夹，和搜索框的习惯一致；AutoCAD 和 XP 样式的打开窗口也一样。
- 🔗 **目录链接自动收进索引** 用 `mklink` 建的、指向别处的目录链接不用再手动添加。
- 📝 **插件库里看更新内容**；🌐 **截图翻译可以用其他翻译插件**；📄 **关键字命令边打边列出文件**（`code 名字` 用 VS Code 打开）。
- 🛠️ **修复** 文件夹菜单自己一层层钻进去；选文件夹时路径变成乱码；F2 改名的输入框盖住原名；资源管理器标签页在部分电脑上不合并（1.1.3）。

详见 [0.32.0 发布说明](docs/release-notes/v0.32.0.md)；以往版本见 [Releases](https://github.com/LingCore/WinShun/releases)。

### 常见问题

<details>
<summary><b>和 Everything、Listary 有什么区别？</b></summary>

Win顺 自己建索引，不用装 Everything。文件名、拼音、应用和文件内容在一个框里搜，针对中文做了优化。对话框里有和 Listary 一样的 `Ctrl+G` 和搜索框，但不嵌进资源管理器窗口。

</details>

<details>
<summary><b>双击 Ctrl 和别的软件冲突？</b></summary>

Listary、PowerToys 的“查找鼠标”默认也用双击 Ctrl，关掉其中一边就行，或者让 Win顺 改用双击 Alt 或 Shift、只认左边或右边的键（设置 › 快捷键 › 打开 Win顺 › 双击哪个键、用哪一边；输入法用 Shift 切换中英文的，双击 Shift 会切换两次，最后回到原来的状态）。也可以在 Win顺 里改用别的组合键。游戏全屏、藏起鼠标时，Win顺 默认不响应双击 Ctrl。

</details>

<details>
<summary><b>AutoHotkey 改的键在 Win顺 里不起作用？</b></summary>

选了“管理员直读”时，Win顺 以管理员身份运行，Windows 不让普通权限的程序拦截发给它的按键。换回别的索引方式，或用 AutoHotkey 的 UI Access 模式、以管理员身份运行脚本。

</details>

<details>
<summary><b>占多少内存？索引能不放 C 盘吗？</b></summary>

- **内存**：用着的时候主要是文件索引，每 100 万个文件约 30 MB；三百万个文件的电脑上，搜索时整个程序约 150～230 MB。关掉搜索框约 20 秒后，Win顺 把用不着的内存还给系统，任务管理器里降到几 MB。想看内存花在哪，打开 **设置 › 运行状况**。
- **索引位置**：在 **设置 › 常规 › 索引位置** 里，可以移到其他内置硬盘。

</details>

<details>
<summary><b>索引很慢，或者出了问题？</b></summary>

到 **设置 › 常规 › 诊断日志** 点“导出诊断日志”，把打开的文件夹里的 `WinShun-diagnostics.zip` 发到 GitHub Issues 或论坛。诊断包记的是版本、系统、磁盘、索引进度和错误、闪退时停在程序的哪个位置，以及界面显示、字体和剪贴板按键转发的状态、已装插件的版本和插件日志末尾（字显示不对时先导出再重启），不含设置、剪贴板内容、搜索历史和文档正文。

</details>

<details>
<summary><b>收费吗？会上传数据吗？</b></summary>

完全免费，MIT 开源。不会上传你的文件和索引，索引和历史只存在本机（`%LOCALAPPDATA%\WinShun`）。只在这些时候联网：

- **检查新版本**：连接 lcstp.com，只发送系统版本和安装方式，不带个人信息（可以关）。
- **匿名统计**：每天最多一次告诉 lcstp.com 这台电脑在用 Win顺，用来统计用户数。只发送电脑标识的单向哈希（不能还原，也对不上作者的其他软件）、版本号和 Windows 版本（设置 › 常规 › 发送匿名统计，可以关）。打开“拾穗计划”页时从 lcstp.com 取各个作品的用户数。
- **下载更新**：你点“下载并安装”后，经第三方镜像或 GitHub 下载安装包，并用 SHA-256 校验。
- **网站图标**：第一次显示某个网页搜索或书签时，取一次那个网站的图标。
- **插件库**：打开设置里的“插件库”页、点“刷新”或安装插件时，取列表和插件；开着“自动更新”时每天检查一次已装插件有没有新版（可以关）。从旧版升级时，以前开着、现在不再随 Win顺 自带的插件（截图与录屏、翻译等）也从插件库取一次。
- **搜索建议**：在网站关键词后打字时，把这些字发给那个网站取搜索建议（不打关键词时一个字也不发，可以关）。
- **你自己选的服务**：“按意思找”插件选了云服务时，把文档的文字发给你选的服务商；用“翻译”插件时，把要翻译的句子发给你填的翻译服务。

</details>

更多问题见 [使用说明 · 常见问题](docs/使用说明.md#常见问题)。

### 反馈

- **故障与需求**：通过 [故障反馈](https://github.com/LingCore/WinShun/issues/new?template=1-bug-report.yml) 或 [功能建议](https://github.com/LingCore/WinShun/issues/new?template=2-feature-request.yml) 提交，便于跟踪处理。提交前请先搜索 [已有 Issues](https://github.com/LingCore/WinShun/issues)。
- **使用问题**：到 [使用问答](https://github.com/LingCore/WinShun/discussions/categories/q-a) 提问。
- **交流与分享**：在 [交流区](https://github.com/LingCore/WinShun/discussions/categories/general) 分享经验，在 [插件与作品分享区](https://github.com/LingCore/WinShun/discussions/categories/show-and-tell) 展示插件、配置和作品。

### 从源码编译

<details>
<summary>需要 Visual Studio 2022、Qt 6.8 或更新版本、CMake 和 Ninja</summary>

需要 Visual Studio 2022（“使用 C++ 的桌面开发”）、Qt 6.8 或更新版本（推荐 6.12，MSVC 2022 64 位套件）、CMake 3.25+ 和 Ninja。

只想编译、不需要完整历史时，用 `git clone --filter=blob:none https://github.com/LingCore/WinShun.git`，历史里的截图等旧文件用到时才下载，克隆小很多。

```powershell
./scripts/build.ps1                                # 编译（不跑测试）
./scripts/build.ps1 -Tests 'tst_capture'           # 编译 + 只跑指定的单元测试
./scripts/build.ps1 -QtDir C:\Qt\6.12.0\msvc2022_64 # 指定 Qt（默认用 QTDIR 或 C:\Qt 下最新的版本）
./scripts/build.ps1 -Deploy                        # 生成可分发的文件夹 dist\WinShun
./scripts/release.ps1                              # 打包安装程序和 SHA256SUMS.txt（需要 Inno Setup 6.5+）；加 -Publish 发布到 GitHub
```

也可以用 Qt Creator 或 VS Code（CMake Tools）打开，预设见 `CMakePresets.json`。源码结构和设计见 [docs/architecture.md](docs/architecture.md)，踩过的坑见 [docs/pitfalls.md](docs/pitfalls.md)。

用 AI 助手改代码：在克隆下来的仓库里运行 `npx skills add LingCore/WinShun --skill winshun-dev`，它会先读到怎么编译、只跑哪些测试、界面规范和常见的坑（就是 [skills/winshun-dev](skills/winshun-dev/SKILL.md) 这份说明）。

</details>

---

<a id="english"></a>

## English

### What is WinShun

WinShun (Win顺, "Windows made smooth"): press `Ctrl` twice and a search box pops up, with results as you type; `Enter` opens one. Files, the text inside them, apps, Windows settings and your clipboard history are all found from this one box.

- ⚡ **Fast**: it reads the NTFS master file table directly, so three million files are indexed in seconds and a search takes milliseconds; new, renamed and deleted files show up at once. No Everything install needed.
- 🀄 **Made for Chinese too**: full pinyin and initials work (`ndbg` finds 年度报告), even a mistyped homophone.
- 🔒 **Free and private**: MIT-licensed, no account; the index and history stay on your PC. The interface comes in English and Simplified Chinese.

### Get started

1. Download `WinShun-<version>-x64-setup.exe` from the [Releases page](https://github.com/LingCore/WinShun/releases/latest) (or, faster from mainland China, the [official site](https://lcstp.com/projects/winshun#downloads) or the [CNB mirror](https://cnb.cool/lcstp/WinShun/-/releases)) and run it. If Windows says “Windows protected your PC”, click **More info → Run anyway**: the app is not code-signed yet.
2. The first start shows two short setup pages: the content index, clipboard history, screenshots, and the key that opens WinShun. Choose **Get started**, or **Set up later** and change anything in the settings whenever you like.
3. Press `Ctrl` twice, type, press `Enter`. `Ctrl` twice again or `Esc` closes it.

No installer, or Scoop and WinGet? See [Download and install](#download-and-install).

### What it does

| | Feature | In one line |
|:-:|---|---|
| 🔍 | [File search](docs/features.en.md#file-search) | By name, pinyin or initials; local drives, USB drives, NAS and network shares, WSL |
| 📄 | [Text inside files](docs/features.en.md#full-text-search) | Word, Excel, PowerPoint, PDF, WPS, text and code, showing the matching line; regular expressions too |
| 🗂️ | [Search panel](docs/features.en.md#file-search) | `Ctrl+E` expands it: filter by kind, date, size and folder, save your own quick filters, sort by column, large icons |
| 👀 | [Preview](docs/features.en.md#file-search) | `Alt+P` shows a picture, document or PDF without opening it |
| 🚀 | [Apps and settings](docs/features.en.md#apps-and-settings) | Start menu apps and about 1,200 Windows settings pages; an optional Apps tab shows the apps as a grid of icons and switches to open windows |
| 📋 | [Clipboard history](docs/features.en.md#clipboard-history) ¹ | Replaces `Win+V` with text, pictures and files, group search and hover previews |
| 📂 | [Open / Save dialogs](docs/features.en.md#open-and-save-dialogs) ² | A search bar beside the dialog; `Ctrl+G` jumps to the folder open in your file manager |
| 🖱️ | [In File Explorer](docs/features.en.md#in-file-explorer) ² | Double-click empty space for your folders and the folders inside them; type in a file list to search |
| ⭐ | [Favorites](docs/features.en.md#favorites) | Give files, folders and apps a keyword to open them; sort them by name, date, type or size with a click; your own commands too, also in the right-click menu of files |
| 🔎 | [Taskbar search box](docs/features.en.md#taskbar-search-box) ² | A WinShun search box on the Windows 11 taskbar |
| 🤖 | [AI assistants and the command line](docs/features.en.md#ai-assistants-mcp) ² | Let Claude, Cursor and others search your files (read-only by default); `ws` searches from a terminal |
| 🎨 | [Looks and backup](docs/features.en.md#looks-and-settings) | Three interface styles, your font and interface size; settings backups and migration, to WebDAV or a private GitHub repository with the Cloud Backup plugin |

¹ Chosen during the first-start setup, or turned on later in the settings.　² Off by default; turn it on in the settings.

Every feature in detail: **[docs/features.en.md](docs/features.en.md)**. Screenshots: **[docs/截图.md](docs/截图.md)**.

### Plugins

Some plugins come with WinShun; the rest are installed when you want them from **Settings › Plugins › Plugin library**.

| Plugin | How | Where from |
|---|---|---|
| Tools | `= 1+2*3` calculator (`=` alone lists past sums), `12 inch to cm`, `#ff8800` colors, timestamps, UUID, Base64, hashes | Bundled |
| Kill | `kill process` | Bundled |
| System switches | Dark mode, Bluetooth, WLAN, volume and more in one step: `bluetooth off`, `mute`, `volume 30`, `awake 1h` | Bundled, off by default |
| [Screenshot and recording](docs/features.en.md#screenshots-and-recording) | Capture, mark up, redact, scrolling screenshots, pins, translating a capture in place, MP4 and GIF recording, presenter and whiteboard | Plugin library |
| Image text search | Search the words in pictures and scans, or `ocr words` | Plugin library |
| Search by meaning | `? last year's expense rules` | Plugin library |
| Translate | `tr sentence` to translate, `dict word` for a dictionary; screenshot translation uses it too | Plugin library |
| Web search | `baidu 天气`, `zhihu 显卡` | Plugin library |
| Browser bookmarks | `bm words` searches Chrome and Edge bookmarks, pinyin too; once turned on, `bh words` searches the browsing history | Plugin library |
| LAN transfer | `ls ` to send files to and from phones and PCs running [LocalSend](https://localsend.org) | Plugin library |
| [Clipboard sync](src/sync-plugin/package/README.md) | Copy on one of your Windows PCs or Macs ([Mac顺](https://github.com/LingCore/MacShun)), paste on another: text, pictures, files; end to end encrypted, no account; `sync` lists the devices and lets a new one in | Plugin library |
| Break reminder | Reminds you to take breaks; `break` shows the time left | Plugin library |
| [Mouse gestures](src/gestures-plugin/package/README.md) | Hold the right button and draw: back, close the window, switch tabs; select text, then draw or press a shortcut: copy, search, translate; circles, screen corners and edges, the wheel with the right button, rules per program, a countdown before shutting down; `gestures` pauses them | Plugin library |
| Text expander | Type `cmd` and a space anywhere and it becomes `command`, with abbreviations of your own; Backspace right after takes it back; `expander` lists and pauses them | Plugin library |
| Explorer tabs | New File Explorer windows open as tabs, and the tabs from last time come back after a logon (Windows 11 22H2 or newer) | Plugin library |
| [Cloud backup](docs/features.en.md#looks-and-settings) | Keeps the settings backup on a WebDAV server or in a private GitHub repository; `cloud` backs up or restores | Plugin library |
| Developer tools | `json` formats, minifies or escapes the JSON on the clipboard, `Enter` opens a tree to browse and search it; JSON pasted into the box works too | Plugin library |

Write your own: a zero-code `plugin.json` or scripts in any language, with SDKs for Python, PowerShell and C++. See [docs/plugins.en.md](docs/plugins.en.md).

Hand it to an AI assistant: `npx skills add LingCore/WinShun` installs WinShun's skills for Claude Code, Codex, Cursor and the like. There are three: `winshun` (search files and install plugins with the `ws` command), `winshun-plugin-dev` (write a plugin) and `winshun-dev` (work on WinShun itself). It can install plugins too: `ws plugin search translate`, `ws plugin install winshun.translate`; where your yes is needed (the permissions a plugin asks for), WinShun asks you in its own window.

### Keys

| Key | Action |
|---|---|
| `Ctrl` twice | Open / close the search box (or Alt or Shift twice, or a shortcut such as `Alt+Space`) |
| `Enter` | Open |
| `Ctrl+Enter` | Show in folder |
| `Ctrl+Shift+Enter` | Run as administrator |
| `Ctrl+1`…`Ctrl+9` | Open the nth row |
| `Tab` | Switch scope: All / Files / Content |
| `Ctrl+E` | Expand to the search panel |
| `Alt+P` | Preview |
| `Ctrl+S` | Sort: best match / newest / oldest |
| `Ctrl+C` / `Ctrl+Shift+C` | Copy the file / copy its path |
| `Ctrl+G` (in a file dialog) | Go to the folder open in your file manager |
| `Ctrl+,` | Open the settings |

All keys: [docs/features.en.md](docs/features.en.md#keys).

### Search tips

| Type | Finds |
|---|---|
| `report 2024` | Names with both words, in any order |
| `ndbg`, `niandubaogao` | Pinyin initials or full pinyin |
| `"annual report"` | The exact phrase |
| `!draft` | Leaves out names with “draft” |
| `*.pdf`, `ext:pdf,docx` | By extension |
| `report \| summary` | Either word |
| `project\readme` | readme inside folders named project |
| `D:\My Files\` | Goes into that folder and lists it |
| `size:>10mb`, `dm:7d` | Over 10 MB, changed in the last 7 days |

Regex, kinds and more: [Query syntax](docs/features.en.md#file-search).

### Download and install

Get it from the [Releases page](https://github.com/LingCore/WinShun/releases/latest), or, faster from mainland China, from the [official site](https://lcstp.com/projects/winshun#downloads) or the [CNB mirror](https://cnb.cool/lcstp/WinShun/-/releases) (the same files). Requires Windows 10 or 11, 64-bit.

| Download | For |
|---|---|
| **Setup** `WinShun-<version>-x64-setup.exe` | Most people. “For all users” comes with the index service, the fastest; “for me only” needs no administrator and indexes only the folders you choose |
| **Portable** `WinShun-<version>-x64-portable.zip` | No installer. Unzip and run `WinShun.exe`; settings and index live in the `data` folder beside it, so the whole folder can travel |
| **Offline setup** `WinShun-<version>-x64-offline-setup.exe` | PCs without internet. Includes enhanced text recognition |
| **Scoop** | `scoop bucket add winshun https://github.com/LingCore/WinShun`, then `scoop install winshun` |
| **WinGet** | `winget install LingCore.WinShun` |

**Updates**: WinShun tells you in the tray; “Download and install” downloads it, checks its SHA-256, installs it and starts again, keeping your settings and index. Uninstall from Windows Settings → Apps.

<details>
<summary><b>Install details</b>: portable copy, Scoop, WinGet, enhanced text recognition, upgrading while an AI tool is connected</summary>

If an AI tool (Claude Code, Cursor …) is connected to WinShun, Setup asks first: install now and reconnect WinShun in that tool (or restart it) afterwards, or install later.

To skip the installer, download `WinShun-<version>-x64-portable.zip`, unzip it anywhere and run `WinShun.exe` in it. Settings, history, clipboard and index are kept in the `data` folder beside it, so the whole folder can go on a USB stick or another PC; on the first start, what this PC already has (`%APPDATA%\WinShun`, `%LOCALAPPDATA%\WinShun`) is copied in, the originals left in place. To upgrade, unzip the newer one over it and keep `data`. Without the `data` folder (quit WinShun first), it keeps them where the installed copy does.

With [Scoop](https://scoop.sh), two commands do it:

```powershell
scoop bucket add winshun https://github.com/LingCore/WinShun
scoop install winshun
```

This installs the portable package (settings and index live in Scoop's `persist\winshun\data`), adds “WinShun” to the Start menu and puts `ws` on the PATH. To update, click “Update with Scoop” in WinShun's update reminder (it opens a window that runs `scoop update`, then starts WinShun again) or run `scoop update winshun` yourself. `scoop uninstall winshun` first takes back what WinShun registered (autostart, Explorer menus ...) and keeps your settings and index; add `-p` to delete the data too. Scoop does not install the index service; to index whole disks turn it on in **Settings › Search scope › Drives › Index access**.

With [WinGet](https://learn.microsoft.com/windows/package-manager/), one command does it:

```powershell
winget install LingCore.WinShun
```

This installs the **offline setup** (enhanced text recognition included) for all users into `C:\Program Files\WinShun`, index service and all, with one administrator prompt. The package is in [microsoft/winget-pkgs](https://github.com/microsoft/winget-pkgs/tree/master/manifests/l/LingCore/WinShun): a release reaches it only through a new submission that is reviewed before it goes live, so it often trails Releases by a day or two, and the version in `winget show LingCore.WinShun` is the one you get. Update with `winget upgrade LingCore.WinShun`; “Download and install” in the tray still works and takes WinShun to the newest release, after which WinGet's version lags the installed one and `winget upgrade` waits until the package catches up (it never moves you back to an older version). Uninstall with `winget uninstall LingCore.WinShun`, which runs the same setup as Settings → Apps.

Standard and portable packages install enhanced text recognition on demand. Open **Settings › Clipboard › Recording › Enhanced text recognition** to download about 12 MB, or install a local component archive. Clipboard and screenshots share it and work offline after installation, with no restart. Existing recognition files remain usable. For a PC without internet, choose `WinShun-<version>-x64-offline-setup.exe`, or copy the Release’s `WinShun-ocr-*.zip` and install it locally. Screenshot recognition keeps the original image and continues once the component is installed. App updates, the plugin library and enhanced recognition prefer two download mirrors for public GitHub files, then the original source; failed connections and rejected files automatically try the next source.

</details>

<details>
<summary><b>Administrator rights</b>: not needed day to day; when they matter</summary>

- **Not needed day to day**: WinShun runs with your normal rights. The installed version comes with a small “WinShun Index Service” that reads the NTFS master file table (MFT) and change journal (USN) for it, so millions of files are still indexed in seconds and changes followed live. An administrator approves the install once; after that there is no UAC prompt at login or when you open it.
- **Which files you see**: as with Everything, the index holds the file names of whole drives, other users' on this PC included; files you have no rights to still won't open.
- **Portable version**: by default it walks the folders one by one, so the first index is slower; changes are followed live after that. Settings → Search scope → Drives → Index access can install the index service or restart it as administrator. When a portable copy installs the service, it puts a copy of the service program in `%ProgramFiles%\WinShun Service`, where only administrators can change it, so no ordinary program can swap it; an older registration gets a warning and a Repair button in the settings.
- **Without administrator rights (“for me only”)**: pick “for me only” in Setup: no index service, and no administrator needed. WinShun then indexes only the folders you choose (Settings › Search scope › Folders; the desktop, documents, downloads, pictures and the like to begin with), and you can drag folders in from File Explorer. Changes are followed live, and everything is checked again once a day (hourly, every 4 hours or only at start are options). When the folder you type is not indexed, the search box says so: “Search it this time” looks there once, “Add to the index” keeps it. Content search still works, only slower.
- **Direct as administrator**: on an administrator account, Settings → Search scope → Drives → Index access → “Direct (administrator)” runs the whole app as administrator and reads the drives itself, as earlier versions did.
- **Programs running as administrator**: Windows doesn't let normal-rights programs type into them. Pasting into such a window, the index service presses the paste keys for you where it is installed (Settings › Clipboard › "Paste into programs run as administrator", on by default); without the service, or with that turned off, WinShun only copies and tells you to press `Ctrl+V`. The search bar beside their Open / Save dialogs appears too: the installed WinShun with the index service jumps to the chosen folder just as in other dialogs, and `Ctrl+G` works there; a portable copy, or one installed for you only, can only copy the chosen path for you to paste into the path box yourself.
- **Double Ctrl does nothing while they are in front**: Windows doesn't give the keys typed into them to normal-rights programs either, so without the index service double Ctrl (Alt, Shift) does nothing while such a window is in front; a shortcut with Ctrl, Alt, Shift or Win (`Alt+Space`, say) still works. With the index service installed this doesn't happen (the service's helper hears the keys), nor when WinShun itself runs as administrator (Direct). The first time such a program stays in front for a moment, WinShun says why in one notification: it names your shortcut if you set one, and where a portable copy (Scoop's too) can install the index service, a click opens Settings › Search scope › Drives › Index access, with the install button just below. Each program is mentioned once, three at most, and one per run.
- **What you open is not elevated**: files and apps you open from WinShun run with your normal rights; only `Ctrl+Shift+Enter` runs one as administrator. For programs you mostly run as administrator (a terminal, say), turn on "Remember “Run as administrator”" in the settings (off by default): once run that way, the program runs as administrator on `Enter` from then on and its row says Admin; right-click it and choose "Run with normal rights" to change it back.

</details>

### What's new

**0.44.0**

- ⬅️ **Back and Forward in Settings**: the title bar arrows, `Alt+←/→` or the mouse's side buttons, back to the same spot; ⏬ scroll past the end for the next page; 🔗 links between related settings.
- 🖱️ **Choose what the tray icon's left and middle clicks do**; ⌨️ a key to **open Settings**; 📸 WinShun screenshots in the clipboard history **set on their own**; 🔍 nothing found? it says what sits **in excluded folders or skipped files**, one click to search them too; 🛡️ **open in Terminal as administrator** (#425); 🔢 the 1–9 numbers follow the scroll.
- 🛠️ **Improved**: the pinned search box has its icon on the taskbar; the clipboard's preview and picture viewer have a frame and shadow.

See the [0.44.0 release notes](docs/release-notes/v0.44.0.md).

**0.43.0**

- 📌 **The pinned search box is a window of its own**: a taskbar button, `Alt+Tab`, minimize; 📋 **select and copy the words in a pin**, `Shift+C` copies them all.
- 🖱️ **The system context menu by default** on results, if you like; 📁 **type or paste folders** to index or skip (`%USERPROFILE%` too); 🗂️ the folder menu can list favorites, recent and open folders **at its first level**; ✏️ `Esc` puts a screenshot tool down first, so earlier annotations are easy to change.
- 🛠️ **Fixed**: the index service would not install (error 5); plugin windows without a frame; no folder menu in Directory Opus's This PC.

See the [0.43.0 release notes](docs/release-notes/v0.43.0.md).

**0.42.0**

- 🔁 **Clipboard sync** (new plugin): copy on one of your Windows PCs or Macs (Mac顺), paste text, pictures and files on another; end to end encrypted, no account.
- 📂 **Type to search in OneCommander**; 🖥️ **screenshots over several monitors**; 📋 a key to **paste as plain text**; 🔍 clipboard **pictures large**; 🖱️ mouse gestures: **a shortcut to translate selected text**; 🌗 a lighter search box shadow, or none.
- 🛠️ **Improved**: regex search is much faster; the empty tag sits by the name; recording works on Windows 10 1809; downloads from the [CNB mirror](https://cnb.cool/lcstp/WinShun/-/releases) for mainland China.

See the [0.42.0 release notes](docs/release-notes/v0.42.0.md).

**0.41.0**

- 🖱️ **Mouse gestures 2.0**: select text and draw to copy, search or translate; circles, screen corners and edges, the wheel with the right button, rules per program, a 5-second countdown before shutting down; a new settings window where you draw a gesture to set it.
- ⌨️ **Text expander** (new plugin): type `cmd` and a space for `command`; 📋 the clipboard opens on the tab you choose; 🗂️ pages on the favorites pie, turned with `Tab`; ❤️ the donate key can move to the Gleaning page.
- 🛠️ **Fixed**: plugins in WinShun's data folder could not be found; switching settings categories is quicker; no empty frame left after a long screenshot.

See the [0.41.0 release notes](docs/release-notes/v0.41.0.md).

**0.40.0**

- 🖍️ **Clipboard notes in colour**, seven to tell entries apart; 🔍 the clipboard's search box can be hidden.
- 📐 **The settings window keeps its size** after a restart, maximized if you left it so; Settings › Appearance › Remember window sizes turns it off.
- 🛠️ **Fixed**: back as administrator after an in-app update; Youdao's selection translation no longer fills the clipboard history; path searches from programs on the Everything SDK; a window closed while thumbnails were still loading could end the program.

See the [0.40.0 release notes](docs/release-notes/v0.40.0.md).

**0.39.0**

- 🗂️ **Sort favorites by a column**: name, date modified, type or size in one click, again for the other way; Restore order brings back the order you had.
- 🧩 **Plugins as tiles** with a page each; 🕘 plugin keyword searches (sums, say) in the search history; ❤️ a Donate button in the settings window.
- 🛠️ **Fixed**: reading the words in a long thin picture took several GB and could end WinShun (Screenshot and recording 1.1.27, Image text search 1.0.24); should it crash, the next start records where it stopped.

See the [0.39.0 release notes](docs/release-notes/v0.39.0.md).

**0.38.0**

- 📌 **Pin text from the clipboard on the screen** (`F3`); ⚙️ words like “capture settings” **open a plugin's settings**, and WinShun's settings come first for “settings”.
- ✂️ **Copy or paste just the part selected in the clipboard preview**, open the clipboard where it was last, use its menu's keys without the menu; 💻 **choose PowerShell, Windows Terminal, Git Bash and more** for Open in Terminal.
- 🫙 **Empty folders and files are marked in the results**; 🔀 quick filters can have their own sort; 📂 folder menu columns as wide as their names, with files on request.

See the [0.38.0 release notes](docs/release-notes/v0.38.0.md).

**0.37.0**

- 🧰 **Developer tools** (new plugin): `json` formats, minifies and escapes JSON, with a tree to browse and search names and values.
- 📊 **Copy as a table** for words a screenshot reads as a table, pasting cell by cell into Excel or WPS; 📄 **Word, Excel and PowerPoint previews as text** where only WPS is installed.
- ⚙️ **Fewer, clearer settings categories**; 📋 the clipboard follows the open animation, shows text with a formula as it looked and previews one copied file as the search does; opening the clipboard no longer hangs for half a minute (#111).

See the [0.37.0 release notes](docs/release-notes/v0.37.0.md).

**0.36.0**

- 🧭 **Listary-style from the start**: the dialog search bar, the folder menu and typing to search in File Explorer are on for new installs (upgrades keep what they had).
- 🧮 **WPS formulas paste back editable from the clipboard history**; 🖼️ **WebP and HEIC thumbnails in the clipboard list**; 💬 **hover tips can be turned off**.
- ⬇️ The first **delta update**: from 0.35.0 only what changed is downloaded; a kind while typing in a folder, and copied picture files under Pictures.

See the [0.36.0 release notes](docs/release-notes/v0.36.0.md).

**0.35.0**

- 🖱️ **Mouse gestures** (new plugin): hold the right button and draw to go back, close a window or open the search box.
- 🔲 **Favorites as icons**; 🎬 **videos play in the preview**; 📊 **sort by Opened most**; ↔️ **sideways long screenshots** (Screenshot and recording 1.1.23).
- ⬇️ **Delta updates**, downloading only what changed from the next update on; web searches in a chosen browser, click to preview, the search panel remembers filling the screen, and the folder menu works in Clover and 360 Folder tabs.

See the [0.35.0 release notes](docs/release-notes/v0.35.0.md).

**0.34.2**

- 🛠️ **Fixed** on older Windows 10: a nearly invisible tray icon, dark title bars not applied, recording failing at once, and WinShun's own windows turning black when left out of a screenshot (Screenshot and recording 1.1.22).

See the [0.34.2 release notes](docs/release-notes/v0.34.2.md).

**0.34.1**

- 🛠️ **Fixed** older Windows 10 builds never giving memory back when idle, holding two or three hundred MB all day (Screenshot and recording 1.1.21 and Picture text search 1.0.20 too).

See the [0.34.1 release notes](docs/release-notes/v0.34.1.md).

**0.34.0**

- 🔤 **Letter jump with your own key and letters**, and `Shift`+letter presses a row's buttons (#357).
- ✖️ **A close button in a pin's corner**, on the side the mouse came in from (Screenshot and recording 1.1.20); ⚡ **the open animation can be turned off** (#365).
- 🛠️ **Fixed** Total Commander getting no folder sizes through the Everything interface, and directory links not taken on walked disks (#364); an update started as administrator comes back as administrator (#363); drop-down scroll bars can be dragged.

See the [0.34.0 release notes](docs/release-notes/v0.34.0.md).

**0.33.0**

- 🗂️ **Folder menu columns** side by side, the earlier ones staying; ✏️ **edit a pin where it is**, the toolbar on the pin itself.
- 🌐 **Screenshot translation on a hotkey**; 🪟 **`Tab` before selecting outlines whole windows only** (Screenshot and recording 1.1.19).
- 🔗 **`winshun://` links** open the search box or the settings from web pages and notes; 📏 **Dense interface size**, one line per result.
- ↪️ **`Tab` into folders, `Ctrl+Backspace` back out**; #️⃣ **Copy a file's SHA-256 from its menu** (Tools 1.2.0).
- 🛠️ **Fixed** the bar by Save As dialogs jumping about and covering a maximised dialog's address bar: it now sits over the taskbar at the bottom (#362); `Ctrl+Shift+1…5` switch the panel's tabs.

See the [0.33.0 release notes](docs/release-notes/v0.33.0.md).

**0.32.0**

- 🖱️ **Drag results out with the right button** into File Explorer, and choose to copy, move or create a shortcut on release.
- 📂 **The bar by file dialogs** opens a file found with Enter, while `Ctrl+Enter` only goes to its folder, as in the search box; AutoCAD's and XP-style Open windows too.
- 🔗 **Directory links indexed by themselves**: links made with `mklink` that lead elsewhere need no adding by hand.
- 📝 **What's new in the plugin library**; 🌐 **screenshot translation through other translators**; 📄 **keyword commands list files as you type** (`code name` opens it in VS Code).
- 🛠️ **Fixed** the folder menu walking down by itself; garbled paths from folder pickers; the F2 rename box covering the old name; Explorer tabs not merging on some PCs (1.1.3).

See the [0.32.0 release notes](docs/release-notes/v0.32.0.md); earlier versions are on [Releases](https://github.com/LingCore/WinShun/releases).

### FAQ

<details>
<summary><b>How is it different from Everything or Listary?</b></summary>

WinShun builds its own index, so Everything is not needed. File names, pinyin, apps and file contents are searched from one box, tuned for Chinese. File dialogs get Listary's `Ctrl+G` and a search bar, but WinShun does not embed itself in Explorer windows.

</details>

<details>
<summary><b>Ctrl twice clashes with another program?</b></summary>

Listary and PowerToys' Find My Mouse use it too: turn it off on one side, let WinShun use Alt or Shift twice instead, or count only the left or the right key (Settings › Shortcuts › Open WinShun › Key to press twice / Which side), or give WinShun another shortcut. (An input method that switches language with Shift switches twice on a double Shift, so it ends where it began.) Full-screen games and games that hide the mouse are ignored by default.

</details>

<details>
<summary><b>AutoHotkey remaps don't work in WinShun?</b></summary>

With “Direct (administrator)” chosen, WinShun runs as administrator, and Windows keeps normal-rights programs from intercepting keys sent to it. Pick another index access, or run the script with UI Access or as administrator.

</details>

<details>
<summary><b>How much memory? Can the index leave drive C?</b></summary>

- **Memory**: mostly the file index while in use, about 30 MB per million files; on a PC with three million files the whole app uses about 150–230 MB while searching. About 20 seconds after the search box closes, WinShun gives what it no longer needs back to Windows, and Task Manager shows a few MB. **Settings › Health** shows where it goes.
- **Index location**: the index can move to another built-in drive under **Settings › General › Index location**.

</details>

<details>
<summary><b>Indexing is slow, or something is wrong?</b></summary>

Under **Settings › General › Diagnostic logs**, click “Export diagnostic logs” and attach the `WinShun-diagnostics.zip` from the folder that opens to a GitHub issue. It holds the version, system, disks, indexing progress and errors, where the program stopped if it crashed, the installed plugins' versions and the tail of the plugin log, not your settings, clipboard, search history or document text.

</details>

<details>
<summary><b>Is it free? Does it collect data?</b></summary>

Free and MIT-licensed. Your files and index are never uploaded; the index and history stay on your PC (`%LOCALAPPDATA%\WinShun`). It goes online only:

- **to check for new versions** at lcstp.com, sending only the Windows version and the kind of install, nothing personal (you can turn it off);
- **for anonymous usage statistics**: at most once a day it tells lcstp.com that this PC uses WinShun, to count the users. It sends a one-way hash of the computer's ID (it cannot be reversed or matched with the author's other apps), the version and the Windows version (Settings › General › Send anonymous statistics; you can turn it off). The 拾穗计划 (Gleaning) page fetches each work's number of users from lcstp.com when it opens;
- **to download an update** when you click “Download and install”, through third-party mirrors or GitHub, checked against its SHA-256;
- **for site icons**: once, the first time a web search site or bookmark is shown;
- **for the plugin library**: when you open its page in the settings, press Refresh or install a plugin, and once a day to check installed plugins for updates while Automatic updates is on (you can turn it off). After an upgrade, plugins you had on that no longer come with WinShun (Screenshot and recording, Translate and others) are fetched from it once;
- **for search suggestions**: what you type after a site keyword goes to that site (nothing is sent without a site keyword; you can turn this off);
- **for services you choose**: document text to the cloud service picked for Search by meaning, and sentences to the translation service entered in Translate.

</details>

### Feedback

- **Bugs and feature requests**: use the [bug report](https://github.com/LingCore/WinShun/issues/new?template=1-bug-report.yml) or [feature request](https://github.com/LingCore/WinShun/issues/new?template=2-feature-request.yml) form so they can be tracked. Please search [existing issues](https://github.com/LingCore/WinShun/issues) first.
- **Usage questions**: ask in [Q&A](https://github.com/LingCore/WinShun/discussions/categories/q-a).
- **Discussion and sharing**: exchange tips in [General](https://github.com/LingCore/WinShun/discussions/categories/general), or share plugins, configurations and projects in [Show and tell](https://github.com/LingCore/WinShun/discussions/categories/show-and-tell).

### Build from source

<details>
<summary>Needs Visual Studio 2022, Qt 6.8 or later, CMake and Ninja</summary>

Needs Visual Studio 2022 (Desktop development with C++), Qt 6.8 or later (6.12 recommended, MSVC 2022 64-bit kit), CMake 3.25+ and Ninja.

To build without the full history, clone with `git clone --filter=blob:none https://github.com/LingCore/WinShun.git`: old files such as past screenshots are fetched only when needed, so the clone is much smaller.

```powershell
./scripts/build.ps1                                # build (no tests)
./scripts/build.ps1 -Tests 'tst_capture'           # build and run only the named unit tests
./scripts/build.ps1 -QtDir C:\Qt\6.12.0\msvc2022_64 # pick a Qt kit (default: QTDIR or the newest under C:\Qt)
./scripts/build.ps1 -Deploy                        # make the distributable folder dist\WinShun
./scripts/release.ps1                              # package the installer and SHA256SUMS.txt (needs Inno Setup 6.5+); -Publish makes the GitHub release
```

Architecture, design notes and benchmarks (in Chinese) are in [docs/architecture.md](docs/architecture.md).

With an AI assistant: run `npx skills add LingCore/WinShun --skill winshun-dev` in your clone. It tells the assistant how to build, which tests to run, the UI rules and the usual pitfalls ([skills/winshun-dev](skills/winshun-dev/SKILL.md)).

</details>

---

## LINUX DO

本项目积极参与并认可 [LINUX DO 社区](https://linux.do)。

WinShun is proud to be part of the [LINUX DO community](https://linux.do).

## 感谢收录 · Thanks

感谢 [果核剥壳](https://www.ghxi.com/winshun.html) 收录 Win顺。

Thanks to [果核剥壳 (ghxi.com)](https://www.ghxi.com/winshun.html) for listing WinShun.

## 许可证 · License

Win顺 以 [MIT 许可证](LICENSE) 发布。Copyright © 2026 LingCore.

WinShun is released under the [MIT License](LICENSE). Copyright © 2026 LingCore.

随附的第三方内容按各自的许可使用，不在 MIT 许可范围内 · Bundled third-party content keeps its own license:

- 图标 · Icons — [Phosphor Icons](https://phosphoricons.com) 2.1.2（MIT），见 · see `third_party/phosphor/LICENSE`；安装后在 · installed in `licenses\`。
- `resources/fonts/` 阿里巴巴普惠体 3.0 · Alibaba PuHuiTi 3.0 — © Alibaba Group，按其免费商用授权随程序附带 · redistributed unmodified under its free commercial-use terms; only the full Regular font is shipped, with heavier UI weights synthesized by Qt. See `resources/fonts/LICENSE.txt`.
- 图片文字识别 · Text recognition — [ONNX Runtime](https://github.com/microsoft/onnxruntime)（MIT）和 · and [PaddleOCR](https://github.com/PaddlePaddle/PaddleOCR) 的 PP-OCRv6 模型 · models（Apache-2.0），见 · see `third_party/ppocr/LICENSE`；安装后在 · installed in `licenses\`。
- 二维码识别 · QR code reading — [zxing-cpp](https://github.com/zxing-cpp/zxing-cpp) 3.1.1（Apache-2.0），见 · see `third_party/zxing-cpp/LICENSE`；安装后在 · installed in `licenses\`。
- 拼音读音数据 · Pinyin data — [pinyin-data](https://github.com/mozillazg/pinyin-data)，MIT，见 · see `tools/data/pinyin-data-LICENSE.txt`。常用字的读音以其中的《通用规范汉字字典》（2013）数据为准。
