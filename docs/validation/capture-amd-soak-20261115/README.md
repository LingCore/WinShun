# 截图测试包：A 卡 + FreeSync（2026-11-15）

#325 的报告人（A 卡，开着 FreeSync）截图时偶尔黑屏或画面定住 2 秒左右，转圈后指针不见。本机是 N 卡，复现不了。这个测试包拿到另一台有 FreeSync 的 A 卡电脑（780M 集显）上无人值守地跑，结果打成一个 zip 带回来。

## 包里有什么

```
WinShunCaptureTest\
  WinShunCaptureTest.exe   测试程序（soak.cpp，静态链接 C 运行库）
  使用说明.txt
  plugin\                  截图插件（WinShunCapture.exe + WinShunCaptureApp.dll，带指针诊断的版本）
  runtime\                 Qt 和 VC 运行库（dist\run-latest\WinShun 去掉 WinShun*.exe、pdfium、bin、plugins）
```

插件独立运行：`WS_APP_DIR` 指向 runtime，`WS_DATA_DIR` 指向这次的结果文件夹，按插件协议发 `action start` 开始、再发一次取消（和 `capture-perf-20261112/drive.py` 一样）。不装任何东西，不碰那台电脑上已有的 Win顺。

## 测试程序做什么

- 12 次截图，间隔交替 3 秒和 80–110 秒（报告人只在隔了 68 秒以上时卡），约 11 分钟。可用 `--gaps` 改，`--flip` 让遮罩改走翻转呈现做对照，`--no-drag` 不拖选区。
- 每次截图：鼠标停在屏幕中间，开始截图后用真实鼠标输入 6 秒转 3 圈，停 1.5 秒，用左键拖一个选区，然后取消。按左键前确认指针下面是截图插件的窗口；指针下面不是它超过 0.4 秒就停止转圈。
- 同时记录：
  - 桌面复制逐帧读主显示器：24×16 采样点的纯黑帧（相对截图前的基线）、转圈时最长多久没有任何更新（画面定住）、复制被打断（`ACCESS_LOST`，显示模式切换）、DWM 报的指针是否可见；
  - 每约 2 ms 读一次 `GetCursorInfo`：隐藏、无光标、指针在别的程序窗口上（记下程序和窗口类）、光标句柄变化；
  - 主显示器的刷新率（截图前后）、`WM_DISPLAYCHANGE`；
  - 看的人按 F9 的时刻（F9 被占用就用 F10、F8）。
- 结束后写 `summary.txt`（中文一览）、`rounds.jsonl`（每次截图一行）、`system.txt`（系统、显卡驱动、显示器名称和接口、缩放、硬件加速 GPU 计划、MPO、Windows 的窗口化游戏优化设置、指针设置）、`log.txt`、`plugin-stderr.log`，以及插件自己的日志 `data\diagnostics\events.jsonl`（`capture-stall`、`ui-stall` 调用栈、`capture-pointer-covered`、`capture-cursor-hidden`），再用系统的 `tar` 打成 `WinShun截图测试结果-<时间>.zip`。

## 构建

```
cl /nologo /std:c++20 /O2 /EHsc /utf-8 /MT /W4 /DUNICODE /D_UNICODE /DNOMINMAX soak.cpp /Fe:WinShunCaptureTest.exe /link /SUBSYSTEM:CONSOLE
```

在 `build\capture-amd-soak\` 下编译，包放在 `dist\capture-amd-test\`。插件取自 `build\run-latest`（630e1194 的构建）。包的 zip 用 .NET `ZipFile` 按 UTF-8 打：系统 `tar` 打的 zip 里中文文件名会变成 CP437 乱码（结果包里都是英文文件名，用 `tar` 没问题）。

## 本机验证

本机：Win11 25H2，RTX 4060 笔记本，2560×1600 240 Hz，150%。

- `--gaps 2,3,8`：3 次都正常，每次都转了圈、拖出了选区（插件日志 `selection` 约 760×507，`pointer_ticks` 约 190），黑帧 0，转圈时最长 14 ms 没有更新，zip 生成。
- 发现光标每 15 ms 才读一次：Windows 11 对窗口被遮住或最小化的进程不理会 `timeBeginPeriod`。用 `SetProcessInformation(ProcessPowerThrottling)` 退出这项节流后，9 秒里读了约 3300 次（约 2.7 ms 一次），读屏约 3870 次更新，转圈时最长 4–7 ms 没有更新。
- 从 zip 解压到新文件夹、在别的工作目录下启动，跑 1 次：正常，结果 zip 生成。

## 第一次实跑：Radeon 780M（2026-11-15）

另一台电脑：Ryzen 7 H 255，Radeon 780M 集显（驱动 32.0.13028.1005），Win11 家庭中文版 24H2（26100.4351），笔记本自带屏 2880×1800 120 Hz，200%，FreeSync 开着。默认参数（12 次，6 次隔 80–110 秒）。

- 12 次截图全部正常：黑帧 0，显示模式切换 0，刷新率前后都是 120 Hz，转圈时最长 8–47 ms 没有更新（47 ms 是第一次），指针隐藏、无光标、在别的窗口上都是 0，DWM 报的指针一直可见，F9 一次没按。
- 每次都转完了圈、拖出了选区（约 860×574）；每次读屏约 2000 帧，读光标约 3250 次。
- 插件日志：`capture-stall` 0，`ui-stall` 0，`pointer_ticks` 每次 189，`hidden_ticks`、`covered_ticks` 都是 0。遮罩出来 59–139 ms（插件自己算的，从指令到第一帧），隔得久的几次（101–111 ms）和连着截的（82–93 ms）差不多；中间有 5 次空闲回收内存（146–236 MB）。

**结论。** A 卡加 FreeSync 本身不足以出问题。和报告人的不同：独立显卡 RX 6800 XT、外接 4K 144 Hz 显示器、Windows 10 22H2；这台是集显、笔记本自带屏、Windows 11。固定 2 秒的黑屏更像外接显示器切换刷新模式时重新同步，笔记本自带屏不会这样，这一点和这次的结果对得上，但还没有证实。桌面上只有 5 个窗口，转圈时能框选的东西比报告人的少。
