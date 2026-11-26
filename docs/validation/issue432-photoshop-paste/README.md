# #432 剪贴板往 Photoshop 粘贴总是最后一张图

日期：2026-11-24　机器：Windows 11 26200，Photoshop 2024（25.11），本地构建 `build/issues-1124b`。

## 原因

Photoshop 只在自己被切到前台时读一次系统剪贴板，之后按 Ctrl+V 贴的是那时读进去的内容。Win顺 的剪贴板窗口默认不抢焦点（按键转发），Photoshop 一直在前台，没有“离开再回来”，所以换了剪贴板它也不知道，贴出的总是它最后一次被激活时剪贴板里的那张（Windows 自带的 Win+V 在 Photoshop 里也有同样问题）。

先用脚本直接验证了这个机制（不经 Win顺）：Photoshop 在前台时把剪贴板从红换成蓝，Ctrl+V 贴出红；换成绿后让它失焦再回前台、等 150 ms 再按 Ctrl+V，贴出绿；回前台后立刻按，仍是旧图。

## 改法

- `paste::readsClipboardOnActivation`：按程序文件名认出这类程序（Photoshop.exe、Illustrator.exe，大小写不限）。
- `paste::reactivate`：写好剪贴板后，让目标先把前台交给一个看不见的 1×1 工具窗口（屏幕外、无任务栏按钮），再立刻交还给它，等同于“点别处再点回来”，它会重新读剪贴板。
- `Clipboard`：目标一直没离开前台的两条路径（剪贴板窗口按键转发/固定模式下粘贴；按住粘贴、粘贴为纯文本等直接粘贴）里调用；回前台后等 150 ms 再发 Ctrl+V。其他程序不受影响，仍然完全不抢焦点。

## 端到端检查

`harness/photoshop-paste.ps1`（Windows PowerShell 5.1，`-STA`）：Photoshop 打开一张白色画布，剪贴板里放一张**绿图**并让 Photoshop 到前台（它读进绿图）；然后运行

```
WinShun.exe --probe-runtime <dir> --activation-paste-check <Photoshop 窗口句柄> [--picture-width 4000]
```

探针实例与正在运行的 Win顺 并存，历史里只有一张**红图**；用它自己的快捷键（Ctrl+Alt+Shift+F21）真按键打开剪贴板（按键转发，Photoshop 不失焦），按 Enter 粘贴。脚本再用 Photoshop 的 COM 取画布中心颜色。所有测试内容都带 `ExcludeClipboardContentFromMonitorProcessing` / `CanIncludeInClipboardHistory=0`，并设 `WINSHUN_TEST_NO_CLIP_HISTORY`，不进 Windows 和 Win顺 的剪贴板历史；剪贴板文字结束后放回。

| 运行 | 结果 |
|---|---|
| [run-control.txt](run-control.txt)：临时关掉 `reactivateTarget` 的同一构建（对照，开关已删） | 中心 `0,255,0`：贴进了 Photoshop 先前读到的绿图，复现 #432 |
| [run-fixed.txt](run-fixed.txt)：修复后 | 中心 `255,0,0`：贴进了剪贴板里选的红图 |
| [run-big.txt](run-big.txt)：修复后，红图 4000×2666 | 中心 `255,0,0` |

三次 Win+V 历史条数前后都是 9 条。另外：`tst_paster` 8/8 通过（新增“按文件名认程序”“reactivate 让窗口失焦一次再拿回前台、按键仍能到达”两项）；`--paste-plain-check`（同文件里的 #408 检查）通过。

## 0.45 之后的反馈（2026-11-26）

报告人说 0.45 里“截了几张图，打开剪贴板依次往 PS 里复制”，贴出的仍是最后一张。用上面的脚本在 Photoshop 2024 上把剪贴板的几种用法都试了一遍：

- 按 `Enter` 直接粘贴：贴对。把回前台后的等待临时改成 0、40、80 ms 也都贴对（本机），150 ms 是给慢机器留的余量。
- 先点一下搜索框（剪贴板拿到焦点）再按 `Enter`：贴对。这条路原来只等 60 ms，现在同样等 150 ms。
- **在条目上按 `Ctrl+C`（或右键“复制”），`Esc` 关掉剪贴板，再自己在 Photoshop 里按 `Ctrl+V`：贴出旧图**，复现报告。复制不经过粘贴流程，Photoshop 一直在前台，没有重新读剪贴板。

改法：复制完成时，如果前台是 Photoshop / Illustrator，就照粘贴时一样让它重新激活一次（`Clipboard::onWritten`）。重新激活时前台短暂落到替身窗口，`App::routedForeground` 按 `paste::isStandIn` 忽略它，剪贴板窗口不会因此关掉。诊断包里的 `clipboard-paste` 记录加了走的哪条路（`route`）、目标是否属于这类程序（`reads_on_activation`）和在前台多久后按的键（`front_ms`），复制后的重新激活记为 `clipboard-copy-reactivated`。

| 运行 | 结果 |
|---|---|
| [run-1126-copy-control.txt](run-1126-copy-control.txt)：`-CopyFirst`，复制后不重新激活（临时开关，已删） | 中心 `0,255,0`：贴出 Photoshop 先前读到的绿图 |
| [run-1126-copy-fixed.txt](run-1126-copy-fixed.txt)：`-CopyFirst`，修复后 | 中心 `255,0,0`，复制后剪贴板窗口仍开着 |
| [run-1126-enter.txt](run-1126-enter.txt)：直接 `Enter` | 中心 `255,0,0` |
| [run-1126-click.txt](run-1126-click.txt)：`-ClickClipboard`，先点搜索框 | 中心 `255,0,0` |

脚本这次的改动：探针带 `--renderer d3d11`（不带时探针是软件渲染的分层窗口，剪贴板空白处会被点穿，点搜索框会点到 Photoshop 上，和真实程序不同）；剪贴板原有内容按全部格式保存和放回（以前只放回文字）；新增 `-ClickClipboard`、`-CopyFirst`。另外 `tst_paster`（加了“重新激活时前台交给的是替身窗口”）、`tst_clipboard` 通过，`--field-focus-check --renderer d3d11` 通过。
