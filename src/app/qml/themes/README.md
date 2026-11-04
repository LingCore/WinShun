# 界面风格（themes/）

设置“外观 › 主题与窗口质感 › 界面风格”选的风格，存在 `Appearance/UiStyle`（`fluent | brand | tactile`，默认 `fluent`）。
C++ 里是 `SystemTheme::uiStyle()`，QML 里是 `Theme.style`。风格与浅/深色正交：每个风格都要有完整的浅色、深色两套。

每个风格的全部实现只放在自己的目录：

```
themes/
  fluent/   精修原生（Windows 11，跟随系统强调色）  —— token 全集的定义处
  brand/    品牌色彩
  tactile/  轻拟物质感
```

## 1. 怎么加载

- 三个目录都编进 WinShun 模块（以及截图录屏、图片文字搜索两个插件各自的 WinShun 模块），资源路径
  `qrc:/qt/qml/WinShun/themes/<style>/`。CMake 用 glob 收 `themes/*/*.qml`（`CONFIGURE_DEPENDS`，新加文件重新配置时自动收进来），
  并设 `QT_QML_SKIP_QMLDIR_ENTRY` + 模块的 `NO_GENERATE_EXTRA_QMLDIRS`：它们**不是模块的类型**，所以三个目录里同名的部件不冲突。
- **目录里不要放 qmldir**，也不要 `import WinShun.Themes.Xxx`。同一目录的文件按类型名互相引用（隐式目录导入）。
- `Theme.qml` 用 `Qt.createComponent("themes/<style>/Look.qml")` 加载当前风格的 Look，第一次用到时才创建；
  创建失败（文件有错）会在日志里写 `Theme: style … could not be loaded` 并退回 fluent。
- 部件文件名**不能**和 WinShun 模块里已有的类型同名（模块类型会抢先解析）：例如不能叫 `WindowFrame`、`KeyHint`、`Glyph`、
  `Chip`、`Tag`、`InputBox`、`ToggleSwitch`、`SoftShadow`……。下面第 3 节的文件名是约定好的。

## 2. Look.qml

- 根对象是任意 `QtObject` 派生，文件名固定 `Look.qml`。只有 Look.qml 可以 `import WinShun`（读 `SystemTheme.dark`、`SystemTheme.roundedCorners`、
  `SystemTheme.accent` 等）。
- token 的全集以 `fluent/Look.qml` 为准。建议：

  ```qml
  import QtQuick
  import WinShun
  import "../fluent" as Fluent

  Fluent.Look {
      id: root
      name: "brand"
      frame: "own"
      windowRadius: r(18)
      backdrop: false
      accent: dark ? "#EDA591" : "#A8402C"
      // ……只覆盖不同的 token
      keySurface: Component { KeySurface { look: root } }   // 本目录的部件
  }
  ```

  不继承 Fluent.Look 时必须给全所有 token 和钩子（少一个，组件读到 undefined）。
- **圆角**：用户“圆角/直角”设置选直角时所有圆角必须是 0（窗口的 18 也是）。所有圆角 token 写成 `r(v)`（继承时直接用 `r(...)`），胶囊用 `pill(h)`。
  部件里不要硬编码圆角，从 `look` 取。
- **透明**：`backdrop` 为 true 时窗口下面是 Mica，层是半透明的（fluent 的那套 rgba）。`frame: "own"` 的风格没有 Mica（DWM 的背景跟着 DWM 的形状走，自画圆角后是方的），
  必须 `backdrop: false`，窗口底和大块表面全用不透明色。
- 主要 token（完整列表和含义看 `fluent/Look.qml` 的注释）：
  - 窗口：`frame`（`"dwm"` | `"own"`）、`windowRadius`、`windowShadow`（own 时的阴影层，`[{alpha, blur, dy}]`，逻辑 px，近的在前，浅深各给一套）、`dwmEdge`、`backdrop`
  - 圆角：`cardRadius` `largeCardRadius` `insetCardRadius` `controlRadius` `largeControlRadius` `smallControlRadius` `smallRadius` `lineRadius`
    `progressRadius` `cardFocusRadius` `menuRadius` `menuItemRadius` `rowRadius`
  - 强调：`accent`（字、图标、线）、`accentFill`（白字底，必须够深）、`accentLine`（指示条、光标）、`accentTint`（选中行底）、`accentText`（彩色底上的字，白）
  - 匹配高亮：`matchColor`、`matchWeight`（只改字本身：颜色 + 字重，PuHuiTi 只有 400/700；对比度 ≥ 4.5:1）
  - 表面：`background` `layer` `layerEdge` `windowEdge` `divider` `text` `subtext` `faint` `hover` `pressed` `selection` `textSelection` `badge` `tagFill`
    `track` `chipActive` `chipHover` `chipBorder` `chipRest` `pieHover` `pieShadow` `keycap` `keyEdge` `checker` `checkerAlt`
  - 设置窗口：`page` `cardFill` `cardBorder` `navHover` `navSelected` `control` `controlHover` `controlPressed` `controlBorder` `inputFocused`
  - 菜单：`menuBackground` `menuHover` `menuEdge` `menuShadow` `menuShadowSpread`
  - 状态：`danger` `dangerFill` `success` `caution` `closeHover` `closePressed`
  - 类型色：`typeHues`（按 hueIndex 0..n）、`folderHue`、`hueFor(index)`
  - `iconFont`（Segoe/Phosphor 图标字体名，给画图标的部件用）

`Theme.xxx` 的名字不变（100 多个文件在用），每个都改为 `look.xxx`；尺寸（字号、行高、搜索行高）仍由 `UiScale`（界面大小）决定，三种风格共用。

## 3. 部件（钩子）

Look 里每个钩子是一个 `Component`，包住本目录的部件并注入 `look`：`keySurface: Component { KeySurface { look: root } }`。
部件**不 import WinShun、不读 Theme / SystemTheme**，颜色和圆角都从 `look` 取。

组件用共用的 `Hook.qml` 放部件：

```qml
Hook {
    anchors.fill: parent
    part: Theme.look.keySurface
    hovered: area.containsMouse
    pressed: area.pressed
    dpr: button.dpr
}
```

- `Hook` 用 `createObject` 造出部件、`anchors.fill` 自己，再对**部件确实声明了的**状态名逐个用 `Qt.binding` 赋值；
  部件没声明的状态不传（不报警告）。风格切换（`Theme.look` 变了）时自动重建。
- 部件的状态用**带默认值的普通属性**，不要用 `required`（Loader / createObject 后赋值会报 “Required property … was not initialized”）。
- **尺寸由宿主决定**：部件填满 Hook。部件可以在自己范围外画（阴影、外侧高光），但不能改变占位；
  需要按下位移、内边距之类时用 token（见下表“宿主读的 token”）。
- 1 px 线自己算：`Math.max(1, Math.round(dpr)) / dpr`；位置取整：`Math.round(v * dpr) / dpr`。`dpr` 是窗口的 devicePixelRatio，宿主传。
- 浅深色：部件里不判断，颜色全部来自 `look`，Look 自己按 `SystemTheme.dark` 给两套。

| 钩子 | 文件名 | 用在哪里 | 部件可声明的状态 |
|---|---|---|---|
| `windowFrame` | `WindowBody.qml` | WindowEdge：所有我们画的窗口。宿主放两份：`paint: "back"`（最底层，own 时画窗口底的圆角形状；`fill` 是该窗口自己的底色，如设置窗口的 page、启动器的 background）和 `paint: "edge"`（最上层，画 1 px 边）；`"all"` 两者都画 | paint, fill, active, maximized, dpr |
| `searchField` | `SearchField.qml` | 启动器搜索行（`flat: true`）、设置搜索框、剪贴板搜索、插件搜索框（`flat: false`） | flat, focused, hovered, dpr |
| `inputField` | `InputField.qml` | InputBox、表单输入框 | focused, hovered, enabled, error, dpr |
| `keySurface` | `KeySurface.qml` | FlatButton、RowAction、钉住/展开、底栏按钮…… | hovered, pressed, checked, enabled（Item 自带，部件不要重新声明）, highlighted（主按钮）, danger（确认删除）, flat（工具栏图标按钮，静止时无底）, corner（宿主要求的圆角，-1 用自己的）, dpr |
| `segmented` | `Segmented.qml` | 范围（全部/文件/内容）、底栏排序、FileViewSwitch、PageTabs、设置里的分段选择 | count, currentIndex, hoveredIndex, pressedIndex, segments（`[{x, width}]`，各段在 Hook 内的位置）, dpr。文字由宿主画 |
| `chipSurface` | `ChipSurface.qml` | 类型条、剪贴板分组、筛选胶囊 | checked, hovered, pressed, hueIndex（-2 文件夹、-1 不限、0..n 按 TypeStrip 的类型顺序，超出调色板取模）, isAll, dpr |
| `rowHighlight` | `RowHighlight.qml` | 结果行、剪贴板行、面板行、菜单项的选中/悬停底 | selected, hovered, current, position（宿主高亮的 y，变化时可做落位动画）, dpr |
| `matchStyle` | —（token） | StyledText 里的匹配字 | `matchColor`、`matchWeight` |
| `typeIcon` | `TypeIcon.qml` | 文件类型图标；fluent 为 `null`（用系统图标）。应用和 exe 一直用系统图标 | kind（folder doc xls ppt pdf image md zip code audio video ebook text other）, size, hueIndex, dpr |
| `menuFrame` | `MenuFrame.qml` | ContextMenu、DropMenu、HoverTip | dpr |
| `toggle` | `Toggle.qml` | ToggleSwitch 的轨道和滑块（宿主 40×20）；`kind: "check"` 是复选框 | checked, hovered, pressed, enabled, mixed, active（false：还没读到状态，只画轨道）, kind, dpr |
| `keyHint` | `HintCap.qml` | KeyHint、底栏键位提示（**不可点**，保持扁平） | dpr。字由宿主画（`subtext`） |
| `separator` | `Separator.qml` | 分隔线、接缝；在宿主框里横向/纵向居中 | orientation, dpr |
| `scrollBar` | `ScrollThumb.qml` | ListScrollBar 的轨道和滑块，宿主决定宽度（靠近时变宽） | active（变宽）, hovered（在滑块上）, pressed, position, extent（滑块 y/高）, dpr |
| `card` | `CardSurface.qml` | 设置卡片、插件卡片、ShareCard | hovered, corner, dpr |

宿主读的 token（部件之外的字和间距，也按风格变）：`chipText` 规则是“选中且不是不限 → `accentText`（白），否则 `text`/`subtext`”，所以选中填充必须够深；
`segment` 的选中字用 `text` + `fontStrong`，其余 `subtext`。

需要新钩子或新 token：写进交付说明，由 A 统一加到 fluent/Look.qml 和 Hook.qml，不要自己改共用文件。

## 4. 窗口外框（frame: "own"）

- `frame: "own"` 且 `windowRadius > 0`（且没选直角）时，WinShun 关掉该窗口的 DWM 圆角、DWM 边和 DWM 阴影，并去掉 Mica；
  窗口变成透明，`windowFrame` 部件画 18 圆角的底（`paint: "back"`，颜色是宿主给的 `fill`）和 1 px 中性灰边（`paint: "edge"`）。
- 阴影由 C++ 画在一个点击穿透的伴随窗口里（紧贴主窗口下方，跟随移动、显示隐藏、最大化、DPI），参数就是 `windowShadow`（每层可选 `color`，缺省黑色），部件不画窗口阴影。
- 最大化时 `maximized: true`：不画圆角和边。
- 窗口里贴边的大块表面（底栏、标题栏、侧栏底色）在 own 时要自己按 `Theme.windowRadius` 处理靠近窗口角的两个角，否则会盖住圆角。

## 5. 预览

`WinShun.exe --preview-results <dir> --search-logo --style brand [--dark] [--scale 1.5]`、
`--preview-settings <dir> --style tactile` 等离屏出图（不碰正在运行的 WinShun）。离屏图里没有 C++ 画的窗口阴影。
