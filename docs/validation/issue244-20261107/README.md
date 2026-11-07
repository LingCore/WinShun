# #244 搜索栏交互验证

日期：2026-11-07。对应 [Issue #244](https://github.com/LingCore/WinShun/issues/244)。本轮为本地实现及验证，尚未提交或发布。

后续修正：用户反馈品牌色彩和轻拟物质感主题的拖动入口不明显，现已统一显示顶部横条；本报告下文有关隐藏横条的说明仅记录当时方案，当前实现和验证见[跟进记录](../issue244-followup-20261107/README.md)。

## 最终交互

- 保留“全部／文件／内容”并排按钮和已有的合并范围设置，一次点击直接选择。保留 Tab、Shift+Tab 的原有语义，不引入范围下拉菜单。
- 搜索栏中只允许可见的 Logo 和横条拖动；输入区周围、类型条和底栏留白不再作为拖动区。横条沿用主题可见性约定，隐藏时不保留拖动热区。
- 双击可见横条请求展开／收回搜索面板。右侧按钮及 Ctrl+E 保留。面板的原生缩放边框继续可用，明确可见的横条矩形优先接收拖动。
- Logo 左键打开设置，中键保持显示，右键打开“靠上／居中／恢复上次拖动位置”菜单。面板中不提供“靠上”；重复右击可关闭或重开，Esc 只关闭菜单。
- 单独保存上次手动拖动的锚点，居中、靠上及恢复操作不覆盖它。保留旧位置配置的读取，按搜索框和面板分别保存。临时任务栏、文件管理器定位继续沿用原有规则。
- Logo 提示在宿主窗口内放不下时，使用不抢焦点且鼠标穿透的独立窗口；窗口变高、变矮时重新判断。仅提高 z 值无法解决原生窗口边界裁切。
- 移除底栏重复的范围切换提示，保留补全、进入文件夹、调音量等与当前结果有关的提示。范围按钮悬停仍显示快捷键。

## 验证入口

使用 MSVC 2022、Qt 6.12.0、Windows 11 x64，D3D11 后端。在任务独有的 `build/issue244/` 中构建 `WinShun` 和 `tst_displaygeometry`，只运行相关检查；没有运行全套测试。

窗口回归入口为现有的 `--preview-results <目录> --search-logo --renderer d3d11`。它使用生产 `Main.qml`、`SearchBar.qml`、`WindowFrame`、`Placement` 和 `Launcher`，配置、索引、历史等使用临时目录。通过定向的 Qt 输入事件和 Windows 消息检验，不向系统注入全局鼠标或快捷键。

覆盖框／面板、紧凑／标准／宽松布局、Logo 原有操作、文本拖选、输入区边缘原生命中、真实单击范围按钮、Tab／Shift+Tab、位置菜单连续开关、Esc、提示窗口及其焦点／鼠标穿透标志。横条双击检查的是生产 QML 发出既有 `panelToggleRequested` 信号，App 接收该信号的展开流程保持原有实现。

位置回归通过 `tst_displaygeometry` 的以下方法运行，三项及初始化／清理均通过：

- `draggedPositionSurvivesPresetsAndRestart`：经 Windows 进入／退出移动消息保存位置，验证框与面板的预设切换、恢复以及重新创建 Placement 后恢复。
- `centeringKeepsSizeAndSavesEachMode`：既有居中与分模式保存回归。
- `windowMayCloseBeforeQueuedMoveIsSaved`：窗口销毁后，待处理的移动保存回调不访问已销毁窗口。

缩放使用独立预览进程的 `--scale` 参数，没有修改系统显示设置。未验证物理多显示器间的真实鼠标拖动或运行中的用户配置迁移。

## 调试中确认的边界

最初按完整原生缩放面板验证时，横条的点击落入原生缩放边框；新增了只针对显式可见抓手的命中优先级，避免改变其他窗口或整条顶部边缘。另一次品牌主题检查中，原测试错误地要求被主题隐藏的横条仍可拖动；最终断言分别检查“可见时可拖”和“隐藏时没有标题栏热区”，没有强制改变主题。

## 证据与范围

窗口回归的 JSON、预览日志和代表性图片保留在下列子目录，位置测试见 [placement.txt](placement.txt)。各轮均通过，Qt 运行警告为零。中英文轮次在最后一次文案上下文调整后额外检查了底栏重复提示的可见性；品牌和触感主题不显示横条，因此不执行横条双击断言。

| 主题／语言 | 进程缩放 | 检查数 | 记录 |
| --- | --- | --- | --- |
| Fluent／中文 | 100% | 162 | [JSON](verified-100/search-logo.json) |
| Liquid／中文深色 | 150% | 162 | [JSON](verified-150/search-logo.json) |
| Brand／中文 | 200% | 161 | [JSON](verified-200/search-logo.json) |
| Tactile／中文 | 100% | 161 | [JSON](verified-tactile/search-logo.json) |
| Fluent／英文 | 100% | 164 | [JSON](locale-en/search-logo.json) |
| Fluent／中文最终回归 | 100% | 164 | [JSON](locale-zh/search-logo.json) |

检查数包含同一契约在不同模式和尺寸下的重复断言，不表示独立测试用例总数。构建成功；编译器仍有既有代码及 Qt 生成代码的告警，本轮没有将它们计作零告警。

实现涉及 `SearchBar.qml`、`Main.qml`、`HoverTip.qml`、`Footer.qml`、`TypeStrip.qml`、`WindowFrame`、`Placement`，并补充相关回归、英文文案和使用说明。既有工作区其他界面改动不属于本轮。

本次临时构建和重复预览图在证据保存后回收，保留当前源码和本目录中的验证记录。

设计参考：[Windows 鼠标交互](https://learn.microsoft.com/en-us/windows/apps/develop/input/mouse-interactions)、[Windows 标题栏设计](https://learn.microsoft.com/en-us/windows/apps/design/basics/titlebar-design)。本项目的搜索面板展开与系统最大化语义不同，因此保留显式展开入口。
