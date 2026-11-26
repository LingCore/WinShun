# 反馈页 5 个 Bug（2026-11-26）

来源：网页反馈（匿名，无法回复）。在 `main`（461c353e 之后）上修改，Release 构建，本机 Windows 11 26200、150% 缩放。

| 反馈 | 结论 | 改动 | 验证 |
|---|---|---|---|
| 已排除的挂载网络硬盘仍能搜到（09824d2a） | 已修 | 排除目录里按映射盘符写的路径换算成共享的 UNC 路径；整个共享被排除时不再读取；网络路径不再调 `GetLongPathNameW`；设置页共享卡片说明原因 | `tst_settings::excludedOnMappedDrive`（`HKCU\Network` 假映射）、`tst_core::uncExcludedPath` 及共享/排除相关 33 项通过；卡片见 `share-excluded-card.png`（浅色 100%，另看过深色 150% 与英文） |
| `.cdr` 没有缩略图（9425e6df） | 已修 | 类型表之外，本机注册了缩略图处理程序的类型也显示缩略图（文档、音频、快捷方式等除外） | `tst_commandicons::thumbnailsOfTypesWithAHandler`：临时注册假扩展名，对改名的 PNG 实取到缩略图并核对颜色。本机没装 CorelDRAW，未用真 `.cdr` 文件验证 |
| 剪贴板搜索框打不了中文（7178733c） | 已修 | 点一下搜索框，窗口接过焦点，输入法正常组字；打开时仍不抢焦点。单按 `Shift` 的提示在底部说明这一点；框内提示语原先在默认宽度下被截断，缩短 | `WinShun.exe --probe-runtime <目录> --field-focus-check`（`WINSHUN_TEST_NO_CLIP_HISTORY=1`）：真实点击后窗口到前台、不再转发按键，`KEYEVENTF_UNICODE` 打入“输入法”成为搜索词并筛出 1 条，`Enter` 把该条粘贴进另一进程的文本框，系统剪贴板原样还原。结果 `field-focus.json`，截图 `clipboard-hint.png`、`clipboard-typed-after-click.png`。未用真实输入法（双拼、五笔）逐个验证 |
| Ctrl+G 跳转后文件要点多次才能打开（f9ace9c1） | 未复现；修了一处相关缺陷并补诊断 | 用标准“打开”对话框 + 资源管理器，对运行中的 0.48.0 触发 Ctrl+G 后立即 / 150 ms / 400 ms / 约 3 s 真实双击文件，4 次均一次打开（脚本在 `ctrlg-harness/`）。代码里确有一处会影响点击：跳转等待期间（慢文件夹最长 1.5 s）用户已点进文件列表，结束时焦点被抢回文件名框，已改为只归还还留在地址栏里的焦点。每次跳转记诊断 `dialog-move`，下次有诊断包可直接看 | `tst_shelllaunch::realDialogFocusAfterGoingToAFolder` 两种情况通过；把条件临时改回旧写法，第二种情况失败（“the focus was taken back from where the user had put it”） |
| 打开网络共享文件夹时文件管理器容易卡死（736d4fd2） | 未复现（没有慢 NAS）；按最可能的原因处理并补诊断 | Win顺 没有装进资源管理器的组件。自动核对共享时原来同时读 8 个文件夹，小 NAS 这时回应同一个人的资源管理器会很慢；改为有人在用电脑时自动核对只同时读 2 个，首次收录、“立即核对”、无人时不变。每遍核对记诊断 `share-pass` | `tst_core::sharePassThreads` 与共享相关用例通过。没有在真实 NAS 上测量卡顿前后的差别 |

相关测试：`tst_settings`、`tst_commandicons`、`tst_aisettings`、`tst_encoding` 全过；`tst_core` 中共享与排除相关的 33 个用例全过；`tst_shelllaunch` 新用例通过，另有 `appsFolderConsole`（2 行）在未改动的构建上同样失败，与本次无关。
