# #183：截图 F1 快捷键回归

2026-11-02，Windows 11、Qt 6.12.0、MSVC x64 Release、D3D11。[原问题及评论](https://github.com/LingCore/WinShun/issues/183)提到 F1 只在截图设置内有效，Ctrl+Alt+Z 可用，同时管理员运行时提示收藏圆盘辅助进程启动失败。

## 已确认的故障路径

0.17 将截图从主程序拆到普通权限的独立插件。即使主程序已提权，插件仍使用普通权限，过去由主程序注册的截图热键也随之降到了插件的权限级别。

独立 Win32 实验中，低权限监听进程的 F1 和 Ctrl+Alt+Z 均注册成功；当前台测试窗口权限更高时，只收到 Ctrl+Alt+Z，收不到 F1。同权限下两者正常。源码与结果见 [integrity-probe.cpp](integrity-probe.cpp)、[integrity-probe.txt](integrity-probe.txt)。实验只向自己的前台窗口发送输入。

生产链路对照使用真实 App、Plugin Host、截图 DLL 和临时配置，将测试插件的 manifest 改为 strict 沙箱，使插件处于 Low、宿主和前台测试窗口处于 Medium。这样不弹 UAC 就能重现同一权限边界。旧截图 DLL 的 F1 注册成功，但遮罩不出现：[旧实现失败报告](baseline-strict-settled/capture-hotkey-failed.json)。这不是报告者原机上的 Medium→High 实测，不能据此声称已复现其全部环境条件。

## 改动

- 新增原生 `PluginHotkeys` 注册器，由宿主按自己的现有权限代注册；插件继续以普通权限运行。按键只通过现有 JSON-RPC 通道通知插件，继续执行现有截图、全屏暂停及贴图动作。
- 用 `initialize.capabilities.hostHotkeys` 协商能力；有 `ui` 权限的常驻插件才能调用 `register_hotkeys`。旧宿主继续使用插件自行注册的路径。中英文插件协议与 SDK 开发说明已同步。
- 保留逐键冲突反馈；录制期间释放注册，忽略过期回复和过期按键；停用、退出及崩溃时释放，避免遗留按键占用。
- 实测发现并修复另一处生命周期竞争：常驻插件正在退出时重新启用，原来的 eager 启动会错过，需要再打开设置或查询才启动。现在记录重新启动请求，等旧进程退出后启动。回归测试修复前失败、修复后通过：[之前](race-before.txt)、[之后](race-after.txt)。

## 当前验证结果

| 验证 | 结果 | 证据 |
|---|---|---|
| 普通插件、中文、Medium→Medium | 24 项通过，0 warning | [报告](normal-verified/runtime.json) |
| strict 测试插件、中文、Medium→Low 转发 | 24 项通过，0 warning | [报告](strict-verified/runtime.json) |
| strict 测试插件、英文、Medium→Low 转发 | 24 项通过，0 warning | [报告](strict-english/runtime.json) |
| 热键注册器、设置、普通权限进程启动测试 | 通过 | [测试记录中的前三组](tests.txt) |
| 插件完整回归，含重新启用竞争测试 | 通过 | [完整插件测试](tests-plugin-final.txt) |
| 宿主热键工具的 ui 权限隔离 | 通过：未授权不调用，已授权可调用 | [测试](permission.txt) |
| 收藏圆盘普通权限辅助进程 | 启动、OLE 目标注册、收藏刷新、退出通过；0 warning | [报告](drop-pie/drop-pie-helper.json)、[日志](drop-pie/preview.log) |

24 项真实链路检查包含：启动后尚未打开截图设置时按 F1、关闭截图设置后按 F1、实际遮罩窗口及前台焦点、Esc 取消、停用释放、重新启用后新键生效、旧键释放。测试只向自己的前台窗口和截图遮罩发送键；每次截图均取消，不写剪贴板。单元测试另覆盖真实 Windows 热键冲突、不同插件所有权、非法请求保留旧注册、替换后丢弃排队的旧按键和析构释放。

截图窗口会排除自身被系统截图，所以 `capture-hotkey-overlay.png` 是早期验证留下的**前台测试底图**，不能当作遮罩像素的视觉证据；后续入口已改名为 `foreground-fixture.png`。本次未改动产品界面布局。

`tests.txt` 中保留了中间一次完整运行：前三组通过，新增竞争测试当时仍失败；之后修复竞争，`tests-plugin-final.txt` 为完整插件组的成功结果，`permission.txt` 为再新增权限用例的成功结果。没有把历史失败删掉或算成成功。

## 尚未确认与交付范围

收藏圆盘启动失败使用另一条 `startUnelevated` / Explorer 令牌路径。当前普通权限实测正常，未复现报告者的管理员启动失败，**没有针对这部分宣称生产修复**。也未直接测试实际提权的宿主及前台程序、Explorer 尚未启动或 Secondary Logon 不可用的场景。

使用独立 `build/issue183` 和 `build/issue183-stage` 验证，未关闭用户运行的 Win顺，未修改用户配置。现有工作区包含大量其他未提交修改，均保留。本次没有提交、推送、发布主程序或插件库，也没有评论或关闭 GitHub Issue；这是本地开发验证产物。修复进入正式发布时需同时更新宿主和截图插件，并按发布约定升插件版本、从干净提交构建及同步插件库。

复现命令（先将当前主程序及 Qt 部署到独立目录）：

```powershell
ctest --test-dir build/issue183 -C Release -R '^(tst_pluginhotkeys|tst_plugin|tst_settings|tst_shelllaunch)$' --output-on-failure
python tools/verify_capture_hotkeys.py --exe build/issue183-stage/WinShun.exe --capture-dir build/issue183 --out <output>
python tools/verify_capture_hotkeys.py --exe build/issue183-stage/WinShun.exe --capture-dir build/issue183 --out <output> --strict --english
```

原始 Win32 实验和旧 DLL 对照负责验证故障机制；当前真实链路与回归用例负责验证修复。测试 fixture 不是用户的浏览器，也没有将模拟输入称为物理键盘实测。
