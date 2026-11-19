# 0.39.0 安装与升级验证

- `inapp-upgrade.json`：`tools/verify-installed-upgrade.ps1 -Mode InApp`，本机装着的 0.38.0 走软件内更新到 0.39.0，下载的是 `WinShun-0.39.0-x64-delta-from-0.38.0.zip`（约 7 MB），44.4 秒，服务、计划任务、剪贴板历史都在。用的是发布前一次打包（`dist\release039`，打包时 `verify-installer.ps1` 6 项全过）；之后只改了设置预览的一条自检和海报，更新链路的代码没动。
- 自动测试只跑了这次改动涉及的：`tst_favorites`、`tst_plugin`、`tst_cliplifecycle`、`tst_ocr`、`tst_stage7`、`tst_uistall`，6/6 通过。
- 收藏夹列头排序（#397）用 `WinShun.exe --preview-settings <目录> --favorites-sort` 点真实列头核对顺序：浅色 150%、深色 125%、英文界面都没有警告。
