# 0.49.0 验证记录

正常推送（`-Push Normal`）。发版时 GitHub 网页登录不了，全程用本机 `gh` 保存的令牌完成（做法见 `docs/releasing.md` 的“GitHub 网页登录不了时”）。

版本内容（0.48.0 之后并入 main 的提交）：

- 本地模型插件、翻译 2.3.0 的本地服务与流式结果、按意思找 1.1.0 的本地嵌入（分支 `local-model`，验证见 `docs/validation/local-model`）。
- 截图“复制为表格”认合并单元格（分支 `table-ocr`，验证见 `docs/validation/table-ocr`）。
- 钉住的搜索框让位、一直钉住、已打开标记（分支 `pinned-stand-back`）；预览放大和钉住提示（9e5fa6ef）。
- 结果行按钮可选可排（分支 `row-actions`，4493554f）。
- 读共享的宽度按实测、每小时核对等人离开（分支 `share-width`，验证见 `docs/validation/bugs-1126`）。
- 独占全屏游戏上的剪贴板（#452，分支 `issue-452-win10`）。
- 开着标注工具时双击和鼠标键动作照常执行（08184d2d）。
- 已在 main 上的：类型标签拖动换序、插件命令快捷键（#466）、跨文件夹选择的系统菜单（#464）、内存（#467）、缩略图处理程序、剪贴板输入法与保留重复项、排除映射盘和整个共享、更新时等安全软件放开安装包、文件对话框焦点。

## 发版中发现并修的（82963b6f）

为新功能出海报时看实际出图发现两处，原分支的检查只比对了位置和 `visible`，没有看图：

- **“结果行按钮”里“复制路径”没有图标**。这个自绘图标原来用 `Shape` 的曲线渲染器画，在设置窗口里一笔都不画（对照：同一位置用普通渲染器画的形状能显示，曲线渲染器的全部不显示；100%、150%、200% 都一样）。改由 `PhosphorGlyph` 按原来的 256 网格、16 线宽给出轮廓并绘制，和字体图标同一套栅格化。
- **换了按钮组合后，结果行上大部分按钮不画**。`--shortcut-panel` 的 `row-buttons-chosen.png` 里选了五个按钮只画出一个；元素树里五个的位置和可见性都对。原因是换顺序时把按钮摘下再挂回 `Row` 末尾。改为按次序计算每个按钮的位置，不重挂。
- 修后 `--shortcut-panel` 跑两次，默认六个、去掉删除的四个、另选的五个都按顺序画出；`--preview-settings` 新增的 `search-结果行按钮.png` 里九个按钮都有图标。

## 自动测试

合并后全量构建（全部目标）通过。只跑了这次改动涉及的测试（`build\release`）：

- 第一轮 56 项：`tst_annotmodel`、`tst_capture`、`tst_capturesettings`、`tst_captureui`、`tst_settings*`（含各设置探针）、`tst_aisettings`、`tst_pinnedstacking`、`tst_core`、`tst_encoding`、`tst_localmodel`、`tst_translate`、`tst_semantic`、`tst_ocr*`、`tst_plugin*`、`tst_previewplacement`、`tst_clipimage`、`tst_commandicons`、`tst_shellcontext`、`tst_updater`、`tst_idletrim`、`tst_scoopinstall`。54 项通过；`tst_settingsreopen`、`tst_settingsopenlink` 失败，重跑通过。失败时真实鼠标指针停在探针窗口的分类栏上，被指着的页面按设计会保留，检查因此不过；探针现在会打印当时创建了哪些页面、指针在哪一项上（eb891e52）。
- 修图标和按钮排列后第二轮 19 项：`tst_appearance`（四档）、`tst_typestrip*`、`tst_previewinteractions`、`tst_scopememory`、`tst_switchprovider_ui`、`tst_settings`、`tst_aisettings`、`tst_capture`、`tst_captureui`、`tst_annotmodel`、`tst_encoding`、`tst_settingsreopen`、`tst_settingsopenlink`、`tst_settingslazy`、`tst_pinnedstacking`、`tst_shortcutpanel`。18 项通过。
- **`tst_shortcutpanel` 不过，没有修**：115 项检查里两项失败，`scope hint follows nonblank input` 和 `clearHistory iconSize matches info button (16 px)`。用 0.46.0 时的构建跑同一个探针，第一项同样失败；0.48.0 之后的构建两项都失败（第二项是图标按容器排版后断言里的 16 没跟着改）。与这一版的改动无关，留待单独处理。

`tools/verify-installer.ps1`：6 项全过（打包时自动运行）。

## 安装和升级

`tools/verify-installed-upgrade.ps1 -Mode InApp`：本机装的 0.48.0 通过软件内更新升到 0.49.0，用时 49.6 秒，用的是增量包 `WinShun-0.49.0-x64-delta-from-0.48.0.zip`。更新后服务在运行，没有提升权限，装好的 `WinShun.exe`（16e888ee…）和打包目录里的相同。结果见 `inapp-upgrade.json`。

增量包三个（从 0.48.0、0.47.0、0.46.0），脚本各自还原比对过，约 16 MB，安装包 42 MB。

## 海报

`tools/screenshots/release049.py`：`row-buttons.jpg`（新），标题“结果行按钮，自己选自己排”，放在 `docs/截图.md` 的“搜文件”一组。其他海报没有重拍。

## 插件库

比对了各插件自 0.48.0 以来编译进去的文件（`ninja -t inputs` 加头文件依赖）：

| 插件 | 版本 | 说明 |
|---|---|---|
| 本地模型 winshun.localmodel | 1.0.0 | 新插件，要 Win顺 0.49.0 |
| 翻译 winshun.translate | 2.3.0 | 本地模型、本机服务、流式结果 |
| 按意思找 winshun.semantic | 1.1.0 | 本地嵌入模型 |
| 截图与录屏 winshun.capture | 1.1.39 | 复制为表格、开着工具时的双击和鼠标键、#467 的内存；安装包附带的是同一份源码 |
| 开发工具 winshun.devtools | 1.0.9 | 编译进去的共用文件变了（`IdleTrim`、`WindowEffects`、共享与剪贴板代码、图标绘制） |
| 鼠标手势 winshun.gestures | 2.1.7 | 同上 |
| 图片文字搜索 winshun.ocr-search | 1.0.34 | 同上，另有识别代码（分条找文字行） |

其余插件编译进去的文件没有变化，不发。

## 没有覆盖的

- 本地模型：HY-MT 模型的下载、AMD 和 Intel 显卡、自己的嵌入模型、只有 CPU 时建索引的速度、两个模型同时加载（见 `docs/validation/local-model`）。
- 复制为表格：没有真的往 Excel 里按 `Ctrl+V`（会动 Windows 的剪贴板历史），HTML 是用隐藏的 Excel 打开核对的；WPS、Word、拍的照片没有试。
- 钉住让位：没有用 Word 等真实程序走一遍，探针里用的是自建窗口。
- 读共享：真实 NAS、局域网上的 Windows 服务器、无线和 VPN 没有试；“等人离开再核对”只测了判断函数。
- 结果行按钮：设置里拖动换顺序没有用真实鼠标拖过；修好的两处是离屏出图核对的，没有在屏幕上的真实窗口里看。
- 开着标注工具时的双击：只有 `tst_annotmodel`、`tst_capture`，没有在真实截图里操作。
- 品牌、质感两套风格下的新界面没有单独渲染。
