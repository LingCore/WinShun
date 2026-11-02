# #182 文件夹搜索同步选择验证

需求：[GitHub #182](https://github.com/LingCore/WinShun/issues/182)。验证日期：2026-11-02。

本次使用当前开发工作树（本地 HEAD 为 `71c749b`），保留全部既有未提交修改；没有提交、推送、替换已安装程序或发布版本。

## 行为与修复

当前开发代码已包含 #141 的资源管理器同步选择实现：从文件夹打字搜索入口打开后，上下键切换结果会在原标签页中选中原目录的直接子项，确保该项目可见，搜索框继续接收键盘输入。连续导航合并到最后一个选择；子目录和其他位置不自动改变原目录，可按原有 `Ctrl+Enter` 打开位置。收起搜索框、失去前台或原标签页失效后不继续同步。

本轮在真实资源管理器中复现了遗漏：查询从 `follow_1` 改为 `follow_2`，两次都只有一条结果，当前行始终为 0。搜索列表已显示 `follow_2.txt`，原窗口却仍选中 `follow_1.txt`。原因是原接线只监听当前行和条数变化，这两个值未变时不会触发同步。

修复在 `App` 中接入现有 `Launcher::resultsReplaced` 信号，结果替换完成后重新同步当前项，继续共用 60 ms 合并定时器、COM 工作线程和 `std::stop_token` 取消机制。定时器间隔使用 `std::chrono` 的 `60ms` 表达。没有增加快捷键、设置项或另一套同步实现。

## 验证结果

环境：Windows 11 专业版 10.0.26200、Qt 6.12.0、MSVC 2022 x64 Release、D3D11。

- [修复前真实窗口报告](red/runtime.json)：17 项中只有“条数和行号不变时替换当前结果”失败；报告记录了期望 `follow_2.txt`、实际仍为 `follow_1.txt`。
- [150% 浅色](light150/runtime.json)、[200% 深色](dark200/runtime.json)：各 17 项全部通过，运行时警告均为 0。同一回归在修复后读到的 Shell 选择路径与新结果一致。
- 真实窗口检查覆盖初始选择、Down、Up、快速连续导航、键盘焦点保留、窗口避让、同数量唯一结果替换、源目录不匹配、嵌套结果不导航、COM 查找后的请求取消，以及收起前取消待执行选择。方向键检查同时断言当前行确实变化，防止按键未生效却因旧选择一致而误通过。
- [相关 CTest](ctest.log)：`tst_launchernavigation`、`tst_searchnavigation`、`tst_displaygeometry`、`tst_core`、`tst_placement` 全部通过，合计 81.64 秒。
- [增量构建日志](build.log)：相关目标构建成功。日志中有任务开始前已有代码的 C4456 局部变量遮蔽警告；不将构建成功表述为零编译警告。
- [保护检查及源码哈希](preservation.json)：除本次列出的文件外，原有已跟踪差异保持一致。用户正在运行的 WinShun 进程保持原 PID 和启动时间。

验证通过生产 `App::showFolderSearch`、真实 Main.qml/Launcher/SearchEngine 和独立临时文件完成；通过 Shell 的 `IFolderView2` 独立读取选中项目及项目游标，不以搜索框高亮或信号发送成功代替资源管理器选中成功。按键通过本进程 `QKeyEvent` 送入真实 QML，未重新实测全局打字钩子、物理键盘、Windows 10 或第三方文件管理器，也未在报告者原机复测。

测试使用临时配置、进程内 HKCU 重定向及自己新建的资源管理器窗口，只关闭测试拥有且未被用户添加其他标签页的窗口，不修改用户文件、剪贴板或快捷键。

## 截图

150% 浅色：[搜索框](light150/typing-search.png)、[资源管理器选择](light150/typing-explorer.png)。

200% 深色：[搜索框](dark200/typing-search.png)、[资源管理器选择](dark200/typing-explorer.png)。

截图保存并人工查看了两个测试窗口区域；选择状态的判定以同轮 Shell 读取结果为准。

## 重跑

已构建目标使用以下入口；命令中的输出目录可自行指定。`--dark --scale 2` 可替换浅色 150% 参数。

```powershell
$env:PATH = 'C:\Qt\6.12.0\msvc2022_64\bin;' + $env:PATH
& .\build\release\WinShun.exe --probe-runtime .\docs\validation\issue182-rerun --folder-typing --settle-ms 500 --renderer d3d11 --scale 1.5
ctest --test-dir build/release -R '^tst_(launchernavigation|searchnavigation|placement|displaygeometry|core)$' --output-on-failure
```
