# 云备份 · Cloud Backup

在设置 → 插件 → 插件库安装（需要 Win顺 0.26.0 或更新）。它把 Win顺 的设置备份传到你自己的云端：WebDAV 服务器（坚果云、Nextcloud、群晖等 NAS、alist），或 GitHub 私有仓库，两种可以同时用。

备份的内容和“设置 › 高级 › 备份 › 导出备份”一样：设置、收藏、对话框固定项、网页图标和装的插件。索引、剪贴板和搜索历史不在里面；密码和 API 密钥留在 Windows 凭据管理器，不进备份。

## 怎么用

1. 打开插件卡片，填 WebDAV 或 GitHub（见下），没填之前不联网。
2. 之后不用管：Win顺 启动两分钟后检查一次，以后每半小时一次，设置变了才上传。每个地方保留最近 10 份（可改），更旧的自动删掉。
3. 在搜索框输入 `cloud`（或 `云备份`）加空格：最上面是每个云端的状态，Enter 立即备份；下面是云端已有的备份，写着时间和来自哪台电脑。
4. 换电脑或重装后：装好 Win顺 和这个插件，填同样的账号，输入 `cloud`，在要的那份备份上按 Enter，再按一次确认。Win顺 会先把现在的设置在本机存一份（数据文件夹的 `backups`），然后重启换成那份备份。

自动备份失败时会弹一条通知，同一个原因只提醒一次。

## WebDAV

- **文件夹地址**：备份直接放进这个文件夹，文件夹不存在会自动建（只建最后一级）。坚果云填 `https://dav.jianguoyun.com/dav/WinShun`。
- **用户名、密码**：坚果云要在“账户信息 › 安全选项 › 第三方应用管理”里生成“应用密码”，不是登录密码。
- 尽量用 `https://`。`http://` 只适合家里的 NAS：密码在网络上没有加密。

## GitHub

- **仓库**：写成 `用户名/仓库名`，或直接贴仓库网址。**必须是私有仓库**：备份里有你的设置和收藏的路径，发现仓库是公开的就不上传。
- **令牌**：在 GitHub › Settings › Developer settings › Fine-grained tokens 新建，只选这一个仓库，Repository permissions 里把 Contents 设成 Read and write。
- 备份放在仓库的 `WinShun` 文件夹里，一份备份一次提交；单份最大 50 MB。删掉的旧备份仍留在仓库的提交历史里，仓库会慢慢变大，装了很多插件时更适合用 WebDAV。
- 访问 `api.github.com` 走系统代理。

## English

Install from Settings → Plugins → Plugin library (Win顺 0.26.0 or later). It keeps Win顺's settings backup in your own cloud: a WebDAV server (Nextcloud, a NAS, alist, 坚果云) or a private GitHub repository, either or both.

A backup holds what Settings › Advanced › Backup › Export holds: the settings, favorites, dialog pins, web shortcut pictures and the plugins you added. No indexes, no clipboard or search history; passwords and API keys stay in Windows Credential Manager.

Fill in the plugin's card; nothing is sent before that. Win顺 then checks two minutes after start and every half hour, and uploads only when the settings changed. The newest 10 backups are kept at each place. Type `cloud` and a space: the first rows show each place (Enter backs up now), the rows below are the backups there with their time and computer. Enter on one, and Enter again to confirm, restores it: the present settings are saved on this computer first (`backups` in the data folder), then Win顺 restarts with that backup.

- WebDAV: the address of the folder (made when missing), a user name and a password; some services want an app password. Prefer `https://`.
- GitHub: `owner/name` of a **private** repository (nothing is uploaded to a public one) and a fine-grained token for that repository with Contents: read and write. Backups go to the `WinShun` folder, one commit each, 50 MB at most; removed backups stay in the history.
