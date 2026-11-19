# 0.40.0 安装与升级验证

- `installer-test.json`：发布时 `scripts/release.ps1 -Publish` 对正式包跑的 `tools/verify-installer.ps1`，6 项全过（普通升级、自己的程序在运行、DLL 被别的程序加载、文件被别的程序读着、文件谁都不许碰、增量安装程序）。
- `setup-over-installed.json`：`-Mode Setup` 用正式发布的安装包覆盖本机那份软件内更新来的 0.40.0，用时 8.1 秒。装好后 `WinShun.exe` 和发布的一致（`34cdc2f9…`），服务、计划任务（XML 仍和升级前一字不差）、设置和剪贴板历史都没变。
- `inapp-upgrade.json`：`tools/verify-installed-upgrade.ps1 -Mode InApp`。本机装着的 0.39.0 走软件内更新到 0.40.0，下载的是 `WinShun-0.40.0-x64-delta-from-0.39.0.zip`（约 6.8 MB），用时 47.5 秒；服务、计划任务、剪贴板历史都在。用的是发布前那次打包（`dist\release040`，打包时 `verify-installer.ps1` 6 项全过）。之后只改了预览夹具（海报画面、两处自检的等待）和海报，更新链路的代码没动。
- #400 保留计划任务：本机的开机自启任务是用户自己的、普通权限的任务。安装日志写着 `The autostart task is the user's own: kept as it is`，升级前后 `schtasks /query /tn \WinShun /xml` 一字不差。“以管理员身份运行时，更新后以管理员身份重启或弹通知”这一支本机没有走到（这里的 Win顺 是普通权限），由 `tst_updater` 覆盖。
- 自动测试只跑了这次改动涉及的 7 个：`tst_clipboard`、`tst_clipimage`、`tst_displaygeometry`、`tst_everything`、`tst_updater`、`tst_settings`、`tst_aisettings`，全部通过。
- `tst_clipimage` 第一次跑时崩了。原因是缩略图的后台任务往 Qt 已删掉的响应里写图，这个问题以前就有，偶尔发生。修好后连跑 6 次都通过；新加的 `clipThumbnailOutlivesItsResponse` 在旧代码上跑 10 次崩 10 次。同一个测试里另有一项从 0.38.0 起就一直失败（没注册 `TextLineSpacing`），也一并修好了。
- 预览夹具 `--preview-results --file-metadata --gallery-image`：0.40.0 和本机装着的 0.39.0 各跑一遍，都报同样 3 条警告，都不是这次引入的：
  - room beside：这台机器的工作区只有 1280 逻辑像素宽，旁边本来就放不下预览；
  - 新结果晚到约 1 秒；
  - 夹具故意卡住的 `.md`。
- 设置窗口记住大小（#401）的开关：`tst_displaygeometry` 的 `rememberingCanBeTurnedOff`，以及探针 `--live` 真实打开设置窗口，验证关掉开关后已记住的大小会清除。
