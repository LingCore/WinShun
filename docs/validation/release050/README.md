# 0.50.0 验证记录

正常推送（`-Push Normal`）。GitHub 网页登录仍不可用，全程用本机 `gh` 保存的令牌（`docs/releasing.md` 的“GitHub 网页登录不了时”）。

版本内容（0.49.0 之后并入 main 的提交）：

- #471 内存紧张时不主动归还、呼出时预读文件名索引、诊断（6f40a6ad，验证见 `docs/validation/issue471-memory-short`）。
- 公式识别（分支 `formula-ocr`，验证见 `docs/validation/formula-ocr`）。
- 类型扩展名填 `*`（分支 `type-all-files`，验证见 `docs/validation/type-every-file`）。
- 选中行与卡片的对比（#474，分支 `row-contrast`）；按键提示用菜单字号（分支 `keyhint-type`）。
- 已在 main 上的：#472 #436 鼠标手势，#458 按意思找，#432 Photoshop，#460 链接文件夹，#121 反作弊游戏，#414 剪贴板大图，#480 #479 #478，#454 文件夹窗口里的子路径，#477 电源命令归入系统开关，#481 项目约定，#469 #470；窗口外框和画廊探针的两处修正（ff99c991、4878a935）。

## 自动测试

合并后全量构建（全部目标）通过；最后并入 `type-all-files` 后再增量构建通过。只跑了这次改动涉及的 32 项（`build\release`），全部通过，共 246 秒：

`tst_idletrim`、`tst_core`、`tst_hotpages`、`tst_foregroundmemory`、`tst_diagnostics`、`tst_settings`、`tst_typestripcounts`、`tst_appearance_1`／`1.25`／`1.5`／`2`、`tst_pinwindow`、`tst_keycaps`、`tst_formula`、`tst_formulareader`、`tst_capturekeys`、`tst_capturesettings`、`tst_settingscapturehotkey`、`tst_ocrpackage`、`tst_ocr`、`tst_gestures`、`tst_semantic`、`tst_archive`、`tst_paster`、`tst_clipboardpictureview`、`tst_clipboarddisplay`、`tst_launchernavigation`、`tst_searchnavigation`、`tst_switchplugin`、`tst_encoding`、`tst_mojibake`、`tst_captureui_d3d11_1.5`。

`tools/verify-installer.ps1`：6 项全过（打包时自动运行）。

## 安装和升级

`tools/verify-installed-upgrade.ps1 -Mode InApp`：本机装的 0.49.0 通过软件内更新升到 0.50.0，用时 50.6 秒，用的是增量包 `WinShun-0.50.0-x64-delta-from-0.49.0.zip`（9.0 MB）。更新后服务在运行，没有提升权限，设置文件大小和剪贴板历史的校验值不变，装好的 `WinShun.exe`（06b0d796…）和打包目录里的相同。结果见 [inapp-upgrade.json](inapp-upgrade.json)。

增量包三个（从 0.49.0、0.48.0、0.47.0），脚本各自还原比对过，9.0–17.4 MB；安装包 42.6 MB。

## 海报

`tools/screenshots/release050.py`：`formula.jpg`（新），标题“公式识别，一键转 LaTeX”，放在 `docs/截图.md` 的“截图、录屏和贴图”一组。用的是打包目录里截图插件的 `--preview-formula`，真实模型识别二次公式（把握度 1.000），各复制格式的核对都通过，0 条警告。其他海报没有重拍。

## 插件库

比对了各插件自 0.49.0 以来编译进去的文件（`ninja -t inputs` 加头文件依赖，对照 `git diff v0.49.0`）：

| 插件 | 版本 | 说明 |
|---|---|---|
| 截图与录屏 winshun.capture | 1.1.40 | 公式识别、按键提示字号、#469、#474、#471；安装包附带的是同一份源码 |
| 鼠标手势 winshun.gestures | 2.2.0 | #472 按住键画手势、#436 组合手势等第二个键、#471 |
| 按意思找 winshun.semantic | 1.1.1 | #458、#478、#471 |
| 压缩包搜索 winshun.archive | 1.0.1 | #478、#471 |
| 开发工具 winshun.devtools | 1.0.10 | 编译进去的共用文件变了（`IdleTrim`、窗口外框、索引与搜索代码、主题） |
| 图片文字搜索 winshun.ocr-search | 1.0.35 | 同上，另有识别运行库代码 |

其余插件编译进去的文件没有变化，不发。系统开关 1.1.0（#477）随程序附带，不经插件库。

## 没有覆盖的

- #471：反馈者那种秒级的卡顿（慢盘、磁盘争用）本机没有复现，只测了硬缺页次数和本机 NVMe 上的时间（见 `docs/validation/issue471-memory-short`）；界面线程那 2.6 秒里内存以外的部分没有找到。
- 公式识别：没有在运行中的 Win顺 里用真实键盘按 `Shift+F`，也没有在贴图右键菜单上实点；WPS、OneNote 的粘贴没有试。
- 类型扩展名 `*`：没有替换运行中的 Win顺 实测，靠单元测试和离屏预览。
- 鼠标手势带键手势、组合手势等待：单元测试覆盖，没有在真实程序里画。
- 压缩包搜索、按意思找在 Windows 10 上的归还内存：没有在 Windows 10 上实跑。
