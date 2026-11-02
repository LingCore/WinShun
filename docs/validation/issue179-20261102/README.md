# #179：剪贴板保留系统搜索焦点

2026-11-02，[原问题](https://github.com/LingCore/WinShun/issues/179)。本页保留第一阶段普通权限兼容修正的证据；后续独立 UIAccess 显示进程的实现和验证见 [第二阶段记录](../clipboard-uiaccess-20261102/README.md)。下面“本次”均指第一阶段，不代表后续实现状态。

## 根因和当前行为

- 剪贴板本来就是独立 `ClipboardWindow`。旧的 `App::showClipboard` 特意把 Shell 浮层排除在不抢焦点的按键转发之外，随后共用的 `bringToFront` 又发送 Escape 关闭 Start/Search。独立窗口本身不等于更高的系统窗口层级。
- Win+V 与自定义快捷键现在均保留原窗口焦点，复用 `KeyRouter` 转发输入。剪贴板显式激活路径也不再合成 Escape；用户主动要求输入法、编辑等而转移焦点时，系统浮层仍可能自行关闭。
- 普通权限路径使用临时避让。前台 `SearchHost.exe` 与可见 `StartMenuExperienceHost.exe` 侧栏可能跨进程，不能只枚举前台 PID。相邻点命中系统宿主后读取实际窗口 region，避免把透明的全屏宿主误认为全屏内容。
- 本机 `StartMenuExperienceHost` 的 `GetWindowRect`、DWM frame bounds、client bounds 都是 `[0,0,2560,1528]`，`GetWindowRgnBox` 为 `[425,193,2135,1528]`，后者才覆盖实际 Start 与手机侧栏。使用其相对窗口左上角的设备像素坐标转换到屏幕坐标。无 region 时回退有限窗口矩形，忽略无边界的透明工作区宿主。
- 两侧放不下已保存宽度时，剪贴板临时缩窄，最低 240 逻辑像素；普通打开恢复原设置。该限制不写入用户宽度偏好。可用空间小于这个下限时无法保证完全避让；手动展开较宽预览也不等同于更高层覆盖。

## Grok 调研与一手资料核对

按用户要求使用已登录浏览器中的 Grok，见[调研及更正对话](https://grok.com/c/b8b53b25-02f8-4cb6-9759-975a0d3f0e70)。其首答错误地把 `UIAccess` 排在 Start 之下；核对原文后要求更正，Grok 第二答承认顺序错误。不能按枚举值 `2 < 6` 推断窗口层级顺序。

- [微软 UIAccess 文档](https://learn.microsoft.com/en-us/windows/win32/winauto/uiauto-securityoverview)说明具备相应权限的应用可保持最上层，标准部署涉及签名、受保护路径和 manifest，并限制其适用场景。
- [ADeltaX 的原始窗口层级研究](https://blog.adeltax.com/window-z-order-in-windows-10/)将 `ZBID_UIACCESS` 排在最高层；普通桌面窗口的 topmost 只在自身 band 中排序。内部 band API 不属于公开稳定契约。
- [GetWindowRgnBox 官方文档](https://learn.microsoft.com/en-us/windows/win32/api/winuser/nf-winuser-getwindowrgnbox)定义了 region 包围矩形和相对窗口原点的设备单位，实际 region 是否存在需检查返回值，不能把 Grok 的“通常没有 region”当成本机事实。
- 本项目主进程 manifest 为 `uiAccess=false`；现有服务只给键盘监听、文件对话框导航辅助进程授予 UIAccess。它不会自动提升主进程创建的 Qt 窗口。真正覆盖 Start/Search 需要由具备 UIAccess 的显示进程创建/承载剪贴板 UI；本次没有修改服务、权限、manifest 或跨进程嵌窗架构。Windows 原生 Win+V 使用系统输入界面；本次未测得其具体 band，不将猜测的 band 值写作事实。

## 验证

Windows 11、Qt 6.12.0、MSVC 2022 x64 Release。使用独立运行时测试配置和本进程 HKCU 重定向；用户正在运行的程序未退出，工作区其他改动未回退，未提交或推送。

| 检查 | 结果与证据 |
| --- | --- |
| 构建 | [相关目标](build.txt)、[最终增量构建](build-final.txt)通过；有一条其他代码既有的 `App.cpp` 局部 `keys` 遮蔽警告 |
| 位置计算 | [27 项通过](placement.txt)，包含居中/左侧系统面板及负坐标工作区；这些是几何回归，不冒充真实多显示器测试 |
| 原生窗口可见性 | [6 项通过](windowvisibility.txt)，包含 `STARTF_USESHOWWINDOW/SW_HIDE` 首次显示边界 |
| 实际键盘钩子 | [6 项通过](keyrouter-live.txt)，`WINSHUN_KEYROUTER_LIVE=1`，自有接收窗口 |
| 真实 App + 独立接收进程 | [全部通过](fixture.json)，两种快捷键入口、独立侧栏避让、9 点可点击命中、字母搜索转发、不发送 Escape、不丢失激活、粘贴目标和宽度偏好保留 |
| 原生 Start + 手机侧栏，应用 100% 缩放 | [全部通过](native-100.json)，剪贴板物理边界 `[2147,306,2560,945]`，Shell 区域 `[425,193,2135,1528]` |
| 原生 Start + 手机侧栏，应用 150% 缩放 | [全部通过](native-150.json)，剪贴板临时宽度约 271 逻辑像素、物理边界 `[0,306,407,1265]`，保存的列表宽度仍为 480；最终增量只补充重新接管其他目标时清除临时宽度，不改变此测试路径 |

原生验证使用 D3D11，与生产窗口相同的无激活显示路径；均确认 Qt/native 可见、未 cloaked、系统窗口保留前台、按键路由启用、原粘贴目标保留、面板与系统区域不相交、面板九个内部点实际命中自身，关闭只收起剪贴板。已查看真实桌面合成截图；含用户桌面信息的截图只留在忽略的 `build/issue179-20261102/`，不复制到仓库。

快捷键探针向生产 `WinShun.MessageWindow` 发送相同 `WM_HOTKEY` 消息（ID 3/4），不是物理按下 Windows 键或验证全局注册。原生测试用 `SC_TASKLIST` 打开 Start；两次 150% 测试因系统菜单未成为前台而在前置检查失败，保留在 build 中，不当作产品失败或通过证据。前置条件满足后通过。150% 是进程绘制比例，不修改 Windows 显示设置。

测试没有复制/粘贴动作或写入系统剪贴板；真实 App 启动可能将当前剪贴板读入独立测试数据目录。本次不声称已完成向原生搜索框实际粘贴，也不以独立接收进程代替真实 Shell 验证。源码和最终二进制指纹见 [fingerprints.json](fingerprints.json)。

复现（Qt DLL 在 PATH）：

```powershell
Copy-Item build/release/tst_keyrouter.exe build/issue179-20261102/fixture/SearchHost.exe
build/release/WinShun.exe --probe-runtime build/issue179-recheck-fixture --clipboard-shell-probe build/issue179-20261102/fixture/SearchHost.exe --renderer d3d11 --settle-ms 100
build/release/WinShun.exe --probe-runtime build/issue179-recheck-native --clipboard-shell-probe native --renderer d3d11 --settle-ms 100 --scale 1.5
```
