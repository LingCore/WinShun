# 0.41.0 验证记录

## 鼠标手势 2.0（Win10 虚拟机实测）

在 Hyper-V 虚拟机 Win10-WinShun 里装发布前的 0.41.0 和手势插件 2.0.0，用真实的鼠标、键盘输入（SendInput）逐项试：

| 项目 | 结果 |
|---|---|
| 记事本里选中文字，向下画 → 复制 | 通过：剪贴板里是选中的字，屏幕上有提示 |
| 只对记事本生效的程序规则 | 通过 |
| 顺时针画圈 | 通过 |
| 选中文字向右画 → 翻译气泡 | 通过：“翻译中”和“翻译失败”两种状态都显示正常（虚拟机不联网） |
| 触发角（推到屏幕角落停一下） | 通过 |
| 摩擦边（在屏幕边上来回蹭） | 通过 |
| 右下角逆时针画圈 → 关机 | 通过：屏幕中间出现 5 秒倒数，按 Esc 取消，没有关机 |
| 画到一半按 Esc | 通过：松开后什么都不做 |
| 单按右键 | 通过：右键菜单照常出来 |
| Edge 里选中文字再向上画 | 通过：不读选中的字，执行平常的“回到顶部” |
| 设置窗口 | 通过：打开、在画板上画、选动作，改动立刻生效并保存下来 |

没覆盖：按住右键滚滚轮（虚拟机里没法合成滚轮），由 `tst_gestures` 覆盖。

### 选中文字读不到的原因

最初插件自己用 UI 自动化读选中的字，在插件里总是超时（16 毫秒就返回 `UIA_E_TIMEOUT`），同样的代码单独运行却能读到。用探针逐项排除后确认：插件所在的作业对象只要设了任何一项界面限制（这里是 `JOB_OBJECT_UILIMIT_READCLIPBOARD`），跨进程的 UI 自动化就会立刻超时，`EM_GETSEL` 也只返回 0..0；`SendInput`、`PostMessage`、`OpenProcess` 不受影响。

改法：读选中文字交给 Win顺 自己做，新加 `selected_text` 工具（权限 `selection:read`），插件通过它拿到文字。Chromium 和 Firefox 的窗口一律不问（一问它们就打开整套无障碍功能、变慢，VS Code 还会进入屏幕阅读器模式），在那里手势照常执行。详见 `docs/pitfalls.md`。

## 自动测试

只跑了这次改动涉及的：`tst_gestures`、`tst_plugin`（及同名前缀的 `tst_pluginhotkeys`、`tst_pluginconfirm`、`tst_pluginhomepage`、`tst_pluginkeywords`、`tst_pluginsliders`、`tst_pluginupdates`、`tst_plugincommands`、`tst_pluginreviewsettings`、`tst_pluginsdk`）、`tst_aisettings`，共 13 个，全部通过。新加的 `selectedTextRequiresSelectionRead` 验证没有 `selection:read` 的插件调 `selected_text` 被拒绝。

## 安装和升级

- `installer-test.json`：发布时 `scripts/release.ps1 -Publish` 对正式包跑的 `tools/verify-installer.ps1`，6 项全过（普通升级、自己的程序在运行、DLL 被别的程序加载、文件被别的程序读着、文件谁都不许碰、增量安装程序）。发布时 13 个自动测试也再跑了一遍，全部通过。
- `inapp-upgrade.json`：`tools/verify-installed-upgrade.ps1 -Mode InApp`。本机装着的 0.40.0 走软件内更新到 0.41.0，下载的是 `WinShun-0.41.0-x64-delta-from-0.40.0.zip`（约 7.2 MB），用时 50.9 秒；服务、计划任务、剪贴板历史都在。用的是发布前那次打包（`dist\release041`，打包时 `verify-installer.ps1` 6 项全过）。之后只改了 5 个插件的版本号、文档和海报，更新链路的代码没动。
- `setup-upgrade.json`：发布后（11-20，用户放行）从 GitHub Release 下载正式的 `WinShun-0.41.0-x64-setup.exe`（SHA256 与 Release 一致），用 `tools/verify-installed-upgrade.ps1 -Mode Setup` 覆盖装到本机：退出码 0，用时 10.2 秒，装完自己重新运行；服务、计划任务、设置、历史都在。装好的 194 个文件与正式便携包逐个比对 SHA256 全部相同（便携包多一个 `data\README.txt`，是便携版专有的）。安装日志里唯一的非零退出码是删除 LocalSend 防火墙规则时规则本来就不存在，属正常。
- 注意：这个测试要让装好的 Win顺 运行，而 Win顺 同一时间只能开一份，所以它会让当时在运行的另一份 Win顺（这次是 `dist\run-latest` 里的 0.40.0 开发版）正常退出。

## 插件库

7 个官方插件发了新版本（源码 7cb999ca）：鼠标手势 2.0.0、缩写展开 1.0.0（新）、LocalSend 1.0.8、休息提醒 1.1.2、翻译 2.2.4、图片文字搜索 1.0.26、开发者工具 1.0.1。后 5 个是编进去的插件开发包或共用代码有变化，用起来没有变化。每个包都用插件库的 `check.check_package` 检查过，Release 附件的 SHA256 和本地一致；线上 index 共 40 个插件，版本都对，签名正确。
