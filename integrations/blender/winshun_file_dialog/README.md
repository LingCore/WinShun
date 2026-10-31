# Win顺文件对话框 / WinShun File Dialog

通过 Blender 官方扩展机制接入 Win顺。Windows x64，Blender 4.2+；目前实机验证版本为 Blender 5.2。

请配合支持 Blender 文件对话框的 Win顺版本使用；早于此功能的安装版需要更新。

1. Blender → 编辑 → 偏好设置 → 获取扩展，右上角菜单选择「从磁盘安装」，选择本扩展 ZIP 并启用。
2. Win顺 → 设置 → 文件对话框，启用搜索栏；需要快速切换到资源管理器目录时启用 Ctrl+G。
3. 在 Blender 打开独立的打开、另存为或导入窗口。下方出现 Win顺搜索栏；选中文件或目录后跳转。打开窗口可用 Ctrl+Enter 确认；保存与导出仍在 Blender 原生窗口确认。

停用扩展即可撤销接入，不改变 Blender 的快捷键或启动文件，不使用系统剪贴板。仅接管带文件操作的独立文件浏览窗口，不接管主界面内嵌的文件浏览编辑器、资产浏览器或其他弹窗。遇到无法唯一对应的同尺寸多窗口时跳过，避免跳错窗口。

通信只在本机当前账户与对应 Blender 进程之间进行。扩展使用公开的 `FileSelectParams` 与 `bpy.ops.file` API；路径检查在后台进行，操作由 Blender 主线程执行。超时请求不会延迟执行。

扩展的最低版本声明不等于所有 Blender 版本和第三方导入器均已验证。超过 255 个 UTF-16 单元的目录仍可跳转，但不显示「当前目录」快捷提示。

## English

Install the ZIP through Blender Preferences → Get Extensions → Install from Disk, then enable it. Enable WinShun's file-dialog search bar and, optionally, Ctrl+G. Separate Open, Save As and Import windows use the existing WinShun search bar. Ctrl+Enter can confirm an Open operation; Save/Export confirmation stays in Blender.

Use a WinShun build that includes this integration; earlier installed versions need an update.

Windows x64 and Blender 4.2+ are declared; Blender 5.2 is the version tested locally. Embedded file/asset browsers and ambiguous multiple windows are excluded. Disable the extension to disconnect. No clipboard changes, key simulation or startup-file changes are involved. A long path still navigates, but its optional current-folder hint is unavailable beyond Windows' 255 UTF-16-unit atom limit.

See the repository's `docs/dialog-compatibility.md` for implementation evidence and reproducible tests.
