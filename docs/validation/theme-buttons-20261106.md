# 2026-11-06：三套风格的按钮状态检查

本次针对纯图标选择与按钮反馈检查 `fluent`、`brand`、`tactile` 的浅色／深色版本，保持各自现有外观。

## 确认的问题与修改

- 默认风格的 `Segmented` 未区分文字分页与纯图标选择，导致搜索面板底部“列表／大图”也画下划线。`Hook.iconOnly` 现在明确这一语义；三套风格的选中图标用各自强调色，配中性选中底面，默认风格不再画图标下划线。文字分页保留选中标识。
- `FileViewSwitch` 未传递按下状态；现已接通。品牌风格的当前段有按下底色，轻拟物当前段的键面与图标按下位移同步，未选中标签按自身区域居中。
- 品牌与轻拟物的 `KeySurface` 未声明 `current`，所以 `RowAction` 的预览打开状态，以及快捷键／菜单入口的打开状态被忽略。现在鼠标移开后仍保留强调色图标和对应底面，关闭后恢复。
- 默认风格的 `KeySurface` 未提供前景色，某些宿主（如搜索面板侧栏开关）在蓝底选中状态仍使用普通深色图标。现在强调色实心底由部件提供白色前景，普通状态仍保留宿主原有颜色。

规则同步到 `docs/colors.md` 和 `src/app/qml/themes/README.md`。保留工作区已有的其他功能改动；验证过程未发布或替换正在运行的安装版。

## 验证

- Qt 6.12.0 / MSVC Release，构建 `WinShun`、`WinShunCaptureApp`、`WinShunOcrSearchApp` 成功。
- 新增隔离入口：`WinShun.exe --preview-results <输出目录> --style-buttons --style <风格> [--dark] --scale <倍率>`。使用生产 `FileViewSwitch`、`RowAction`、`FlatButton`、`PageTabs` 和真实 `Launcher`，在专用窗口中发送局部鼠标事件；模型数据与配置在临时目录，不读写系统剪贴板。
- 三套风格 × 浅／深 × 100%／150%，12 组全部退出 0。覆盖选中颜色、无图标下划线、文字分页标识、打开／关闭预览、白色主操作前景、选中工具按钮、鼠标按下／切换／移开、禁用截图和键盘焦点截图。
- 逐张检查六种配色的 150% 截图，并检查 `--edges` 输出；修改的默认风格图标选择底面使用对齐设备像素的内缩。记录仍包含未改的文字分页、按钮隐式宽度和轻拟物阴影的非整数几何，本次不宣称全界面像素审查通过。
- 真实 `Main.qml` 搜索框／搜索面板：默认浅色、品牌深色、轻拟物浅色，150%。品牌和轻拟物各通过现有 `--search-logo` 的 123 项检查。默认风格首轮 122/123：面板 Logo 悬停检查时指针已变成 `(-1,-1)`；相同二进制单独复查 123/123、0 警告。原始失败保留，不能据此宣称修复了 Logo 提示逻辑。
- 真实截图插件 `EditWindow`：同上三种风格组合，`--preview-capture-tools <目录> --edit-window --app-dir <隔离程序目录>` 均退出 0、0 警告，截图已核对。
- `git diff --check` 对本次涉及文件通过。没有运行全量测试。离屏预览不证明原生 DWM 阴影、所有系统强调色、所有窗口或真实桌面指针下的全部交互。

## 本地证据

独立程序与原始输出：`F:\wswt\ui-buttons-20261106\`。

- `matrix.json`：12 组检查与二进制 SHA256。
- `theme-buttons-comparison.png`：六种配色的实际组件截图汇总。
- `<style>-<light|dark>-<scale>/buttons.json`、`buttons-*.png`、`*.edges.txt`：各状态与几何记录。
- `main-*/search-logo.json`、`logo-panel.png`，默认风格复查在 `main-fluent-recheck/`。
- `editor-*/edit-window.png`、`preview.log`：截图插件真实编辑窗口。
- 构建日志：工作区 `build/ui-buttons-build.log`、`build/ui-buttons-final-build.log`、`build/ui-buttons-probe-build.log`。

主程序 SHA256：`01dc923bda151ed34045a22b06d9762801a92a5edf737e068538ca1d01efa14d`。
