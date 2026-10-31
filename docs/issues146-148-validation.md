# #146–#148 实现与验证

2026-10-30，处理 [#148 搜索学习排序](https://github.com/LingCore/WinShun/issues/148)、[#147 自定义预览快捷键](https://github.com/LingCore/WinShun/issues/147)、[#146 OneCommander 打开标签](https://github.com/LingCore/WinShun/issues/146)。基于 `20b9af9` 和工作区已有修改实现；没有提交、推送或发布。下述结果对应当前工作区，不代表其他未提交改动均由本次引入。

## 行为和实现

- **搜索学习**：原来“全部”先拼接应用、系统入口，再追加文件，文件历史无法跨越类型边界。现在复用现有 `History` 的关键词权重和 14 天半衰期，通过值快照传入搜索线程；合并候选后、截断前提升反复选择的结果。选择其他目标几次即可更新偏好，避免累计多年次数锁住旧结果。显式网站关键词、系统命令、收藏以及筛选、排序和当前目录优先规则继续走原有路径。
- **预览快捷键**：在“设置 → 结果列表 → 显示与预览”中录制，支持单独空格、功能键和未占用组合键；可取消、清除、恢复默认 `Alt+P`。录制器、配置读取、设置编辑及 AI 设置入口共用 `PreviewShortcut.h` 的规则，匹配使用缓存的 `QKeySequence`。保留常用编辑和导航键，检查与启动器、剪贴板、文件夹菜单全局键的冲突。空格模式下 `Shift+空格` 输入空格，输入法预编辑时不截取空格，长按不反复开关。菜单和底部提示跟随配置；剪贴板预览保持原来的键。
- **OneCommander**：活动目录继续使用作者公开的 `CurrentPathGet` UI Automation 接口；其他标签由本地会话文件与实时标签数、选中位置及活动目录共同核对。公开接口的 `RecentFolders` 包含已关闭目录，不能当作打开标签。匹配不唯一或会话未同步时只返回可确认的活动目录。UIA 查询移至 `EnumWindows` 返回后，保留窗口前后顺序，避免在 User32 枚举回调中进行跨进程 COM 查询。纯会话解析放入独立的 `OneCommanderTabs.h`，不改剪贴板、不靠切换标签读取目录。

中英文 README、使用说明、架构说明及英文界面翻译已同步。

## 环境和隔离

Windows 11，Qt 6.12.0，MSVC 2022 x64 Release。构建输出为 `build/release`；证据保留在 `build/issue146-148`。

预览验证使用生产 QML、真实控制器和临时配置，不注册全局快捷键，不启动剪贴板监听，也不读取用户配置。图片由测试在临时目录生成，实际经索引、搜索、选中和 `FilePreview` 解码。

OneCommander 使用官网下载的便携版 **3.108.0.0**，解压到 `build/issue146-148/OneCommander`。仅操作该独立进程和测试目录，完成后已结束该进程。用户安装版 Win顺 PID 74088 在验证后仍运行，没有覆盖安装目录或用户 `data`。

## 构建和自动测试

在 VS x64 开发环境下构建：

```powershell
cmake --build build/release --target tst_core tst_settings tst_aisettings tst_shelllaunch WinShun -j 6
```

最终增量构建成功。构建日志为 `build-acceptance.txt` 和只调整预览测试准备后的 `build-preview-acceptance.txt`。

| 测试 | 最终结果 | 证据（`build/issue146-148/` 下） |
| --- | --- | --- |
| `tst_core` 全组 | 236 passed，0 failed，1 skipped | `core-accepted.txt` |
| `tst_settings` 全组 | 28 passed，0 failed | `settings-accepted.txt` |
| `tst_aisettings` 全组 | 154 passed，0 failed | `ai-accepted.txt` |
| `tst_shelllaunch oneCommanderSessionTabs oneCommanderOpenTabs oneCommanderNavigation` | 6 passed，0 failed，0 skipped | `onecommander-accepted.txt` |

共 **424 passed、0 failed、1 skipped**（QtTest 统计包含初始化和清理）。唯一跳过项是需显式设置 `WINSHUN_TEST_REAL_VOLUMES=1` 的真实 MFT 读取，不涉及这三项功能。`git diff --check` 通过。

搜索回归使用实际临时 `FirefoxPortable.exe`、同名应用和系统入口，覆盖跨类型置顶、只返回一条、大小写、前缀、无关关键词、持久化、140 天衰减，以及从高频旧目标转向新目标；仅文件列表也验证了偏好更新。新排序测试在修复前有失败记录 `ranking-red.txt`。

OneCommander 实测准备 `Current-A`、后台标签 `Hidden-B` 和随后关闭的 `Other-Pane-C`。生产 `openFolders()` 同时找到 A、B，排除仍在 RecentFolders 中的 C，检查去重、剪贴板序列和前台窗口不变。导航测试分别运行生产 STA 和 MTA，验证含中文、空格、单引号的目录跳转、文件选中、新标签及恢复。纯解析测试覆盖过期计数、选中不符、隐藏面板、虚拟目录和无效 JSON。实测入口需显式提供隔离窗口和已准备的目录：

```powershell
$env:WINSHUN_ONECOMMANDER_TABS_ROOT = 'F:\电脑便捷工具\build\issue146-148'
$env:WINSHUN_ONECOMMANDER_TEST_WINDOW = '<独立测试窗口的 HWND>'
# 窗口由测试人员确认，不能指向用户工作窗口。
```

## 真实界面和预览验证

以下命令均通过 `Start-Process -Wait -PassThru` 等待程序退出并检查退出码。`<out>` 使用独立的输出目录；Qt bin 目录需在 PATH 中。

```text
WinShun.exe --preview-settings <out> --preview-hotkeys --renderer d3d11 --compact --dark --scale 1.5
WinShun.exe --preview-settings <out> --preview-hotkeys --renderer d3d11 --compact --english --scale 2
WinShun.exe --preview-results <out> --preview-hotkeys --dark --scale 1.5
```

- 设置页两次均 **退出码 0、0 warnings**：生产快捷键录制器、`SettingsEditor` 和持久化；覆盖空格、Esc 取消、保留键反馈、清除、全局键冲突及恢复默认。
- 结果页 **退出码 0、0 warnings**：生产 `Main.qml` 和真实 `FilePreview` 窗口；覆盖默认 Alt+P、空格、输入法预编辑、自动重复、Shift+空格、重新绑定、精确修饰键和禁用。同时通过原有图库的导航、选区、切换视图、缩放及最小窗口检查。
- 已查看中文深色 150%、英文浅色 200% 的 760×420 最小设置窗口，以及实际图片预览截图；说明能换行，录制器和恢复按钮无裁切。

证据已复制到 `build/issue146-148/accepted-ui/`：`settings-zh.log/png`、`settings-en.log/png`、`preview-results.log`、`preview-space.png`。初次独立结果页检查因测试入口未展开面板而失败；补齐入口准备后重跑通过，没有据此修改无关图库行为。较早的完整图库入口还记录了三条既有 `PanelRow.qml` 析构警告，因此本记录只将上述最终独立入口作为本次验收证据。

最后验证的 `WinShun.exe` SHA256：

```text
CDFA38A13536EE6198FF5D5D85A2D60807D469D90C38545458C52A74E9B5BFCA
```

## 验证边界

OneCommander Store 版、V4 和其他版本未实测；本次真实程序证据限于 3.108.0.0 便携版。会话文件可能短暂落后于公开导航接口，无法核对时按设计回退活动目录。未把历史 RecentFolders 当作实时标签。

预览按键通过本地 Qt 输入事件投递到真实界面；输入法覆盖的是实际 TextInput 预编辑状态，不代表逐种 Windows 输入法的人工物理键测试。OneCommander 测试覆盖对话框使用的生产目录枚举与导航桥接，未另行触发用户桌面的全局 Ctrl+G 手势。本次没有打包、发布或关闭 GitHub issue。
