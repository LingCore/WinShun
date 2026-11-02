# Issue #165：Windows 10 圆角与阴影

验证日期：2026-11-01。关联 [issue #165](https://github.com/LingCore/WinShun/issues/165)。

**本机是 Windows 11，以下通过强制兼容模式验证 Windows 10 使用的渲染路径；尚未在 Windows 10 实机或混合 DPI 多屏环境验收。**

## 问题与实现

原实现的 Qt 窗口在 Windows 10 上使用 `CreateRoundRectRgn` / `SetWindowRgn` 裁剪。Region 只包含整数像素，无法表达圆弧边缘的半透明覆盖。项目已经使用 DWM 阴影，并没有 issue 中推测的 `QGraphicsDropShadowEffect`。现有文字使用 NativeRendering，GPU / 软件分别走 FreeType / GDI，此次保留字体、字体引擎及 hinting 选择。

Windows 10 保留圆角，并改为以下处理：

- `WindowMask` 直接在原场景的四角做 destination-out 抗锯齿。D3D11 使用预编译 shader，软件后端直接使用原 QPainter 目标；没有把整个窗口捕获为纹理再缩放。
- 原生 Region 只约束输入范围，向外留出 2 个物理像素以包含全部抗锯齿覆盖，并与实际窗口边界求交。分数 DPR 的遮罩尺寸与实际像素缓冲对齐，避免右侧和底部残留边线。
- 阴影使用独立、不可激活、鼠标穿透的 layered HWND。只绘制阴影，不复制 UI 内容；仅大小、圆角或 DPI 变化时重建位图，移动只更新位置，隐藏时释放位图。
- 跟随窗口移动、尺寸变化、DPI 通知、隐藏/显示、cloaking、最大化、全屏和销毁；保持圆角/直角设置。
- Windows 11 的正常路径继续使用原生 DWM 圆角、边框和阴影。

## 调研

已在 Grok 中针对本项目架构进行查询，见 [原始对话](https://grok.com/c/c778abef-b0aa-4b19-8200-8b8574fa80b2)。相关原文：

> 不必整体离屏。正确做法可以避免文字被模糊。

其建议采用应用层抗锯齿并将阴影与正文分开处理。实现保留了包住抗锯齿像素的粗输入 Region；它不再定义可见圆弧。未采用答复中混淆 Qt Quick 后端和 RHI 后端的环境变量写法，也未对逻辑坐标重复乘 DPR。

核对的主要资料：

- [Microsoft：Windows 11 圆角适用条件及限制](https://learn.microsoft.com/en-us/windows/apps/desktop/modernize/ui/apply-rounded-corners)
- [Qt：QSGRenderNode 的直接渲染与软件 PainterResource](https://doc.qt.io/qt-6/qsgrendernode.html)
- [Qt：QSGMaterialShader 混合状态](https://doc.qt.io/qt-6/qsgmaterialshader.html)
- [Qt：qt_add_shaders 与预编译](https://doc.qt.io/qt-6/qt-add-shaders.html)

## 构建与自动验证

环境：Windows 11 专业版 build 26200、Qt 6.12.0、MSVC 2022 x64 Release。Shader 包使用最低支持版本 Qt 6.8.3 的 qsb 生成，并包含预编译 HLSL；当前 Qt kit 未安装可选 ShaderTools，实际验证了使用随源码提交 shader 包的构建路径。

```powershell
cmake --build --preset release --target WinShun tst_windowmask tst_appearance tst_darkframe
ctest --test-dir build/release -R '^(tst_windowmask_.*|tst_appearance_.*|tst_darkframe)$' --output-on-failure
```

**13 组 CTest 全部通过，0 失败**，耗时 15.96 秒：[完整日志](tests.txt)。

| 检查 | 覆盖 | 结果 |
| --- | --- | --- |
| 新增圆角测试 | 软件/GDI、D3D11/FreeType × 100%、125%、150%、200% | 8 组通过 |
| 四角抗锯齿 | 明暗背景、8/4/0 半径、圆角→直角→圆角 | 存在部分透明像素，直角恢复完整画面 |
| 文字不被再次采样 | 程序自带字体、中英文和数字、NativeRendering | 遮罩内侧文字像素与无掩膜基准逐像素一致 |
| 原生 Region | 分数 DPR、三次窗口尺寸、全部四角 | 未裁掉任何非零 alpha 边缘像素 |
| 阴影生命周期 | 移动、隐藏、cloaking、最大化、全屏、恢复、销毁 | 位置/可见性/资源销毁断言通过 |
| Windows 11 原生外观 | 100%、125%、150%、200% | 4 组既有回归通过 |
| 深色原生框架 | 既有 tst_darkframe | 通过 |

生命周期测试的测试窗口设置了 `WindowDoesNotAcceptFocus`，状态恢复时 Qt 会输出一次 requestActivate 警告；该警告不来自生产 QML，相关断言全部通过。

最终程序构建成功，见 [产品构建日志](build-final.txt) 与 [预览字体路径对齐后的增量构建](build-preview.txt)。后一次改动只让软件预览使用与生产一致的 GDI；产品遮罩和阴影代码与 13 组测试时一致。

构建时工作区 HEAD：`07f9d3d5065a1e189a0c631cf003786aecc4fefd`，包含本次及其他并行任务的未提交修改，此 hash 不代表本次补丁的提交。

最终 `build/release/WinShun.exe` SHA256：`F7A03BC4A72C517EDB2674C7BA449E425505D4000169E241023F637EBFDACC26`。

## 桌面合成与实际 QML

两种后端在 100% 下都补做了桌面合成验证：测试自建纯色背景，真实显示兼容窗口及其阴影，截图并断言窗口外存在阴影。日志：[D3D11](desktop-d3d11.txt)、[软件](desktop-software.txt)；截图：[D3D11 圆角](desktop-d3d11/frame-0.png)、[软件圆角](desktop-software/frame-0.png)。此处截图包含真实桌面合成，区别于 `grabWindow()` 的窗口内容截图。

实际 `Main.qml` 与 `SettingsWindow.qml` 使用隔离预览入口及临时配置验证，全部预览日志为 0 QML warnings。内容使用本地测试数据，未改用户剪贴板，未启动全局热键或替换用户正在运行的程序。

```powershell
Start-Process -FilePath ./build/release/WinShun.exe -Wait -PassThru -ArgumentList @(
  '--preview-settings', 'docs/validation/issue165-20261101/settings-light',
  '--page', '1', '--corners', 'round', '--composited-frame', '--renderer', 'd3d11', '--scale', '1')
# 加 --dark 验证深色；去掉 --renderer d3d11 验证软件/GDI。
Start-Process -FilePath ./build/release/WinShun.exe -Wait -PassThru -ArgumentList @(
  '--preview-results', 'docs/validation/issue165-20261101/main-light',
  '--file-metadata', '--composited-frame', '--corners', 'round', '--scale', '1')
```

| 生产界面 | 截图 | 日志 |
| --- | --- | --- |
| 设置页，D3D11 浅色 | [截图](settings-light/p01-t0.png) | [日志](settings-light/preview.log) |
| 设置页，D3D11 深色 | [截图](settings-dark/p01-t0.png) | [日志](settings-dark/preview.log) |
| 设置页，软件/GDI | [截图](settings-software/p01-t0.png) | [日志](settings-software/preview.log) |
| 主窗口，浅色 | [截图](main-light/panel-default.png) | [日志](main-light/preview.log) |
| 主窗口，深色 | [截图](main-dark/panel-default.png) | [日志](main-dark/preview.log) |
| 剪贴板，浅色/深色 | [浅色](main-light/clipboard-resized.png)、[深色](main-dark/clipboard-resized.png) | [浅色尺寸检查](main-light/clipboard-sizing.json)、[深色尺寸检查](main-dark/clipboard-sizing.json) |

## 验证边界

- 待 Windows 10 22H2 实机确认：主窗口/设置页的最终桌面外观，尤其原 issue 的 1920×1080、100%、扩展多屏环境。
- 单进程分别设置 DPR 的测试不能代替跨不同 DPI 的物理显示器拖拽。代码处理了 DPI 通知，但这项场景尚未实测。
- 文字像素比较证明遮罩没有引入二次模糊，不等于已经独立复现并消除了原 issue 的全部主观字体清晰度问题。
- 未将非 Qt 的原生菜单/预览宿主统一改造为 alpha 窗口。无 ShaderTools 的构建已实测，自动编译 shader 的可选分支未在本机 kit 实测。
- 此次没有发布、提交或推送。用户原有运行中的 WinShun 未被退出或覆盖。

## 后续修复（2026-11-02）

用户在 Windows 10 上实测新构建后反馈：四个圆角外侧仍有黑色细弧，底边与阴影之间多出一条透明缝。

- **黑弧**：生产窗口先 `create()` 再调用 `styleFramelessWindow`，`prepareFramelessWindow` 因句柄已存在而跳过，窗口没有 alpha。Region 外扩的 2 像素羽化区里透明像素在 DWM 中显示为黑色。上文的验证使用提前设置 alpha 的测试窗口，生产 QML 又只用 `grabWindow()` 检查，因此没有发现。现改为在 `create()` 前取得 alpha；窗口仍无 alpha 时不再外扩 Region、不执行四角擦除。
- **底边缝**：独立阴影向下偏移 2×DPR，挖空区域却按偏移后的形状计算，底边下方一段被挖掉。现改为只按窗口自身形状挖空。
- **回归测试**：`tst_windowmask` 新增 `desktopEdges`，用 `BitBlt` 读取真实桌面合成像素，检查四角最暗像素与底边下第一行阴影，覆盖“create 前准备”和“无 alpha 兜底”两种顺序。临时撤销修复时 4 组（软件/D3D11 × 100%/150%）全部失败，角上像素为 0、底边下为背景原色；恢复后，设置 `WINSHUN_APPEARANCE_LIVE` 时 `tst_windowmask` 8 组全部通过；不设置时（`desktopEdges` 跳过）`tst_windowmask` 8 组、`tst_appearance` 4 组、`tst_darkframe` 共 13 组全部通过。`desktopEdges` 与其他桌面合成检查一样需显式开启。
- `tst_appearance` 在设置 `WINSHUN_APPEARANCE_LIVE` 且使用软件后端时，`light-backdrop`/`dark-backdrop` 两行报告 Mica 原生阴影缺失。按代码路径判断（Windows 11 原生边框分支，本次修改只在 Windows 10 合成分支内）与本次修改无关，未在修改前复跑确认，未处理。
- 仍未在 Windows 10 实机复测；需用户在原环境确认。

### 第二轮：描边外发白（2026-11-02）

Windows 10 复测（上一节的安装包）：黑弧和底边缝已消失，描边外侧仍有一圈浅色。截图中描边外侧像素约为 (213,203,194)，按“描边灰与窗口白底混色后再被擦成半透明”计算为 (212,196,179)，吻合。

- `WindowEdge.qml`：遮罩改为 `z: -1`，先于描边绘制；`WindowMask` 的擦除弧线向内收半个设备像素，描边的抗锯齿边缘落在透明区域上。
- `WindowMask`：D3D11 将场景拉伸到整个像素缓冲，遮罩按控件尺寸计算；软件渲染仍按整设备像素对齐。修正前 D3D11 125% 底部两角漏色 60。
- `desktopEdges` 增加描边并将窗口内部设为纯绿，检查描边外缘 0.5 像素内的 G−B：旧绘制顺序在 150% 下为 32–69 并失败；修正后 8 组通过，不设 `WINSHUN_APPEARANCE_LIVE` 时 13 组通过。
- 安装包：`F:\wsstage\win10-165-20261102b\WinShun-0.17.0-x64-setup.exe`，SHA256 `C41A59949FB46C85BD2C7EA93CE704074CC8A3606F246A37DB63842A731142D6`（未跑全量测试，按用户要求）。

### 第三轮：阴影减淡，直角改走原生（2026-11-02）

用户在 Windows 10 上确认黑弧、底边缝和描边外发白都已消失，但阴影偏浓。参考 VS Code（Electron/Chromium `HWNDMessageHandler` 的 1 像素 `DwmExtendFrameIntoClientArea`）与 Windows Terminal 的资料，用户决定：直角走原生，圆角无法原生则自绘并调整。

- 直角（Windows 10，非分层窗口）：不设 Region，`DWMNCRP_ENABLED`，`DwmExtendFrameIntoClientArea {0,0,1,0}`，隐藏自绘阴影。切换圆角/直角时实时生效。软件渲染（分层窗口）DWM 不绘制框架，直角仍用自绘阴影。
- 圆角：自绘阴影改为形状的高斯模糊（`erfc`，边缘处半强度），峰值 52/255、σ 4、下移 1.5、外延 12 逻辑像素。150% 桌面实测窗口边缘变暗：右 20%→9%，底 20%→13%，上 18%→7%；可见范围约 18→9 像素。参数为观感估计，Windows 10 原生阴影没有公开的精确数值。
- 测试：`shadowLifecycle` 增加圆角→直角→圆角切换，检查自绘阴影显隐、`DWMWA_NCRENDERING_ENABLED` 与窗口区域；13 组常规、8 组桌面合成测试全部通过。本机为 Windows 11 强制兼容路径，DWM 直角阴影的最终观感需在 Windows 10 确认。
- 安装包：`F:\wsstage\win10-165-20261102c\WinShun-0.17.0-x64-setup.exe`，SHA256 `EE649E64BFFE6B05DDFA443D943AEF32F874987B955943D5B972D49DD270CD42`（未跑全量测试）。

### 第四轮：Windows 10 只做直角（2026-11-02）

圆角、直角来回切换后，Windows 10 上四角仍出现蓝色或灰色残留。查阅 Telegram Desktop #16964、QWindowKit #52 后，用户决定 Windows 10 窗口只做直角。

- Windows 10 与 Windows 11 走同一条原生路径，只是不申请圆角：不带 alpha、不设 Region，DWM 画阴影，`WindowEdge.qml` 画 1 像素直角边。“圆角”设置在 Windows 10 上只影响窗口内控件。
- 删除 `WindowMask`、`FrameShadow_p.h`、`prepareFramelessWindow`、`tst_windowmask` 和遮罩着色器（本地备份，不入库）。
- 测试：`tst_foldermenu`、`tst_darkframe`、`tst_appearance`（4 种缩放）共 6 组通过。未在 Windows 10 实机复测。
