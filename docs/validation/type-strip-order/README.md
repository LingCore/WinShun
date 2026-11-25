# 类型条排序验证

2026-11-25，Windows 11 Pro 26200，`build/release`（Qt 6.12.0）。用户反馈：类型条（文件夹、文档、图片……）调不了顺序，希望设置里能排，类型条上也能按住拖。

| 验证 | 结果 |
|---|---|
| `ctest -R "^tst_settings$"`（新增 `stripOrder`） | 通过。文件夹在类型之间任意移动、类型越过文件夹、越界和原地不动不算移动；保存后读回一致；`Results/FolderChipAt` 超出范围时收进 0 到类型数 |
| `WinShun.exe --preview-results <dir> --type-strip-drag --renderer d3d11`（fluent 100%；tactile 150%；brand 125% 深色） | 全部通过。鼠标经 `qt_handleMouseEvent` 走 `QGuiApplication`，和真实输入同一条路：按住“文件夹”拖过“文档”“图片”，被越过的两个让位、后面的不动，松手后顺序与设置一致；“压缩包”拖到最左停在“不限”之后；选中的“图片”换了位置仍选中；角标数量跟着各自类型走；不拖只点仍是点击；“不限”拖不动；`Ctrl+Tab` 按新顺序；14 个类型时按住拖到右端，类型条继续滚动，松手不跳回开头；松手后各标签不重叠、间距一致、都在整设备像素上；无绑定循环 |
| `WinShun.exe --preview-settings <dir> --file-types --renderer d3d11`（100%；`--compact` 下 100% / 150% 深色 / 175%） | 全部通过。“文件夹”成为列表里的一行，按把手拖到第三行、上移、把类型移到最前、删掉它前面的类型时位置都对；“恢复默认”连顺序一起还原；窄窗口各缩放下每行控件不超出卡片（扩展名框最窄 64） |
| `--type-strip-counts`、`--type-strip-scroll`、`--content-kinds` | 仍全部通过 |

实拍：类型条拖动中 [strip-drag.png](strip-drag.png)，tactile 150% [strip-drag-tactile-150.png](strip-drag-tactile-150.png)，设置页拖把手 [settings-drag.png](settings-drag.png)。

没有验证的：没在正在运行的 Win顺 里实拖（用户的实例在运行，没替换）；`App` 收到拖动后保存设置的那一步（和类型条上“内容索引”开关同一条路）只在离屏检查里用等价代码模拟；触屏和触控板拖动没有试。
