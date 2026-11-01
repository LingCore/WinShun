# PR #152：优先贴剪贴板图片

验证日期：2026-10-31。根据 [PR #152](https://github.com/LingCore/WinShun/pull/152) 的功能意图，适配本地最新的独立截图插件架构。验收基线为 `d6dea423005128a47f96506062f5107cd1200db3`，Windows 11、Qt 6.12.0、MSVC Release、D3D11。

## 最终行为与实现

- 托盘菜单、全局贴图快捷键、搜索结果共用 `CaptureApp::pinCurrentImage`。即使没有截过图，也能贴剪贴板图片；没有可用图片时回到最近截图；剪贴板被占用、图片损坏和两者皆空分别提示。
- 读取图片、解码、平滑缩放和像素格式转换都在工作线程完成。触发时保存鼠标位置和屏幕工作区，外来图片缩小到工作区的 90% 以内；普通图片不会被标记为截图历史。
- 普通截图产生独立来源 ID，随宿主的实际剪贴板写入一起写入原生格式 `WinShun.ImageOrigin`。复制尚未完成时用剪贴板序列号基线识别；复制完成后用来源 ID 识别。最近截图因此仍通过 `Capture::pinLast` 恢复原位置和可编辑标注，不依赖 IPC 返回时间或像素相等。
- 在工作线程返回期间又产生截图时重新读取，避免把旧截图套到新状态上。长截图复制显式不携带上一张普通截图的 ID。
- 使用现有图片解码器和 WIC 文件读取路径。优先尝试编码图片、DIBV5、DIB，再尝试文件列表中的可读图片；损坏的首选格式可以回退，TIFF 不依赖 Qt 图片插件支持。
- 保留原快捷键存储字段及菜单动作 ID，只更新用户看见的名称和说明。新增的 `clipboard_add_image.origin` 是可选参数，旧调用不需要修改；协议说明见 [plugins.md](plugins.md)。

## 构建与自动测试

在 `build/pr152-fix-20261031/source` 冻结基线和本次修改，单独构建、部署到临时目录。没有把其他会话的引导页、搜索和设置修改纳入本次验收；共享的 `App.cpp` 只提取本次 `clipboard_add_image` 工具块，`tests/CMakeLists.txt` 只追加本次测试目标。

| 验证 | 结果 | 证据（相对仓库根目录） |
|---|---|---|
| Release 完整构建及临时部署 | 通过 | `build/pr152-fix-20261031/build-final-verified.txt`、`deploy.txt` |
| 全量 CTest | 105/105 通过，256.81 秒 | `build/pr152-fix-20261031/ctest.txt`、`ctest.xml` |
| 最终相关回归 | 8/8 通过 | `build/pr152-fix-20261031/focused-final.txt` |
| 原生剪贴板用例 | 17 通过，0 失败、0 跳过；计入上述 105 个测试目标之一 | `build/pr152-fix-20261031/pinclipboard-final.txt` |
| 用户剪贴板隔离检查 | 交互剪贴板序列号前后均为 3865 | `build/pr152-fix-20261031/clipboard-isolation.json` |

原生测试使用非交互窗口站、独立桌面及站内互斥锁。进入测试前要求该站剪贴板为空，不清空已有内容；不会在用户交互窗口站运行写入测试。覆盖首次贴位图、DIB/DIBV5、损坏 V5 回退、异步复制前后来源识别、外部同像素图片、旧截图 ID、Qt 备用 MIME 写入、文件列表里的中文路径 TIFF、不可读图片、剪贴板占用，以及负坐标和 100%–200% 缩放下的定位与大小限制。

可复跑相关测试：

```powershell
ctest --test-dir build/pr152-fix-20261031/source/build/release --output-on-failure --parallel 2
ctest --test-dir build/pr152-fix-20261031/source/build/release -R '^tst_(pinclipboard|capture|capturesettings|annotate|editor|stage7|clipimage|cliplifecycle)$' --output-on-failure --parallel 2
```

六项 Qt/C++ 静态专项审查的问题已修复并复查；编译仍有既有警告，不宣称全项目零编译警告。源码快照及校验值见 `build/pr152-fix-20261031/base.txt`、`owned-files.txt` 和 `source-manifest.json`。

## 实际窗口与捕获验证

- 使用真实截图设置窗口，中文浅色 150% 和英文深色 125% 均定位到“贴图的快捷键”这一行；检查可见范围，文字完整、无裁切，两次均为 0 个 Qt 警告。截图及日志分别在 `build/pr152-fix-20261031/ui-pin-zh/` 和 `ui-pin-en/`。验证入口为 `--preview-capture-settings <目录> --pin-image`。
- 通过真实 WGC 路径先完成普通截图，再完成长截图复制：普通截图回调携带当前来源 ID，后续长截图回调的来源为空；输出为预期的 600×2000，最大像素通道差为 0，0 失败。日志在 `build/pr152-fix-20261031/long-copy/preview.log`，入口参数为 `--preview-long-capture <目录> --app-dir <隔离部署目录> --renderer d3d11 --copy --preceding-capture`。
- 长截图验证拦截的是测试窗口的复制回调，不写用户剪贴板。补齐该预览入口缺失的宿主 QML 导入路径后，复跑通过。

## 验证边界

原生剪贴板、实际捕获回调和设置窗口分别验证，未宣称已经完成普通宿主中“全局热键 → 插件 IPC → 系统剪贴板 → 贴图窗口”的整链路人工验收。多显示器负坐标和缩放组合由自动几何用例覆盖，实际 WGC 验证是在单显示器完成。其他并行修改不包含在上述通过结论中。

初次验收时，改动保留在本地，未提交、推送、合并 PR 或发布版本，也没有退出用户正在运行的程序。

后续状态（2026-10-31）：按用户要求，先将作者原版 PR #152 合并到远端 `main`，合并提交为 [`8a90cc2343fc800cde1194aa7b4b9491e32852b2`](https://github.com/LingCore/WinShun/commit/8a90cc2343fc800cde1194aa7b4b9491e32852b2)，已核对 PR 的 `merged` 状态和远端分支提交。本记录描述的适配修复仍在本地，未包含在该次合并中；上述 105 项通过结论针对本地修复快照，不作为作者原版已全部通过的证明。没有拉取、重置共享工作区或发布版本。
