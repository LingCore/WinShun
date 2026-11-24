# 索引、存储与搜索架构

本文描述 Win顺 当前的索引存储、权限分离、远程位置收录、文件名与全文检索架构及关键安全／线程边界。

## 1. 内存索引与持久化

### 1.1 `FileIndex` 内存布局与元数据

- **热冷分离条目**：条目拆分为 `EntryScan`（9 字节：`nameOffset`、`parent`、`flags`）和 `EntryLinks`（8 字节：`childOrSize`、`nextSibling`），合计 17 字节。全量文件名扫描仅顺序遍历 `EntryScan`，压缩缓存行占用。
- **共享字段与独立时间列**：
  - `Entry::childOrSize` 由目录的 `firstChild()` 与文件的 32 位压缩大小 `FileSize` 共用（`packSize`/`unpackSize` 以 IEEE-754 `float` 位模式存储，16 MB 内精确、以上误差十万分之一，可直接按无符号整数比较；未知与目录为 `kUnknownSize`）。
  - 修改时间 `FileIndex::modified` 与创建时间 `created` 各自独立为 4 字节列（`FileTime`：1970 年起 UTC 秒数至 2106 年，早于 1980 的 `kFirstKnownTime` 视作未知 `0`），按 65,536 项分块分配，命中后才读取。单条目核心元数据总计 25 字节（不含去重名池）。
- **去重名池与标志位**：文件名以 WTF-8 存于去重名池，前缀记录扩展名长度与 `Han` 标记。`EntryFlag` 含 `Directory`、`Deleted`、`Root`、`LowPriority`、`Hidden`、`Offline`、`Han`、`Remote`，其中 `LowPriority`、`Hidden`、`Remote` 沿目录树继承并持久化。
- **辅助结构与并发原语**：每 NTFS 卷维护开放寻址表 `RecordTable`（`folderRecords`，MFT 记录号 → `EntryId`）；`pinIds()` 在共享核对与脱锁内容查询期间阻止内存压实重编号；`takeChanges()` 供 `ContentIndexer` 增量消费变更。
- **文件与文件夹大小展示**：精简列表在 `Results/ShowFileSize` 开启或按大小排序时复用 `ResultModel::SizeRole`（不访盘、不递归目录）；完整搜索面板的文件夹列由 `FolderSizes` 后台调用 `indexedFolderSize()` 遍历内存子链显示 `≈`，按代次取消且不改变排序契约。

### 1.2 `Snapshot` 持久化与索引位置

- **分列压缩格式**：`ws::snapshot::save` / `load` 当前版本 `kVersion = 10`（兼容读 v2–v10，缺时间列的旧快照立即可搜并后台补读 MFT）。先序遍历写出：去重名表 → 按 `kGroup = 65536` 分列存放父偏移、标志、名字号、修改时间差、大小、创建时间差 → zigzag 差分 `RecordTable` → 扫描规则（v10 含 `keptPaths`）→ 内容索引文档表附件。
- **并行压缩与保存节律**：主体按 1 MB 块用 Windows Compression API（XPRESS + Huffman）并行压／解压并附字节校验和（魔数 `QFINDEZX`），经 `QSaveFile` 原子替换 `index.bin`。有改动时最多每 30 分钟保存一次（间隔变动下次启动靠 USN 补齐；内容索引滞留旧段达 32 MB 提前保存）。普通用户遍历模式的范围与核对状态独立存于 `scope.json`。
- **自定义索引位置与 HDD 预热**：`[Index] Folder` 经 `indexfolder::onExternalDisk` 仅接受内置盘（拒绝可移动盘、USB/SD/MMC/1394 总线及控制器可热拔的外置盒）。`IndexService::moveTo` 后台复制刷盘并切换段文件，失败自动回退。机械硬盘上的内容索引段文件由 `IndexService::prewarmSoon` 在启动 90 秒、搬迁后、休眠唤醒 30 秒或单次查询超 50 ms 后以 `IoPriorityHintLow` 分片预读（不进 `THREAD_MODE_BACKGROUND_BEGIN` 避免内存页被优先回收）。

### 1.3 `Pinyin`、`AppCatalog` 与 `SystemCatalog`

- **`Pinyin`**：`PinyinTable.cpp` 由 `tools/make_pinyin.py` 生成，每汉字保留最多 3 个无声调读音；`ws::pinyin::Matcher` 不可变供多线程共享，匹配首字母、全拼与混合前缀（`isPinyinTerm` 要求至少 2 个 ASCII 字符且含字母）。
- **`AppCatalog`**：枚举 `shell:AppsFolder`，结合 `src/core/apps.txt`（跨语言显示名、别名、全词简写 `short`）匹配应用；通过 `PKEY_Link_Arguments` 标记经宿主运行的快捷方式（`AppInfo::hosted`，如 `cmd /k`、`rundll32`、`chrome_proxy`），避免按宿主程序名误匹配。
- **`SystemCatalog`**：后台解析 `%WINDIR%\ImmersiveControlPanel\Settings\AllSystemSettings_*.xml`、补全 `ms-settings:` 并评估 `shcond://` 条件，合并 `src/core/places.txt` 与 Win顺/插件自身设置页（`setOwnPlaces`）；**全部** 范围最多列 5 条并与应用共排，启动经资源管理器降权执行。

## 2. 权限分离与 NTFS 读取

### 2.1 `WinShunService` 安全边界与 `ServiceLocation`

- **进程职责与最小特权**：主程序 `WinShun.exe` 以 `asInvoker` 普通权限运行并持有全部索引与搜索；NTFS 卷读取抽象为 `ntfs::VolumeAccess`（`ServiceVolumeAccess` / `DirectVolumeAccess` / `NoVolumeAccess`）。独立服务 `WinShunService.exe`（服务名 `WinShun`，`LocalSystem`）监听 `\\.\pipe\WinShun.Volumes`（`FILE_FLAG_FIRST_PIPE_INSTANCE | PIPE_REJECT_REMOTE_CLIENTS`）。
- **管道 ACL 与数据边界**：管道 SDDL `D:P(A;;GA;;;SY)(A;;GA;;;BA)(A;;FRFW;;;IU)S:(ML;;NW;;;ME)` 仅放行交互登录用户，拒 AppContainer、低完整性与远程连接。除 `SeChangeNotifyPrivilege` 外，仅保留 `SeTcbPrivilege` / `SeAssignPrimaryTokenPrivilege` / `SeIncreaseQuotaPrivilege`（平时禁用，仅在拉起 UIAccess 助手线程的私有令牌上临时启用）。服务从不发送原始扇区/原始 MFT 记录或复制卷句柄；文件名元数据不按调用者 ACL 过滤（不含安全描述符/所有者），文件内容仅在直读路径按调用者令牌严格 `AccessCheck` 后返回。
- **防本地提权部署校验（`ws::svcloc`）**：`WinShunService.exe --install` 用 `svcloc::pathProtected` 逐级检查所有者（`SYSTEM`/`Administrators`/`TrustedInstaller`）与 DACL（`Object` 全套写删、`Parent` 禁建文件/子目录防 DLL 劫持、`Ancestor` 禁换子项）。位于用户可写目录时先复制到 `%ProgramFiles%\WinShun Service`（显式 DACL，不落入 `{app}`）再注册，并在 ImagePath 记录 `--installed-from`；日志目录 `%ProgramData%\WinShun` 同受保护且不跟随联接点。

### 2.2 `VolumeProtocol` 协议与能力协商

- `ws::volproto`（无 Qt）当前版本 `kMajor = 1`、`kMinor = 8`。每操作一条管道连接，服务端按 `GetNamedPipeClientProcessId` 认证（每进程 ≤16 连接、单卷同时仅 1 次 MFT 读取，帧载荷 ≤4 MiB）。
- **能力位分层**：`kCapabilityBulkRead`（内容直读）；`kCapabilityKeyListener`（UIAccess 双击监听，仅回传心跳与修饰键双击）；`kCapabilityDialogNav` / `DialogNavName` / `HotkeyRelay` / `PasteRelay`（仅授予同目录或 `--installed-from` 匹配的同会话 `WinShun.exe`）；`kCapabilityClipboardDisplay`（仅授予同目录受保护 `WinShun.exe`）。UIAccess 助手均以非提权用户令牌（`TokenIsElevated=0`）+ uiAccess + 高完整性在净化环境中运行。
- **访问模式与重连**：`Index/Access=auto|service|direct|walk`。开机服务晚到时含快照卷最多等 30 秒，之后每 5 分钟重试，连上无缝接续 USN。服务空闲零卷句柄；日志断线后在 30 秒窗口内从同一 USN 重连，服务重启/升级不触发 MFT 重扫。开发测试支持 `WinShunService.exe --console --pipe NAME [--no-privileges] [--no-bulk]` 配 `WINSHUN_PIPE=NAME`。

### 2.3 `Ntfs`、`NtfsIndexer`、`UsnApplier` 与 HDD 串行化

- `ntfs::MftReader` 先解析 `$MFT` `$BITMAP` 跳过空闲记录，按 4 MB 块维持 4 路异步在途读取，支持任意簇/记录大小；`MftTree` 收集全部非 DOS 硬链接（不用仅返单路径的 `FSCTL_ENUM_USN_DATA`）。
- 依据 `StorageDeviceSeekPenaltyProperty` 与 `IOCTL_VOLUME_GET_VOLUME_DISK_EXTENTS`，同一物理机械盘上的分区串行读 MFT，SSD 并发读。
- `UsnApplier` 分小批幂等回放日志；改名旧名缓存于 `m_oldNames`，`safePosition()` 将快照游标卡在最老未配对旧名之前。`USN_REASON_BASIC_INFO_CHANGE` / 写入关闭出锁后以 `GetFileAttributesEx` 补读时间与大小（不触发杀毒/云盘下载）。

### 2.4 磁盘范围、热插拔、目录链接与普通用户模式

- **磁盘筛选与热插拔**：`selectedDrives`（`std::optional`）区分全盘与空选；`isExternalVolume` 识别外接盘，未勾选时不持句柄防阻塞弹出。收到弹出/锁定通知时 `IndexService::suspendVolume` 发 `CloseVolume` 等服务释放句柄（≤2 秒），服务自身也监听设备通知主动放行。
- **目录链接**：整盘 MFT 与遍历默认不跨重解析点；`IndexService::checkLink` 筛除排除路径、已索引共享、整盘目标、环路、低优先工具目录（如 `node_modules`）与系统兼容联接后，在独立路径下补扫并挂监视，子项不写入目标文件的 NTFS 记录号防别名冲突。
- **普通用户遍历模式**：无 USN 访问且非等待服务启动时启用 `IndexService::Options::folders`，仅索引选定目录（祖先仅存路径名），定期核对按用户空闲节流（`THREAD_MODE_BACKGROUND_BEGIN`）；卷根 `ChangeWatcher` 1 MB 缓冲（失败降 64 KB），事件丢失退避重核，卷根不可打开时降级逐目录监视（≤62 个）。

## 3. 网络共享、WebDAV 与 WSL

- **独立线程模型**：每个远程位置（SMB / WebDAV / WSL）作为 `FileIndex` 独立根（继承 `EntryFlag::Remote`），由 `ShareIndexer` 分配专属线程，不打断本地索引。
  - **SMB（`ShareLister`）**：`share::probe` 多地址并发竞速连 445 发 SMB1/SMB2 NEGOTIATE 区分 SMB2 与 SMB1Only；列目录用 `FileFullDirectoryInfo`（16 路并发）；监听挂独立 `watchLoop` 线程（移除卡死监听不阻塞主线程）并配合定期全量核对。
  - **WebDAV（`WebDav`）**：经 Windows WebClient 访问；`unc::canonical` 归一 URL/映射盘/`DavWWWRoot` 为 `\\host@SSL@port\path`；在线探测用独立线程 DNS + 并发 HTTP `OPTIONS` / TLS ClientHello（穿透 TUN 代理假连通）；列目录用 `FindFirstFileEx`；在线期每 5 分钟及打开启动器超 1 分钟轻量探活；WebClient 停止时由 `webdav::startClient` 发 ETW 触发事件无提权拉起并预热。
  - **WSL（`Wsl`）**：`distributions()` 只读注册表（绝不拉起已停发行版），后台有界调 `wsl --list --running --quiet` 共享 5 秒运行态缓存，列目录用 `FindFirstFileEx`。
- **延后删除、离线重试与凭据**：核对期 `pinIds()` 锁编号，遇断线/共享级错误整轮不删，消失项超 `max(1000, 一半)` 标 `State::Suspect` 待半小时复核或用户确认。离线按 1/2/4/5 分钟退避，网络变化/唤醒安静 5 秒重试，并用 `share::openRoot`（15 秒超时）纠偏慢 NAS 误报。Win顺 不自存密码，交互登录写 Windows 凭据管理器，后台永不弹框；UI 与搜索线程绝不阻塞等远端服务器。
- **远程内容索引（`RemoteContent`）**：开启 `[Shares] n\Content` 后每位置一独立线程，以大小+秒级修改时间作戳（读完 `stampAfterRead` 核验防半写），SMB 额外监听写入（3 秒安静 + 10 分钟冷却）。单文件上限取分类上限、WebClient 上限与 16 MB 最小；文档先整份缓存再交 `DocExtractor`；连错 3 次停止；门控避开按流量计费网络。内容搜索遇 `ContentVerdict::Remote` 仅查已存文字（离线可搜）。

## 4. 文件名搜索与排序

- **查询解析（`ParsedQuery`）**：未加引号 `|` 拆 OR 分支，各分支独立维护 AND、`!排除`、通配符、`ext:`、`size:`、`dm:` 与快捷筛选 `conditions`（未实现布尔括号）；`^词`/`词$`/`regex:` 预编译正则并限步数/深度、不做拼音扩展；语法错误直接报错不退回历史。完整盘符/UNC 路径先现场 `listTyped` 再补索引子项；相对路径经 `findRelativePath` 最多 3 条置顶。
- **两阶段并行扫描（`NameSearch`）**：持有读锁在 `WorkerPool` 并行：① 256 KB 分块扫去重名池填位图 `NameBits`；② 16,384 项分片扫 `EntryScan` 对置位项做完整匹配（每 OR 分支独立 1024 格祖先缓存）。文件/文件夹分堆（`kindsApart`），堆初始仅预留 1,024 项，大上限改用收集 + `nth_element` 防峰值内存暴涨。
- **排序、优先类型、历史与边缘提示**：
  - `RankBy::Match` 按历史匹配 → 完全同名 → 匹配分排序；`PreferredTypes`（默认 `exe, bat, cmd, ps1, msi`）在同名组置前并加分（非对话框/MCP 生效）。
  - `History` 按 14 天半衰期衰减，请求带不可变快照供工作线程读取；含汉字零命中时 `homophoneQueries` 展开最多 8 个拼音查询复搜并标 `approximate`。
  - 结果不满一屏时 `Launcher::leftOutHints` 统一挂出单次补搜入口（超大文件、系统目录、未列文本类型、未收录外接盘、普通用户范围外、自动内容搜索延迟项及排除项）。
  - 可选 `EverythingIpc` 响应 Everything 1.4 `WM_COPYDATA`/`WM_USER` 子集查询。

## 5. 内容索引与文档提取

- **倒排存储与合并（`ContentIndex`）**：提取 CJK 单/双字与含空格标点的 3 字符 ASCII gram（`kStateVersion = 8`）；按 96 位 SHA-256 指纹去重；合并经 MinHash 聚类 + 多线程递归图二分重排 + Moffat-Stuiver 二分插值编码。写段（约 8 MB）移交 `m_writing` 脱锁并行排序编码；查询先 `pinIds()` 脱出 `FileIndex` 读锁执行。
- **调度与 NTFS 卷直读（`ContentIndexer` / `NtfsBulk`）**：常规轮次仅扫 `takeChanges()`、`dueEntries()`、上轮残留与 `Intake`。按活跃状态限流（使用中 1/4 核、空闲 1/2 核；HDD 使用中单盘单文件极低 I/O；搜索框打开及关闭 5 秒内暂停）。首次或重建待读 ≥2,000 文件时开启服务直读（按 LCN 顺序大块读卷绕开 Defender 逐文件扫描，单批 ≤16 MB、总队列 ≤64 MB）；服务端先经 `ImpersonateNamedPipeClient` + `OpenFileById(READ_CONTROL)` + `AccessCheck(FILE_GENERIC_READ)`（`AccessGate` 单连接按 `security_id` 缓存 10 秒）严守权限，5 分钟内修改与压缩/加密/云占位/文档回退普通路径。
- **文档提取沙箱（`DocExtractor` / `WinShunExtract.exe`）**：按魔数识别 OOXML/Office 97–2003/WPS/PDF/HTML 伪 `.xls`，先按文件大小+SHA-256 去重同字节副本（`addCopy`），`.texts` 按压缩后 SHA-256 去重。`WinShunExtract.exe` 以受限令牌、低完整性、作业限制（2 GB 内存、禁子进程、隔离 UI）及缓解措施运行，**不打开路径，仅读主进程复制的只读句柄**；≥16 MB 大文档走 `alone` 串行独占并在提取后立即结束该进程释放高水位内存。
- **正则、目录范围与自动搜索边界**：`ContentPattern` 最多 32 根 `ContentNeedle` / 16 条合取路径；`RegexNeed::of` 静态抽取必含字面树缩减候选，正则始终在解码行上实跑核验（加 `(*ANYCRLF)`、`(*LIMIT_MATCH)`、`(*LIMIT_DEPTH)`）。目录范围经 `splitContentQuery` → `UnderFolders` 预筛。**全部**范围的自动内容补充置 `contentIndexedOnly`，仅单线程查已有索引与 `.texts` 缓存、绝不拉起 `WinShunExtract.exe`，跳过项由 `deferred` 引导切到**内容**范围完整扫描。

## 6. 已知边界与测试入口

- **当前已知边界**：拼音仅认开头且 `!排除` 不走拼音；大小写折叠仅限 ASCII，1–2 字母英文词不进内容索引；非 NTFS 卷不建内容索引；未实现布尔括号分组；`DocExtractor` 不支持加密文档及 `xlsb`/`odt`/`RTF`/`EPUB`/`OFD`（图片/扫描件 OCR 由 `winshun.ocr-search` 插件提供）；`EverythingIpc` 为 1.4 子集；远程内核同步 I/O 取消后退出仍可能等待系统超时。
- **测试与基准**：`tst_core`、`tst_querysyntax`、`tst_searchworkload`、`tst_everything`、`tst_service`、`tst_remotecontent`、`tst_wsl`、`tst_contentdetails`、`wsbench`。
