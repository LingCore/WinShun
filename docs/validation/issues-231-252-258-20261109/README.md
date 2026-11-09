# #258 / #231 / #252：预览恢复开关与贴图提示、文件夹窗口进最近、常驻插件上限；顺带修好退出慢

2026-11-09，Windows 11 25H2、Qt 6.12.0、MSVC 2022 x64 Release，构建目录 `build/issues-249-253-256-20261108`（沿用上一轮）。

## #258

**1. Alt+P 预览窗口被记住**：#33 时定下"Esc 不先关预览，预览状态随搜索框记住"，这位用户要的相反。改成设置项「结果列表 › 显示与预览 › 再次打开时恢复预览」，默认开（照旧），关掉后搜索框收起时顺带清掉记住的状态（`Launcher::handleHidden`，`Results/PreviewRestore`）。

真机检查（本目录 `live-preview-restore-off.txt` / `live-preview-restore-on.txt`，独立 `--profile` 的测试副本，`--query` 后 Alt+P 开预览、Esc 收起、再 `--query`）：
- 关：Esc 后 `state.ini` 的 `PreviewOpen=false`，再打开只有搜索框一个窗口。
- 开（默认）：Esc 后 `PreviewOpen=true`，和原来一样。预览本身回不回来由原有逻辑决定，这次没改。
设置行离屏渲染见 `preview-restore-row-light.png`（0 条 QML warning）；`tst_settings` 55 项通过（含新增的读写、默认值检查）。

**2. 贴图"拖选文字 · Esc 退出"遮住文字**：提示胶囊原来一直压在图片左下角。现在和缩放提示一样是"一会儿"：识别中一直显示；识别完显示 2.2 秒后淡出；每次拖选完再显示一会儿（已复制 / 再拖选）；拖选过程中不画。新测试 `tst_pinwindow`（真 Win32 钉图窗口，读字慢时不淡出、读完后淡出、退出文字模式清零）通过。截图插件属于插件库，随下次主程序发布一起更新。

## #231（hhuimie 的追加评论）

搜到已打开的文件夹窗口、回车切换过去后，这个文件夹没有进"最近打开"。现在切换成功后，用 `filemanager::frontFolder` 问窗口当前显示的文件夹（资源管理器、Total Commander、Opus），是真实文件夹就按"打开过"记入历史（带本次输入的词）；别的程序的窗口不记。

真机检查（同上两份记录的 `231` 行）：开一个独立的资源管理器窗口到临时文件夹 `Zqxwvj231`，`--query Zqxwvj231`，回车后前台就是那个窗口，`history.txt` 里出现该文件夹路径。

## #252

「插件 › 同时常驻的插件」：4 / 8 / 16 / 不限（`Plugins/MaxResident`，0 为不限，读入时夹在 0–64）。改动从下一个常驻插件启动时生效，不会为此停掉正在跑的。`tst_plugin` 的 `residentCap`（原有）、`residentCapLifted`（新增：不限时都留着；重新设上限不打扰在跑的）通过；渲染见 `resident-limit-row-light.png`。

## 顺带：退出要等 15–50 秒

测试脚本里 `--quit` 多次要等 20 秒以上才退出。用 cdb 抓栈（`quit-stacks-excerpt.txt`）：主线程在 `std::thread::join`，等的是文件夹菜单启动时后台收集"最近文件夹"的线程，它在 `QDir::entryInfoList` 列 Windows 的 Recent 文件夹里的 `*.lnk`——Qt 为了判断快捷方式指向文件还是文件夹，逐个解析目标，目标在失联的网络路径上就要等几十秒（本机 Recent 里有 162 个快捷方式）。原代码自己已经避免碰网络盘（`LocalDrives`），但 Qt 的列目录绕开了这层保护。

改成用 Win32 `FindFirstFileEx` 只列文件名和写入时间、按时间排序（`recentfolders::shortcutsNewestFirst`），之后仍只用 `IPersistFile::Load` + `GetPath` 读快捷方式本身。同一测试副本、开着"最近使用"：改前 15.3 秒和 50.2 秒退出，改后 0.59 秒。新测试 `tst_recentfolders` 覆盖排序、大小写、`.lnkx` 和同名文件夹的排除、上限和不存在的目录。

## 其它

- 只跑了本次改动涉及的测试：`tst_settings`、`tst_plugin`（两个上限用例）、`tst_pinwindow`、`tst_recentfolders`；没有跑完整 ctest。
- 测试副本在会话临时目录，`--quit` 后清理，"在 Win顺 里搜索"注册表项与自启动任务每次核对、恢复。用户的 Win顺 测试期间没有运行，保持原样。
- 工作区另有未提交的改动（Logo 位置一轮），提交时只取本轮的 hunk。
