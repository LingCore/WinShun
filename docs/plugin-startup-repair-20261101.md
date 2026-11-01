# 本地运行目录的插件启动修复（2026-11-01）

用户报告启动时出现“部分功能尚未恢复”和“插件已停用”。这是两个真实失败，此前菜单测试及主窗口启动检查没有覆盖它们。

## 证据与原因

- `plugins.log`：`winshun.switches` 反复提示 `cannot load provider, error 126`，随后连续退出三次，被宿主停用。运行目录没有 `Qt6Core.dll` 等 Qt DLL，主程序靠开发环境 PATH 启动；插件启动器通过 `SetDefaultDllDirectories` 和 `AddDllDirectory(WS_APP_DIR)` 限定搜索目录，不查 PATH。
- `WinShun.log`：`winshun.semantic`、`winshun.capture` 自动恢复返回“插件库里没有找到它”。用户配置已经启用这两项并确认权限，但可加载的用户／随包插件目录里缺少程序包；`library-plugins` 仅是构建包输出位置，宿主不会扫描它。
- 没有将插件库恢复失败推断成用户网络故障，也没有改动 DLL 搜索安全策略或插件最低版本字段。

## 本次修复

- 用当前 Qt 6.12 的 `windeployqt` 在独立 ASCII 路径生成运行依赖，再通过 PowerShell 原生文件操作复制到实际运行目录；Qt 工具直接接收此工作区的中文路径时，本次观察到日志扫描路径被截断、退出码仍为 0，不能只用退出码断言部署完整。
- 将已有本地截图、语义搜索包放入此开发运行目录的 `plugins`，保留用户的 `.data`、模型、设置及已确认权限。不下载或发布插件，不改版本号。
- 正常退出 Win顺 后完成复制并重启；被覆盖的运行文件和配置先备份，位置见 [修复记录](validation/plugin-startup-20261101/repair.json)。

## 当前验证

- 从启动子进程的 PATH 移除 Qt 开发目录后，主程序以及截图、语义搜索、开关插件均正常运行。
- 实际插件页显示上述三项为“运行中”；生产搜索的 `cap` 入口正常出现。
- 修复后观察 104 秒，当前启动日志没有 DLL 加载失败、连续退出停用或功能恢复失败。见 [插件日志](validation/plugin-startup-20261101/plugins-after.log)、[主程序日志](validation/plugin-startup-20261101/app-after.log) 和 [状态记录](validation/plugin-startup-20261101/verified.json)。
- 本次验证范围为启动、插件初始化和入口展示，不代表重新验证全部截图、录屏或语义模型搜索功能。

## 后续部署约束

不能把“主程序借助开发环境能启动”当成部署完整。启动检查必须覆盖真实宿主的插件状态和当前启动日志，并在没有 Qt 开发目录 PATH 的条件下确认依赖能找到。

插件运行位置以实际加载路径为准。本次截图插件加载自 `build/release/plugins/winshun.capture`，之后更新截图 DLL 必须同步这个位置，不能只更新根目录和 `library-plugins` 的构建包。完整发布仍遵循项目既有发布流程；本次没有提交、推送或发布。
