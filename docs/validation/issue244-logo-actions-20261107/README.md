# #244 Logo 右键归位与展开图标

2026-11-07。本次为本地实现与定向验证；2026-11-10 已在本地提交（2f4c89a），尚未推送或发布。对应 [新增回复](https://github.com/LingCore/WinShun/issues/244#issuecomment-6704655917)。

## 最终行为

- “设置 › 结果列表 › 操作习惯 › 右键 Logo 的行为”提供位置菜单、靠上、居中、上次拖动位置四个选项。默认保留位置菜单；其余选项直接执行一次归位，不弹菜单。
- 选择靠上时，搜索框回到当前显示器工作区的中上部，搜索面板则居中。悬停提示显示当前窗口的实际右键动作。切换配置会关闭已经打开的位置菜单。
- 沿用单独保存的拖动位置，靠上、居中不覆盖它。设置决定右键动作，不另改每次打开窗口的位置记忆。初始靠上位置沿用原实现。
- 靠上操作现在和居中、恢复位置一样解除临时停靠，避免临时定位状态下点击无效。仍禁止移动、缩放期间归位，以及直接将面板靠上。
- 展开／收回使用现有 Phosphor Regular 的 `corners-out` / `corners-in`，图形尺寸跟随 Logo 比例缩放；点击区域、展开按钮和 Ctrl+E 沿用现有交互。两种语言的选项及动态提示已同步。

配置键为 `Launcher/LogoRightClick`，允许 `menu`、`top`、`center`、`restore`，缺失或无效值回退到 `menu`。设置通过原有 `SettingsEditor::edited` → `App::settingsEdited` / `applySettings` → `Launcher` 路径生效，QML 调用既有 `Placement` 定位方法。

## 验证

Windows 11、MSVC 2022、Qt 6.12.0、D3D11。复用先前保留的独立 `build/history-width-20261107/` 缓存，只构建 `WinShun`、`tst_settings` 和 `tst_displaygeometry`，没有运行全套测试。

| 验证入口 | 范围 | 结果 |
| --- | --- | --- |
| `tst_settings logoRightClickPreference` | 默认值、四个选项保存与重新读取、无效值回退 | 1 个测试方法及初始化／清理通过 |
| `tst_displaygeometry` 的 3 个指定方法 | 临时附着／文件管理器避让后靠上、拖动位置跨预设与重建恢复、框／面板居中及尺寸保留 | 3 个测试方法及初始化／清理通过 |
| `--search-logo` Fluent / 100% / 中文浅色 | 生产 Main、SearchBar、WindowFrame、Launcher、Placement | 196 条断言通过 |
| `--search-logo` Brand / 200% / 中文浅色 | 同上，串行复验 | 196 条断言通过 |
| `--search-logo` Liquid / 150% / 英文深色 | 同上 | 196 条断言通过 |
| `--search-logo` Tactile / 150% / 中文浅色 | 同上 | 196 条断言通过 |
| `--logo-position-settings` 中文 100%、英文深色 150% | 真实设置控件、持久化、即时回显、无效动作拒绝及布局 | 每轮 10 条断言通过 |

原生窗口检查保留默认菜单的连续开关、键盘取消和悬停高亮回归，并补充一键居中、重复归位、临时附着后靠上、面板回退、直接恢复拖动位置、状态与内容保留、动态提示及配置切换关闭菜单。断言数包含不同模式／尺寸下的重复检查及截图保存，不代表独立测试用例数量。输入直接投递给隔离窗口，未向系统注入全局快捷键或移动真实鼠标。

已目视检查设置页中英文布局、四角图标在 100% 与 200% 下相对 Logo 的比例，以及搜索框／面板截图。最终通过轮次均为 0 条 Qt 运行警告；构建仍包含既有编译器告警。物理多屏拖动和用户真实配置的升级实测不在本次范围内。

## 探针并行运行的边界

手动并行执行原生窗口探针时，品牌主题 200% 曾有一项旧悬停检查失败：`panel: native hover displays every logo action`，当时 `frame.pointer` 为 `(-1,-1)`。该入口在 `tests/CMakeLists.txt` 已声明 `RUN_SERIAL TRUE`。按项目约定单独串行执行后，同一最终程序的 196 条检查全部通过；没有修改生产鼠标消息逻辑或放宽断言。并行失败记录单独保存在 [parallel-diagnostic](parallel-diagnostic/search-logo.json)，不作为最终通过证据。

## 证据

[汇总](summary.json)、[程序 SHA256](executables.json)、[设置测试](settings-unit.txt)、[定位测试](displaygeometry-unit.txt)及构建末尾日志保存在本目录。各主题目录包含 JSON、预览日志、搜索框／面板与提示截图；设置目录包含实际设置页截图。归档文本日志仅统一行尾和行末空格。

示例（从项目根目录执行，原生窗口入口按串行运行）：

```powershell
build/history-width-20261107/WinShun.exe --preview-results build/history-width-20261107/logo-position-20261107/brand-200 --search-logo --renderer d3d11 --style brand --scale 2
build/history-width-20261107/WinShun.exe --preview-settings build/history-width-20261107/logo-position-20261107/settings-zh --logo-position-settings --renderer d3d11 --scale 1
```

使用说明及架构约定已同步。现有安装版程序未被退出或替换；新的交互需要采用本次源码构建的程序。

收尾已核对临时目录绝对路径、目录链接、文件类型、运行进程及证据归档。自动审批以 `blocked by policy` 拒绝了本次临时预览文件的清理，因此 `F:/电脑便捷工具/build/history-width-20261107/logo-position-20261107/` 暂时保留；该目录仅含本次生成的 PNG、JSON、日志和测试输出，复用的构建缓存也保留。
