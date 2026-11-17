# 0.34.1 发布验证记录（2026-11-17）

内容：IdleTrim 在 Windows 10 上又会归还内存（86e22576，会话 claude-45）。`PROCESS_MEMORY_COUNTERS_EX2::PrivateWorkingSetSize` 要 Win10 22H2 打 2023 年 9 月累积更新或 Win11 22H2 才有，更早的系统上调用成功但读出 0，IdleTrim 就从不归还（用户内存报告：1809 LTSC 17763）。读到 0 时改用整个工作集，日志写 `working_set_mb`。

## 测试

- `release.ps1 -Tests '^tst_hotpages$'`：通过。打包在干净的工作树 `.worktrees/release0341`（ef662a43）里做：第一次在主工作区打包时，另一个会话正在改 Win10 兼容的 7 个文件，有一部分被编进去了，那个包已作废，没有用。
- Win10 22H2 虚拟机（19045.6456，claude-45 测）：正常路径约 20 秒后 `idle-trim private_mb 62`，主进程降到 6–8 MB；强制走旧系统路径的临时版本 `idle-trim working_set_mb 119`，降到 6–8 MB 专用，150 秒内没有反复归还。

## 安装和升级

- `tools/verify-installer.ps1`：5 项全过。
- `tools/verify-installed-upgrade.ps1 -Mode InApp`：本机 0.34.0 经应用内更新真实升级到 0.34.1，49.6 秒；普通权限启动，索引服务在运行，开机自启任务和搜索历史（SHA256 不变）保留。

## 没有覆盖的

- 真实的 Win10 1809 机器；截图与录屏、图片文字搜索里的 IdleTrim（同一份源码，没有单独跑）。

## 插件

- 截图与录屏 1.1.21、图片文字搜索 1.0.20：都编进了 `IdleTrim.cpp`，所以升版本。

## 打包

| 文件 | SHA256 |
|---|---|
| WinShun-0.34.1-x64-setup.exe | 5a1be1c1ba52e130bfceed76c05cfe91b26fe32aec9974f662806efb29f83d7b |
| WinShun-0.34.1-x64-portable.zip | 078e8d839cd6c09ed191a2e590569c4a41b64a5666e537d27811b6f48103745d |
| WinShun-0.34.1-x64-offline-setup.exe | 205cc1153b6d2e976fc32c15956a6f2a5c847b25da4817c47b0b0db83f57a943 |
| WinShun-ocr-ppocr-v6-ort1.30.0-x64.zip | ccb82d77a545f183504edc51e6353370742d0423b71fa1cf7800db919494277e |
