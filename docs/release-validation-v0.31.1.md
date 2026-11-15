# 0.31.1 发布验证记录（2026-11-15）

内容：#325 界面字体（`215a5c96`）、截图卡顿的调用栈记录（`c3bf257e`，截图与录屏 1.1.17）。功能验证见 `docs/validation/issue-325-capture-stall-20261115/`（`tst_uifont` 两种字体引擎、`tst_uistall`、Win10 虚拟机和本机实测）。

## 这次按用户要求没做的

用户明确要求“不用测试和截图，搞快”：

- 发布时没有再跑 CTest（两处改动各自的测试在提交前已通过，见上面的记录）。
- 没有补拍海报，`docs/截图.md` 不变（没有新界面）。
- 没有做真实升级（`tools/verify-installed-upgrade.ps1`）、没有在干净虚拟机里做首次启动检查。
- `tools/verify-installer.ps1` 由 `scripts/release.ps1` 在打包前自动运行，结果见下。

## 打包

见本文件末尾的校验值（由发布流程填入）。
