# 局域网传输 · LAN transfer

随 Win顺 安装的 C++ 常驻插件（`WinShunLocalSend.exe`，源码在 `src/localsend-plugin`），默认关着。和同一个网络里开着 [LocalSend](https://localsend.org) 的手机、电脑互传文件和文字，不经过任何服务器，也不用登录。

**收**
1. 设置 → 插件 → 打开**局域网传输**。这台电脑就会出现在别的设备 LocalSend 的列表里，名字可以在卡片上改。
2. 有人发过来时会弹通知，说是谁、几个文件、多大（不写文件名）。点通知或输入 `传 `（或 `ls `），第一行就是这次请求：Enter 接收，Ctrl+Enter 拒绝。`传` 本来就开着时，请求排到第一行，键盘留在原来那行（免得按 Enter 收下没看过的请求），那行写着「按 ↑ 选中，再 Enter 接收」。2 分钟不答就拒绝；拒绝后 15 秒内同一台设备再发，直接不收。
3. 文件存到「下载」（卡片上可以换文件夹），同名的不覆盖，自动改成 `名字 (1).jpg`。收到的文件带“来自网络”的标记，打开程序时 Windows 照常提醒。存的文件夹打不开时（U 盘拔了、网络盘断了），别人发来的直接拒绝，对方会看到“保存位置不可用”；`传` 里有一行说明，Enter 打开设置。一次最多 10,000 个文件。
4. 对方发来的文字不存文件：弹通知（点一下打开 `传`），列在 `传` 里，Enter 复制。通知不会自己往剪贴板里放东西。

**发**
- 先复制文件（资源管理器里 Ctrl+C），再输入 `传 `：每个设备一行，写着要发什么，选好按 Enter。文件多时大小先显示“正在统计…”，数完自己更新；网络位置上的文件只数个数，不看大小（睡着的 NAS 要好几秒才答）。
- 或者在 Win顺 的搜索结果上右键 → **用局域网发送…**，多选也行。
- 在 `传 ` 后面打字，Enter 就把这段文字发过去；Ctrl+Enter 发剪贴板里的文字（剪贴板刚换过内容就先不发，让你看一眼）。
- 对方点了接收才开始发。发送中那一行 Enter 是“隐藏”（窗口收起，照样在发），Ctrl+Enter 才是取消。发完以后那一行写着结果：发成了 Enter 再发一次，没发出去 Enter 重发。

**安全**：每次接收都先问你，没有“自动接收”。Windows 把网络设成“公用”时（咖啡馆、机场），先不接收；在 `传` 里说明原因，家里或公司的网络按一下 Enter 就在这个网络上接收（只记住这一个）。只是其中一个网络是公用的（比如 Wi-Fi 能收、网线是公用网络）时，状态照常，下面另有一行说那个网络上收不到。卡片上可以设接收密码。通知标题先写发生了什么，再写对方的名字（最多 20 个字），别人改名字冒充不了系统提示。连接是加密的（TLS），对方的身份用证书指纹核对。安装版会在 Windows 防火墙里加一条只对本地子网开放的规则（TCP 53317–53320、UDP 53317），卸载时删掉。这台电脑上也开着 LocalSend 时，它用 53317，插件用 53318；LocalSend 关了以后，没在传东西的时候换回 53317。

---

A resident C++ plugin that comes with Win顺, off by default. It sends files and text to and from phones and computers on the same network that run LocalSend — no server, no account. Incoming transfers are always asked about (a notification, then Enter or Ctrl+Enter in `ls `; one that comes in while `ls ` is open goes on top with the keyboard left where it was, and its row says "Press ↑ to pick it, then Enter accepts"); files land in Downloads (or the folder you choose) without overwriting, with the "from the internet" mark. When that folder cannot be reached (a USB disk pulled out, a share gone), requests are refused with "save location unavailable" and `ls ` says so. Up to 10,000 files at once. Text messages are listed in `ls ` (Enter copies); their notification opens `ls ` and never puts anything on the clipboard by itself. To send, copy files and type `ls `, or right-click results → Send over LAN…; words typed after `ls ` go as text, and Ctrl+Enter sends the clipboard's text unless it changed since the row was shown. Sizes are counted on the side ("Counting…"); files on network shares are counted, not looked at. While a transfer runs, Enter hides the window; afterwards its row says how it ended, and Enter sends the same again. On a network Windows calls public it does not receive until you allow that network in the list; when only one of several networks is public, the status stays and a row below names that network. Notification titles put what happened first and the other device's name (at most 20 characters) after it. The installer adds a firewall rule for the local subnet only (TCP 53317–53320, UDP 53317), removed on uninstall. With LocalSend also running on this PC, it keeps 53317 and the plugin takes 53318, going back to 53317 between transfers once LocalSend has quit.
