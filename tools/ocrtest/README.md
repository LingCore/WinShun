# 读图测试（WinShunOcrTest.exe）

用户反馈 Win顺 启动后不久就闪退、内存暴涨，而且怀疑和剪贴板历史补读图片文字有关，可我们这边复现不了时，把这个工具发给用户。它在用户的电脑上，用 Win顺 自己的文字识别组件，照 Win顺 启动时的顺序把剪贴板历史里的图读一遍，记下每一步做了什么、用了多少内存，以及出问题时停在哪。用户运行一次，我们就能知道：

- 这台电脑读普通图片是否就有问题（处理器、内存环境），还是只有某一张图有问题；
- 是哪张图，它多大，读到哪一步（找文字行、切行、按批识别、找二维码）时内存涨上去；
- 程序停下时各线程的调用栈（dump）。

读图代码就是 Win顺 的 `PaddleOcr` 和 `ws::capture::withQrCodes`（ZXing），编译进了这个 exe，不是另写的一套。用的是打包时的源码：修复之后重新打包，就能拿去让用户确认问题在他的电脑上已经解决。识别组件（`onnxruntime.dll` 和两个模型）用用户电脑上已经装好的那份，所以 ONNX Runtime 的版本、文件也和用户的 Win顺 完全一样。

## 构建与打包

```powershell
. .\scripts\devshell.ps1
.\scripts\ocrtest.ps1
```

打包结果放在 `dist\ocrtest\`：

| 路径 | 内容 |
| --- | --- |
| `WinShunOcrTest.zip` | 发给用户的压缩包（约 11 MB）：exe、Qt（Core、Gui、Sql，SQLite 驱动，GIF/JPEG 解码）、VC++ 运行库、`使用说明.txt`。不用安装，没装 VC++ 运行库的电脑也能运行 |
| `WinShunOcrTest\` | 同样的文件，没有压缩 |
| `symbols\` | 这次构建的 exe 和 pdb，分析用户带回的 dump 时需要。**每次打包都会覆盖**：发给用户之后，在结果回来之前不要再打包，或者把这个目录另存一份 |

脚本最后会打印压缩包的 SHA256，可以和用户手里的那份核对。

exe 链接了 wscapture，它会带进 `windowsapp` 伞库。CMake 里把 `kernel32`、`user32` 放在 wscapture 前面，这样系统函数直接从 kernel32/user32 导入，不走 API set。用 `dumpbin /dependents` 检查过，剩下的 `api-ms-win-*` 只有 UCRT 的 `api-ms-win-crt-*`。

## 用户怎么用

见 [使用说明.txt](使用说明.txt)，打包时会放进压缩包。简单说：解压，双击 exe，按窗口里的提示退出 Win顺，等它读完，把桌面上的 `WinShun-ocr-test-<日期>-<时间>.zip` 发回来。不需要管理员权限，也不会改动剪贴板历史和设置。

## 它怎么测

1. **找位置**：如果 Win顺 正在运行，先记下它的路径，再请用户退出它（不退出也能测，只是两边同时读图，内存数字不准）。数据文件夹是便携版 exe 旁边的 `data`，否则是 `%LOCALAPPDATA%\WinShun`；识别组件按 Win顺 自己的规则（`ws::ocr::folder`）找：先找数据文件夹里 `components\<版本>` 下校验过的那份（有 `.ready`），没有时找程序文件夹。
2. **限额**：每次读图都在一个子进程里进行，子进程放进 Job 对象，提交内存的上限是 min(3 GB, 可提交余量 − 768 MB)，最少 1 GB。用户电脑不会因为测试被拖死。线程数和 Win顺 一样：逻辑处理器数的一半，在 1～4 之间。
3. **示例图**：先读两张工具自带的图（1440 × 1440 的界面海报，6000 × 360 的长行表格）。这两张图在哪台电脑上都一样，用来区分是电脑环境的问题还是图片的问题。
4. **剪贴板历史**：把 `clipboard.db` 复制到临时文件夹再读，不碰原文件。顺序和 Win顺 一样：`kind = 4`，按 `used DESC, id DESC` 排序。Win顺 启动时要补读的图（`ocr IS NULL`；识别器换过时是全部）排在前面，后面接着读已经读过的图，所以没有待读的图时也能测到用户自己的图。最多读 80 张，最长 20 分钟。
5. **隔离**：一起读出了问题，先找出是哪张图。子进程中途停下（崩溃、超时、被限额停下、抛异常）时，取当时正在读的那张；读完了但内存超过 1.5 GB，取内存最高的那张；否则取第一张没读出来的。找到后把那张图单独再读一次，然后只用 1 个线程再读一次，再接着读剩下的图。
6. **结论**：窗口最后会给出一句结论。如果找到了出问题的图，会告诉用户可以在剪贴板历史里删掉它，好让 Win顺 先不闪退；也可以把图发给开发者（这一步由用户自己决定）。

子进程每做一步就向父进程报一行，同时父进程每 100 毫秒采样一次子进程的提交内存和工作集。`PaddleOcr::setTrace` 在识别过程中报告每一步：图片原尺寸、实际读的尺寸、找文字行的尺寸、每条检测带、找到几个框、切出几行和其中最长一行、每批识别几行以及这一批的输入宽度、倒过来重读。程序本身不设置 trace，所以这些代码在 Win顺 里不会构造任何字符串。

## 什么时候生成 dump

dump 由父进程在进程外用 `MiniDumpWriteDump` 写，参数是 `MiniDumpNormal | MiniDumpFilterMemory | WithThreadInfo | WithUnloadedModules`：包含各线程的调用栈和模块列表，不包含堆。

| 文件 | 时机 |
| --- | --- |
| `runN-1GB.dmp`、`runN-2GB.dmp` | 子进程的提交内存第一次超过 1 GB、2 GB 时 |
| `runN-limit.dmp` | 离上限只剩 160 MB 时（随后分配失败，子进程被停下） |
| `runN-thrownK.dmp` | 抛出 C++ 异常的那一刻，栈还没展开（向量化异常处理），每次运行最多 2 个。ZXing 在多数图片上会自己抛出 `ZXing::Error` 再自己接住，这类异常不算。每个异常（最多 20 个）还会在步骤里记一行 `thrown <类型名> on thread <id>` |
| `runN-crash.dmp` | `abort()`（SIGABRT）或未处理的 SEH 异常时，带异常上下文 |
| `runN-timeout.dmp` | 一次运行超过 240 秒还没结束 |

N 是第几次运行（1 是示例图）。子进程里同时装着 Win顺 的 `CrashRecord`，它写的 `crash.txt` 会进 `results.json` 里这次运行的 `crash`。

## 压缩包内容

| 文件 | 内容 |
| --- | --- |
| `report.txt` | 用户在窗口里看到的全部文字 |
| `results.json` | 电脑信息：Windows 版本和 UBR，处理器型号、AVX2/AVX-512、是否在虚拟化层上；测试前后的物理内存和可提交内存；Win顺 的版本和是否在运行；识别组件（ONNX Runtime 文件版本，三个文件的 SHA-256 是否和 `resources/ocr/package.json` 一致）；剪贴板历史的统计（多少张图、待读几张、文件缺几个、记录的识别器、最大尺寸）。`runs[]`：每次运行的结果（`ok`、`some-failed`、`threw`、`not-loaded`、`crashed`、`memory-limit`、`timeout`）、退出码、用时、CPU 时间、内存峰值、最后在读的图、每张图（编号、复制时间、格式、尺寸、文件大小、是否待读、读它时的内存峰值）、每一步（毫秒、文字、开始时和这一步期间的最高提交内存）、内存时间线（最多 600 个点） |
| `dumps/*.dmp` | 见上一节 |

压缩包在每次运行之后都会重写，所以测试中途窗口被关掉，桌面上也会留下已经做完的部分。

## 分析思路

1. 先看 `report.txt` 的结论：示例图有问题，说明是环境问题；找到了某张图，就看是哪张；都正常，说明读图这条路没问题，闪退要到别处找（这时看 Win顺 0.39.0 起诊断包里的 `app-crash`）。
2. 在 `results.json` 里找出问题的那次运行，看它的 `steps`：内存在哪一步涨上去（`highest_mb`），是 `detect`、`recognize N lines, 48 x W` 还是 `qr codes`；`picture ... read at ...` 是不是被缩小过。对照用 1 个线程读的那次，判断内存和线程数有没有关系。
3. 看 dump：

   ```powershell
   $env:_NT_SYMBOL_PATH = 'F:\电脑便捷工具\dist\ocrtest\symbols;C:\Qt\6.12.0\msvc2022_64\bin;srv*C:\symbols*https://msdl.microsoft.com/download/symbols'
   $cdb = 'C:\Program Files (x86)\Windows Kits\10\Debuggers\x64\cdb.exe'
   # dump 里没有模块的映像：-i 给出同版本的 onnxruntime.dll（构建目录里那份，三个文件和发布时一致才算同版本），系统 DLL 从符号服务器取
   $img = 'F:\电脑便捷工具\build\release;F:\电脑便捷工具\dist\ocrtest\symbols;srv*C:\symbols*https://msdl.microsoft.com/download/symbols'
   & $cdb -i $img -z run2-thrown1.dmp -c '.ecxr; kc 30; q'        # crash 和 thrown：带异常上下文
   & $cdb -i $img -z run2-1GB.dmp -c '~~[0x2d14]s; kc 30; q'      # 1GB、2GB、limit、timeout：读图线程（步骤里 thrown 那行有线程号）
   ```

   不加 `-i` 的话，回溯走到 `onnxruntime.dll` 就断了。微软的符号服务器上有官方 ONNX Runtime 的完整 `onnxruntime.pdb`（1.30.0 的约 420 MB，第一次会下载），所以能看到具体是哪个算子，比如 `NchwcMaxPool::Compute`、`MlasConvNchwFloatKernelSse`（SSE 内核说明这台电脑没有 AVX）。

## 本机参考值

| 电脑 | 线程 | 示例图（界面截图 / 宽表格） | 合计 |
| --- | --- | --- | --- |
| i9-14900HX，Windows 11 25H2，32 GB | 4 | 约 125–145 MB / 155–190 MB | 约 307 MB，1.3–1.4 秒 |
| 同一台电脑上的 Hyper-V Windows 10 22H2，4 个虚拟处理器，2 GB，没装 VC++ 运行库 | 2 | 145–215 MB / 189–306 MB | 约 306 MB，1.7–2.9 秒 |

普通截图一张通常在 30–200 MB 之间。如果用户电脑上的示例图比这里高出很多，就是环境问题。

## 隐私

报告里只有数字：图片尺寸、文件大小、复制时间（月-日 时:分）、内存和用时，以及读到了哪一步、读出了几行几个字。没有图片，也没有读出的文字。用户文件夹的路径写作 `%USERPROFILE%`。dump 只包含线程栈和模块列表；`MiniDumpFilterMemory` 会去掉栈上和回溯无关的内容，堆不包含在内。剪贴板历史只读副本，读完就删。

## 参数（排查用，普通用户不需要）

| 参数 | 作用 |
| --- | --- |
| `--program 目录` | Win顺 的程序文件夹（默认是正在运行的 Win顺 所在的文件夹，否则是 `Program Files\WinShun`）；放着 `onnxruntime.dll` 和 `ocr\` 的构建目录也可以 |
| `--data 目录` | Win顺 的数据文件夹 |
| `--out 目录` | 压缩包保存位置，默认是桌面 |
| `--most N` | 最多读剪贴板里的 N 张图，默认 80 |
| `--minutes N` | 读剪贴板历史的时间上限，默认 20 |
| `--threads N` | 识别线程数，默认和 Win顺 一样 |
| `--limit-mb N` | 子进程的内存上限 |
| `--timeout N` | 一次运行超过 N 秒就停下，默认 240 |
| `--no-wait` | 开始时不等用户退出 Win顺，结束后不等按回车，用于自动测试 |
| `--no-open` | 结束后不打开资源管理器 |
| `--fault 种类[:编号]` | 让子进程故意出错，用来试验这个工具：`abort`、`throw`（另一个线程里抛出没人接的异常）、`oom`（一直分配到上限）、`hang`、`access`（写空指针）。不带编号时在示例图那次出错；带上剪贴板图片的编号时，在读到那张图时出错，用来检验隔离流程 |

试验结果见 [docs/validation/ocrtest-20261119](../../docs/validation/ocrtest-20261119/README.md)。
