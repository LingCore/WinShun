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

## 没测的

- 真正的 1809 环境。这次是用 22H2 虚拟机代替的，导入表另外做了核对。
- 用户那台电脑上的实际图片。这正是要交给用户去跑的部分。
