# 录屏：Windows 10 1809（LTSC 2019）验证（2026-11-20）

起因：用户反馈，虚拟机里的 Win10 企业版 LTSC，Win顺 0.40.0、截图与录屏 1.1.28，录屏报“录屏失败：不支持此接口（0x80004002）”。原因和做法见 [docs/pitfalls.md](../../pitfalls.md) 最上面一条。

## 环境

- Hyper-V 虚拟机，Windows 10 企业评估版 LTSC 10.0.17763（微软评估版 ISO `17763.107.101029-1455.rs5_release_svc_refresh_CLIENT_LTSC_EVAL_x64FRE_zh-cn.iso`），zh-CN，1024×768，没有 GPU（Microsoft Hyper-V 视频，D3D11 没有视频功能，走 CPU 转 NV12 + 软件编码）。
- Win顺 0.41.0，用正式安装包 `/VERYSILENT` 装的。
- 用插件自己的录屏验证入口：`WinShunCapture.exe --preview-recording <目录> --renderer d3d11 --auto --app-dir "C:\Program Files\WinShun"`。它用真实的截图遮罩、录屏面板、`Recorder` 和录屏工作进程，录一个带移动蓝块的测试窗口两段（第二段是“再录一段”）。

## 结果

| 插件 | 结果 | 日志关键行 |
|---|---|---|
| 已发布的 1.1.29 | 两段都没录成 | [preview-1.1.29.txt](preview-1.1.29.txt)：`录屏失败：不支持此接口（0x80004002）`，`stage screen capture startup`，硬件、软件编码两次都一样 |
| 本次修改 | 两段都保存了 | [preview-new.txt](preview-new.txt)：`screen GDI (Windows.Graphics.Capture: 0x80004002 不支持此接口)`，第一段 95 帧 / 3181 ms（30 帧/秒），第二段 22 帧 |

- 画面：第一段视频 800×460、30 fps，逐帧量蓝块的位置，从左往右再往回，每帧移 9–10 像素，和测试窗口的动画一致。文字清楚，颜色对。[frames-1809.jpg](frames-1809.jpg) 是第 10、45、85 帧。
- 指针：把虚拟机的鼠标放在录制区域里再录，视频里画出了指针，热点在箭头尖上（[pointer-1809.png](pointer-1809.png)，放大 3 倍）。
- 第一次重录时虚拟机的显示器已经熄了，DWM 不合成，录下来每帧一样（蓝块不动）。关掉“关闭显示器”的计时、唤醒屏幕后重录才正常。这是测试环境的问题，不是程序的问题；真机熄屏时本来也没东西可录。

## 本机（Windows 11）

- `tst_recording` 全部 23 项通过，包括新的 `screenFrames`：在屏幕上放一个纯色窗口，分别用 Windows.Graphics.Capture 和 GDI（`Options::screenByGdi`）真录，解码后至少 5 帧，中心颜色在 BT.709 来回转换的误差内。一个进程里连跑两遍也通过，验证了工厂缓存的修复。

## 没测的

- 真机（有 GPU）上的 1809：GDI 读屏的 CPU 占用没量过。
- 1809 上开着 HDR 的显示器（这时 DWM 给 GDI 的已经是 SDR）。
- Windows 10 2004–22H2 上没有重测：它们还走 Windows.Graphics.Capture，这部分代码只是搬了位置，`screenFrames` 的 `windows-graphics-capture` 那一行在本机覆盖了这条路。
