# 截图保存位置与下拉菜单验证（2026-11-02）

## 实现行为

- 截图与图片编辑窗口共用保存分离按钮。保存按钮 / Ctrl+S 打开 Windows 原生 IFileSaveDialog，自动提供 PNG 文件名，用户选择目录和文件名；覆盖已有文件由系统确认。
- 取消另存为后保留冻结画面、选区和标注，不保存、不进入截图历史，也不记录最近选区。选择位置期间忽略重复保存及截图快捷键，避免重入；其他屏幕的遮罩暂时禁用。
- 旁边箭头打开单项菜单“保存到旧路径”，直接使用现有截图保存目录（未配置则系统“屏幕截图”目录），自动命名并避开已有文件。另存为不会更改这个设置。
- 菜单使用 Qt Quick Controls Menu，宽度随翻译内容增长；再点箭头、Esc 或点击外部均关闭。外部点击被菜单消费，不能误触底层复制按钮；关闭后 WASD 微调继续有效。
- 两条保存路径复用已有异步 PNG 写入；普通截图的 HDR 旁文件路径保持接入。本次没有发布或替换正在运行的插件。

## 自动测试

本地独立构建目录：`build/capture-keyboard-20261102`，Qt 6.12.0 / MSVC 2022 / Release。构建目标 `WinShunCaptureApp WinShunCapture tst_capture tst_editor` 成功，最终增量构建见 [build.txt](build.txt)。

```powershell
cmake --build build/capture-keyboard-20261102 --target WinShunCaptureApp WinShunCapture tst_capture tst_editor --parallel 6
ctest --test-dir build/capture-keyboard-20261102 -R '^(tst_capture|tst_editor)$' --output-on-failure
```

两套测试通过（[tests.txt](tests.txt)）。新增回归覆盖：另存为等待确认再记录选区、取消后继续编辑、默认保存转交所选路径、直接保存跳过路径选择。原有选区、WASD / 最近区域、编辑器测试同时通过。

## 真实原生保存流程

以下两轮均通过，程序退出码 0，未报告 QML 警告：

| 环境 | 日志 | 实际菜单 |
| --- | --- | --- |
| 中文、浅色、150% | [preview.log](native-zh-150-final/preview.log) | [save-menu.png](native-zh-150-final/save-menu.png) |
| 英文、深色、200% | [preview.log](native-en-dark-200-final/preview.log) | [save-menu.png](native-en-dark-200-final/save-menu.png) |

```powershell
build/capture-keyboard-20261102/WinShunCapture.exe --preview-capture-tools docs/validation/capture-save-20261102/native-zh-150-final --save-options --renderer d3d11 --scale 1.5 --app-dir 'C:\Program Files\WinShun'
build/capture-keyboard-20261102/WinShunCapture.exe --preview-capture-tools docs/validation/capture-save-20261102/native-en-dark-200-final --save-options --renderer d3d11 --scale 2 --english --dark --app-dir 'C:\Program Files\WinShun'
```

`CaptureSavePreview.cpp` 创建生成内容的测试窗口，使用生产 Capture、Grabber、QML 工具栏和真实 IFileSaveDialog。验证原生窗口属于当前遮罩；取消与重入不会清掉标注；保存至中文文件名；再次保存到同一测试文件并通过原生覆盖确认；随后通过菜单直接保存到隔离的旧目录。读取最终 PNG，与选区及标注合成图逐像素比较，两条路径均一致。菜单还验证了连续点击箭头、Esc、关闭后的 WASD，以及点击复制按钮所在位置只关闭菜单。仅三次确认保存进入历史，剪贴板回调始终未被调用。

测试文件只写到本记录下各自的 `old-path` / `chosen-path`，不读取或改写用户设置、不注册全局快捷键、不操作系统剪贴板；测试结束后原用户截图插件进程仍在运行。当前硬件为单屏 SDR，日志为 BitBlt 捕获（WGC 0）；多屏禁用和 HDR 旁文件本轮没有硬件端到端验证。图片编辑窗口使用单元测试验证路径选择及取消，原生窗口实测针对截图遮罩。

## 调试记录与证据范围

`native-zh-150-v1` 至 `v6` 是本地排查过程，未纳入提交，均由上面的 final 两轮结果替代。早期测试驱动误认现代文件框的文件名控件、把 Windows 无子 HWND 的 TaskDialog 覆盖提示当成经典按钮；已分别复用项目既有控件层级定位方式及 TaskDialog 消息处理。产品菜单修正了自定义背景后丢失默认宽度，以及 modal 菜单使用 CloseOnPressOutsideParent 导致箭头无法重复关闭的问题；最终采用显式内容宽度与 CloseOnPressOutside。

Qt 行为依据：[Menu](https://doc.qt.io/qt-6/qml-qtquick-controls-menu.html)、[Popup](https://doc.qt.io/qt-6/qml-qtquick-controls-popup.html)。验证时源文件和构建产物的摘要见 [sha256.json](sha256.json)；验证使用含有其他任务编辑的工作区，本次提交仅包含相关截图功能，未发布。
