# #104 复报：向日葵安装程序的“浏览文件夹”跳不过去（fx104，2026-11-03）

## 对话框是什么

向日葵 16.8.0.33445 官方安装包（`dl.oray.com/sl/windows/AweSun_16.8.0.33445_x86.exe`，MD5 与官网接口一致，DigiCert 签名有效），以管理员身份运行（高完整性，32 位）。主界面是自绘的 `OrayUI` 窗口，点“更改”弹出**旧版 `SHBrowseForFolder`**：`#32770`“浏览文件夹”，树是对话框里直接的 `SysTreeView32`（id 0x3741，没有 `SHBrowseForFolder ShellNameSpace Control`，即没有 `BIF_NEWDIALOGSTYLE`），还有状态文字 id 0x3743、确定/取消。初始选中“此电脑”，确定是灰的。`dialognav::classify` 判为 `Kind::Browse`，走 UI Automation 选中树节点那条路。

## 为什么普通权限的安装版跳不过去

复报的截图提示是“管理员窗口：搜索后复制路径再手动粘贴（安装版才能直接跳转）”。安装版里出这句只有一种情况：服务握手回 `kCapabilityDialogNavBesideOnly`（`Reach::OtherCopy`），也就是服务认为请求方不是“和服务同目录的 WinShun.exe”。

装到普通用户也能改的文件夹时会这样（复报者的搜索结果都在 `D:\`，比如 `D:\Softwares`）：安装程序的 `[Run] WinShunService.exe --install` 发现自己不在受保护的位置，就把服务复制进 `%ProgramFiles%\WinShun Service` 再注册。界面留在 `D:\…\WinShun.exe`，不在服务旁边，导航能力就不给它了。以管理员运行时，Win顺 不走服务，自己发 `BFFM_SETSELECTION`，所以能跳。搜索框和 Ctrl+G 用的是同一个判断（`ServiceDialogNav::available`），所以两个一起失效。便携版自己装的服务也是同样的情况。

## 改了什么

`--install` 复制服务时，把装它的那份 WinShun.exe 用 `--installed-from "<路径>"` 写进服务的 ImagePath（只有管理员能改）。服务给导航能力时也认这一份（`svcloc::servesDialogNav`、`mayNavigateDialogs`）。受保护的剪贴板显示仍然只认服务旁边的 WinShun.exe。提示从“安装版才能直接跳转”改成：便携版没有服务时说“装 Win顺 服务可直接跳转”，服务是另一份 Win顺 装的时说“服务属于另一份 Win顺”。

## 实测

- 暂存的界面在 `C:\Users\young\wsi12-stage\fx104`（普通用户能改，不提权，`WINSHUN_PIPE=WinShun.fx104`）。服务用临时计划任务以 SYSTEM 跑 `WinShunService.exe --console --pipe WinShun.fx104`，exe 放在另一个只有管理员能写的文件夹里，相当于 `WinShun Service`。
- **改前的服务**（base 构建的 WinShunService.exe）：搜索框显示复制路径的提示（`before-copy-only.png`；提示比较长，在这个宽度下末尾被省略），和复报一致。
- **改后的服务**加上 `--installed-from <暂存的 WinShun.exe>`：15 秒内重新探测后提示变成“管理员窗口：直接跳转到文件夹”。
  - 在搜索框输入 `C:\Users\young\wsi12-test\fx104\target-bar` 回车：服务日志记 `dialog nav … navigated`，树选中 target-bar，确定变成可点（安装程序收到了 `BFFM_SELCHANGED`）。见 `after-bar-jump.png`。
  - 资源管理器开着 target-ctrlg 时，在对话框里按 Ctrl+G：服务日志记 `navigated`，树选中 target-ctrlg。见 `after-ctrl-g.png`。
  - 另外跑了一次只输入“C”的搜索，回车后跳到排第一的同名文件夹，确定也变成可点。
- 自动测试：`tst_dialognav::requesterInstalledFrom`（8 行）通过。`tst_service` 以管理员身份整套通过，新增的 `dialogNavForTheCopyInstalledFrom`：带 `--installed-from <本测试程序>` 的控制台服务对它不再回 `ERROR_ACCESS_DENIED`（管理员控制台启动不了助手，回的是 `ERROR_NOT_SUPPORTED`）；`dialogNavRefusedForOthers` 照旧通过。
- 收尾：取消了文件夹选择框，结束了安装程序进程，没点过安装。这台机器原来就装着向日葵 16.6.0.32198，测完版本号、AweSun.exe 时间、两个服务的启动方式和状态都没变；只有 `config.ini` 的修改时间变成了安装包启动的那一刻（00:26），里面版本仍是 16.6.0.32198。临时计划任务和服务文件夹已删。资源管理器右键菜单的三个 SearchHere 键和 `\WinShun` 计划任务都还指向 Program Files。

## 没测的

- 没有真的用改后的安装包装到 `D:\` 再测：真实服务的 `--install` 和 ImagePath 带参数启动，是从代码和上面的控制台服务推断的。不能替换用户机器上已经注册的 `WinShun` 服务。
- 便携版用设置里的“安装索引服务”装上服务后能不能跳，没有实机测；它走的是同一个 `--install` 分支。
