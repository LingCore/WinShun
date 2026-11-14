# #328 剪贴板图片行、#61 右键菜单里的自定义命令：验证记录（2026-11-14）

分支 `issues-1114b`（接在 `issues-1114` 后面），Qt 6.12.0 / MSVC 2022 Release，构建目录 `build/issues-1114b`。

## #328 剪贴板的两条视觉建议

1. **条目等高。** “大图片缩略图”开关改成“图片缩略图”三档：小 / 标准 / 大（`Clipboard/PictureSize`；原来开着 `LargePictures` 的读成“大”）。选“小”时缩略图放在图标的位置，图片那一行和别的行一样高。
2. **复制的多张图片。** 在资源管理器里复制几张图片（剪贴板里是文件），现在显示第一张的缩略图，右下角标出张数；里面只要有一个不是图片，仍然显示文件图标。

验证：

- `tst_clipselect`：新增 `severalPictureFilesShowTheFirst`（两张图 → 第一张的缩略图、数量 2；图片加文本文件 → 没有缩略图；文字条目数量 0），原有“单个图片文件”各项照旧通过。
- `tst_clipimage`：“小”档缩略图不超过 32×32、比例不变；张数角标在缩略图范围内，“小”档和只有一个文件时不显示。截图 `328-small.png`、`328-count.png`（单行）。
- `tst_settings`、`tst_aisettings` 和几项设置窗口测试通过。

没测：真实剪贴板窗口里整列的样子（只有单行截图和单测）；旧设置 `LargePictures=true` 升级后读成“大”这一条没有单独的测试；125% / 150% 下的像素。

## #61 把右键的文件交给自定义命令

收藏夹里的命令多一个选项“显示在文件和文件夹的右键菜单里”（默认关，命令里要有 `{query}`）。勾上后，资源管理器里右键文件或文件夹，“Win顺 命令 ›”里列出这些命令，选一个就把完整路径交给 `{query}`；工作文件夹没填时用这个文件所在的文件夹。

在 Win10 虚拟机（22H2）里，`favorites.json` 放两条命令，一条勾了、一条没勾：

- 注册表：`HKCU\Software\Classes\*\shell\WinShun.Commands` 和 `…\Directory\shell\WinShun.Commands`，`SubCommands` 为空值、`MultiSelectModel=Document`，下面只有勾了的那一条：`"…\WinShunSearch.exe" --run-command vmtest-notepad "%1"`。
- 右键文本文件：菜单里有“Win顺 命令 ›”，带图标，`61-menu.png`。
- 运行那条命令行：记事本打开了 `C:\ws\files\aaa-notes.txt`。用没勾的那条命令的 id 运行：什么也没发生。
- `tst_commandlaunch`（这个选项随收藏夹保存、读回）、`tst_favorites`、`tst_pastefile`（菜单标题）通过。

没测：真的展开子菜单再点（看到的是主菜单项和注册表里的子项）；设置里那个新选项的样子（和上面一项“关键词后面的字用来搜索文件”是同一个控件）；一次选多个文件；Windows 11；把选项取消后菜单项消失。
