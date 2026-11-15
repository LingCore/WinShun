# 0.31.1 发布验证记录（2026-11-15）

内容：#325 界面字体（`215a5c96`）、截图卡顿的调用栈记录（`c3bf257e`，截图与录屏 1.1.17）。功能验证见 `docs/validation/issue-325-capture-stall-20261115/`（`tst_uifont` 两种字体引擎、`tst_uistall`、Win10 虚拟机和本机实测）。

## 这次按用户要求没做的

用户明确要求“不用测试和截图，搞快”：

- 发布时没有再跑 CTest（两处改动各自的测试在提交前已通过，见上面的记录）。
- 没有补拍海报，`docs/截图.md` 不变（没有新界面）。
- 没有做真实升级（`tools/verify-installed-upgrade.ps1`）、没有在干净虚拟机里做首次启动检查。
- `tools/verify-installer.ps1` 由 `scripts/release.ps1` 在打包前自动运行，结果见下。

## 打包

`scripts/release.ps1 -BuildDir build\run-latest -OutputDir dist\release0311`，从 `55d841eb`（标签 `v0.31.1`）构建。`tools/verify-installer.ps1` 5 项全过（正常升级、自己的程序在运行、库被别的程序加载、文件被别的程序读着、文件被独占）。

| 文件 | SHA256 |
|---|---|
| WinShun-0.31.1-x64-setup.exe | d1010a28ec491cee3947889ca6a97c0acaf1223bab2ad4596175056ad1099511 |
| WinShun-0.31.1-x64-portable.zip | 7e9ea1df07f3ddd7fc74ccf46fefb704cb89014477f3f00c5c0c920b19e4faab |
| WinShun-0.31.1-x64-offline-setup.exe | 1857e3dc5fcf93f19f8c8583a09c2cf61a77bc38aec85c83af4ccbef99764002 |
| WinShun-ocr-ppocr-v6-ort1.30.0-x64.zip | ccb82d77a545f183504edc51e6353370742d0423b71fa1cf7800db919494277e |

GitHub 上五个附件的大小和 SHA256 与本地一致后才发布。

## 插件库

只有“截图与录屏”有改动：1.1.17（`winshun.capture-v1.1.17`，源码 `1ab46575`，SHA256 `7cc4ca53eb485834e146771407db83518021c50e489c947d72b6b8fb33e6f628`，权限不变）。包取自同一次发布构建（工作区干净，等于 `55d841eb`），没有另外从提交副本重建。其余官方插件的源码自 0.31.0 以来没有变化。
