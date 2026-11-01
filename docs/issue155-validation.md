# #155：剪贴板窗口调整大小

需求：[GitHub #155](https://github.com/LingCore/WinShun/issues/155)。验证日期：2026-10-31，Windows 11、Qt 6.12.0、MSVC 2022、D3D11；基于 `760e9f2` 的工作区修改。

## 行为与实现

- 四边、四角使用现有 `WindowFrame` 的原生 Windows 缩放边框；缩放时 `Placement` 暂停自动定位，内容区实时跟随窗口尺寸。保持目标程序的前台焦点。
- 松手后保存改变过的维度：列表宽度继续使用历史库的 `listWidth`，新增 `windowHeight`。只改变宽度不会把原来的自动高度锁定；没有历史高度时继续按空间显示最多 8 行。
- 双击窗口边缘恢复列表宽度 480 和自动高度。最小宽度 360、高度保留 3 行及工具栏；上限受工作区约束。尺寸用逻辑像素保存。
- 预览展开后仍可拖外边缘，预览与列表保持独立宽度；内部预览分隔条保留原有行为。列表、预览的滚动条避开原生缩放区域。
- 小屏或光标旁空间不足时，仅限制本次显示尺寸；保存的偏好不变。重新获得空间后恢复偏好高度。预览显示宽度也受当前屏幕约束。
- 不增加定时轮询，不在拖动过程中写历史库，不启动第二份常规应用或修改用户历史。

## 已完成验证

独立构建位于 `build/issue155`，未退出或替换正在运行的 `build/release/WinShun.exe`。

```powershell
cmake --build build/issue155 --target WinShun tst_placement tst_displaygeometry -j 6
ctest --test-dir build/issue155 -R '^(tst_clipboardsizing|tst_placement|tst_displaygeometry)$' --output-on-failure
```

三个 CTest 均通过。`tst_clipboardsizing` 使用真实 `ClipboardWindow.qml`、`ClipboardPage.qml`、`Clipboard`、`Placement` 和 `WindowFrame`，配合临时历史库；剪贴板监听器不启动，不读写系统剪贴板内容。

100%、150%、200% 缩放各通过 22 项检查：八方向原生命中、宽度保存及自动高度保留、高度保存、角落缩放、取消后偏好不变、配置重读、隐藏后重开、预览展开/关闭、两处滚动条命中、临时空间不足及恢复、光标上方倒序布局、双击恢复默认、非法高度限制及截图保存。取消场景通过原生消息和恢复初始矩形模拟；另以真实 `SendInput` 拖动右下角，并检查前台窗口未变化。测试结束恢复鼠标位置。

检查结果与实际窗口截图：

- 默认比例：`build/issue155/tests/clipboard-sizing/clipboard-sizing.json`
- 150%：`build/issue155/scale-1.5/clipboard-sizing.json`
- 200%：`build/issue155/scale-2/clipboard-sizing.json`
- 每个目录的 `clipboard-sizing-preview.png`、`clipboard-sizing-flipped.png`、`clipboard-sizing-default.png`；已目视核对预览与倒序窗口的工具栏、列表和底栏。

原有 `--clipboard-send` 隔离交互检查也通过，覆盖双击粘贴、预览、菜单发送、快捷键循环选择等路径；退出码 0。修改的三个 QML 文件 `qmllint` 无警告，相关文件 `git diff --check` 通过。

共享边框路径另运行了 `--file-metadata` 搜索面板回归，退出码 0，覆盖列布局、滚动条与原生缩放区域的边界、拖动列和配置保存。

## 边界与发现

Windows 原生缩放边框取决于窗口的原生 DPI，不一定等于 `96 × Qt DPR`，尤其在 `QT_SCALE_FACTOR` 覆盖时。滚动条内缩改为按 `GetDpiForWindow`（窗口创建前取系统 DPI）的实际边框宽度换算，避免滚动条末端仍被识别成 `HTRIGHT`。

本轮没有验证 Windows 10、真实跨显示器 DPI 切换、断开显示器和触控/笔输入。没有提交、推送或发布新版本；README 中既有宣传截图未在本次开发任务中替换。
