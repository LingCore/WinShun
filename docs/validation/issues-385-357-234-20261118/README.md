# #385、#357 后续、#234 后续 验证记录（2026-11-18）

环境：Hyper-V 虚拟机 Windows 10 22H2（19045，1024×768，100%），WinShun 0.36.0 安装版换上新编的 `WinShun.exe`；`Renderer=d3d11`（WARP），资源管理器直接打字搜索开、字母直达开（键 `auto`）。按键和点击都走 Hyper-V 合成键盘鼠标（对客体就是真实输入）。

| 问题 | 对照（旧版） | 新版 |
| --- | --- | --- |
| #385 资源管理器里呼出剪贴板（Ctrl+Alt+V）后按 `1` | 0.36.0：剪贴板消失，文件夹搜索框弹出、里面是“1”（[385-before-0360.png](385-before-0360.png)） | “1”进了剪贴板的搜索，过滤到 10 条（[385-after.png](385-after.png)） |
| #385 同一原因：文件夹菜单（热键打开）里打字 | — | 按 `h` 菜单留着，没有弹出搜索框 |
| #357 剪贴板 `Shift`+字母 | — | 轻按 Alt 出字母，`Shift+S`、`Shift+D` 选中两行（角标 1、2），字母还在，顶部显示“已选 2 条…Enter 粘贴”（[357-clipboard-shift-letters.png](357-clipboard-shift-letters.png)） |
| #357 对话框搜索栏的数字键和字母直达 | 0.37.0：数字只在按住 Ctrl 时出现，没有字母 | 数字常显在行左边（和搜索框一致；Ctrl+1 被截图插件占用，显示删除线），轻按 Ctrl 出 A S D F G，`Shift+D` 只选中第 3 行、搜索词不变（[357-dialog-letters-shiftd.png](357-dialog-letters-shiftd.png)） |
| #357 资源管理器打字搜索框被 Windows 拒绝前台（`WINSHUN_TEST_REFUSE_FRONT=1`，日志确认“keys routed”） | 0.37.0：轻按 Ctrl 没有字母，`Shift+S` 把“S”打进了搜索框（[357-routed-before-0370.png](357-routed-before-0370.png)） | 轻按 Ctrl 出字母，`Shift+S` 选中第 2 行，↓ 正常移动（[357-routed-after.png](357-routed-after.png)） |
| #234 对话框搜索栏列表展开时黑一下 | 0.37.0：点进搜索栏、列表展开那一帧下面一大块黑（新增黑像素 44,532，[234-dialog-bar-flash-0370.png](234-dialog-bar-flash-0370.png)） | 同一帧新增黑像素 136（文字），之后打字列表变化的各帧 0 |

自动检查：`WinShun.exe --preview-results <dir> --search-navigation` 的对话框部分 43 项全过，含新增 5 项（行左侧数字、字母出现且输入框只读、`Shift`+字母只选中、`Esc` 只收起字母、超出可见行的字母不动作）。同一组检查里“文件画廊重命名”两项第一次失败、重跑通过，0.37.0 上也通过，属于时序偶发，与本次改动无关。

没测到的：Windows 11 上的剪贴板 `Shift`+字母和对话框字母（逻辑与 Windows 10 相同，没有平台分支）；粘贴为文件开着时，剪贴板盖在资源管理器上按 `Ctrl+V` 的情形（代码上已不再当作粘贴为文件）。
