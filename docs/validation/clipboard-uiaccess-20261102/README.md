# #179：独立 UIAccess 剪贴板显示

2026-11-02，接续[普通权限兼容修正](../issue179-20261102/README.md)。剪贴板可由 UIAccess 显示进程创建窗口，覆盖原生开始菜单，同时保留原生窗口的焦点。实际层级由 `GetWindowBand` 读取，要求为 2；不把普通 topmost 窗口的测试称作 UIAccess 验证。

## 实现范围

- 使用原有剪贴板 QML，显示进程在专用入口运行，不创建 App、历史数据库、监听器或插件运行时。主进程通过有长度和排队上限的本地管道发送模型与外观，处理搜索、选择、删除、撤销、分类和粘贴等操作；图标在普通进程生成后传入，限制为 256×256 PNG、最多 2 个并行任务和 128 个排队请求。没有第二份持久化索引或剪贴板数据库。
- 服务只启动受保护安装目录中的固定 WinShun.exe 入口，核对实际调用进程、会话和管道名称；显示进程在交互会话核对父窗口所属 PID，两端认证管道实际对端 PID。子进程使用会话用户自己的 UIAccess 令牌，环境使用白名单；父连接消失时 job 结束子进程。主程序与插件继续使用普通权限。
- 普通窗口保留作为回退场景；显示端异常时恢复其按键路由、查询和预览。普通场景的窗口不重复响应远程界面的菜单、命名等一次性信号。旧服务和便携副本走避让 Start/Search 的普通窗口路径。

## 验证

使用隔离 profile、独立测试窗口与原生 Windows 开始菜单。真实 UIAccess 测试把候选程序放进临时受保护目录，以独立 SYSTEM 计划任务启动本次服务，通过专用命名管道请求显示进程；没有替换既有服务或退出用户运行的 Win顺。测试结束清理自己的计划任务和受保护目录。

- 构建：WinShun、WinShunService 和相关测试目标成功。
- 协议：分帧、合帧、长度上限、非法 JSON 和缓冲上限通过。
- 回归：窗口放置、窗口显示状态与按键路由测试通过。
- 普通令牌的独立显示进程：两个快捷键入口、搜索、全选/取消、删除/撤销、预览开关、独立菜单与菜单关闭通过；终止本次拥有的显示测试进程后，普通窗口恢复可见、目标和按键路由保留。此项仅证明跨进程交互和回退。
- 真实 UIAccess：最终 Release 构建，原生 Start/Search 在前台、150% 进程比例，显示窗口实际 `band=2`，与 Start 区域重叠，九个内部采样点命中剪贴板自身；搜索、全选/取消、删除/撤销、预览开关、菜单打开/关闭和两种快捷键开关均通过。菜单实际也是 `band=2`，中心采样点命中菜单自身，原窗口保持焦点和粘贴目标。已查看真实桌面合成截图。
- 服务边界：在独立 SYSTEM 服务上，握手、拒绝其他程序请求对话框导航/剪贴板显示、拒绝非法显示管道名，以及原有键盘监听能力广告检查通过。

测试触发生产 `WM_HOTKEY` ID 3/4，不修改全局快捷键注册，也不合成 Windows 键。不会写入系统剪贴板或执行粘贴；启动时可能把当前剪贴板读进测试 profile，测试删除/撤销只操作隔离数据库。未新增混合 DPI 多显示器、向原生搜索框实际粘贴或 IME 组词的验证。包含用户桌面或剪贴板内容的截图仅保存在忽略的 build 目录，不提交。

## 复现

```powershell
# 真实 UIAccess + 原生 Start；会请求 Windows 管理员确认。
./tools/verify_clipboard_uiaccess.ps1 -OutputDir build/clipboard-uiaccess-fresh

# 普通令牌的独立显示界面；不能据此证明 Shell 上层显示。
Copy-Item build/release/tst_keyrouter.exe build/issue179-20261102/fixture/SearchHost.exe
build/release/WinShun.exe --probe-runtime build/clipboard-display-fresh --clipboard-shell-probe build/issue179-20261102/fixture/SearchHost.exe --clipboard-display-probe --renderer d3d11 --settle-ms 100
```

证据文件保留检查结果和源码/二进制 SHA256；桌面截图不入库。
