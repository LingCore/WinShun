# 0.47.0 验证记录

发布提交 136fe829，正常推送（`-Push Normal`）。这一版的起因是一条用户反馈：用户已经用 Win顺 换掉了 Listary 和截图软件，剪贴板要是能保留重复项、能给单条内容设粘贴快捷键，就连 Ditto 也能换掉。版本内容：

- 剪贴板单条内容的快捷键（2d5d0220）和“保留重复项”。实现和逐项检查见 `docs/validation/clipboard-ditto`。
- 0.46.0 之后 main 上已经并入的提交：#452 全屏游戏上的剪贴板面板、#461 截图画笔点一下就是一个点、压缩包搜索插件和宿主工具 `list_files`、休息提醒 1.1.4（另一个会话的 eaff44ec）。
- 发版中修的压缩包搜索副标题（bd041bfd）：压缩包最外层的文件原来显示成 `backup.zip › \`，看着像被截断，现在只显示压缩包本身。

## 自动测试

只跑了这次改动涉及的测试。

- 打包（`build\release046`）时跑了 12 组：`tst_aisettings`、`tst_keycaps`、`tst_settings`、`tst_annotmodel`、`tst_cliplifecycle`、`tst_break`、`tst_archive`、`tst_core`、`tst_clipboard`、`tst_mcp`、`tst_plugin`、`tst_clipboarddisplay`，全部通过。正式发布那次又跑了 `tst_clipboard`、`tst_archive`、`tst_break`。
- `tst_clipboard` 的 `entryHotkeys` 关掉数据库再打开，检查快捷键确实存下来了：被另一条抢走后旧的那条是空的；去掉以后也是空的。前一个问题是开发中查出来的：去掉快捷键时绑定了 NULL，被 `NOT NULL` 约束拒绝，改成 `COALESCE(?, '')`。
- `tools/verify_archive_plugin.py`：全部通过，新加了一条检查，副标题不能以 ` › \` 或 ` › ` 结尾。
- `tools/verify-installer.ps1`：6 项全过。

## 界面

离屏预览在 100%、150%、200% 下都检查过：

- `--preview-results --clipboard-entry-hotkey`：右边的键盘标记、三种菜单文字（设置、更改、去掉）、多选时不出现这些项、请求设置的信号；去掉快捷键后这一条仍然固定，变更信号只发一次。
- `--preview-settings --entry-hotkey-settings`：设置页打开到这一条，录入框获得焦点并在可见区域里；按下 `Ctrl+Alt+K` 后发出设置请求，录入停止。

截图在 `docs/validation/clipboard-ditto`。200% 下第一次检查失败：当前行显示的是按钮而不是标记。这是检查本身的问题，改成先把当前行放到没有快捷键的那一条上再检查。

## 真实运行

- 单条快捷键：在最终的二进制上用 `--probe-runtime --live` 跑真实的 App。按下快捷键后，目标窗口收到了这一条的文字，历史仍是 2 条，系统剪贴板恢复成原来的内容。
- 压缩包搜索：在真实启动器里测了结果行、徽标、`zip 名字` 关键字模式，以及按压缩包名匹配。

## 截图插件保存夹具

`WinShunCapture.exe --preview-capture-tools <dir> --save-options`（截图 1.1.37）：全部通过。

## 安装和升级

`tools/verify-installed-upgrade.ps1 -Mode InApp`（`dist\release047`）：本机装的 0.46.0 通过软件内更新升到 0.47.0，用时 53.6 秒。更新后服务在运行，没有提升权限；开机任务、设置和历史都保留，历史的 SHA256 没变。结果见 `inapp-upgrade.json`。测的是发布前那次打包，但其中的 `WinShun.exe`（421b0615…）和正式发布的便携包里的完全相同。

这次增量包没用上，下载的是完整安装包。日志写着 `changed here: library-plugins/winshun.capture/README.md`。原因是本机装的是 0.46.0 发版前的那次打包（见 `docs/validation/release046`），它的截图插件 README 和正式发布的不同。核对过：增量包里这个文件的基准 SHA256 是 `da65fa3e…`，和正式发布的 0.46.0 便携包（`dist\release046pub2`）里的一致，所以从正式 0.46.0 升级的用户会用到增量包。回退到完整安装包的那条路径也在这次升级中走通了。

## 海报

`tools/screenshots/release047.py` 调用 `WinShun.exe --preview-results <dir> --clipboard-entry-hotkey --entry-hotkey-poster`（离屏），预览里是 7 条像样的剪贴板内容，其中 3 条带快捷键。

- `clipboard-hotkeys.jpg`（新）：标题“常用的一条，一键粘贴”，副标题“给剪贴板里的一条设快捷键，在哪个程序里都能直接贴”。放在 `docs/截图.md` 的剪贴板那一组。

## 插件库

比对了各插件已发布版本的源码提交和这次编译进去的文件（`ninja -t inputs` 加 `#include` 闭包）。以下插件都从 136fe829 构建：

| 插件 | 版本 | 说明 |
|---|---|---|
| 截图与录屏 winshun.capture | 1.1.37 | #461 画笔圆点、笔尖指针、“选择”按钮；带上 #452 的 `KeyRouter.h`；安装包附带的是同一份 |
| 休息提醒 winshun.break | 1.1.4 | 提前 0 秒时不再闪提示条 |
| 开发工具 winshun.devtools | 1.0.7 | 编译进去的 `ClipStore.cpp`、`GameGuard.h` 变了；用起来没有变化 |
| 鼠标手势 winshun.gestures | 2.1.5 | 同上 |
| 图片文字搜索 winshun.ocr-search | 1.0.32 | 同上 |
| 压缩包搜索 winshun.archive | 1.0.0 | 新插件，最低 Win顺 0.47.0（要宿主工具 `list_files`） |

其余插件和已发布版本比没有变化，这次不发。插件库在 `tools/registry.py` 里登记了 `winshun.archive`，条目由 `tools/entry.py` 生成（提交 dd88a81）；`check.py --online` 报 0 个错误。

## 发布

`scripts/release.ps1 -Publish -Push Normal`（`dist\release047pub`，构建目录 `build\release046`）。第一次直接报“Push the release commit before publishing”，因为 main 还没推到 GitHub；推送后重新跑。这次发布又跑了一遍 `tools/verify-installer.ps1`，6 项全过。

- 标签 v0.47.0 指向 136fe829，latest 接口返回 v0.47.0。
- 8 个附件（安装包、便携包、离线安装包、增强 OCR 组件、3 个增量包、`SHA256SUMS.txt`），GitHub 上每个附件的大小和 SHA256 都和 `SHA256SUMS.txt` 一致。
- CNB 镜像：脚本里那一步报“No CNB token”。原因是推送时用环境变量把 git 的凭据助手换成了只用 `gh`，存在系统凭据管理器里的 CNB 令牌就读不到了。之后在没有这些环境变量的 shell 里单独跑 `scripts/cnb-mirror.ps1 -Tag v0.47.0`，8 个文件上传后，下载回来校验通过。
- Scoop：530b627b（Scoop: winshun 0.47.0），已推送。
- lcstp.com：安装版、便携版两条回答都是 v0.47.0，文件大小和 SHA256 与 `SHA256SUMS.txt` 一致，第一个镜像是 cnb.cool；正文有增量那一行，没有“立即推送”那一行。
- 插件库：CI 发布通过，线上 `index.json` 里 capture 1.1.37、break 1.1.4、devtools 1.0.7、gestures 2.1.5、ocr-search 1.0.32、archive 1.0.0 都已更新（devtools、ocr-search、archive 最低 Win顺 0.47.0，由 CMake 写入）；`check.py --online` 0 个错误。
- #452、#461 已回复“0.47.0 已发布”并关闭。用户反馈里的 Ditto 替代需求没有对应的 issue，不用回复。

## 没有覆盖的

- 压缩包搜索在“内容”范围里右边的详情预览：没有在真实启动器里点到（探针没有找到 `scopeLink-content`），以 `verify_archive_plugin.py` 和 `tst_archive` 为准。
- “保留重复项”只有单元测试和设置页预览，没有在真实桌面上长时间复制。
- #452、#461 和压缩包搜索各自的验证见 `docs/validation/issue-452`、`issue-461`、`archive-plugin`；这次发布没有在真实全屏游戏和真实截图里重测。
