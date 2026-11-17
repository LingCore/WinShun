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

## 链接的诊断记录

之前日志里一个链接都不写，用户那边出问题没法查。现在诊断里多了这几种记录（`IndexService::takeLinks`），都不记路径：
- `directory-links`：读了哪些盘、找到几个链接、各结果的个数（`walked`、`disk-indexed`、`share-indexed`、`target-excluded`、`link-excluded`、`unreadable`、`no-volume`）。前 24 个链接各记一条：在哪个盘、指向哪个盘或哪种位置、盘的类型（`drive_type` 1 表示这里没有这个盘，比如管理员身份看不到的映射网络盘）、读不出来时的错误码、要跟进的能不能打开。日志里也写一行个数。
- `directory-links-over-limit`：超过 256 个，没收的个数。

本机以管理员身份跑 `tst_core directoryLinksFoundWithLiveMft directoryLinkTargets` 通过（`WINSHUN_TEST_REAL_VOLUMES=1`，另一块盘在 F:）。这个用例新加了检查：诊断里有 `walked`、`disk-indexed`、后来建的链接，跟进的那条指向 F:、能打开，整份记录里没有测试目录的名字。日志行：`directory links: 2 found, 1 to walk under the link ({"disk-indexed":1,"walked":1})`。

## 遍历的磁盘也找链接

以前只有读主文件表（管理员直读、索引服务）时才找链接，“遍历”方式建索引的盘、读主文件表失败退回遍历的盘，链接一个都不会自动收录。现在：
- 遍历时碰到的链接（`Crawler::SyncStats::links`）和读主文件表找到的一样交给 `takeLinks`。
- 之后新建的链接：监视器报的变化里有链接时（`applyChanges` 的 `links`），交给工作线程收录（`m_linksMade`）：正在跟日志的工作线程被唤醒，没有在跑的就起一轮。不能在监视器自己的线程里做：收录会改监视的范围，第一版这样写，测试卡死在那里。
- 联接是先建文件夹、紧接着才设成链接（`mklink /j` 也是）：监视器在中间看到时会把它当普通文件夹走进去。走完再看一次，已经是链接的，走进去的内容撤掉、当链接处理。测试里实际碰到过这种情况。

`tst_core directoryLinksFoundByWalking`（新加，不用管理员，要 `WINSHUN_TEST_OTHER_DISK_DIR`）：遍历模式下，指向 F: 的联接按链接路径收录，指向本盘的不重复收录；运行中新建的指向 F: 的联接也收录；诊断里有这两次记录。同改动相关的 34 个用例（监视、遍历、改名、选中的文件夹、网络共享等）和管理员身份的 3 个真实主文件表链接用例都通过；全部目标编译通过。

没有覆盖：Windows 11 上的 TC；用户那台机器上的链接（指向哪里还不知道，已在 issue 里问）。
