# GitHub #474：选中行、选中磁贴与悬停的区分

报告：brand 风格深色 + Mica，候选（键盘所在行）的底色太浅，分辨率低的屏上看不清。

## 量法

- `tst_appearance` 的 `rowStates`：各风格真实的 `rowHighlight` 部件（加上风格自己的托盘）画出三行——选中、悬停、静止——在窗口里取像素，算选中面与列表、与悬停行的 CIELAB ΔE（白色浮起面取面和 1 px 描边里更显眼的一个），并核对选中面上 `text`/`subtext` ≥ 4.5:1、`faint` ≥ 3:1。
- `selectionTokens`：`Theme.selection`（磁贴、选中的芯片和卡片）与 `Theme.hover` 叠在底色上算同样的数。
- 底色：纯色（风格自己的 `background`/托盘）、Mica `#202020`/`#F3F3F3`、偏色 Mica `#1D2332`（取自报告截图）/`#E9EEF6`。tactile 永远不透明，只算纯色。
- 门槛（docs/colors.md 第 9 条）：浅色离底 ≥ 6.5、离悬停 ≥ 4.5；深色离底 ≥ 9、离悬停 ≥ 6。
- ctest 的 `tst_appearance_1`/`_1.25`/`_1.5`/`_2`（software 后端）全部通过；d3d11 直接运行数值一致。

## 结果行（离列表 / 离悬停行，ΔE，纯色底）

| 风格 | 改前 | 改后 | 改动 |
|---|---|---|---|
| brand 深色 | 7.7 / 3.8（Mica 7.6 / 3.8） | 15.0 / 11.1（Mica、偏色 Mica 14.4 / 10.5） | `rowSelected` 白 7 % → 10 %，`rowSelectedEdge` 白 5 % → 14 %（比面亮一级的边，同键帽） |
| liquid 浅色 | 4.5 / 1.4 | 8.8 / 5.6 | `selection` 黑 5.5 % → 10 % |
| liquid 深色 | 8.1 / 2.8 | 12.6 / 7.3 | `selection` 白 8 % → 12 % |
| fluent 浅/深 | 7.3 / 5.8、11.7 / 9.9 | 不变 | 行用强调色淡底，原本达标 |
| brand 浅色 | 15.1 / 8.9（靠描边） | 不变 | #267 定的白卡片 + 描边 + 阴影 |
| tactile 浅/深 | 7.7 / 10.7、10.3 / 7.0（在托盘上） | 不变 | 浮起卡片原本达标 |

## 磁贴、选中芯片和卡片（`selection` 离底 / 离 `hover`，纯色底）

| 风格 | 改前 | 改后 |
|---|---|---|
| fluent 浅/深 | 5.2 / 2.4、7.2 / 2.8 | 7.7 / 4.9、11.3 / 6.9 |
| brand 浅/深 | 5.3 / 1.8、8.7 / 3.2 | 8.8 / 5.3、11.8 / 6.3 |
| tactile 浅/深 | 4.0 / 2.0、6.9 / 3.9 | 7.4 / 5.4、9.7 / 6.7 |
| liquid 浅/深 | 4.7 / 1.7、8.5 / 3.1 | 8.5 / 5.6、12.5 / 7.2 |

## 选中底上的灰字（WCAG）

- fluent 浅色 `faint` 在选中行上 2.88:1（Mica 2.73:1），低于卡片上 ≥ 3:1 的规则：`#8A8A8A` → `#7A7A7A`，现 3.58:1（Mica 3.39:1）；深色不变。
- fluent 浅色 `subtext` 在偏色 Mica 的选中行上 4.35:1：`#666666` → `#5C5C5C`（比 WinUI 的次要文字在 Mica 上的 `#5F5F5F` 略深），现 5.07:1。
- brand 浅色 `ink3`（`faint`）在加深后的选中磁贴上不足 3:1：`#8A8A93` → `#74747D`。

## 图

`--preview-results <dir> --row-states --style <风格> [--dark] --scale 1 --renderer d3d11`（真实搜索窗，第一行是键盘所在行，第三行在指针下），裁出列表区，自上而下 fluent、brand、tactile、liquid：

- [rows-dark.jpg](rows-dark.jpg)
- [rows-light.jpg](rows-light.jpg)

离屏图里 brand、tactile 的文件类型图标只有底块：它们用 `Shape` 曲线渲染器画符号，与 [UI 踩坑](../../pitfalls/ui-qml-and-windows.md) 里记的同一问题，不属于本次配色改动。
