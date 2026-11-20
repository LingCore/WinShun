# 剪贴板同步 · Clipboard sync

插件库里的 C++ 常驻插件（`WinShunSync.exe`，源码在 `src/sync-plugin`），不随 Win顺 安装。设置 → 插件 → 插件库 里装上**剪贴板同步**并打开，它就在后台工作：在一台电脑上复制，另一台上直接粘贴。和 Mac 上的 **Mac顺** 互通（ShunSync v1 协议），几台 Windows 和 Mac 可以组成一个组。

**第一次连起来**
- 两台设备在同一个网络里：在新的那台的 `同步` 里会看到「附近的…」，按 Enter 请求加入；已经开着的那台会弹出加入请求，两边屏幕上显示同一串 6 位数字，核对一样后在那台上按 Enter 同意。和 Mac顺 一样，开着同步就能收到请求，不用先去对方那里点什么；同一台设备 8 秒内只能请求一次，同时只处理一个请求。不想被请求时，在插件设置里关掉「别的设备可以请求加入」，以后只在 `同步` 里选「允许新设备加入」后的 2 分钟里接受。
- 不在同一个网段（比如用 Tailscale、公司的两个网段）：在组里任意一台上「复制邀请码」，粘贴到新设备的 `同步` 后面按 Enter，不用配对。邀请码里有组的密钥，只发给自己的设备；复制时不进剪贴板历史。也可以在 `同步` 后面输入对方的地址（比如 `100.64.0.2`，不用写端口）请求加入，Ctrl+Enter 记住这个地址。
- 第三台、第四台加进来后，组里的设备会自己互相连上。

**同步什么**
- 文字、图片（PNG；收到时同时给出普通位图，老程序也能粘贴）、在资源管理器里复制的文件和文件夹。
- 文件：复制后别的设备马上知道有文件，右下角提示一下（插件设置里可关）；和 Mac顺 一样默认不自动下载，点提示或打开 `同步`，按 Enter 接收并放进剪贴板（直接 Ctrl+V），Ctrl+Enter 存到「下载」并在资源管理器里选中，都能看到进度，Ctrl+Enter 取消。想让小文件自己过来，把「自动接收不超过」改成比如 20（MB）。收到的文件存在插件的数据文件夹里，7 天后或超过 2 GB 时自动清理。连不上复制文件的那台时（它在路由器后面、隔了一层 NAT），改从已经连着的那条连接上传，不用额外设置。
- 密码管理器标记为不让剪贴板工具看到的内容不同步。从别的设备收到的内容不会再转发出去，也不会被这台电脑当成新复制再发回去。
- 几台设备差不多同时复制时，以最后复制的为准。

**`同步` 列表**：加入请求（核对数字后 Enter 同意、Ctrl+Enter 拒绝）、别的设备复制的文件（Enter 接收并复制、Ctrl+Enter 存到「下载」）、本机（Enter 暂停或继续）、组里的设备（Enter 把当前剪贴板只发给它，Ctrl+Enter 连按两次移除；移除后组换一把新密钥）、附近的设备、复制邀请码、最近收到的文字和图片（Enter 再放进剪贴板）。关掉「别的设备可以请求加入」时多一行「允许新设备加入」。

**安全**：不用账号，没有服务器。组靠一把 32 字节的密钥，每条连接都用它验证对方，再用一次性的 X25519 密钥协商、AES-256-GCM 加密。密钥用 Windows 的数据保护（DPAPI）加密后存在插件的数据文件夹里。

**防火墙**：第一次运行时 Windows 可能会问是否允许 WinShunSync 通过防火墙，选「允许」（专用网络）。别的设备要连进这台电脑才需要它；这台电脑主动去连别的设备不受影响。

**权限**：联网，在局域网上开 TCP 37219（被占用时换一个）和 UDP 37220（发现附近的设备），读写剪贴板，写文件（收到的文件；选「存到下载」时写进「下载」文件夹），系统通知（加入请求、文件到了）。

---

A resident C++ plugin in the plugin library (Settings → Plugins → Plugin library), not in the setup. Copy on one computer, paste on another: text, pictures and files are shared between your Windows PCs and Macs running Mac顺 (the ShunSync v1 protocol), end to end encrypted, with no account and no server. To connect two devices on the same network, pick the other one under "nearby" in `sync` on the new device; the other is asked (as with Mac顺, whenever sync is on: once in 8 s per device, one request at a time; turn off "Other devices may ask to join" to take requests only in the 2 minutes after "Let a new device in"), both show the same six digits, agree there. Across subnets (Tailscale, say), copy the invite code on a member and paste it after `sync` on the new device, or type the other device's address. Files are offered, with a notification, and come on Enter onto the clipboard or on Ctrl+Enter into Downloads, with progress (as Mac顺, nothing is downloaded by itself unless "Receive by itself up to" is raised); when the device the files were copied on cannot be reached directly (NAT, a second router) they come over the connection already open. Password managers' secrets are not shared, what came from elsewhere is never sent on, and when two devices copy at once the newest wins. Members can be removed (the group then moves to a new key). The key is kept encrypted with Windows' data protection (DPAPI). Allow WinShunSync through Windows Firewall (private networks) when asked, so the other devices can connect to this PC. Permissions: network, TCP 37219 and UDP 37220 on the LAN, the clipboard, writing files (what comes, and Downloads when chosen), notifications.
