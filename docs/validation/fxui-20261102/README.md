# 验证：快捷键面板与设置概览（#185）、剪贴板大小（#174 第一部分）、设置页头与滚动（#163）

日期 2026-11-03，分支 fxui（基于 fixes-1102），Qt 6.12.0，Release。

## 自动测试

| 测试 | 内容 | 结果 |
|---|---|---|
| `tst_shortcutpanel`（`WinShun --preview-results … --shortcut-panel`） | 搜索框和剪贴板各一个：悬停不弹出；点一下展开；分组紧凑（行数、组数）；常用键在、复制键等不在；点窗口别处收起；再点按钮收起；最后一行“全部快捷键…”发出打开设置的请求并收起；剪贴板四档大小（密集 32、紧凑 52、标准 60、宽松 68 px 一行） | 通过（40 项） |
| `tst_settingsscroll`（`--preview-settings … --page-scroll`） | 长页滚动时页名和子标签不动；换子标签回到顶部；搜索跳到深处的行、`revealRow`、Tab 到视野外的控件（先下后上）都完整落在页头下面 | 通过（9 项） |
| `tst_settingsscreen` | Screen 对象不超过 500 | 通过 |
| `tst_settings` | 新增 `clipboardUiSize`：默认“跟随”，五个值存取一致，未知值回到默认 | 通过 |
| `tst_clipboard`、`tst_clipboarddisplay`、`tst_numberhints`、`tst_keycaps`、`tst_placement`、`tst_pinclipboard`、`tst_cliplifecycle`、`tst_searchlogo`、`tst_searchnavigation` | 相关回归 | 通过 |

未通过、与本次改动无关（改动前的基线或合入的别的分支就有）：`tst_clipboardsizing`（`real_mouse_corner_drag`，真实鼠标拖角，基线 E:\wsi12 同样失败）；`tst_aisettings`（`Launcher/OpusTypingScope` 的行标题“在 Directory Opus 里搜索的范围”和目录里的“Directory Opus 里打字搜索的范围”对不上，不是本次引入）；`tst_contentpane`（内容搜索 `real_search`，本树上失败，和快捷键、字号都无关）。

## 真机（stage，非管理员，隔离 profile）

`C:\Users\young\wsi12-stage\fxui`：`--query` 打开搜索框，鼠标悬停 0.7 秒不弹出；点右下角的 i 弹出面板，搜索框仍在前面、没有收起；点结果区空白处面板收起；再点 i，点“全部快捷键…”，面板收起、设置窗口打开在 **打开 Win顺 › 按键一览**。150% 缩放、深色。

## 像素

- 150% 缩放（`QT_SCREEN_SCALE_FACTORS=1.5`，离屏渲染 720×560 设备像素）：密集的行高、图标、按钮的逻辑尺寸（32、24、24）乘 1.5 都是整数（48、36、36），所有字号为偶数；看截图，序号竖直居中，行内没有被截，面板的键帽、分组标题、分隔线没有重叠。
- 100%：测试里的行高读数（32、52、60、68）与表一致。没有逐像素量边，只做了目视检查。

## 截图

- `shortcut-panel-launcher-150.png`：搜索框的快捷键面板（点开后）。
- `clipboard-dense-150.png`：剪贴板“密集”一档。
- `settings-shortcuts-100.png`：设置 › 打开 Win顺 › 按键一览。

## 密集一档为什么要加

Ditto 的默认列表一行约一行 9 pt（12 px）的字、行高约 20 px。Win顺 的“紧凑”一行 52 px，标题 16 px、下面还有 12 px 的第二行，仍比 Ditto 大得多；所以剪贴板单独多一档“密集”：行高 32 px、一行 14 px 的字（不显示第二行，来源和时间在预览里）、图标 24 px、按钮 24 px。只有剪贴板有这一档；搜索框的界面大小不变。
