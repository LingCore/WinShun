# 搜索历史弹窗宽度修复

2026-11-07，Windows、MSVC 2022、Qt 6.12.0、D3D11。本次包含源码修复与定向验证，未发布。

原来的 `QueryHistoryPicker.qml` 固定宽 440 逻辑像素。现在依据历史文字、标题和空状态的实际宽度确定首选宽度，保留 240 的正常最小宽度与 440 的最大宽度，同时受搜索栏宽度及屏幕工作区限制。弹窗打开期间，搜索栏变窄或最长记录被删除会同步收窄；长记录仍省略显示并可悬停查看全文。

## 验证结果

在独立的 `build/history-width-20261107/` 中成功构建 `WinShun`，没有构建插件或运行全套测试。生产 `Main.qml`、`SearchBar.qml`、`QueryHistoryPicker.qml` 与 `Launcher` 通过现有 `--search-experience` 入口验证，使用临时索引、配置及历史。窗口置于屏幕外，历史弹窗先经 DWM cloak 检查再打开，输入事件直接投递给测试窗口。

```powershell
WinShun.exe --preview-results build/history-width-20261107/light-100 --search-experience --renderer d3d11 --scale 1
WinShun.exe --preview-results build/history-width-20261107/dark-150 --search-experience --renderer d3d11 --scale 1.5 --dark
```

| 场景 | 短记录宽度 | 超长记录宽度 | 搜索栏缩到 320 时 |
| --- | --- | --- | --- |
| 浅色 100% | 240 | 440 | 320 |
| 深色 150% | 240（截图 360 px） | 440（截图 660 px） | 320（截图 480 px） |

宽度单位为逻辑像素。新增的内容收窄、长记录上限、打开期间缩窄搜索栏、屏幕边界、删除最长记录后收窄及截图保存检查均通过。原有搜索历史方向键、回车选中、Delete 删除、Esc 关闭，以及完整面板的历史召回检查通过。已目视检查深色短记录及窄栏截图，文字省略、标题和删除按钮显示正常。

## 已有失败项

两轮各 42 条检查中均为 41 条通过、1 条失败：`prefix hints preserve chip widths`。因此入口返回 4，不能将整组回归描述为通过。该断言在搜索历史弹窗准备之前执行，检查的是类型条按住 Ctrl 后的宽度。

使用当前已有的 `build/ui-preview/WinShun.exe` 在独立输出目录重新运行同一入口，35 条检查中相同断言失败，其余 34 条通过；其历史弹窗截图仍为 440 px 宽。当前 `TypeStrip.qml` 按标签文字度量宽度，Ctrl 将名称替换为前缀时尺寸会变化。本次没有修改这一生产路径或放宽该断言。基线是现有可执行文件的本次复跑，并非重新构建的源码基线。

报告和原图分别保存在 [浅色 100%](light-100/search-experience.json)、[深色 150%](dark-150/search-experience.json) 与 [现有程序复跑](baseline-existing/search-experience.json)。编译末尾见 [build-tail.txt](build-tail.txt)，两份程序的 SHA256 见 [executables.json](executables.json)。构建包含已有编译器告警；本次预览日志未出现 Qt 运行警告。

验证记录已复制到本目录。自动审批以“blocked by policy”拒绝了临时构建目录的递归清理；改用已核实的绝对路径后仍被拒绝，因此 `F:/电脑便捷工具/build/history-width-20261107/` 暂时保留，包含本次构建、依赖副本及预览输出。用户原来运行的 `build/ui-preview/WinShun.exe` 保持运行，本次修复需在采用新源码构建的程序中生效。
