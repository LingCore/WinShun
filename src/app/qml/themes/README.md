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
- token 的全集以 `fluent/Look.qml` 为准。brand、tactile 都是这样写的（独立的 `QtObject`，不继承 fluent 的 Look；
  fluent 的 `name` 是 readonly，继承后改不了名）：

  ```qml
  import QtQuick
  import WinShun

  QtObject {
      id: root
      readonly property string name: "brand"
      readonly property bool dark: SystemTheme.dark
      readonly property bool backdrop: SystemTheme.backdrop
      readonly property color accent: dark ? "#EDA591" : "#A8402C"
      // ……只写和 fluent 不同的 token
      readonly property Component keySurface: Component { KeySurface { look: root } }   // 本目录的部件
  }
  ```

  没写的 token 和钩子由 `Theme.pick()` 退回 fluent 的（`Theme.base`），所以只写不同的就行；
  只有某些风格才有的 token（fluent 里也没有）用 `Theme.tok("名字", 默认值)` 读。
- **圆角**：用户“圆角/直角”设置选直角时所有圆角必须是 0。窗口本身的圆角是 DWM 的，风格改不了（见第 4 节）。所有圆角 token 写成 `r(v)`（继承时直接用 `r(...)`），胶囊用 `pill(h)`。
  部件里不要硬编码圆角，从 `look` 取。
- **透明**：`backdrop` 为 true 时窗口下面是 Mica，层是半透明的（fluent 的那套 rgba，brand 用原型的窗口底色 86 % / 88 %）。
  按设计不透明的风格（tactile）写 `backdrop: false`、`backdropCapable: false`，窗口底和大块表面全用不透明色。
- 主要 token（完整列表和含义看 `fluent/Look.qml` 的注释）：
  - 窗口：`windowRadius`（DWM 的，只读来对齐）、`dwmEdge`、`backdrop`、`backdropCapable`
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
- **造一个部件比画它贵得多**（Hook 里 `createObject` 加绑定，约是直接写一个同样部件的 5–10 倍）。列表行里的 Hook
  （行的按钮、悬停底、提示框、只有一行有的分隔线）写 `lazy: true`：第一次显示时才造，之后只隐藏不销毁，造出来之前
  `out()`/`call()` 给宿主自己的值。要随状态出现/消失的部件用 Hook 的 `visible`，**不要**放进 `Loader { active: … }`：
  滚动时鼠标不动，行从它下面经过，每过一行就造一次、毁一次（剪贴板 brand 滚动 12 s 造了 142 次，最长一次 4.6 ms，240 Hz 下一帧只有 4.2 ms；见 docs/pitfalls.md）。
- Hook 造部件时把当时的状态和 `extra` 作为初始属性传进去：部件的 Behavior 只对建成之后的变化起作用，所以晚造的部件
  （lazy、复用的行、换风格时重建的）一出来就是最终状态，不会从默认值渐变过来；之后的变化照常有动画。
- 部件的状态用**带默认值的普通属性**，不要用 `required`（Loader / createObject 后赋值会报 “Required property … was not initialized”）。
- **尺寸由宿主决定**：部件填满 Hook。部件可以在自己范围外画（阴影、外侧高光），但不能改变占位；
  需要按下位移、内边距之类时用 token（见下表“宿主读的 token”）。
- 1 px 线自己算：`Math.max(1, Math.round(dpr)) / dpr`；位置取整：`Math.round(v * dpr) / dpr`。`dpr` 是窗口的 devicePixelRatio，宿主传。
- 浅深色：部件里不判断，颜色全部来自 `look`，Look 自己按 `SystemTheme.dark` 给两套。

| 钩子 | 文件名 | 用在哪里 | 部件可声明的状态 |
|---|---|---|---|
| `windowFrame` | `WindowBody.qml` | WindowEdge：所有我们画的窗口。`paint: "back"`（最底层，风格的窗口底，方的，`fill` 是该窗口自己的底色，如设置窗口的 page、启动器的 background；fluent、brand 没有）；`paint: "edge"` 只用 fluent 的（Windows 10 的 1 px 边）。不画圆角、不画阴影 | paint, fill, active, maximized, dpr |
| `searchField` | `SearchField.qml` | 启动器搜索行（`flat: true`）、设置搜索框、剪贴板搜索、插件搜索框（`flat: false`） | flat, focused, hovered, dpr |
| `inputField` | `InputField.qml` | InputBox、表单输入框 | focused, hovered, enabled, error, dpr |
| `keySurface` | `KeySurface.qml` | FlatButton、RowAction、钉住/展开、底栏按钮、工具条按钮…… | hovered, pressed, checked, current（打开着的东西：行的预览键）, enabled（Item 自带，部件不要重新声明）, highlighted（主按钮）, danger（确认删除）, flat（静止时无底）, corner（宿主要求的圆角，-1 用自己的）, role, dpr |
| `segmented` | `Segmented.qml` | 范围（role "scope"）、底栏排序（"sort"）、FileViewSwitch（"switch"）、PageTabs 和设置里的分段（"tabs"） | count, currentIndex, hoveredIndex, pressedIndex, iconOnly（全部选项只有图标）, segments（`[{x, width}]`，各段在 Hook 内的位置）, role, dpr。文字由宿主画；部件可给 segmentX(i) 等自己排 |
| `chipSurface` | `ChipSurface.qml` | 类型条（role "type"）、剪贴板分组（"group"）、筛选胶囊（"filter"） | checked, hovered, pressed, hueIndex（-2 文件夹、-1 不限、0..n 按 TypeStrip 的类型顺序，超出调色板取模）, isAll, role, dpr |
| `rowHighlight` | `RowHighlight.qml` | 结果行、剪贴板行、面板行（role "list"/"panel"）、菜单项（"menu"）的选中/悬停底；一个列表共用一个跟着当前行走的高亮 | selected, hovered, current（键盘所在行，未选中）, pressed, role, position（宿主高亮的 y，变化时可做落位动画）, dpr |
| `matchStyle` | —（token） | StyledText 里的匹配字 | `matchColor`、`matchWeight` |
| `typeIcon` | `TypeIcon.qml` | 文件类型图标；fluent 为 `null`（用系统图标）。应用和 exe 一直用系统图标 | kind（folder doc xls ppt pdf image md zip code audio video ebook text other）, size, hueIndex, dpr |
| `menuFrame` | `MenuFrame.qml` | DropMenu、HoverTip、工具条/浮层面板、对话框卡（窗口里画的；ContextMenu 等菜单窗口是 DWM 的框，只用 `menuBackground`） | dpr |
| `toggle` | `Toggle.qml` | ToggleSwitch 的轨道和滑块（宿主 40×20）；`kind: "check"` 是复选框 | checked, hovered, pressed, enabled, mixed, active（false：还没读到状态，只画轨道）, kind, dpr |
| `keyHint` | `HintCap.qml` | KeyHint、底栏键位提示（**不可点**，保持扁平） | dpr。字由宿主画（`subtext`） |
| `separator` | `Separator.qml` | 分隔线、接缝；在宿主框里横向/纵向居中 | orientation, dpr |
| `scrollBar` | `ScrollThumb.qml` | ListScrollBar 的轨道和滑块，宿主决定宽度（靠近时变宽） | active（变宽）, hovered（在滑块上）, pressed, position, extent（滑块 y/高）, dpr |
| `card`（宿主读 `Theme.cardPart`） | `CardSurface.qml` | 设置卡片、插件卡片、ShareCard | hovered, corner, inset（卡片里的卡片：只画边，经 extra 传）, dpr |
| `tray` | `Tray.qml` | 结果列表下面的托盘；fluent、brand 为 `null` | dpr |
| `actionCapsule` | `ActionCapsule.qml` | 结果行按钮组后面的胶囊；fluent、tactile 为 `null` | count, hovered, dpr |

宿主读的 token（部件之外的字和间距，也按风格变）：`chipText` 规则是“选中且不是不限 → `accentText`（白），否则 `text`/`subtext`”，所以选中填充必须够深；
`segment` 的选中字用 `text` + `fontStrong`，其余 `subtext`。

### 3.1 role 和 extra
- 纯图标分段选择传 `iconOnly: true`，选中图标用当前风格的 `accent`，配中性选中底面，不画文字标签式下划线。不能按 `role: "switch"` 推断是否纯图标：录屏的文字选项也用这个 role。宿主同时传悬停、按下状态。
- `keySurface.current` 表示入口对应的预览／菜单仍打开。三套风格都须保留图标标识和底面，不能只在 hover 时显示；关闭后恢复。强调色实心按钮（含 `checked`）由部件提供白色 `foreground`，不依赖宿主碰巧选对颜色。
- `role`（字符串）告诉部件它在哪：keySurface `"rowAction"|"fieldKey"|"toolbar"|"footer"|"dialog"`；segmented `"scope"|"sort"|"tabs"|"switch"`；
  rowHighlight `"list"|"menu"|"panel"`；chipSurface `"type"|"group"|"filter"`。Hook 还派生旧名：raised（rowAction）、inField（scope/fieldKey）、
  bar（不是 menu）、inMenu、keyed（scope）、checkbox（kind "check"）。部件不要把这些名字声明成 readonly（Hook 要赋值）。
- 只有某个风格懂的输入：宿主写 `extra: ({ name: value })`，部件声明了同名属性才赋值（例：`inset`、`shadow`、`corner`、`indicator`）。
- Hook 的 `fill` 会传给所有声明了 `fill` 的部件，部件内部别的用途请换名。

### 3.2 部件给宿主的值（out / call）
宿主用 `hook.out("名字", 宿主默认值)`、`hook.call("函数", [参数], 默认值)` 读，部件声明同名属性/函数即可：
- keySurface / chipSurface：`foreground` `contentOffsetY` `fontWeight` `leadingInset` `iconColor` `disabledOpacity`
  （部件禁用时自己变淡就声明 `readonly property real disabledOpacity: 1`，宿主 `opacity: enabled ? 1 : out("disabledOpacity", 0.45)`）
- segmented：`segmentX(i)` `segmentWidth(i)` `labelOffsetY(i)` `labelColor(i)` `labelWeight(i)` `leadingInset`
- searchField / inputField：`textColor` `placeholderColor` `caretColor` `iconColor`（searchField 另有 `leadingInset` `contentOffsetY`）
- keyHint：`foreground` `contentOffsetY`；toggle：`settle()`（宿主行被复用/窗口重新显示时调用；Behavior 里的动画 complete() 停不住，
  做法见 fluent/Toggle.qml：暂时关掉 Behavior 再把属性绑回终值）
- 部件创建时 `look` 可能还没赋上、退出时可能先被置空：读 look 的绑定都要判空。

### 3.3 宿主读的几何 token（Theme.xxx；Look 缺的退回 fluent）
`fieldInset` `fieldRadius` `fieldKeyInsetAt(dpr)` `rowInset` `rowRadius` `rowSlide`（ms，0 不滑） `iconInset` `textStart` `actionsInset`
`capsuleRadius` `capsulePadding` `capsuleKeyRadius` `capsuleFill` `typeChipHeight`（0：UiScale） `menuItemHeight` `menuPadding`
`pageTabsInset` `checkSize` `scrim` `menuHoverEdge` `animFast` `animMove`，函数 `edge(dpr)`（窗口边、窗口级分隔线）、`hair(dpr)`（部件内部的 1 设备像素线）。
只有部分风格有的值：`Theme.tok("名字", 默认)`。

需要新钩子或新 token：写进交付说明，由 A 统一加到 fluent/Look.qml 和 Hook.qml，不要自己改共用文件。

## 4. 窗口外框：永远是 Windows 自己的（DWM）

- **禁止自绘窗口圆角**（用户 2026-11-05 定）。所有风格、所有窗口（菜单、提示、插件窗口也是）的圆角、1 px 边、阴影和 Mica
  都由 DWM 画：Windows 11 上 8（菜单也是 8），直角设置或 Windows 10 上是直角。不要关 DWM 圆角（`DWMWCP_DONOTROUND`）、
  不要把边设成无、不要用分层窗口画阴影，也不要为了“更大的圆角”把窗口做透明再自己画。
- 风格只能改窗口**里面**：`windowRadius` 一律写成 fluent 的表达式
  （`SystemTheme.roundedCorners && SystemTheme.nativeWindowBorder ? 8 : 0`），只给托盘图、设置卡片之类当作“窗口的圆角”来读。
- 靠近窗口角的东西和窗口同心：**内圆角 = max(8 − 间距, 4)**（直角时 0）。例：搜索框离窗口边 10 → 圆角 4；
  菜单项离菜单边 4 → 4。框里再套的键、分段最少 2。
- 每个窗口：`color: <底色>`，加 `WindowEdge { fill: <同一个底色> }`。WindowEdge 只在 Windows 10 上画 1 px 边（fluent 的，所有风格一样），
  另在最底层放风格的窗口底（`windowFrame` 的 `paint: "back"`，方的，例如 tactile 的面板高光和颗粒；fluent、brand 没有）；
  菜单和提示写 `WindowEdge { body: false }`，它们只有自己的底色。
- 贴边的大块表面（底栏、标题栏、侧栏底色）直接画成方的：窗口的圆角由 DWM 裁出来，不用自己处理。
- 透明：风格的 Look 用 `backdrop`（跟随“透明效果”设置，`SystemTheme.backdrop`）和 `backdropCapable`（这个风格能不能透明；
  false 时设置里那一行变灰并说明原因）。tactile 按原型就是不透明的（两个都是 false），brand 和 fluent 一样跟随设置。

## 5. 预览

`WinShun.exe --preview-results <dir> --search-logo --style brand [--dark] [--scale 1.5]`、
`--preview-settings <dir> --style tactile` 等离屏出图（不碰正在运行的 WinShun）。离屏图里没有 DWM 的圆角、边和阴影。

`--preview-results <dir> --style-buttons --style fluent|brand|tactile [--dark] [--scale 1.5]` 检查真实按钮部件：图标选中、文字分页、打开预览、主操作、选中工具按钮、禁用、键盘焦点，以及用窗口内鼠标事件切换列表／大图。输出各状态 PNG 与 `buttons.json`；数据在临时目录，不操作系统剪贴板。
