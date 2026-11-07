# 液态玻璃下拉框与控件检查

日期：2026-11-07。Windows 11、Qt 6.12.0 MSVC 2022 x64 Release、D3D11。
验证对象为当前工作区构建，包含此前尚未提交的修改；不是已发布版本。
最终 `build/ui-preview/WinShun.exe` SHA256：`ADFC93DC6641FFA8AEB4699AC180A251477EC361EB321FCA9C00EE1DB616576E`。

## 问题和实现

- 字体框只替换了外层背景。Qt Basic/ComboBox.qml 的原生可编辑 TextField 在 `editable && !flat` 时还绘制内层 Rectangle，因此玻璃表面出现两块不一致的矩形。以本机 Qt 6.12.0 的 Basic/ComboBox.qml 源码核实。
- 新增共享 `ChoiceBox`，通过 `flat: true` 关闭内层背景，外层使用现有 `Theme.inputField`；保留 Qt 编辑器的输入、选择与校验。字体选择框、打开 Win顺 的双击动作框、内容报告的范围/格式框共同使用它。焦点覆盖整框，展开时保持焦点标记，长项省略，菜单继续使用 `ComboPopup`。双击动作框保留此前修好的左侧 12 px 对齐。
- 抽查发现 InputBox 和 ToggleSwitch 在液态玻璃中禁用时仍保持正常外观。补充禁用透明度，保留输入值/开关值和原有 busy 状态。优先读取部件的 `disabledOpacity`，尊重品牌/拟物主题自己绘制的禁用文字或底色，避免重复淡化。
- 检查了报告页复选框、设置滑块和分段控件；本轮未观察到与字体框相同的双背景问题，没有为它们另建样式。

## 验证

- 主程序、截图插件和 OCR 搜索插件构建通过，见 `build-verified.log`。编译器有 Qt 头文件 C4702 和原有 Preview.cpp 局部变量遮蔽警告；不是零编译警告声明。
- 最终共享控件的液态玻璃、品牌、拟物圆角深色 150% 下拉框回归 **3/3 通过**，见 `verified-combos.txt`。生产 QML 断言检查单层背景、12 px 文本起点、整框焦点、禁用外观、展开焦点、菜单边界与间距。液态玻璃详情见 `liquid-combo.log`。
- InputBox/ToggleSwitch 新回归先复现 **2 项失败**（`disabled-before.txt`），修复后 **2 项通过**（`disabled-verified.txt`，另含 init/cleanup）。测试还检查禁用前后保留值、重新启用恢复正常透明度。
- 源码部件组合检查 **12 passed**，包含深浅色、圆/直角、实色/原生透明配置（`appearance.txt`）。这是部件验证，不能替代真实 DWM 渲染。
- 浅色字体行为探针退出码 0、无 QML 警告：输入字体、持久化、即时示例、未知字体恢复、默认字体以及独立剪贴板字体继承，见 `font-behavior.log`。
- 英文浅色、实际 **760×720** 窗口探针退出码 0、无 QML 警告，见 `english-760.log` 和 `font-english-760.png`。窄窗口中字体行正常换行容纳选择框。
- 实际内容面板探针退出码 0，`content-pane.json` 的 **88 个布尔检查全部为 true**；包含范围/格式下拉展开，以及原有报告导出和取消路径。截图为 `report-scope.png`、`report-format.png`。
- 独立真实窗口手动检查：原生 Acrylic 背景下字体框为连续表面，整框焦点明确，菜单可读，Escape 关闭菜单。原始截图 `native-field.jpg`、`native-menu.jpg`。离屏 PNG 不用作 DWM 外框或桌面透明的证明。

复现主要检查（先把 Qt 6.12.0 的 bin 加入 PATH，QT_PLUGIN_PATH 指向同一安装的 plugins）：

```powershell
ctest --test-dir build/ui-preview -R '^tst_combopopup_(liquid|brand|tactile)_round$' --output-on-failure
./build/ui-preview/tst_appearance.exe disabledFields liquidParts
# GUI 程序用 Start-Process -Wait -PassThru 等待并核对退出码：
# WinShun.exe --preview-settings <任务目录> --combo-popup --style liquid --renderer d3d11 --english --width 760
```

## 范围与产物

这是共享控件和相关页面的定向检查，并非全应用所有控件、所有插件、Windows 10 或高对比度模式的完整认证。未发布、未提交推送，也未替换或关闭用户正在运行的 Win顺。

任务构建与中间诊断位于 `build/liquid-controls-audit-20261107/`，最终构建仍在项目既有 `build/ui-preview/`。独立预览进程已退出。原有 QFileSystemWatcher 清理路径报拒绝访问，留下一份仅含合成测试数据的隔离配置：`build/liquid-controls-audit-20261107/temp/WinShun-preview-62a575fb/clipboard/clipboard.db`，作为清理失败诊断原件保留，不是用户配置。真实窗口截图和精选验证日志已保存在本目录；用户现有运行实例与数据未改动。
