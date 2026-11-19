# 读图测试工具验证（2026-11-19）

工具：[tools/ocrtest](../../../tools/ocrtest/README.md)（`WinShunOcrTest.exe` 1.0），用 `scripts/ocrtest.ps1` 打包。起因是一位用户的 Win10 LTSC 1809 云电脑启动约两分钟就闪退（见 [docs/pitfalls.md](../../pitfalls.md)“闪退什么都没留下”）。

## 打包

- 压缩包 10.9 MB：exe、Qt6Core/Gui/Sql、qsqlite、qgif/qjpeg、VC++ 运行库（msvcp140*、vcruntime140*）、`使用说明.txt`。windeployqt 不允许跳过平台插件（会报 “Unable to find the platform plugin.”），所以先让它部署，再删掉 `platforms\`。工具用的是 `QCoreApplication`，不会加载平台插件。
- 导入表：wscapture 带进了 `windowsapp` 伞库，所以 kernel32 的函数都通过 `api-ms-win-core-*` 导入，其中 5 个 WinShun.exe 自己都不用（控制台函数、`AddVectoredExceptionHandler`、`GetSystemMetrics`）。链接时把 `kernel32 user32` 放在它前面以后，只剩下 6 个 UCRT 的 `api-ms-win-crt-*`，这些是 Windows 10 自带的。

## 运行

| 环境 | 结果 |
| --- | --- |
| Windows 11 25H2，i9-14900HX，PATH 里去掉 Qt 和 VS，运行 `dist\ocrtest\WinShunOcrTest\` 里的副本 | 示例图正常，307 MB，1.4 秒；剪贴板 8 张，448 MB，4.6 秒 |
| Hyper-V Windows 10 22H2（19045），4 个虚拟处理器，2 GB，**没装 VC++ 运行库**，Win顺 组件放在 `C:\ocrprog` 用 `--program` 指定 | 能启动，控制台中文显示正常；示例图正常，306 MB；剪贴板 5 张，64 MB；按回车后进程全部退出 |

第一次在虚拟机里运行时，剪贴板那一轮判为正常，却留下了两个 `thrown` dump。用 cdb 打开，类型都是 `ZXing::Error`：ZXing 找二维码时会自己抛出再自己接住。这样会把仅有的两个异常 dump 名额占掉，所以改成先从 ThrowInfo 读出类型名，跳过 ZXing 的异常，其余异常在步骤里记一行 `thrown <类型>`。再次在虚拟机里运行，没有再生成 dump。

## 故意出错（`--fault`）

| 情形 | 结果 | dump |
| --- | --- | --- |
| `abort` | crashed，退出码 0x3 | crash |
| `access` | crashed，0xC0000005 | crash（带异常上下文） |
| `throw`（另一个线程里抛出没人接的异常） | crashed，步骤里有 `thrown .?AVruntime_error@std@@` | thrown1、crash |
| `oom --limit-mb 1600` | memory-limit，步骤里有 `thrown .?AVbad_alloc@std@@` | 1GB、limit、thrown1 |
| `hang --timeout 8` | timeout | timeout |
| `oom:<剪贴板图片编号> --limit-mb 1600` | 一起读时被停下，然后单独读、用 1 个线程读，都被停下；剩下 3 张正常读完；结论里列出这张图（尺寸、复制时间） | 每次运行各有 1GB、limit、thrown1 |

`abort`、`access`、`hang` 是在最后两处改动（跳过 ZXing 异常、待读的图之后接着读已读过的图）之前测的，这两处改动不经过它们的路径。`throw`、`oom` 和隔离流程在改动之后又测了一遍。

用 cdb 加载本次构建的 pdb 打开 dump，调用栈完整，例如 oom 的 `thrown1`：`KERNELBASE!RaiseException ← _CxxThrowException ← __scrt_throw_std_bad_alloc ← operator new ← fail ← child ← main`。

## 用户的结果（同一天）

用户的电脑：Windows 10 LTSC 2019（17763），虚拟化层上报的处理器是 Core 2 Duo T7700，没有 AVX，4 个逻辑处理器，16 GB 内存。剪贴板历史里有 69 张图，其中 1 张还没读过字。

- 示例图正常：307 MB，5.2 秒，比本机慢 3–4 倍。
- 剪贴板一起读的那一轮：最高 2.4 GB，停在 `detect 28064 x 2016`，之后识别报错（ORT 的 `bad_alloc`，在 `NchwcMaxPool` 里）。那张图是 **32739 × 35**，11-17 10:42 复制，也就是唯一一张还没读过字的，用户从那天开始闪退。
- 工具 1.0 选错了“出问题的图”：子进程读完、只是有图没读出来时，它取了最后读的那张（979 × 64）。现在改成：读完的取内存最高的（超过 1.5 GB 时），否则取第一张没读出来的。

## 修复之后

用 System.Drawing 画了一张同尺寸的长条（106 段中英文），放进临时的剪贴板历史，新旧两版工具各读一次，都是 2 个线程、3 GB 上限：

| 版本 | 结果 |
| --- | --- |
| 修复前（发给用户的那版） | 和用户一样：2.4 GB，停在 `detect 28064 x 2016`，识别报错 |
| 只修了找文字行 | 301 MB，接着在读二维码时抛 `Maximum image width/height is 65535`（0.38 在这里会 `abort()`） |
| 两处都修好 | 301 MB，3.2 秒，读出 1 行 2973 个字；找文字行分成 15 块 2016 × 512，外加 1 块小的，识别分成 2 批（3 × 11996、2 × 12000） |

自动测试（只跑相关的）：`tst_ocr` 35 个全过（新增 `veryWideStrip`），`tst_ocrsearch` 37 个全过，`tst_stage7` 41 个通过、1 个跳过（新增 `longStripReadsWithoutThrowing`；跳过的 `realPicturesAreRead` 需要设置 `WS_QR_SAMPLES`，本来就跳过）。

另外，编译 WinShun、截图插件、OCR 搜索插件时发现，`f04304c8` 起截图插件一直链接失败：它编译了 `UiStall.cpp`，却没有编译 `CodeAddress.cpp`。已补上。

## 没测的

- 工具本身在 1809 上能运行，已经由用户那次运行证实；修复后的代码还没在用户电脑上跑过。那张图没拿到（用户发来的是工具选错的 979 × 64），本机用同尺寸的合成长条代替。修复后重新打包的工具可以发给用户确认。
