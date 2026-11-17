# 0.34.0 发布验证记录（2026-11-17）

内容：#357 字母直达换键换字母 + `Shift`+字母点按钮、#363 管理员身份更新后回到管理员、#364 Total Commander 文件夹大小 + 遍历磁盘收录目录链接 + 链接诊断、#365 弹出动画开关、贴图悬停关闭按钮、下拉菜单可拖滚动条、匿名用户数统计和拾穗计划用户数、内存报告工具（不随包分发）。发布前本地 main 变基到 origin/main（重复的资源管理器标签 1.1.4 提交自动去掉）。

## 测试

- `scripts/release.ps1 -Tests '^(tst_numberhints|tst_pinwindow|tst_updater|tst_settings|tst_aisettings)$'`：5 个全过。
- 各项功能在提交前的实测见对应提交说明（#357、#364 在 Win10 虚拟机实测，截图在 `docs/validation/issue-357-20261117`、`issue-364-20261117`）。

## 安装和升级

- `tools/verify-installer.ps1`（打包前自动运行）：5 项全过。
- `tools/verify-installed-upgrade.ps1 -Mode InApp`（经用户同意）：本机 Program Files 的正式版 0.33.0 经应用内更新真实升级到 0.34.0，49.6 秒；新版本以普通权限启动，索引服务在运行（同一路径），开机自启任务和搜索历史（SHA256 不变）保留；设置文件多了新版的默认项。

## 没有覆盖的

- 以管理员身份运行的 0.34.0 再升级（#363 的新路径）要等下一个版本才能真实走一遍；这次从 0.33.0 升上来按设计还是普通权限。
- 没有在干净的 Windows 10 虚拟机里试首次启动；Wizard、Portable 升级方式这次没跑（没有改安装程序和换文件的助手）。
- 没有补拍海报：改动的界面（字母直达设置、贴图悬停按钮、下拉滚动条、弹出动画开关）不在现有海报里，现有海报内容不受影响。

## 插件

- 对比基准 `59524f2b`（0.33.0 插件源码）：截图与录屏升 1.1.20（PinWindow/PinTools、共用的 InputBox）；图片文字搜索升 1.0.19（共用的 InputBox 与 wscore）。
- 翻译、云备份只因 `Http.cpp` 新增 `post()`，原有 GET 路径不变，不升版不重发。资源管理器标签 1.1.4 已在 0.33.0 之后单独发布。其余没有变化。

## 打包

`scripts/release.ps1 -BuildDir build\release0340 -OutputDir dist\release0340`，从 `dac3cad2` 加版本号改动构建（之后只改了文档和本记录）。

| 文件 | SHA256 |
|---|---|
| WinShun-0.34.0-x64-setup.exe | fc8fdb43785af60da21fd03eda4d65610f1a3d7b49d41d8916dfce7eddf5cd54 |
| WinShun-0.34.0-x64-portable.zip | 48a7220565291e7f79cfe87c177d0400f12c144bb913cb5a74ab97bc7f206294 |
| WinShun-0.34.0-x64-offline-setup.exe | 812734254d617e4d8ff3a2eaf7380b6359f74fe75d116a0a37150c1dd2575322 |
| WinShun-ocr-ppocr-v6-ort1.30.0-x64.zip | ccb82d77a545f183504edc51e6353370742d0423b71fa1cf7800db919494277e |
| winshun.capture-1.1.20.wsplugin | 7f068175c6db258a1578491df2140786f37661a6e480b3d4fc7dfa734b5ce391 |
| winshun.ocr-search-1.0.19.wsplugin | 3f2b570bd5a6cc534233d6ede2083e2a542e4c8295ac0bf4271d43e32c672d78 |
