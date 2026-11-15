# #325 第二轮（霞鹜文楷常规体）和截图卡顿的调用栈记录：验证记录（2026-11-15）

基于 `42da1401`（0.31.0 之后），Qt 6.12.0 / MSVC 2022 Release，构建目录 `build/run-latest`。同一位报告人（Win10 22H2、RX 6800 XT、4K 200%、144 Hz、Win顺 以管理员运行）反馈了两件事：0.29.0 里霞鹜文楷常规体还是用不上；截图偶尔黑屏或卡一两秒，附了诊断包。

## 一、霞鹜文楷常规体（#325）

**原因。** 见 pitfalls.md 最上面一条。简单说：常规体的名称表里“全名”只有英文，Qt（FreeType 引擎）靠名字去注册表找字体文件，对不上，这个文件就没进 Qt 的字体名单。在 Win10 虚拟机（中文，装了 v1.530 的 Regular 和 Light，和报告人同一版）里用 `fontprobe.cpp` 查到的：

| 问 Qt 要 | 得到 |
|---|---|
| `QFont("霞鹜文楷")` | Tahoma（后备字体），`hasFamily` 为假 |
| `QFont("LXGW WenKai")` | 家族 LXGW WenKai，但样式是 **Light**（细体那个文件） |
| `QFont("霞鹜文楷 Light")` | 霞鹜文楷 Light，正常 |
| 把常规体的文件 `addApplicationFont` 之后 `QFont("LXGW WenKai")` | 样式 Regular |

0.29.0 把名字改成 Windows 列出来的“霞鹜文楷”，正好落在第一行。上一轮（11-14）验证时看到的“LXGW WenKai 能画”其实是第二行的细体。

**改法。** `UiFont.cpp`：Qt 按 Windows 列的名字画不出来的字体，用 DirectWrite 找到它的文件直接加载，再用 Qt 报的家族名和 Windows 报的字重。

**结果。**

- 虚拟机里实际运行（`UiFont=LXGW WenKai`，对应到“霞鹜文楷”；`Renderer=d3d11`，FreeType）：搜索框是霞鹜文楷，`325-regular.png`。换成 `LXGW WenKai Light`：`325-light.png`。两张图搜索框区域的墨量（灰度低于 200 的像素按深浅求和）：常规 786,671，细体 714,818，常规多约 10%，即常规体确实是常规体那个文件。
- `tst_uifont` 在 FreeType 和 GDI 下都通过（8 项）。新增：
  - `aFontQtDoesNotList`：把仓库里的 `Phosphor-Fill.ttf` 只加进本进程的 Windows 字体（`AddFontResourceEx(FR_PRIVATE)`，注册表里没有，Qt 的名单里也没有），选它之后界面字体就是它，不是后备字体；再选一次不重复加载。这条走的是“DirectWrite 没有文件、用 `GetFontData` 取字节”那条路。
  - `everyListedFontIsDrawn`：本机 276 个字体逐个选一遍，没有一个画成后备字体。改之前 FreeType 下有 90 多个画不出来（见 pitfalls.md）。

**没测。** 报告人机器上的原样（真显卡；字体是双击安装的，可能只装给当前用户，这种情况走的是同一段 DirectWrite 代码，但没实测）；可变字体的各个字重家族落在哪个实例；设置窗口里点选字体的那一下（只测了存好设置后启动）。

## 二、截图卡顿（诊断包 `WinShun-diagnostics -1114.zip`）

**日志里看到的。** 截图插件的日志里 103 次截图、25 条 `capture-stall`（11-02 到 11-13，0.2x 到 0.27.0）：

- 15 条是同一个样子：卡 2.06–2.17 秒（有一次 1.75 秒），恢复的时刻离“遮罩显示出来”固定 2181–2206 ms，也就是从遮罩第一帧显示的那一刻（前后 50 ms 内）开始卡。另有两次是两段各约 1 秒（964 + 778，983 + 953）。早期版本还有 4 次同样时长但出现在几秒之后。
- 卡的那些截图，距上一次截图都在 68 秒以上，或者是进程启动后第一次；间隔 30 秒以内的几十次一次都没卡。间隔长的也大多不卡。
- 0.26.0 起遮罩改成拷贝呈现（日志里 `presented: copy`），之后照样有。

时长固定，说明界面线程在等一个超时，或者等一个耗时固定的东西。代码里查过、排除了的见 pitfalls.md。本机和 Win10 虚拟机都没有复现（虚拟机里这一轮截了 3 次，上一轮的日志里也没有）。

**所以这次做的是让日志记下卡在哪**（architecture.md“运行诊断”）：截图期间 300 ms 不应答就记界面线程的调用栈，并把其他线程的也记下来，Windows 自己的模块带上导出函数名。

**验证。**

- `tst_uistall` 通过（9 项）。新增 `theOtherThreadsAreLookedAt`（另开 3 个线程等着，日志里 3 个都在，线程池里一样的只记一次，每段不超过 7 KB，只在第一次看的时候记）；`aWaitIsSeenAsWaiting` 多查一条：最里层是 `ntdll.dll+… NtDelayExecution+14`，Win顺 自己模块的帧后面不带名字。
- 本机（Win11、RTX 4060，独立运行新插件，`docs/validation/capture-perf-20261112/drive.py`）：遮罩显示时把界面线程挂起 1.3 秒。日志：`ui-stall`（300 ms，界面线程在 `Qt6Quick` 里等）、`ui-stall-threads` 两段（4866 和 2724 字节；进程 91 个线程，26 种调用栈，其中 `QSGRenderThread`、`QQmlThread` 有名字，显卡驱动 `nvwgf2umx.dll` 的 50 个工作线程记成一条）、`ui-stall`（1072 ms）、`ui-stall-end`（1240 ms）、原有的 `capture-stall`（1281 ms）。第一版把所有线程写在一条里，超过日志单条 8 KB 的上限被截成 `{"truncated":true}`，所以改成分段。
- Win10 虚拟机（22H2，WARP）：新主程序加新插件，截 3 次，第 3 次挂起界面线程 1.6 秒：`ui-stall` 两条、`ui-stall-end`（1524 ms）、`capture-stall`（1595 ms）都在，另两次没有任何卡顿记录。（这一次跑的是分段之前的版本，10 种调用栈在一条里。）

**没做到的。** 卡顿本身没有复现，原因仍未知；这次没有任何针对原因的修改。要等报告人升级后再发一次诊断包。
