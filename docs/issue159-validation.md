# #159：隐藏桌面图标后无法双击呼出文件夹菜单

日期：2026-11-01。[原反馈](https://github.com/LingCore/WinShun/issues/159) 的操作是取消桌面右键“查看 → 显示桌面图标”，随后双击空白处不弹菜单；恢复显示图标时可用。

## 原因与修复

旧的 `FolderViewHit::classify` 只接受 `SHELLDLL_DefView` 下的图标列表 `SysListView32`。隐藏图标后，鼠标命中可能变为 `SHELLDLL_DefView` 本身或桌面宿主 `Progman` / `WorkerW`，这些输入原先都返回 `Surface::None`，因此尚未进入空白判断便退出。

文件夹菜单现在使用独立的 `classifyMenu`，在保留已有列表识别的同时接受桌面背景。没有桌面视图的背景宿主还须属于真实 Shell；非主 Shell 宿主须覆盖所在显示器，避免把普通窗口当成背景。

背景空白检查在工作线程重新核对可见性及 `WindowFromPoint`，图标恢复显示或其他窗口／编辑控件盖住原点击点时拒绝旧命中。图标列表仍走原来的 MSAA 判断；打字搜索和粘贴焦点继续用原识别 API。桌面开关关闭、Listary 让路及已有菜单取消机制保留。桌面焦点在真实 Shell 的两个宿主间转移时，前台检查允许它们归为同一桌面。

## 验证与边界

- 原识别 API 对自建桌面背景窗口的 [四个用例均失败](validation/issue159-20261101/red.txt)，失败点为返回 None、预期 Desktop；使用新入口后通过。
- [文件夹菜单回归 12 passed、0 failed、0 skipped](validation/issue159-20261101/green.txt)。新增用例覆盖 Progman / WorkerW、图标列表隐藏后命中根窗口或 DefView、桌面开关关闭、图标列表恢复显示、编辑控件遮挡、同名非 Shell 窗口及焦点识别隔离。既有用例覆盖命令执行、长列表及亮暗／直角／圆角的真实 QML 菜单。
- [Opus 焦点回归 3 项通过](validation/issue159-20261101/opus.txt)。[主程序及相关测试构建成功](validation/issue159-20261101/build.log)，`git diff --check` 通过。没有要求或声称整个已有工程零编译警告。
- 测试使用自建原生窗口重建相关层级，不切换用户真实桌面图标设置。因此已经验证的是生产识别和空白检查的回归，以及原有 QML 菜单；没有将其冒充“实际 Explorer 中隐藏图标 → 原始鼠标双击 → 菜单显示”的完整实机验证。报告者未提供 Windows 版本，其系统仍待复测。
- 主程序已备份、正常重启并同步到当前本地运行目录，移除 Qt 开发目录 PATH 后启动，见 [产物记录](validation/issue159-20261101/runtime.json)。保留之前补齐的 Qt 依赖与插件包，未覆盖用户配置和数据；没有发布、提交、推送或回复／关闭 #159。

早期尝试用自建桌面窗口做前台菜单集成检查时，前台窗口条件与测试窗口生命周期不可靠；这些输出不作为修复前后证据。上面的 red / green 记录针对明确的识别边界，前台 GUI 行为由已有菜单回归验证。

## 后续回复

用户明确授权后，已以 LingCore 账号 [回复 #159](https://github.com/LingCore/WinShun/issues/159#issuecomment-6553708536)，并读回核对。说明本地开发版改动、测试范围与尚未正式发布的状态，向报告者请求补充 Windows 版本。上文“未回复”的记录指初次验证阶段；未关闭 issue。
