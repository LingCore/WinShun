# #487–#498（2026-11-27）

| Issue | 改动 | 验证 |
|---|---|---|
| #490 0.50.0 闪退 | 诊断包里的崩溃记录是 `c0000005 at Qt6Qml.dll+1a79f2`，按导出表对到 `MemoryManager::onEventLoop` → `GCStateMachine::transition`（Qt 6.8 起默认的增量垃圾回收），崩溃前约 20 分钟电脑睡眠、唤醒过几次。所有带 QML 的进程启动时设 `QV4_GC_TIMELIMIT=0`（`QmlRuntime.h`），回收不再分步。日志里“上次闪退”那行补上异常位置 | 没有复现崩溃（空闲时偶发）。改后设置导航探针、预览交互测试、截图预览照常通过 |
| #493 关掉文件夹菜单后子项不收起 | “文件夹快捷菜单”下的 9 行改为随开关显示（`shown`），各分组自己的行随分组显示；设置内搜索找到被收起的行时仍指向它的开关（`unlocks`）。“记录图片”随“记录剪贴板历史”收起 | `WinShun.exe --preview-settings <目录> --navigation-settings`：新增 5 项检查全部通过；[493-folder-menu-off.png](493-folder-menu-off.png) |
| #492 清空搜索框后预览窗还在 | 搜索框清空且列表里没有任何一行时，预览窗收起；下一次有可预览的行时自动回来。输入了字但没找到时不收（避免打字时闪） | `ctest -R ^tst_previewinteractions`（含 150% 英文）通过，新增两项检查 |
| #489 截图里 `Shift+C` 没有结果卡片 | 截图和贴图里的“识别文字并复制”改走“识别屏幕文字”同一条路：复制后在选区旁显示卡片，可点“编辑” | `WinShunCapture.exe --preview-ocr <目录> --bubble` 卡片检查通过；`ctest -R ^tst_capture$` 通过。没有用真实按键跑整条链路（会改写这台电脑的剪贴板） |
| #497 复制截图后粘贴成空白图 | `writeDibRows` 对 `Format_RGB32` 的像素补上不透明的第四字节（原来原样拷贝，抓屏得到的可以是 0） | `tst_clipimage` 新增 `rgb32UnusedByteIsWrittenOpaque`：修复前失败（写出 `0x00204060`），修复后通过。**没有坐实这就是反馈里的原因**：截图插件交给主程序的图在本机测得第四字节已是 255；生成 .bmp 的是哪个程序也不清楚 |
| #487 检查更新只查到 0.48.0 | 更新服务现在对安装版和便携版都返回 0.50.0，客户端代码没有找到会停在旧版本的地方。日志里新增 `update-check` 事件（服务回答的版本、本机版本、是否正在安装、已跳过的版本） | `ctest -R ^tst_updater$` 通过。原因未找到 |
| #498 标注时出现黑块 | 未修改。黑块的位置和大小就是选中标注后的选中框 | `WinShunCapture.exe --preview-capture-tools`：软件渲染和 d3d11、100%–300% 共 13 种组合下选中矩形内部都是原图（新增的检查，[498-rect-selected.png](498-rect-selected.png)），本机没有复现，需要对方的诊断包（显卡和驱动） |

## 顺带量到的

- 设置窗口把所有页面都建出来时，主程序提交内存峰值：100% 缩放 548 MB，200% 缩放 1005 MB（`--preview-settings --renderer d3d11`，含出图）。#490 的截图里 200% 缩放、核显（Intel Iris Xe）下开着设置窗口时到过 2.6 GB，没有查明，另开一轮。
- `tst_clipimage` 里需要私有剪贴板的用例（含新增的 `screenshotReadAsOtherProgramsReadIt`）在本机直接运行时被跳过。
