# 搜索体验验证：#161、#168、#169

2026-11-01，Windows 11 x64，Qt 6.12.0 / MSVC 2022，Release，D3D11。
代码基线：`07f9d3d5065a1e189a0c631cf003786aecc4fefd` 加工作区改动。

## 改动

- [#161](https://github.com/LingCore/WinShun/issues/161)：一次最近打开即可提升后续匹配结果；应用/文件的全局使用权重参与排序，中文、全拼和首字母不必分别学习。关键词关系由前缀扩展到双向包含。保留原历史格式、14 天半衰期、实体使用频率及明确关键词选择的优先级；过期关键词记录不会遮蔽最近的实体偏好。不让不匹配的实体混入结果。
- [#168](https://github.com/LingCore/WinShun/issues/168)：在“全部”输入“设置”“配置”“settings”“config”或“WinShun”可找到内置设置动作；`Ctrl+,` 打开设置。关闭应用或 Windows 系统设置搜索后仍可使用。复用现有设置窗口入口，显示齿轮、可读说明和适用的打开操作。
- [#169](https://github.com/LingCore/WinShun/issues/169)：`Ctrl+J/N` 向下、`Ctrl+K/P` 向上、`Ctrl+U/D` 翻页。启动器、面板和对话框列表共享映射，复用原有边界、滚动和选中行为，组字和 AltGr 不被当成导航。
- 冲突处理：当前文件夹切换改为 `Ctrl+Shift+D`；旧的 `Ctrl+P` 预览配置加载后回到默认 `Alt+P`，仍可自定义为 `Ctrl+Shift+P`。同步设置提示、英文翻译、README 和使用说明。

## 当前验证结果

- 主程序及 `tst_core`、`tst_settings`、`tst_aisettings` 编译成功：[构建日志](build.log)。
- 完整 `tst_core`：**239 通过，0 失败，3 跳过**：[日志](core.txt)。覆盖一次打开、中文/拼音/首字母切换、结果限额为 1、持久化重载、包含关系、实体频率、衰减、清除历史、不匹配查询，以及既有应用、文件夹、系统开关排序回归。
- `tst_settings`、`tst_aisettings`、`tst_searchnavigation` 均通过：[日志](settings-and-navigation.txt)。补充图标后再次通过导航测试：[最终日志](navigation-final.txt)。
- 真实生产 QML：中文浅色 100% 和英文深色 150% 各 **118 项启动器检查、23 项对话框检查** 全通过。检查搜索框/面板、首尾行、翻页、长按、焦点、输入法、AltGr、文件夹快捷键冲突、设置动作和菜单；两次运行日志均为 0 warning。证据：[中文启动器](search-navigation.json)、[中文对话框](dialog-navigation.json)、[英文启动器](150-en-search-navigation.json)、[英文对话框](150-en-dialog-navigation.json)、[英文运行日志](150-en-preview.log)。
- 检视实际渲染截图：[中文](search-settings-entry.png)、[英文 150%](150-en-search-settings-entry.png)。
- `qmllint` 返回 0：[日志](qml-lint.log)。仍报告部分既有作用域/类型提示及直接检查单例源文件时的 qmldir 提示；生成的模块 qmldir 已包含 `singleton SearchKeys`，生产模块加载和上述交互检查通过。
- `git diff --check` 通过。

## 验证边界与复现

构建、配置、索引、历史和图片夹具都在 `build/search-improvements-20261101/` 的隔离副本中。保留测试开始前已有的 OCR 改动；排除当时并行修改中的 WindowMask/ShaderTools、appearance 测试及合成窗口帧预览接口。没有终止或替换用户正在运行的安装版，也没有读写用户的历史数据。22 个涉及本次行为的源文件与工作区一致，另核对了 Preview 仅有上述并行接口差异：[源文件指纹](source-fingerprints.json)。这些结果不代表并行窗口模块已通过整体集成验证。

核心测试跳过项为需显式启用的真实 MFT 读取，以及依赖本次未构建的 `WinShunExtract.exe` 的两个文档提取测试。UI 测试使用本进程 Qt 按键和输入法事件，设置动作验证到 `settingsRequested` 信号；现有 `App::showSettings` 连接已核对。没有向用户窗口发送系统输入，也没有实际启动用户安装的“火绒强力卸载”。

复现命令（在仓库根目录，Qt bin 在 PATH）：

```powershell
cmake --build build/search-improvements-20261101/bin --target WinShun tst_core tst_settings tst_aisettings
build/search-improvements-20261101/bin/tst_core.exe -o build/search-improvements-core-final.txt,txt
ctest --test-dir build/search-improvements-20261101/bin -R '^(tst_settings|tst_aisettings|tst_searchnavigation)$' --output-on-failure
build/search-improvements-20261101/bin/WinShun.exe --preview-results build/search-improvements-20261101/ui-150-en --search-navigation --renderer d3d11 --scale 1.5 --english --dark
```
