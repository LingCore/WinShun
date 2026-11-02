# #111：设置窗口跨屏拖动卡一下

日期：2026-11-03。分支 `fx111`（基于 `fixes-1102`）。单屏机器（1 台显示器，150%，D3D11）。

## 报告者环境（诊断包）

Windows 10 22H2；主屏 1920×1080、副屏 1080×1920（竖放），两屏都是 100%、60 Hz；D3D11；NVIDIA RTX 2080 + Intel UHD 630。两屏缩放相同，所以换屏时没有 `WM_DPICHANGED`，走的是 Qt 的同 DPI 换屏路径：`WM_MOVE` → `QWindowsWindow::checkForScreenChanged` → 同步发出 `QWindow::screenChanged`。

此前 `docs/issue111-multimonitor-validation.md` 在真实双屏（两屏 150%）上记录到：最慢的 4 次 `WM_MOVE` 为 37–44 ms，每次都正好落在一次 `screenChanged` 上。说明卡顿是在换屏消息里同步做的事，而不是下一帧的渲染。

## 原因

QML 里每个读 `Screen.xxx` 的项都有自己的 `QQuickScreenAttached`。每个附加对象都连在窗口的 `screenChanged` 上，并往共用的 `QScreen` 上接约 9 个连接。换屏时，每个附加对象先 `oldScreen->disconnect(this)`（Qt 6.12 `qquickscreen.cpp`，`setWrappedScreen`），这一步要扫一遍这个屏幕上的全部连接；然后再接到新屏幕上。所以总耗时随附加对象个数的平方增长。

设置窗口的所有页面一直都在（隐藏页只是不可见），`SettingRow` 每行有约 5 个、`ScopeTabs` 每个标签一个，加起来约 1,550 个附加对象。几个窗口共用同一个 `QScreen`，所以设置窗口打开过以后，搜索框换屏也会变慢。

Qt 的 `forcePolish`（换屏时对所有项调用 polish，也包括隐藏的项）实测只占 1–3 ms，不是主因。

## 复现方法（单屏）

使用一个临时探针（`screen-probe.h.txt`，**不提交到代码**，用环境变量 `WINSHUN_SCREEN_PROBE` 打开）。它运行在隔离的 stage 副本里（`--profile`，不注入任何输入）。对每个显示出来的窗口交替做 20 轮普通 `update()` 和 20 轮模拟换屏：

1. 对每个附加对象调用 `screenChanged(nullptr)`。这就是真实换屏中“从旧屏断开”的那一步，耗时记为 leave。
2. 再调用 `screenChanged(原屏幕)`，记为 join。这一步是上限：从“无屏幕”回来时每个属性都算变了，DPR 也会触发通知。
3. 最后发 `QWindow::screenChanged`，记为 signal，包括 Qt 的 `forcePolish` 和其他接收者。

GUI 线程阻塞时间 = 1–3 步耗时 + 下一帧从 `UpdateRequest` 到同步结束的时间（polish + sync）。

## 结果（中位数 / 最大，ms；首轮预热不计）

本机是共用机器，模拟里 join 又是上限，所以这里的绝对值（修改前 135 ms）不能直接和负责人真机上的 37–44 ms 比较；请看改前改后的比例。

| 窗口 | Screen 附加对象 | leave | join | signal（含 forcePolish） | 下一帧 polish | GUI 线程阻塞合计 |
|---|---|---|---|---|---|---|
| 设置，修改前 | 1,547 | 118.6 / 144.5 | 10.7 / 13.9 | 2.9 / 3.8 | 1.2 / 1.7 | **135.5 / 163.2** |
| 设置，修改后 | 307 | 3.6 / 5.8 | 1.0 / 1.6 | 1.6 / 2.0 | 0.6 / 0.9 | **6.8 / 9.6** |
| 搜索框，修改前 | 60 | 8.6 / 19.3 | 0.6 / 6.1 | 0.1 / 0.2 | 0.1 / 0.2 | **9.9 / 20.8** |
| 搜索框，修改后 | 47 | 1.1 / 1.6 | 0.4 / 1.1 | 0.1 / 0.2 | 0.1 / 0.1 | **1.8 / 2.7** |

普通 `update()` 一帧的 GUI 线程阻塞，改前改后都 < 1 ms。设置窗口约 1.35 万个项、7,200 个有内容的项；隐藏页面的 polish 只占约 1 ms。修改前另跑过一轮，中位数为 173 ms，说明共用机器上的波动较大，但量级一致。

## 自动测试

- 新增 `tst_settingsscreen`：运行 `WinShun --preview-settings <目录> --screen-objects`，数设置窗口里的 Screen 附加对象，超过 500 个就失败。旧 QML 下数到 1,631 个，测试失败（已实测）；修改后为 355 个，测试通过。
- 以下测试通过：`tst_aisettings`、`tst_appearance_1/1.25/1.5/2`、`tst_scrollalignment_1/1.25/1.5/1.75/2`、`tst_searchlogo`、`tst_searchnavigation`。

## 画面不变

用 `--preview-settings` 在 150% 和 125% 下分别抓修改前后的 53 张图，逐字节比较：

- 150% 下 47 张完全相同，125% 下 49 张完全相同。
- 其余几张的差异都与本次修改无关：
  - `p07`：预览临时目录名是随机的。
  - `p10`：拾穗页的动画。
  - 搜索命中行：高亮颜色的渐变时刻不同，差值 ≤ 4/255，只在填充色上，边缘没有变化。
- 同一个 exe 连跑两次，`p07`、`p10` 也会不同。

## 不能证明的

- 这台机器只有一个屏幕。没有真实的第二个 `QScreen`，Windows 平台插件和 DWM/DXGI 一侧（交换链跨输出、跨显卡合成）都没有测到。
- 报告者有独显和核显两块显卡。如果竖屏接在核显上，跨适配器合成本身也可能卡一下，那部分与 Win顺 无关。
- join 是上限。真实换屏时，旧屏幕上的连接会随断开逐渐变少。
- 最终效果要请报告者在 Windows 10 双屏上复测。
