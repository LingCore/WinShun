# 0.38.0 安装与升级验证

- `installer-test.json`：发布时 `scripts/release.ps1 -Publish` 对正式包跑的 `tools/verify-installer.ps1`，6 项全过（普通升级、自己的程序在运行、DLL 被别的程序加载、文件被别的程序读着、文件谁都不许碰、增量安装程序）。
- `inapp-upgrade.json`：`tools/verify-installed-upgrade.ps1 -Mode InApp`，本机装着的 0.37.0 走软件内更新到 0.38.0，下载的是 `WinShun-0.38.0-x64-delta-from-0.37.0.zip`（约 7 MB），52.7 秒，服务、计划任务、剪贴板历史都在。这次用的是发布前一次打包（之后又修了 #394 和 #384 的滚轮，更新链路的代码没动）。
- `setup-over-installed.json`：`-Mode Setup` 用正式发布的安装包覆盖本机那份打包前的 0.38.0，装好后 `WinShun.exe` 和发布的一致，设置和剪贴板历史不变。
