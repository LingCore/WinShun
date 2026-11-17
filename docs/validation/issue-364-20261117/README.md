# #364 Total Commander 通过“当作 Everything”取不到文件夹大小（2026-11-17）

main，构建目录 `build/run-latest`（Release，Qt 6.12.0）。

## 原因

- TC 11.58 的“文件夹大小：只用 Everything”（`GetDirSize=3`）启动时先问 `?:`，再问 `root:`（用 `tst_everything live` 加一份临时 `wincmd.ini` 在本机录到的）。在 Everything 里 `?:` 列出各个盘，Win顺 一直回答 0 条，TC 就当作没有盘建了索引，后面一个文件夹大小都不问。
- 盘列出来以后，TC 问 `folder:parent:"C:"`（要名字和大小）。Program Files、ProgramData、Users、Windows 仍是“不知道”：里面只要有一个索引里没有大小的文件，整个文件夹就算不出来。这样的文件是索引建好以后才写的、还开着的（日志、锁文件），或者读不到属性的（WindowsApps），用着的电脑上总有（虚拟机里 C: 有 34 个）。

## 改动

- `?:`、`*:` 和 `root:` 一样列出各个盘（`EverythingIpc.cpp`）。
- 文件夹大小跳过没有大小的文件，其余照加；里面一个有大小的文件都没有时才是“不知道”（`FolderSize.h`，Win顺 自己的“≈ 大小”也用它）。

## 验证

- `tst_everything`、`tst_panelcolumns` 通过（新加：`?:`、`*:` 列出盘；有没有大小的文件时照算，全都没有时不知道）。
- Win10 22H2 虚拟机，Win顺 以管理员身份运行（管理员直读）、开“让其他软件把 Win顺 当作 Everything 搜索”，未注册的 TC 11.58 用普通权限、`GetDirSize=3`：
  - 改前：文件夹都显示 `<DIR>`（`vm-before-no-sizes.png`）。
  - 只改 `?:` 后：多数有大小，Program Files、Users、Windows 仍是 `<DIR>`（`vm-drives-fixed-some-unknown.png`）。
  - 两处都改后：C:\ 下的文件夹全都有大小（Windows 22,186,605,026），`C:\Users\ws` 1,402,486,266（`vm-after-all-sizes.png`）。
- 同一台虚拟机，管理员直读、只索引 C:，`mklink /d C:\ws\lnk T:\data`（T: 是没有索引的 VHD）：`ws find` 找到 `C:\ws\lnk\deep\zzlinked364.txt`，链接照常跟进去。

没有覆盖：Windows 11；用户那台机器上的链接（指向哪里还不知道，已在 issue 里问）。
