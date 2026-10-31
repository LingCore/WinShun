# #149–#151 修复与验证（2026-10-30）

对应 [#149](https://github.com/LingCore/WinShun/issues/149)、[#150](https://github.com/LingCore/WinShun/issues/150)、[#151](https://github.com/LingCore/WinShun/issues/151)。基于 `20b9af9` 的当前本地工作树，保留已有及并行修改；本次没有提交、推送或发布版本。

## 修复范围

| 问题 | 实现与交互 |
| --- | --- |
| 录屏停在 00:00、停止后无响应 | `RecordingWorker` 将 WGC、Media Foundation 和 GIF 转换放到独立进程，使用有界 JSON 行传递控制、输入与进度。连续 10 秒无进展或停止后 10 秒仍未结束会结束工作进程；暂停不累计无进展超时。硬件初始化失败或第一秒内卡住时尝试一次软件编码，界面显示切换状态。只有收到视频帧才显示正在录制。 |
| 收藏圆盘用不了 | 普通模式保留原路径；管理员模式由普通权限的 `DropPieHelper` 承担 Raw Input 和 OLE 拖放。两个设置入口都可用。辅助进程只取得文件夹／系统位置收藏及外观快照，收藏变更同步更新；启动失败显示通知。OLE 注册失败后清除失败状态，允许下次重试。 |
| 搜索分隔线难拖、滚动条挡住缩放边缘 | 分隔线常驻可见，悬停强调，命中宽度为 16 逻辑像素。滚动条按 `WindowFrame` 的系统边框尺寸内移，滚动和窗口缩放分别命中 `HTCLIENT`、`HTRIGHT`。 |
| #149 附带的文件夹大小建议 | `FolderSizes` 在后台汇总已有索引，以“≈”显示估算值，与文件使用同一大小格式。未知文件大小不按零处理；旧查询按代次取消。不扫描磁盘或下载云文件。估算不包含未索引内容，只用于显示，文件夹排序契约不变。 |

采集和编解码调用留在子进程，界面投递控制请求。录制暂存文件使用本次 UUID，GIF 使用单独的暂存目录；GIF 转换失败保留原始 MP4，中途失败尽量保留已经产生的媒体。子进程沿用宿主 Qt、字体和语言，错误中英文与界面保持一致。

## 环境与自动回归

- Windows 11 专业版，build 26200；NVIDIA RTX 4060 Laptop，驱动 `32.0.15.9579`。
- MSVC 14.44、C++20、Qt 6.12.0、Release；实际界面使用 D3D11。
- 构建通过：`WinShun`、`WinShunCapture`、`WinShunCaptureApp` 及相关测试目标。
- 8 个套件全部通过：`tst_panelcolumns`、`tst_recording`、`tst_keycaps`、`tst_settings`、`tst_capturesettings`、`tst_piegeometry`、`tst_pielabels`、`tst_favorites`。该轮耗时 26.24 秒。工作进程语言传递更新后，`tst_recording` 再次通过，耗时 17.00 秒。

新增回归覆盖：多层索引大小、未知大小、删除、空目录和取消；真实子进程编码与独立解码、奇数区域、音轨、GIF 导出、硬件失败后的软件回退，以及不响应子进程的限时结束和取消。故障注入用于验证监督边界，不等同于复现 AMD 驱动故障。

在 MSVC 开发环境中复跑：

```powershell
ctest --preset release -R '^(tst_panelcolumns|tst_recording|tst_keycaps|tst_settings|tst_capturesettings|tst_piegeometry|tst_pielabels|tst_favorites)$' --output-on-failure
```

## 实际程序验证

使用独立部署目录和临时配置，没有替换安装中的程序、退出用户窗口或改写用户数据。主要证据保存在 [本地验证目录](../build/issue149-151-validation-20261030/)。

- **搜索面板**：100% 亮色、150% 深色。真实 `Panel` / `Launcher` / `ResultModel` 检查列宽拖动、顺序、持久化、文件夹估算，以及溢出列表的滚动条和原生缩放命中；预览均为 0 个警告。文件夹测试明确要求结果包含目录，避免空循环通过。
- **圆盘辅助进程**：真实进程报告普通权限，实际注册 OLE 目标；收藏由 2 个更新为 3 个，外观更新后保持可用；关闭后确认进程退出。最终 `drop-pie-helper.json` 的 `passed`、`updatedFavorites`、`stopped` 均为 `true`。
- **两个设置入口**：收藏夹中文亮色、插件页英文深色 150%，完成页面与设置搜索验证；最终两次预览均为 0 个警告。补测发现的临时目录监听警告已通过将静态预览的清理推迟到 QML 引擎析构后修复，清理路径只允许本次临时预览配置。
- **MP4**：真实 Capture → 框选 → Recorder → WGC → 工作进程 → 保存，包含系统声音、暂停／继续、再录一段。独立解码首段为 800×460、30 fps、107 帧、3583 ms，具有音轨；两段文件均保留。
- **GIF**：英文深色 150%，开启点击高亮选项，完成相同的暂停、保存和再录流程。Pillow 逐帧读取两段 1200×690 的 GIF，分别为 55 帧／3670 ms、19 帧／1270 ms，循环标记均为 0。该项证明选项和生成链路，不据此声称验证了真实鼠标输入高亮。
- **错误语言**：直接运行实际插件工作入口，输入无效区域，确认返回英文错误及正常退出；新增字符串经 `lupdate` 提取核对，无缺失翻译。

复跑界面检查应使用带 Qt 运行库的独立部署目录：

```powershell
WinShun.exe --preview-results <输出目录> --file-metadata --drop-pie-check --scale 1.5 --dark --renderer d3d11
WinShunCapture.exe --preview-recording <输出目录> --app-dir <宿主运行库目录> --auto --no-countdown --system-audio --renderer d3d11
WinShunCapture.exe --preview-recording <输出目录> --app-dir <宿主运行库目录> --auto --gif --clicks --english --dark --scale 1.5 --renderer d3d11
```

## 证据边界

#151 的诊断包显示 AMD RX 7900 GRE、Windows build 26300、管理员运行，但没有可定位具体录屏调用的阶段记录。此次没有该 AMD 环境，不能断言已经复现其驱动故障；新增阶段、编码器和帧数日志用于后续判断。

#150 在 2026-10-30 尚未给出操作步骤和运行权限。已修复两个代码中可确认的问题，并验证普通权限辅助进程的实际注册、同步和退出；该轮测试父进程是普通权限，没有通过 UAC 实测提权父进程，也没有用真实资源管理器按键拖放复现报告者的原始场景。2026-10-31 已收到补充反馈并补测普通权限路径，见下节；原机原因仍未确定。

## #150 新反馈复核（2026-10-31）

[报告者的新回复](https://github.com/LingCore/WinShun/issues/150#issuecomment-6542380454) 明确：本人和朋友都没有以管理员身份运行，按前述操作仍完全不出现圆盘。这排除了“仅管理员运行时被阻挡”作为这次报告的充分解释，不能把之前两项修复当作报告者问题已解决的证据。

本次只补充诊断，没有宣称修复原机故障：

- `KeyListener` 在普通诊断日志中记录原始鼠标监听注册失败、普通触发键注册失败、拖动启动输入未完整发送、按键时没有文件拖动，以及文件列表未在 300 ms 内建立拖动。热键冲突每次主键按下最多记录一次；不需要开启记录所有按键的 `WINSHUN_KEYLOG`。
- 导出诊断包的 `summary.json` 增加圆盘是否启用、使用本进程还是辅助进程、本进程 OLE 目标与监听状态、键盘 Raw Input 是否仍归当前监听器，以及已有辅助进程状态。新增字段和日志不包含文件名、窗口标题或输入正文。
- 保留现有触发和拖放行为；尚未推送、发布或回复 issue。

验证使用本次专属目录 `build/issue150-20261031/`、临时收藏及文件、独立进程和新开的资源管理器窗口；结束前先按 Esc 取消拖动再松开鼠标，只关闭测试自己新开的窗口。没有移动用户文件或退出用户程序。

| 路径 | 结果 | 证据 |
| --- | --- | --- |
| 原生 Windows Shell 文件视图 → 真实 `KeyListener` | 按住不动、已拖动、移动后立即按键，3/3 收到触发 | `baseline.log`、`probe.cpp` |
| 真实资源管理器 → 普通权限圆盘辅助进程 | 三种操作 3/3 接收真实文件数据并显示圆盘窗口，辅助进程正常退出 | `real-explorer-final.log` |
| 真实资源管理器 → `--probe-runtime` 的真实 `App::start` | 三种操作 3/3 接收文件并显示圆盘；进程退出码 0、Qt 警告 0 | `real-app-runtime.log`、`profile/runtime/runtime.json` |
| 构建与自动回归 | `WinShun` 构建通过；`tst_keylistener`、`tst_piegeometry`、`tst_pielabels`、`tst_diagnostics` 通过 | `build.log`、`build-final.log`、`tests.log`、`diagnostics-tests.log` |

完整 App 启动测试使用已有的临时配置与 HKCU 重定向，关闭剪贴板监听、服务和无关全局快捷键；该配置没有收藏，显示的是圆盘的空收藏提示。辅助进程测试另用一个真实临时目录收藏。输入通过 Windows `SendInput` 发送，真实鼠标、触摸板及其他文件管理器仍未覆盖。本机通过不代表报告者原机已修复。

后续需取得报告者的 Win顺 版本、操作来源（桌面／资源管理器／其他文件管理器）、鼠标或触摸板，以及失败后 **设置 → 高级 → 诊断日志 → 导出诊断日志** 得到的诊断包。旧版导出的包可确认环境；本节新增的圆盘状态与失败记录需要包含这些改动的测试版或后续新版。issue 继续保持未解决。
