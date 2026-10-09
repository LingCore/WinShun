# Win顺 架构与设计

面向想读代码、改代码的人。使用说明见 [README](../README.md)，开发中踩过的坑见 [pitfalls.md](pitfalls.md)。

## 设置文件

设置窗口里的每一项都存在 `%APPDATA%\WinShun\WinShun.ini`，直接改这个文件也会立即生效：

```ini
[Launcher]
DoubleCtrl=true          ; 双击 Ctrl 呼出
Hotkey=                  ; 额外的全局快捷键，例如 Alt+Space、Ctrl+Shift+F
DialogJump=true          ; “打开”“另存为”对话框里按 Ctrl+G，转到资源管理器正在显示的文件夹
Renderer=auto            ; auto（内存 ≤ 16 GB 用 software，否则 d3d11）| d3d11（文字清晰）| software（省内存，文字偏模糊）

[Appearance]
Theme=system             ; system（跟随 Windows 的浅色 / 深色）| light | dark
Language=system          ; system（Windows 显示语言是中文就用中文，否则英文）| zh | en

[Update]
Automatic=true           ; 启动时和之后每 12 小时到 GitHub 检查新版本

[Index]
ExcludedPaths=...        ; 不建索引的文件夹，支持 %WINDIR% 这类环境变量，逗号分隔
ExcludedNames=...        ; 任意位置的同名文件夹，也可写尾部路径，如 .svn/pristine
IncludeRemovableDrives=false
RescanOnStartup=true     ; 启动 15 秒后在后台核对一次磁盘，补上程序未运行期间的变化

[Content]
Extensions=txt, md, ...  ; 内容搜索的扩展名，逗号分隔（只支持纯文本格式）
MaxFileSizeMB=64         ; 超过此大小的文件不搜内容
IncludeSystemFolders=false ; “内容”范围也搜系统、程序目录和 node_modules 等（文件多，会慢不少）
Index=true               ; 建立内容索引（中日韩文字和英文、数字；NTFS 磁盘，后台建立，几十万个文件约占 200 MB 磁盘）

[Clipboard]
Enabled=true             ; 记录剪贴板历史；第一次运行时和 Windows 自带的剪贴板历史开关一致
WinV=false               ; 用 Win+V 打开，代替 Windows 自带的剪贴板（资源管理器重启或下次登录后生效）
Hotkey=                  ; 另一个打开剪贴板的组合键，例如 Win+Alt+V
MaxItems=1000            ; 分组以外最多保留几条
MaxDays=30               ; 多少天没再复制或粘贴过就删除；0 = 一直保留（分组里的都一直保留）
Images=true              ; 也记录图片
ExcludedApps=KeePass.exe, KeePassXC.exe, 1Password.exe, Bitwarden.exe ; 不记录这些程序复制的内容
```

修改“不搜索的文件夹”后不需要重建索引：新排除的文件夹直接从索引里去掉，取消排除的文件夹单独补读一遍。只有取消排除“文件夹名称”（它可能出现在任何位置）时才会重新读取所有磁盘。

索引、最近使用记录和日志（`WinShun.log`，只记警告和错误）保存在 `%LOCALAPPDATA%\WinShun`；剪贴板历史在其中的 `clipboard\`（`clipboard.db` 和存图片的 `images\`）。

本程序原名“快搜”（QuickFind）。第一次以 Win顺 启动时，会先让还在运行的快搜退出，再把 `%APPDATA%\QuickFind`、`%LOCALAPPDATA%\QuickFind` 搬到新位置，开机自启（计划任务“QuickFind”）也换成“WinShun”，设置、索引和最近使用记录都保留。

## 性能（实测）

测试机：i9-14900HX，4 块本地磁盘，共 **316 万**个文件和文件夹。

| 项目 | 结果 |
|---|---|
| 首次建立索引（读 MFT，C 盘的 MFT 有 4.4 GB） | 约 9 秒（含保存） |
| 之后启动时加载索引 | 0.5 秒，随后补读变更日志（通常几十毫秒），不再全盘重扫 |
| 同样 316 万项改用遍历目录 | 约 7 秒（文件系统缓存已热时；冷启动要慢得多） |
| 文件名搜索 | 一般 4–9 毫秒（单字母约 16 毫秒） |
| 内容搜索（查内容索引） | 33 万个候选文件：查索引 1–4 毫秒，筛出可能含有的文件约 12 毫秒，再只打开这些文件取行号。例如 `季度报告` 剩 18 个 |
| 内容搜索（索引用不上：一两个英文字母，或还没收录的文件） | 开着 Windows Defender 时每个文件第一次打开约 7–10 毫秒（它要扫描），16 个文件同时读，每秒约 800 个；结果边找边显示 |
| 内容索引 | 33 万个文件占 172 MB 磁盘（2.5 亿个“片段-文件”对，每对约 0.7 字节）；文档表约 4 MB 内存，索引文件按需映射，不占私有内存 |
| 打开窗口 | 约 35 毫秒；第一次也只要约 60 毫秒（启动时已预先画好一次，否则第一次要 200 多毫秒） |
| 内存 | 文件名索引：323 万项约 97 MB（约 31 字节/项）。整个程序在任务管理器里约 120 MB（刚启动），打开过窗口、内容索引建好后约 140 MB；另占显存约 10–25 MB |

一般电脑有 50 万到 100 万个文件，文件名索引大约占 15–30 MB。可用 `wsbench` 在自己的机器上测（见下文）。

## 界面语言

界面文字的源文是中文（QML 的 `qsTr()`、C++ 的 `tr()`），英文翻译在 `src/app/i18n/winshun_en.ts`，编译成 `winshun_en.qm` 嵌进程序资源。切换语言时装上或卸下这个翻译器，再调 `QQmlEngine::retranslate()`，C++ 里拼出来的文字（状态栏、托盘提示等）由各自的 `retranslate()` 重算，不用重启。

改了界面文字以后：

```powershell
cmake --build --preset release --target update_translations   # lupdate：把新字符串收进 .ts
```

然后在 `.ts` 里补上英文（Qt Linguist 或直接编辑），未翻译的条目会显示中文原文。数量相关的句子用 `%n` / `%Ln` 写，英文按单复数给两种形式。英文通常比中文长，搜索框、状态栏这些一行放不下就会被截断，翻译时要短。

## 检查更新和发布

没有自己的服务器：新版本就是 GitHub 上的 Release。`Updater` 用 WinHTTP（走系统代理设置，不用给 Qt Network 带 TLS 插件）请求 `api.github.com/repos/LingCore/WinShun/releases/latest`，比较 `tag_name` 和当前版本；有新版本时在托盘提示（设置窗口开着时直接弹对话框），“去下载”用浏览器打开那个 Release 的页面。上次检查的时间、跳过的版本、上次提醒的时间存在 `%LOCALAPPDATA%\WinShun\update.ini`；同一个版本自动提醒最多三天一次，跳过的版本只在手动检查时显示。测试时可以用环境变量 `WINSHUN_UPDATE_FEED` 指向一个假的接口地址。

不做自动下载和替换：没有代码签名的程序在后台下载 exe、再改写自己的文件，正是杀毒软件启发式检测盯的行为，何况 Win顺 以管理员身份运行。FlClash 也是只提示、打开浏览器下载。

发布新版本：

1. 改 `CMakeLists.txt` 里 `project(... VERSION x.y.z)`。
2. 写 `docs/release-notes/vx.y.z.md`。更新对话框只显示其中两部分：开头一段话（中文在前、英文在后，中间以中文句号分开），以及 `## 新功能 · What's new` 下的列表（每条是 `- 图标 中文`，下一行缩进写英文）。格式见 `src/core/Release.cpp` 和单元测试。
3. 退出正在运行的 Win顺，运行 `./scripts/release.ps1 -Publish`：编译、部署，打包安装程序 `WinShun-x.y.z-x64-setup.exe` 和 `SHA256SUMS.txt`，再用 `gh` 建 `vx.y.z` 的 Release。只发安装程序，不提供免安装的压缩包（0.2.0 及更早的版本是压缩包）。

安装程序用 Inno Setup 6.5 或更新版本编译（`installer/WinShun.iss`；6.5 起才有 Windows 11 风格和深色模式的向导），脚本会先找 `%LOCALAPPDATA%\Programs\Inno Setup*` 下的，也可以用 `-Iscc` 指定。

- 装到 `Program Files\WinShun`（Win顺 本来就要管理员权限），开始菜单一个快捷方式，“设置 → 应用”里可以卸载。`AppId` 永远不要改：新版靠它找到旧版、原地覆盖。
- 安装和卸载前先让正在运行的 Win顺 退出：装过的用 `WinShun.exe --quit`，别处的旧版免安装副本给它的消息窗口发 `WM_CLOSE`（0.2.1 起会照 `--quit` 退出、先保存索引），过 15 秒还在就强制结束。
- 装好后，已有的开机自启任务（旧版免安装副本设的）改成启动安装的这份（安装程序运行 `WinShun.exe --take-autostart`）；卸载时只删除指向本安装目录的任务，并问是否删除 `%APPDATA%\WinShun`、`%LOCALAPPDATA%\WinShun`。
- 向导图片由 `tools/make_installer_images.py` 生成（浅色、深色两套），中文界面文字是 Inno Setup 仓库里的非官方翻译 `installer/ChineseSimplified.isl`。

## 代码结构

```
src/core/            搜索引擎和剪贴板历史，只依赖 Qt Core、Qt Sql（SQLite）和 Win32，可单独测试
  FileIndex          紧凑的内存索引：每项 20 字节 + 文件名（WTF-8，重复名字只存一份）
  Ntfs               直接读 NTFS：主文件表（MFT）解析、USN 变更日志读取
  NtfsIndexer        MftTree（MFT → 按文件夹分组的名单）、UsnApplier（把日志记录应用到索引）
  Crawler            把一个文件夹的名单与索引“对账”；名单来自 MFT 或多线程遍历目录
  ChangeWatcher      ReadDirectoryChangesW 监听非 NTFS 磁盘（U 盘等）的增删改名
  IndexService       加载快照 → 追上变更日志 → 实时跟踪 → 定期保存
  Snapshot           索引的磁盘格式（流式写入，原子替换）
  Query / NameSearch 查询解析、打分、多线程扫描（SSE2 加速）
  Pinyin             拼音匹配（读音表由 tools/make_pinyin.py 从 pinyin-data 生成）
  AppCatalog         已安装应用：读 shell:AppsFolder（开始菜单“所有应用”），开始菜单文件夹或应用包有变化时才重读；按名称 / 拼音 / 首字母 / 程序名匹配
  AppLogo            Store 应用的图标文件：按 AppxManifest.xml 和资源限定符（targetsize、altform-unplated / lightunplated）选最合适的一个
  SystemCatalog      系统入口：Windows 设置检索清单里的设置页、控制面板项和任务，加上自带的 places.txt；按名称 / 关键词 / 拼音匹配
  ContentScanner     文本内容搜索：流式读取、编码识别、按行定位
  ContentIndex       内容索引：每个文件有哪些中日韩单字、相邻两字和英文三字片段，倒排表存在内存映射的段文件里
  ContentIndexer     后台读文件建内容索引；变更日志说哪个文件被写过，就重读哪个
  SearchEngine       后台搜索线程；新输入会取消正在进行的搜索
  ClipStore          剪贴板历史：SQLite 里的条目和分组、图片 PNG 文件；去重、过期清理、按分类和拼音筛选、多选合并
src/app/             界面与 Windows 集成
  Launcher           QML 用的视图模型（查询、结果、状态、操作）
  Clipboard          剪贴板页的视图模型（分类、分组、多选、预览、粘贴回原窗口）；ClipModel 是它的列表
  ColorText          文字里的颜色值（#rrggbbaa、rgba()、hsl() …）和它的几种写法；ColorSwatch 画色块，半透明的垫棋盘格
  SettingsEditor     设置窗口的视图模型（改动即保存）
  App                组装各部分，管理窗口、托盘、热键
  platform/          双击 Ctrl（Raw Input）、托盘、Shell 操作、驱动器插拔、窗口效果、对话框里的 Ctrl+G（DialogJump）、录快捷键时的键盘钩子（ShortcutCapture）、读写剪贴板（ClipboardWatcher）、把按键送回原窗口（Paster）、接管 Win+V（WinV）
  qml/               界面：Main / SearchBar / ResultRow / Footer / ClipboardPage / SettingsWindow …
tests/               单元测试（Qt Test）
tools/wsbench.cpp    在真实索引上测内存和搜索耗时
```

## 设计取舍

- **磁盘可以随时插拔。** 每个已索引的磁盘都登记了设备通知：要弹出或锁定（格式化、chkdsk）时，Win顺先关掉在这个磁盘上的所有句柄再答应；其他磁盘照常跟踪。快照按磁盘保存，磁盘增减时只读新增的那个。
- **始终以管理员身份运行，NTFS 磁盘直接读 MFT 和 USN 变更日志。** 建索引时一次顺序读完主文件表，不用逐个文件夹遍历；文件的每个硬链接都按各自的路径收录（`FSCTL_ENUM_USN_DATA` 只给一个名字，System32 里很多文件会因此丢失，所以没用它）。快照里记着每个盘的日志读到了哪里，启动时只补读这之后的记录，不再全盘重扫；运行中跟踪日志，没有 ReadDirectoryChangesW 缓冲区溢出的问题。日志被覆盖（程序关闭太久）或重建时，才重新读一遍该盘的 MFT。U 盘等非 NTFS 磁盘仍用遍历目录 + ReadDirectoryChangesW。
- **日志记录按顺序、可重复地应用。** 每一步都是“确保成某个状态”，所以从较早的位置重放也不会出错；改名的旧名和新名分成两条记录，保存的位置不会落在两者之间。日志只用文件夹的编号指明父文件夹，所以每个文件夹的 MFT 编号记在一张表里（约 40 万个文件夹占 5 MB 左右），随快照保存。
- **内容索引：少打开文件。** 内容搜索慢在打开文件：Windows Defender 在每个文件第一次被打开时扫描它，每个要好几毫秒，而且过一会儿就忘了扫过，下次搜索又要扫；读和匹配本身几乎不花时间。所以与其让读文件更快，不如少打开文件：索引记下每个文件里出现过哪些中日韩字和相邻两字，以及哪些“三个连在一起的英文字母、数字或下划线”（不分大小写）。搜一两个中日韩字时它就是答案；搜更长的词时，文件必须含有词里的每个片段才可能含有这个词，候选因此筛到很少，只打开这些文件取行号和摘要。索引的文档表随快照保存，和文件名索引永远对得上；NTFS 变更日志里的“内容被改写”记录告诉它该重读哪个文件。日志断了（程序关太久）时，只比较文件大小和修改时间，变了的才重读。新读的文件先在内存里攒着，攒够约 8 MB 写成一个段文件；同一级的段文件攒满 8 个就合成一个上一级的，所以每个片段只被重写几次，合并时边读边写，不会一下占很多内存。段文件尽量紧凑：每个片段的文件号列表按长短选最省的写法（逐个记间隔、按 128 个一组定宽打包、或者干脆一位一个文件的位图）；片段目录只记与前一个的差，每 64 个留一个可二分查找的入口；还有极少数大文件（33 万个里约 1700 个）各含上万种英文片段，与其把它们记进几万个列表，不如每个片段带一行“这几个大文件里有没有它”的位。33 万个文件因此从 279 MB 降到 172 MB，筛出的结果一个不差。
- **系统入口用 Windows 自己的设置检索清单。** 开始菜单搜设置靠的是 `%WINDIR%\ImmersiveControlPanel\Settings\AllSystemSettings_*.xml`：约 1500 条，每条有名称和几组关键词，都是资源字符串，`SHLoadIndirectString` 按显示语言解析（中文的关键词里也带英文和常见拼错，“查看网络连接”的有“适配器;卡;adapters;adaptor”）。几份清单对应不同版本的“设置”，以条目最多的为准。控制面板的条目直接给出打开方式（`shell:::{…}`、`Microsoft.DeviceManager`、命令行）；新版“设置”的页面只记页面 ID（`SettingsPageNetworkEthernet`），`ms-settings:` 地址要从较旧那份清单的 `PolicyIds` 里找。那是按字母排的一串名字，第一个不一定是页面本身（“声音”的第一个是 `apps-volume`），所以取页面 ID 里写着的那个（`installed-apps`），否则取被其余名字当前缀的那个（`sound` 之于 `sound-devices`）。带 `shcond://` 条件的条目（只在服务器上、要有触控笔、家庭版没有……）按注册表和系统信息判断，判断不了的不显示。Windows 安全中心的条目名称只解析得出英文，不用它们，改由 places.txt 写中文名。
  - 清单的关键词偏书面，“网卡”“梯子”这类说法没有，`src/core/places.txt` 补上（加在页面本身那一条上，不加在页面里的各个任务上，否则“壁纸”会显示成“视差背景”），也加开始菜单里没有的工具和文件夹。`wsbench --places [词 …]` 列出本机所有入口或每个词的前几名，并报告 places.txt 里对不上任何入口的条目。
  - 排序：places.txt 的关键词完整命中（50 分）高于名称以这个词开头（30 分加覆盖比例），Windows 的关键词完整命中（30 分）高于偶然对上的拼音首字母（`dns` 对上“电脑设置”）。只有一个字时只看名称。反过来，只靠拼音首字母对上关键词的（`glq` 对上“隔离区”）排在名称被这个词占了一半以上的入口之后（“凭据管理器”）：首字母容易撞上，关键词又不显示，用户看不出为什么是它；名称里只沾到一点的（`wk` 里的“当我看向别处时”）不算，“网卡”照样排第一。同一个打开命令只出一行（一个页面里的几个任务都打开这个页面），**全部** 范围里最多列 5 个，和应用按分数排在一起。
  - 读一遍约 0.3–0.5 秒，在后台；每次打开搜索框时比较清单文件的时间和显示语言，变了才重读。打开时和应用一样交给资源管理器，以普通权限运行。
- **窗口预先画好一次。** 窗口第一次显示时要建立显卡绘图环境、着色器和字形缓存，之后隐藏也一直保留，所以只有第一次打开慢。启动 1 秒后先在隐身、不抢焦点的状态下画一帧再藏起来，第一次打开就和之后一样快。代价是一启动就多占这部分内存（约 20 MB 内存和 10 MB 显存），反正打开过一次后也会占着。
- **打开的文件仍是普通权限。** 管理员进程直接打开文件，被打开的程序也会带管理员权限，不安全；所以“打开”交给资源管理器代为启动，只有 `Ctrl+Shift+Enter`（以管理员身份运行）才提权。资源管理器正在重启时会等几秒再试；用的是别的桌面外壳时，用它的权限启动；都不行就不打开（托盘会提示），绝不悄悄提权。
- **开机自启用计划任务。** Windows 会跳过启动项（注册表 Run 键）里需要提权的程序，所以改为登录时以最高权限运行的计划任务“WinShun”，开机不弹 UAC。旧版本写在 Run 键里的设置会在启动时自动迁移。
- **对话框里的 Ctrl+G 只靠窗口消息。** Listary 把钩子 DLL 注入其他程序，在对话框进程里直接让它换文件夹；一个以管理员身份运行的程序往别的进程里注 DLL，正是杀毒软件盯的行为，注进去的代码出错还会拖垮别人的程序。Win顺 从外面像用户一样操作对话框：
  - **只在文件对话框在最前面时注册 `Ctrl+G`。** `SetWinEventHook(EVENT_SYSTEM_FOREGROUND)` 跟着前台窗口走：前台是文件对话框（`#32770`，里面有 `DUIViewWndClassName` 和地址栏，或者是 XP 风格的 `SHELLDLL_DefView` 加文件名框）就 `RegisterHotKey`，换走就注销。别的程序里 `Ctrl+G` 照旧，也用不着键盘钩子。对话框刚到前台时控件可能还没建全，所以 `#32770` 窗口在 0.5 秒内再看几次。
  - **找文件夹**：`IShellWindows` 列出资源管理器的每个标签页，各取 `IShellBrowser` 的窗口和当前文件夹（`SIGDN_FILESYSPATH`）；再按 `EnumWindows` 的前后顺序找最前面的资源管理器窗口，取它排在最前的 `ShellTabWindowClass`（正在显示的标签）。显示的不是磁盘上的文件夹（主页、此电脑、搜索）就看下一个窗口。
  - **让对话框换文件夹**：在地址栏面包屑最右边的空白处投递一次单击，地址栏变成输入框（和资源管理器里一样），`WM_SETTEXT` 填入路径，再发回车。等输入框收起后用 `WM_NEXTDLGCTL` 把焦点还给原来的控件（通常是文件名框）。不往文件名框里填文件夹再按“确定”：选择文件夹的对话框会直接选中它并关闭，另存为对话框会丢掉已经输入的文件名。XP 风格的对话框没有地址栏，只能用文件名框：路径末尾加 `\`，另存为对话框就不会把它当成文件名（文件夹不存在时只报错），跳过去后再把原来的文件名填回去。
  - 实测（2026-10-08）：打开、另存为、选择文件夹、高 DPI 程序、XP 风格五种对话框，按下到对话框换好 35–161 毫秒。
- **双击 Ctrl 用 Raw Input，不用键盘钩子。** 低级键盘钩子会被系统里每一次按键同步调用，钩子一慢就拖慢所有程序的打字；回应超时后 Windows 还会悄悄把它摘掉，双击 Ctrl 从此失灵。Raw Input 是按键之后才异步送来的消息（在单独的线程上接收），两个问题都没有。
- **只在录快捷键时临时装低级键盘钩子。** 设置里点了快捷键方框，到录完、按 Esc 或焦点离开为止，`ShortcutCapture` 在自己的线程上装 `WH_KEYBOARD_LL` 钩子（PowerToys 的快捷键框也这样做）。设置窗口在前台时，按键在 Windows、Qt 和其他程序处理之前就被拿走，换成普通的按键事件交给录制框。不这样的话，Alt+Space 被 Qt 拿去弹系统菜单，Alt+F4 关掉设置窗口，Win+E 打开资源管理器，别的程序注册了的组合键（PowerToys Run、Copilot 常用 Alt+Space）直接打开那个程序，都录不上。现在都能录下来，注册不上的照常提示“已被其他程序或系统占用”。只吞录制开始后按下的键，以及这些键的抬起：之前就按着的键 Windows 要看到它完整抬起，否则会以为它一直按着。Win 键按下和抬起都不让 Windows 看到，所以不会弹开始菜单。音量、媒体键照常放行。钩子装不上时，录制框退回 Qt 自己的按键事件（录不到 Alt+Space）。
- **剪贴板历史和搜索共用一个窗口。** `Win+V` 打开的是同一个启动器窗口的另一页（`ClipboardPage.qml`），预先画好的窗口、云母效果、主题、多选和右键菜单都是同一套；和 Raycast、Alfred 一样，左边列表、右边是当前这一条的全文或大图。历史在内存里（每条文字的前 64K 个字符），打开不用等任何东西。
  - **接管 Win+V 不用键盘钩子。** `Win+V` 是资源管理器用 `RegisterHotKey` 占着的，别的程序注册不上（错误 1409）。资源管理器启动时读 `HKCU\Software\Microsoft\Windows\CurrentVersion\Explorer\Advanced\DisabledHotkeys`，里面写着的字母它就不注册；所以打开设置里的开关时往里加一个 `V`（保留原有的字母），资源管理器重启后 Win顺 就能 `RegisterHotKey(Win+V)`。重启资源管理器是结束任务栏所在的那个 `explorer.exe` 进程（退出码非零）。Winlogon 只重新启动它在登录时启动的那个外壳，被别的程序重启过的它不管，所以结束前先复制这个进程的令牌：3 秒内没有新外壳，就用这个令牌 `CreateProcessWithTokenW` 启动 `explorer.exe`，权限和原来一样（Win顺 直接启动的话会带着管理员权限，任务栏上打开的一切也就都是管理员的）；这也不行（要用到“Secondary Logon”服务），就以用户自己的 `sihost.exe` 为父进程启动，子进程拿父进程的令牌。这几秒的等待放在后台线程，任务栏没回来时托盘通知告诉用户怎么手动启动。`TaskbarCreated` 广播说明新的资源管理器起来了，这时重新登记一次。关掉开关、恢复默认设置或卸载时把 `V` 去掉；开关从没打开过时，注册表里原有的 `V`（用户为别的程序加的）不动。代价：Win顺 退出期间 `Win+V` 没有反应。
  - **读剪贴板在单独的线程上。** `ClipboardWatcher` 有自己的线程和隐藏窗口，用 `AddClipboardFormatListener` 收通知，等 40 ms 让程序把各种格式放齐再读。读别人复制的内容可能要那个程序现场生成（Excel 画单元格的图、Word 转 RTF），所以只在没有文字时才读图片，图片转 PNG 也在这个线程上，界面不受影响。
  - **不记录的内容按 Windows 的规定来。** 带 `ExcludeClipboardContentFromMonitorProcessing`、`Clipboard Viewer Ignore` 格式，或 `CanIncludeInClipboardHistory` 为 0 的不记（密码管理器和 Windows 凭据管理器复制密码时会带上，Windows 自带的剪贴板历史也认这几个）；前两个不打开剪贴板就能判断，不会妨碍那个程序。另有一份按程序文件名排除的名单。
  - **粘贴回原来的窗口。** 窗口从隐藏状态打开时记下当时的前台窗口。粘贴时先等 Shift、Ctrl、Alt、Win 都松开（按着 Shift+Enter 时直接发 Ctrl+V，对方收到的是 Ctrl+Shift+V），再由剪贴板线程写入这一条原来的各种格式（文字、HTML、RTF、文件列表、PNG + DIB），然后把前台交还给那个窗口（Win顺 这时在前台，所以 `SetForegroundWindow` 可以成功），等它真的到了前台、再等 60 ms（它要先把焦点放回输入框，否则按键落在窗口上就丢了）再用 `SendInput` 发 `Ctrl+V`；Git Bash（mintty）和 PuTTY 发 `Shift+Insert`。Win顺 以管理员身份运行，所以管理员权限的程序里也能粘贴。剪切来的文件以后再粘贴一律按复制处理，不会再移动一次。
  - **自己写的不重复记录，也不读回来。** 写剪贴板的就是监听线程，写完记下剪贴板的序号，收到这次变化的通知时直接跳过，把那一条挪到最前；多条合并的不记录。不能写完再打开剪贴板读一遍：那正是目标程序处理 `Ctrl+V`、打开剪贴板的时候，抢不到的一方粘贴失败。写入时仍带一个私有格式 `WinShun.ClipboardEntry`（这一条的编号），别的程序把它原样放回剪贴板时也认得出来。启动时剪贴板上已有的内容（序号和启动时一样）记下来，但已在历史里的不算新复制，不挪位置、不改时间。
  - **SQLite 存储。** 每粘贴一次都要更新那一条的时间，SQLite（WAL，不每次同步到磁盘）只改这一行，程序崩溃也不会坏掉；Ditto 等工具也这样存。图片单独存成 PNG 文件，删除的条目在能撤销期间保留图片。只依赖 Qt 自带的 SQLite 驱动，部署时排除其他数据库驱动。
- **D3D11 渲染 + FreeType 字体引擎。** 界面字体阿里巴巴普惠体没有字体微调，GDI 下中文横笔画会糊成两行像素；FreeType 能把它们对齐到像素上。但软件渲染器会按估算的字形边界裁剪文字，FreeType 的字形会超出一点、被裁掉（比如“毫”顶上的点），所以只能配 D3D11，比软件渲染多占约 50 MB 内存。设置里选“省内存”（software）时自动改用 GDI，文字完整但偏模糊。默认“自动”：物理内存不超过 16 GB 的电脑用“省内存”，更大的用 D3D11。
- **界面字体随程序附带**（`fonts\` 下的阿里巴巴普惠体 3.0 常规 / 粗体），缺失时退回系统默认字体（中文系统为微软雅黑）。界面里不用 `↵` 这类字体缺字的符号，字体回退一旦触发，内存要多出 30 MB 左右。

## 测试

```powershell
ctest --preset release                         # 单元测试
build\release\wsbench.exe                      # 读取已保存的索引，输出内存和搜索耗时
build\release\wsbench.exe 报告 "*.pdf"          # 指定查询
build\release\wsbench.exe --places 适配器 网卡   # 系统入口：读取耗时、每个词的前几名（不带词则全部列出）

# 读磁盘数据的解析代码（MFT、USN 日志、文本编码）用 AddressSanitizer 和模糊测试检查
cmake --preset asan; cmake --build --preset asan  # 在 VS 开发者命令行里运行
ctest --preset asan                            # 单元测试，越界读写当场报错
build\asan\wsfuzz.exe -max_total_time=300      # 用随机数据测 5 分钟
```

命令行参数：`--background`（启动时不显示窗口，开机启动用的就是它）、`--toggle`、`--query <文字>`、`--settings`（打开设置窗口）、`--quit`。

## 已知限制和后续可做的

- 拼音只认读音的开头，不做模糊纠错；排除词（`!xx`）只按字面匹配，不按拼音。
- 内容索引不记带重音的字母、西里尔字母等，也不记一两个字母长的英文：搜这些时仍要逐个读文件（结果边找边出）。U 盘等非 NTFS 磁盘上的文件不建内容索引，也是逐个读。
- 内容索引第一次在后台建立时，开着 Windows Defender 每秒只能读几百个文件，几十万个文件要二十分钟左右；建好之前仍会逐个读还没收录的文件。
- 名字里的非英文字母（如 `Ä`/`ä`）不区分大小写的匹配只对英文字母生效。
- “打开 / 保存”对话框里只有 `Ctrl+G`，没有 Listary 那种嵌在资源管理器和对话框里的搜索框；也只认 Windows 自带的资源管理器，Total Commander、Directory Opus 等文件管理器里的文件夹还不认。程序自己画的对话框（Qt、Java、GTK 等的非系统对话框）和老式的“浏览文件夹”树形对话框不支持。
- 非 NTFS 磁盘（U 盘、exFAT/FAT）在程序没运行期间的改动，要靠启动后的后台同步补上（约十几秒内完成）；弹出后又取消的非 NTFS 磁盘会重新遍历一次。
- 剪贴板历史：多选时不能把图片和别的内容合在一起粘贴，图片要一张一张地粘贴；还没有“依次粘贴”（每按一次 Ctrl+V 贴出下一条）。剪贴板窗口开在搜索框的位置，不跟着文字光标。终端里只有 mintty 和 PuTTY 用 `Shift+Insert`，其他不接受 `Ctrl+V` 的程序要自己按它们的粘贴键。
- 文件夹被隐藏或取消隐藏时，里面已有文件继承来的“隐藏”标记不会马上跟着变，要到下次重新读取该盘时才更新。
