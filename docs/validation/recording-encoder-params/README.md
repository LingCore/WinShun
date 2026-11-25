# 录屏：各家编码器的画质参数（对照 Cap）

日期：2026-11-25　主机：Windows 11 26200，RTX 4060 Laptop（NVIDIA H.264 Encoder MFT，驱动 32.0.15.9579），2560 × 1600、150% 缩放。构建 `build/recording-sharp`（分支 `recording-sharp`）。本机没有 AMD、Intel 显卡。

起因：反馈“最清晰的档位录出来还是模糊”。对照了 Cap（github.com/CapSoftware/Cap）的 Windows 录制管线，并查了 OBS、Chromium、GStreamer 对各家编码器的处理。

## 本机（NVIDIA）量到的

真实桌面截图当画面，2560 × 1600、30 帧，取解码后的亮度平面和原图比：

| 画面 | 小体积（QP 25） | 标准（QP 22） | 超清（QP 16） |
|---|---|---|---|
| 静止 | 47.3 dB | 49.8 dB | 54.0 dB |

`tiers-real-desktop.png`：从上到下是原图、超清、标准、小体积（放大 3 倍），看不出差别。整屏滚动和叠加视频的画面里，文字区域放大看也和原图一致（没有留准确的数）。应用自身流程（选区 → 录制 → 保存）在“超清”下把 800 × 460 录成 1600 × 920、60 帧，3.13 秒 188 帧。结论：NVIDIA 上文件本身不糊。

## 找到并改掉的

| 事实 | 依据 | 改法 |
|---|---|---|
| 质量模式是固定 QP；NVIDIA 给了 QP 就按 QP（质量 100 + QP 30 → 实际 30，+ QP 10 → 10），没给才按质量换算（100 / 90 / 80 / 70 → 16 / 19 / 22 / 25）；系统软件编码器只按质量值 | 本机实测，读码流里的 `slice_qp_delta` | 质量值和 QP 一起给，数值一致；NVIDIA 上画质和体积不变 |
| 以前只给质量值：不自己换算的显卡编码器四档会是同一个默认 QP | OBS、GStreamer 都是两个值一起给或优先 QP | 同上；拒收 QP 时再只给质量值 |
| AMD 的质量模式不可靠；只给输出类型的码率时实际码率是目标的 2 倍，显式设模式才守得住；“平均 + 1.5 倍峰值”的可变码率正常 | Chromium 对 AMD 关掉了质量模式 + QP；RX 6800 XT / 6900 XT 上别人的实测；Cap 在 AMD 上只用按码率的可变码率 | AMD 直接走显式的峰值受限可变码率 |
| 按码率时我们给得偏低（1080p、30 帧“标准”7.5 Mbps） | Cap：每像素 0.15 / 0.3 / 1 bit（1080p、30 帧 9 / 19 / 62 Mbps） | 四档 0.15 / 0.3 / 0.6 / 1 bit，上限 100 Mbps |
| 编码器接受了参数不等于按它编 | – | 协商后读回模式、质量值、QP 写进日志（`encoder holds: mode 3, quality 100, QP 16`） |
| Cap 的视频处理器按“最高质量”用途建 | Cap `video_processor.rs` | 同样改成 `D3D11_VIDEO_USAGE_OPTIMAL_QUALITY` |

本机各条路径的日志：

- 质量模式：`quality 100, QP 16 (encoder holds: mode 3, quality 100, QP 16)`
- 按码率（`--bitrate-only`，1920 × 1080 标准）：`bitrate 18662 kbps, peak 27993 (encoder holds: mode 1, …)`；近乎静止的 2 秒样片只有 282 KB，码率没有被白白用满
- 软件编码：`quality 80, QP 22 (encoder holds: mode 3, quality 79, QP 22)`

## 看过、没有照搬的

- Cap 记录过 Intel 编码器在宽度不是 16 的倍数时直接崩溃：出在把内存里的帧交给编码器的路径上；我们交的是显卡纹理，而且工作进程崩溃后会自动改用软件编码重录，所以没有为它禁用 Intel 的显卡编码。
- Cap 录制前先试编几帧灰色再解码核对（防编码器吐全零画面）：每次录制要多等几百毫秒，本次没加。
- Cap 同样是 4:2:0，没有 4:4:4；彩色小字的颜色仍靠“超清”的 2 倍尺寸保住。

## 测试

`tst_recording`：`quantizerByQuality`、`bitrateByQuality`、`clarityByQuality`（硬件、软件）等 41 项通过。另有 3 项（`encodedVideo` 的两个 click-highlight、`trailInFrames`）在这台机器上此刻失败，用未改动的 main 构建跑结果相同，不是本次改动带来的；原因没有查。

## 没有验证的

- AMD、Intel 显卡：没有硬件。AMD 那条路按别人的实测和 Cap 的做法定的，等带日志的反馈确认。
- 播放端：录的选区比播放窗口小或大时播放器会缩放，画面会比屏幕上软，这和文件无关，没有处理。
