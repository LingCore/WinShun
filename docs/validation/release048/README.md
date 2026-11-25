# 0.48.0 验证记录

发布提交 4bc7da47，正常推送（`-Push Normal`）。版本内容（0.47.0 之后并入 main 的提交）：

- 剪贴板历史备份与还原（d24df87d），实机验证见 `docs/validation/clip-backup`。
- “文件”的类型标签显示数量（ff8914b0）。
- 增强文字识别的较大识别模型（读日文）和“用自己下载的模型”（58dd7143、cd58c4c4）。
- 收藏圆盘在文件夹上停一下展开子文件夹（#463，ff4ebbb7），证据见 `docs/validation/issue-463-drop-pie-columns`。
- 录屏控制条按住空白处拖动、设置里的“录屏保存文件夹”（4b32d572）。
- 插件“使用说明 / 更新内容”对话框重排、拖滚动条后空白和滚动抖动的修复（fe499eab）。
- 选项标签改小圆角（f78c4041）；全部图标按容器排版（8a929075）。
- `wsbench --app-dir`（#462，f71b2fec）。
- 发版中修的：`tst_capturesettings` 链接不过（97b7bc22）。合并 4b32d572 之后 `CaptureSettings.cpp` 用到了 `Recorder::defaultFolder`，而这个测试不带录屏器；默认文件夹的名字挪到 `RecordingFolder.h`。

## 图标按容器排版是怎么查的

- 改之前实测了旧的 em：正常字体 DPI（96）下 `size` 10/12/13/14/15/16/20/24 的图标 em 是 11/13/15/16/17/18/23/27；离屏预览不设 `QT_FONT_DPI=96` 时是 17/20/22/24/25/27/34/41（大 1.5 倍）。此前类型标签那几轮预览图就是在后一种情况下出的，图标偏大。
- 新尺寸取最接近旧 em 的偶数容器（16→18、14→16、12→14、10→12、20→22、24→28），所以图标的视觉大小基本不变；`Glyph` 的 em 现在等于 `size`，不再随字体 DPI 变。
- 144 处用法里，居中的只换尺寸；约 75 处非居中的逐处看过：保留原有边距数值（现在是到容器的距离），去掉了只为给笔画留位置而设的 `width`/`height`，写死旧尺寸的算式改了（文件夹菜单、筛选项、设置导航等）。
- `tst_appearance`（100%/125%/150%/200%）、`tst_typestripcounts`（100%–175%，检查容器是偶数方格、到左和到上相等、到文字 4）、`tst_previewinteractions`、`tst_scopememory`、`tst_switchprovider_ui` 通过。
- 离屏渲染后看过图（150%，`QT_FONT_DPI=96`）：类型标签四档缩放、结果窗口、设置窗口全部页面（58 张）里的剪贴板 › 保留页。
- **没有覆盖的**：截图插件工具条、手势编辑器、开发工具、图片文字搜索窗口、拖放圆盘、文件夹菜单、右键菜单、剪贴板面板没有逐张出图检查，只靠编译通过和尺寸换算；品牌、质感两套风格没有单独渲染。

## 自动测试

只跑了这次改动涉及的测试，打包时（`build\release`）：`tst_encoding`、`tst_appearance`（四档）、`tst_typestripcounts`（四档）、`tst_previewinteractions`（两档）、`tst_scopememory`、`tst_clipbackup`、`tst_core`、`tst_ocrpackage`、`tst_piegeometry`、`tst_capturesettings`、`tst_captureui`、`tst_backup`、`tst_settings`、`tst_aisettings`，共 28 项。第一次 27 项通过，`tst_encoding` 失败：压缩包搜索的 `Lister.cpp` 用 `GetACP` / `GetOEMCP` 读压缩包里非 UTF-8 的文件名，是有意的，但 0.47.0 加这个插件时没登记到 `kCodePageUsers`（54661762 补上）。补上后重新打包，`tst_encoding`、`tst_capturesettings`、`tst_clipbackup`、`tst_typestripcounts` 全部通过。

打包出来的程序（`distelease048`）上另跑了 `--preview-results --drop-pie-columns`（150%）：`drop pie columns: passed`。

`tools/verify-installer.ps1`：6 项全过（打包时自动运行）。

## 没有覆盖的

- 较大识别模型从 ModelScope 的真实下载（设置里的按钮）：测试用本地服务，`--ocr-precise` 用本地文件。构建配置时从同一个固定地址按大小和 SHA256 取到了这个文件，地址和校验值是对的。
- “用自己下载的模型…”的文件对话框（探针直接把路径交给 `OcrModel::useOwnFile`）。
- 截图插件、图片文字搜索用新模型识别；换模型后图片文字搜索不会重新索引旧图片。
- 类型标签数量没有在真实索引的运行程序里看过，只有单元测试和离屏渲染。
- 剪贴板分组、内容的文件夹和种类、筛选项的小圆角在品牌、质感风格下没有渲染检查。

## 安装和升级

`tools/verify-installed-upgrade.ps1 -Mode InApp`（`distelease048`）：本机装的 0.47.0 通过软件内更新升到 0.48.0，用时 50.5 秒。更新后服务在运行，没有提升权限；开机任务、设置和历史都保留，历史的 SHA256 没变，装好的 `WinShun.exe`（9370fc41…）和打包目录里的相同。结果见 `inapp-upgrade.json`。

增量包这次也没用上，下载的是完整安装包（日志：`changed here: library-plugins/winshun.capture/WinShunCaptureApp.dll`）。原因和 0.47.0 那次一样：本机装的 0.47.0 是发版前那次打包，附带的截图插件和正式发布的不是同一次编译。增量包是脚本拿 GitHub 上正式发布的 0.47.0、0.46.0、0.45.0 便携包生成的，并各自还原比对过，所以从正式版本升级的用户会用到增量包。

## 海报

`tools/screenshots/release048.py` 调用 `WinShun.exe --preview-settings <dir>`（离屏，2 倍，`QT_FONT_DPI=96`），取其中剪贴板 › 保留一页（`p07-t2.png`）。

- `clipboard-backup.jpg`（新）：标题“剪贴板历史，备份带着走”，放在 `docs/截图.md` 的剪贴板那一组。
- 其他海报没有重拍：类型标签的小圆角和数量、收藏圆盘的子文件夹列在现有海报里还是旧样子。

## 插件库

比对了各插件已发布版本（0.47.0 的 136fe829）之后编译进去的文件（`ninja -t inputs` 加头文件依赖）。以下插件都从 4bc7da47 构建：

| 插件 | 版本 | 说明 |
|---|---|---|
| 截图与录屏 winshun.capture | 1.1.38 | 录屏控制条拖动、录屏保存文件夹、新的识别模型代码、图标按容器排版；安装包附带的是同一份 |
| 开发工具 winshun.devtools | 1.0.8 | 编译进去的 `Glyph.qml`、主题和搜索代码变了；图标排版随之更新 |
| 鼠标手势 winshun.gestures | 2.1.6 | 同上 |
| 图片文字搜索 winshun.ocr-search | 1.0.33 | 同上，另有 `PaddleOcr.cpp`、`OcrFiles.cpp`：装了较大模型或自己的模型时用它识别 |

其余插件（压缩包搜索、书签、休息提醒、云备份、文本扩展、资源管理器标签、局域网传送、语义搜索、同步、窗口置顶、翻译、网页搜索）编译进去的文件没有变化，不发。
