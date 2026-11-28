# 0.53.0 验证记录

正常推送（`-Push Normal`）。GitHub 网页登录仍不可用，全程用本机 `gh` 保存的令牌。

版本内容（0.52.0 之后）：

- main 上的提交：#507 对话框旁的搜索框默认又搜整个电脑、“搜索的范围”三选一（fb42e2ab）；#506 诊断信息记系统和任务栏缩放、是否远程会话（eca30a55，问题本身没复现，等反馈者回答，issue 保持打开）；插件设置 `showWhen`，翻译 2.4.1、按意思找 1.2.2 只读自己卡片上的字段，本地模型不再读共用的接口地址（eaf154c5、690fe423）。验证见 `docs/validation/issues-506-507`。
- 合并的分支 `favorite-keys`（1 个提交）：收藏快捷键（验证见 `docs/validation/favorite-keys`）。没有冲突。
- 合并的分支 `issues-448-472-452`（6 个提交）：#448 插件给出的文件行当文件用、#472 手势组合键按住 40 毫秒和提示文字、#452 独占全屏游戏上的剪贴板面板、#505 便携软件和应用别名、拖文件圆盘上的“收藏”。合并时 4 处冲突，都是两边各加了东西：
  - `src/core/Favorites.cpp`：`Favorite::pressesKeys`（收藏快捷键）和 `favoriteForPath` / 收藏请求的读写（圆盘），都保留；
  - `src/app/Launcher.cpp`：复制快捷键收藏的分支改用分支上新加的 `Launcher::copyText`（测试接缝），不再直接调 `shell::copyText`；
  - `docs/features.en.md`：收藏快捷键一条和圆盘“收藏”的新写法都保留；
  - `src/app/i18n/winshun_en.ts`：对话框“搜索的范围”的说明用 main 上 #507 的新原文，撇号按 lupdate 的写法转义。

## 自动测试

合并后 `build\release` 全量构建通过。只跑了这次改动涉及的 21 项，20 项通过：

`tst_pluginreadme`、`tst_mojibake`、`tst_encoding`、`tst_settingscapturehotkey`、`tst_settingslazy`、`tst_appalias`、`tst_settings`、`tst_droppiehelper`、`tst_droppiefavorite`、`tst_droppiehit_{revealed,unrevealed}_{d3d11,software}`、`tst_gestures`、`tst_translate`、`tst_core`、`tst_favorites`、`tst_semantic`、`tst_plugin`、`tst_dialogbarsuggestions`。

`tst_shortcutpanel` 失败的是两项早就有的检查（“scope hint follows nonblank input, including no results”、“footer: clearHistory iconSize matches info button”），分支 `issues-448-472-452` 的记录里写过它们在 main 上也失败，这次合并前后一样，没有新增失败。

`tools/verify-installer.ps1`：6 项全过（打包时自动运行，含增量安装程序）。

## 安装和升级

- `tools/verify-installed-upgrade.ps1 -Mode InApp`：本机装的 0.52.0 通过软件内更新升到 0.53.0，用时 45.5 秒，走的是增量包 `WinShun-0.53.0-x64-delta-from-0.52.0.zip`（8.4 MB）。更新后服务在运行，没有提升权限，自启任务不变，剪贴板历史的校验值不变，装好的 `WinShun.exe`（8ae59a24…）和打包目录里的相同。结果见 [inapp-upgrade.json](inapp-upgrade.json)。
- 增量包三个（从 0.52.0、0.51.0、0.50.0），脚本各自还原比对过，8.4–9.1 MB；安装包 42.8 MB。
- 安装程序、更新代码这一版没有改，没有跑便携版、向导和“只为我安装”的升级。

## 海报

两张新海报，都用 `tools/screenshots/release053.py` 从 `--preview-settings` 离屏渲染（真实设置窗口、临时配置，不碰用户的 Win顺、剪贴板、主题和缩放）：

- `docs/images/favorite-keys.jpg`：收藏夹 › 添加快捷键，`--keys-form` 通过真实录制框填好的表单（新建文件夹、xj、Ctrl+Shift+N）。
- `docs/images/portable-apps.jpg`：搜索范围 › 应用（#505），预览里编出来的便携软件文件夹和别名，没有读这台电脑上的软件。

`droppie.jpg`（收藏夹列表）没有重拍：预览里的收藏列表带着临时文件夹和程序自己的路径，会露出这台电脑的构建目录。两张新图加进了 `docs/截图.md` 对应的分组。

## 插件库

用 `build\release` 里 ninja 记下的真实输入对照各插件上次发布的源码提交：

| 插件 | 版本 | 变了什么 |
|---|---|---|
| 截图与录屏 winshun.capture | 1.1.43 | `HotkeyRecorder.qml`：设快捷键时不再收小键盘的键；`InputBox` 的“清空”补了英文。安装包附带的是同一份源码 |
| 鼠标手势 winshun.gestures | 2.3.0 | #472：组合键按住 40 毫秒再松开、手势的提示文字；同上的录制框 |
| 翻译 winshun.translate | 2.4.1 | 只读所选服务的字段，本地模型只连本机（eaf154c5、690fe423 已升版） |
| 按意思找 winshun.semantic | 1.2.2 | 同上，各家云服务各存各的密钥 |
| 本地模型 winshun.localmodel | 1.0.1 | 只改了端口说明（`plugin.json`） |

图片文字搜索和开发工具也编进了这次改过的共用文件（`Foreground.cpp`、`wscore` 里的应用目录、收藏、快捷键），但新代码没有链接进它们的程序：在构建出的 `WinShunOcrSearch*.exe/.dll`、`WinShunDevTools*` 里找不到这次新加的字符串（`winshun:keys/`、`PortableFolders`、`front_taker`），`WinShun.exe` 里都有。所以不升版。其余插件编译进去的文件没有变化。权限都和已发布的版本相同。

## 没有覆盖的

- 收藏快捷键：运行时探针（Ctrl+Alt+Shift+F17）在分支上过了两次；从文件夹菜单按下、以管理员身份运行的窗口收到通知，没有实测。
- #505 圆盘“收藏”：`tst_droppiefavorite`、`tst_droppiehelper` 用测试窗口核对；没有从真实资源管理器拖到“收藏”上（本机正在用的 Win顺 也会接这个键）。
- #472：没装 WPS，只用探针核对了按住 40 毫秒时程序能读到 Ctrl。
- #452：本机是 Windows 11，没有真正独占全屏的游戏，只有规则的单元测试；还会消失的话下一次诊断包里有 `front_taker`。
- #507：有单元测试和对话框搜索框的预览，没有在真实的“打开”“另存为”对话框里打字（本机正在用的 Win顺 也会附上去）。
