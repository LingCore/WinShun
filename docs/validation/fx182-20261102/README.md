# #182 与 #82 余下部分验证

验证日期：2026-11-02。Windows 11 专业版 10.0.26200、Qt 6.12.0、MSVC 2022 Release、D3D11、150% 缩放。

## #182 文件夹内搜索项移动时同步位置

- 报告者截图里，输入的是 `...\Downloads\fpga`：第一条是 Downloads 的直接子项（已同步选中），其余是子文件夹和其他位置的结果。它们不在原目录里，同步就要让标签页切换目录，所以一直没有同步。
- 资源管理器 Win11 多标签页：输入所在的标签页总是窗口里排在最前（Z 序第一）的 `ShellTabWindowClass`，切换标签后会跟着变（用真实窗口读到）。所以“只认第一个标签页”并不会漏掉第 2 个标签页，代码没有改。
- 改动：结果不在原文件夹的直接子项时，底部第二个提示从“打开位置”改为“Ctrl+Enter 在原窗口定位”。按下后原标签页转到该文件夹并选中该文件（`App::revealInTypingTab`，沿用 #58 的同标签页逻辑）。
- `--probe-runtime --folder-typing` 在真实 `App`、真实 QML、新建的资源管理器窗口里通过：第 2 个标签页（用 UI Automation 点“+”打开）里首项选中、Down 逐项跟随、直接子项提示仍为“打开位置”、子文件夹结果提示为“在原窗口定位”、移到子文件夹结果时第 2 个标签页位置不变、Ctrl+Enter 后第 2 个标签页进入子文件夹并选中该文件（用 Shell 的 `IFolderView2` 读取）。
- 保留 `SVSI_DESELECTOTHERS` 和 60 ms 合并定时器。

## #82 Directory Opus 直接打字搜索

- 新设置“在 Directory Opus 里搜索的范围”（`Launcher/OpusTypingScope`）：当前文件夹（默认）/ 整个电脑。整个电脑时搜索框里不带当前路径。资源管理器没有这个选项，不受影响。
- Opus 打字时，搜索框放在文件列表的右下角（距右、下边缘 12 px，限制在工作区内），不再居中压住 Opus（`windowavoidance::corner`、`Placement::avoidNative(..., inside)`）。资源管理器的行为不变（仍旁边避让）。
- 自动测试：`tst_placement` 的 `cornerOfFileList`（常规、最大化、列表比搜索框小、部分在屏幕外、负坐标显示器）；`tst_settings` 的 `opusTypingScope`（默认、保存读取、未知值回默认）。
- 探针（同一个 `--folder-typing`，加 `--opus-everywhere` 切到整个电脑）：用一个类名为 `dopus.lister` 的替身窗口，两种范围各 30 项检查全部通过：搜索框完全在文件列表内、距右边缘 12 px（150%：18 物理像素）、前缀和范围一致。[当前文件夹](runtime-folder-scope.json)、[整个电脑](runtime-everywhere-scope.json)。
- 未实测：真实 Directory Opus（本机装了但没有运行，没有启动用户的 Opus；真实的键盘钩子、`DOpusRT` 取文件夹、双栏焦点没有重测）；设置页新行的 150% 像素截图（该行与“在文件夹里打字时”同一写法，预览工具默认隐藏它）；125%/175%（几何由单元测试覆盖）。
- 探针退出码等于警告数：替身窗口和资源管理器窗口抢前台会产生“启动器失去前台”警告，不是产品问题；30 项检查以报告为准。
