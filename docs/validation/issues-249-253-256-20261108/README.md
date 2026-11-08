# #249 / #253 / #256：插件窗口前台、残留文件夹安装、开发者消息筛选

2026-11-08，Windows 11 25H2、Qt 6.12.0、MSVC 2022 x64 Release，构建目录 `build/issues-249-253-256-20261108`。三项都是 bug 或健壮性问题，本机可复现、可验证。

## #249 插件启动时无法稳定切换到前台

**原因**：宿主只在 `menu`、`picture`、`showSettings` 这几种请求前调用 `AllowSetForegroundWindow`，用户在结果上按 Enter 触发的 `action` 没有。常驻插件收到 action 后开窗口时，搜索框还在前台，Windows 拒绝后台进程抢前台，窗口落在后面、任务栏闪烁；一次性插件因为搜索框先隐藏，碰巧成功。

**修改**：`Host::action` 发 action 前，以及一次性插件为 action 起的进程报到时，给插件进程 `AllowSetForegroundWindow`；docs/plugins.md 的"前台"一条补上 `action`。AI 和命令行的 `_run` 不给。

**验证（真机 A/B）**：两个测试插件（本目录 `test-plugin-resident.ps1`、`test-plugin-once.ps1`，PowerShell 常驻 / 一次性各一个）在 action 里开一个 WinForms 窗口并 `Activate()`，1.2 秒后记录它是不是前台窗口（`Form.ActiveForm`）。测试脚本用 0.22.0 发布版和本次构建各起一个独立 `--profile` 的副本，`--query "fgt go"`，确认搜索框在前台、插件已回答查询后发 Enter，再读结果；结束后 `--quit`、恢复 HKCU 的"在 Win顺 里搜索"注册表项、核对自启动任务仍指向 Program Files。

| 副本 | 常驻插件窗口 | 一次性插件窗口 |
| --- | --- | --- |
| 0.22.0 发布版 | 在后面（`foreground=False`，显示时和 1.2 秒后都不是前台） | 在前面 |
| 本次构建 | 在前面（显示时和 1.2 秒后都是前台） | 在前面 |

原始记录：`foreground-0.22.0.txt`、`foreground-new.txt`。测试时用户另有程序在前台（窗口化游戏、VS Code），搜索框仍能正常到前台，不影响结论。

## #253 插件安装被拒后没有出路

**原因**：`plugins\<id>` 存在但没有 `plugin.json` 时，安装直接拒绝（"不是已安装的插件文件夹，为保留其中的数据，没有覆盖"），用户只能手动删文件夹。这种文件夹最常见的来源是卸载：原来 `remove()` 就地逐个删文件，插件程序还在运行（exe 被占、库没卸载）时 `plugin.json` 已删、其余留下，于是既不算已安装、又装不上。本机实测：文件夹里有打开着的文件时，改名整个文件夹会被拒绝（Access denied），而删它旁边的文件照样成功，正是这种半删状态的成因。

**修改**：
- 卸载改成"先把文件夹改名到 `.winshun-remove-XXXXXX\plugin`，再删"：改名失败就整个不动，插件保持完整，提示"它的程序可能还在运行"；改名成功但删不掉的，留在隐藏的临时目录里，不挡再次安装，下次启动（`updates::prepare`）清扫。
- 安装碰到这种文件夹时问一句（`install::obstacle` / `asideName`）：对话框说明情况和路径，"打开文件夹"可以先看看里面是什么，"改名并安装"把它改成 `<id>.old-<日期时间>` 留在旁边，不删任何文件；安装完成的提示里写明改成了什么名字。链接（开发中的插件）仍按原来的规则拒绝。

**验证**：`tst_plugin` 新增 `installOverForeignFolderRenamesItAside`（默认仍拒绝、名字带路径时拒绝、同名已存在时拒绝、改名后旧文件完整且不被目录扫描当作插件、链接是另一种阻碍）和 `removeIsAllOrNothing`（文件被占用时整个不删、释放后删干净、残留的临时目录下次启动清掉）；连同原有的安装、更新、卸载相关 14 项一起运行，16 项全部通过（`tst_plugin-picked.log`，含 `initTestCase`/`cleanupTestCase`）。对话框用 `--preview-plugins --dialog-aside` 离屏渲染浅色 100%、深色 150%，0 条 QML warning：`aside-dialog-light.png`、`aside-dialog-dark-150.png`。

## #256 开发者"消息记录"的插件筛选 tab 溢出

**原因**：筛选是一排分段式 tab，插件一多就超出卡片宽度，被裁掉。

**修改**：tab 放得下时还是 tab；一行放不下（`implicitWidth` 超过行宽）就换成同一套选项的下拉列表（`ChoiceBox`），不再溢出，和窗口宽度、插件数量无关。另外按反馈给方向着色：→（发给插件）用强调色、←（插件回的）用绿色并加粗，·（事件）保持灰色，stderr 仍是警示色。

**验证**：`--preview-plugins` 离屏渲染。两个插件时（`log-filter-two-light.png`）仍是 tab，树里 `ScopeTabs w=210` 可见；八个插件时（`log-filter-many-light.png`、`log-filter-many-dark-150.png`）`ScopeTabs w=1105` 隐藏、`pluginLogFilterBox` 显示为下拉列表；三张图各 0 条 QML warning。

## 其它

- 构建：主程序、插件宿主、`tst_plugin` 全部通过；第一次构建时 `qmlcachegen` 在一个未改动的 QML 文件上偶发失败，重跑即过。Preview.cpp 原有的 C4456 警告保留。
- 没有运行完整 ctest；只跑了本次改动涉及的插件安装、更新、卸载测试。
- 没有停止或替换用户的 Win顺（测试期间没有在运行）；测试副本放在会话临时目录，`--quit` 后清理，注册表项已恢复。
- 截图按 #247 的顾虑只保留裁剪后的相关区域。
