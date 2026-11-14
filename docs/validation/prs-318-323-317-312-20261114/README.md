# PR #318、#323、#317、#312 的验证记录（2026-11-14）

分支 `prs-1114`（基于 0.28.0，`b2afb537`），Qt 6.12.0 / MSVC 2022 Release，构建目录 `build/prs-1114`。

## #318 按住 Caps Lock 算组合键

Win10 虚拟机（22H2），`ExplorerTyping=true`，焦点在资源管理器文件列表，用 Hyper-V 合成键盘按扫描码发键，`WINSHUN_LOG_INFO` 日志里临时加了一行按键记录（已去掉）：

| 按键 | 钩子看到的 | 结果 |
|---|---|---|
| 按下 Caps → 按下 R → 松开 R → 松开 Caps | R 按下时 `caps=true` | 不拦，没有“Explorer typing”，搜索框没出来 |
| 点一下 Caps（松开）后单按 R | `caps=false` | “Explorer typing: key 0x52”，搜索框带着 R 出来 |

第一次跑出过“按住 Caps 仍然弹出搜索框”，那是测试本身错了：Win顺 重启后搜索框本来就开着，键直接打进了框里，没有经过钩子。没测：真装着 CapsLock+ / AutoHotkey 的机器。

## #323 按“设置 › 应用”里的名字找应用

- `tst_core`：`appInstalledNames`（同目录挂名、最深的安装目录优先、说的是某个应用的名字只给它、共享目录不算、不是精确匹配、重复读不重复加）通过；整套 276 过、1 败——`relocatedPersonalFoldersKeepPriority`，和这次改动无关（PR 作者在改动前同样失败；本机没有在干净的 main 上再跑一遍确认）。
- 本机真实软件（`WinShun.exe --preview-results <目录> --apps-grid`，应用 › 全部）：

| 输入 | 结果 |
|---|---|
| `microsoft office`、`office ltsc` | Excel、Office 语言首选项、PowerPoint、Spreadsheet Compare、Visio、Word |
| `visio` | 只有 Visio（Visio 装在 Office 的目录里，名字不借给别的应用） |

## #317 书签拼音

- `tst_bookmarks` 全部通过（改用 `src/core/Pinyin` 之后）。
- 插件库构建出的 `WinShunBookmarks.exe`（静态 C 运行库）对本机真实书签：`zh` 50 行、`bd` 19 行，各 10–12 ms（第一次 201 ms 是冷启动）；`sql` 没有匹配（不再把整栏书签都列出来）；`qt` 4 行，都是标题里真有 qt 或拼音对得上的。

## #312 应用 › 运行

`--apps-grid` 探针全部通过：类型条 4 项；“运行”列出本机 24 个窗口（探针自己的不算），格子是窗口行、没有星标；选中第 2 格后过一个刷新周期（1.7 秒）选中项没有跳回第 1 格；打不存在的词为空；Ctrl+Tab 从“运行”到“常用”；选择记在 `Search/AppsView=running`。截图 `apps-running.png`（图标是异步加载的，截得早所以还没出来，“全部”页同样）。

没测：真实窗口里用鼠标点格子切换窗口（走的是原有窗口行的 `windowsearch::activate`，没改）、125% / 150% 下的像素。
