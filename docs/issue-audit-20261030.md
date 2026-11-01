# #139 及以前 issue 复查记录

日期：2026-10-30。范围：#1–#139 中实际存在的 137 条 issue，包含关闭后的补充回复。代码仍在本地工作区，**尚未提交、推送或发布**；版本号保持 0.16.0，不代表现有下载已包含本轮变化。

后续补正：#3、#77 的关闭后请求在本表中记录不完整；#69、#109、#4 等也有后来的新回复。当前结论见 [2026-11-01 新回复复查](issue-replies-audit-20261101.md)，不要把本表的“未见新增待处理回复”继续作为这些问题已结束的依据。此页的提交状态描述的是当时的验证轮次。

## 处理结果

- 原来 27 条打开、110 条关闭；复查后 32 条打开、105 条关闭。
- 回复并关闭 #103、#115：用户明确确认输入/搜索故障已解决；关闭 #113：局域网传输原功能请求已交付，最新回复未报告本项目的新故障。
- 恢复跟踪 #4、#56、#61、#82、#93、#97、#104、#123：关闭后的补充仍有有效问题或尚未发布的修补。
- 本轮新增 23 条回复（含最早关闭 #103/#115 的两条），逐项区分本地修复、未实现需求和待原环境复现；没有把“请升级试试”当成已验证解决。
- 保留任务开始前的未提交修改、用户数据和运行中的已安装 Win顺。没有进行提交、推送、安装替换或发布。

## 本轮代码

1. **#111 / #134**：双击修饰键不再因指针移动而失效。清理了废弃的位移状态和诊断分支，保留鼠标按键/滚轮、其他键及组合键干扰的排除。覆盖 Ctrl/Alt/Shift 左右六种。
2. **#139**：开始菜单 AppsFolder 条目解析实际控制台目标和参数，复用普通权限控制台启动路径；普通 GUI、商店应用仍走其现有启动路径。
3. **#123 / #125**：计算器触发器允许连续减法、除法，覆盖 `500-498-1=1`、`1/2/2=0.25`，保留日期与电话样式排除。
4. **#93 / #119 / #135**：`re:` 别名；词间/后置类型条件；英文程序词首/驼峰首字母。程序首字母同时覆盖真实索引与开始菜单应用，避免快速预筛选丢掉结果。类型解析保护引号、未知条件和盘符。
5. **#119 / #135**：最近打开数量 1–200（默认 5）；收藏在输入前是否显示独立控制，关闭后关键词查询仍工作。CLI/AI 设置映射同步。
6. **#105 / #121**：剪贴板鼠标指针旁打开的独立选项；剪贴板、截图各自独立的全屏热键暂停选项。截图选项在可选插件内；菜单手动操作与剪贴板记录保留。
7. **#137 / #107**：文件夹菜单增加中键入口、当前目录命令提示符/PowerShell、收藏命令和设置；按稳定命令标识执行，移除后提示。剪贴板末尾循环，本机窗口获得焦点后可继续浏览，松开不会自动粘贴。
8. **#116**：打开/定位优先沿用活动映射盘符，最长根目录匹配；索引与历史标识保留 UNC。网络属性与盘符解析移到 shell 工作线程。
9. **#97**：预检测提示增加重新检测/尝试安装/取消；只在交互安装明确选择后忽略名单，实际写入失败仍由 Inno 处理。静默安装保持有限等待与失败行为。
10. **#56**：读取 Ditto `lDontAutoDelete` 并导入到固定分组；普通 sticky 排序保持独立，重复导入不改变已有条目。
11. **界面质量**：设置行按实际控制区宽度重排，修复英文最窄窗口说明被挤成逐字竖排；新增选项及相邻未完成翻译已补齐。既有 README 宣传截图版式未改动。

## 当前验证及其边界

- Qt 6.12.0 / MSVC Release 完整构建通过。修正旧循环选择断言后，**CTest 101/101 通过，133.91 秒**。
- 最后补齐 AppsFolder 默认/明确工作目录用例并完成当前树构建后，相关 7 项测试再次全通过，8.33 秒；这次没有新增生产逻辑变化。
- 另跑相关六组并发测试 6/6 通过，66.67 秒；核心实际状态机、真实索引、收藏列表、配置和命令启动均覆盖。Ditto 保护属性新用例通过，导入 19 个用例通过；可选真实个人数据库用例因未提供数据库而跳过。
- 在 **4,036,330 条目的隔离索引副本**上跑 `hsm`、`report`、`sh`、`zzzqx` 各 7 次热查询，中位数分别为 5.578 / 5.588 / 6.844 / 5.452 ms，快速预筛选与逐条完整匹配的总数全部一致。该结果覆盖文件名查询，不等于整个窗口打开、NAS 或内容索引的端到端耗时，也没有拿它替代改动前后的对照基准。
- `tst_shelllaunch::appsFolderConsole` 使用系统 PowerShell 的真实 AppsFolder 条目，检查父进程、参数，以及明确指定/默认目标工作目录。生产路径是 `Launcher::performApp → shell::launchApp → executeUnelevated`；测试在后者共同执行边界观察真实子进程，没有把它当成原版 Surge 的用户现场复测。
- `tst_foldermenu` 用自有窗口、临时命令和目录验证执行、工作目录、设置入口、移除命令后的反馈，恢复原前台窗口；没有关闭用户程序。
- 真实 `ClipboardWindow.qml` 循环选择/回首条测试通过，未启用原生剪贴板监听、未改动系统剪贴板。此证据覆盖 QML 选择处理，不宣称全局热键已在所有第三方窗口逐个实测。
- 真实 `CaptureSettingsWindow.qml` 在最小宽度、英文、深色、150% 缩放下验证全屏开关、绑定和独立配置的保存/读取，退出码 0、QML 警告 0。
- 结果列表及剪贴板真实设置页在中文/英文、760 逻辑像素窄窗口、150% 下渲染与目视检查，修复挤压后无警告。界面截图位于本地证据目录及本报告的验证图。
- Inno Setup 6.7 完整脚本编译通过。使用**原样生产 TaskDialog 调用与消息**生成不执行安装的独立测试程序，逐个点击三个按钮，记录 `4/5/3`（Retry/Ignore/Abort），全部匹配生产分支。没有在用户安装目录执行更新，没有验证用户杀软的具体文件占用现场。
- 隔离 `App::start` 使用临时配置和 HKCU 重定向：观察到单个核心进程，启动 593 ms、私有内存 79,740,928 字节、警告 0；关闭剪贴板、可选扩展，索引只含临时测试夹具。这检查真实生产启动的隔离与生命周期，不能作为真实全盘启动、长期占用或全机性能保证。

### 仍须保留的测试异常

首次并行全量运行中，`tst_plugin` 曾以 `0xc0000374`（堆损坏）退出，不能忽略。其后两次独立运行、两次 CDB 运行（第二次与核心/按键等测试并行），以及一次完整并行全量运行均通过，每次是 124 通过、1 个需要提升权限的用例跳过。**根因尚未定位，不能宣称已修复**；正式发布前要复核。没有改成串行、删测试或忽略异常来让测试变绿。

本地证据目录：`.git/codex-issue-audit-20261030/`。保留了开始时的状态与补丁、GitHub 前后快照、各轮构建/测试日志、CDB 日志、独立安装提示夹具及结果。原先已有的 issue #138 验证文档属于历史证据，本报告只引用本轮实际重新运行的结果。

## 仍打开的原因

32 条打开并不表示都等待编码：其中包含已在本地修复但未交付的条目、缺原设备/安装器/输入法复现的缺陷，以及明确未实现的独立需求。Everything IPC、DO 大小列、Flow 插件兼容、文件右键自定义命令、右下角定位、图片右键贴图、逐文字框点选打码及“全部/文件”合并都没有冒充完成。各项见下表。

## 界面验证图

这些是隔离配置的真实界面验证图，不替代 README 的发布宣传图，也不改变原截图项目的排版。

- [中文结果选项](validation/issue-audit-20261030/results-zh.png)、[英文最窄窗口](validation/issue-audit-20261030/results-en.png)
- [剪贴板位置与全屏选项](validation/issue-audit-20261030/clipboard-en.png)
- [截图插件全屏选项](validation/issue-audit-20261030/capture-en.png)
- [文件夹菜单的中键设置](validation/issue-audit-20261030/folder-options-zh.png)

## 逐项台账

“保持关闭”的常规行表示历史已有处理记录且本轮未见新增待处理回复，**不表示重新端到端测试过每个外部应用和硬件组合**。具体重新验证的部分只以上方证据和下方条目为准。

| Issue | GitHub 状态 | 结论 | 依据与剩余范围 |
|---|---|---|---|
| [#1 优化设置页面的字体渲染](https://github.com/LingCore/WinShun/issues/1) | CLOSED | 保持关闭 | 历史已有处理记录；未见新增待处理回复。本轮未逐项重跑外部环境。 |
| [#3 建议：适配第三方文件管理器，比如Dopus。](https://github.com/LingCore/WinShun/issues/3) | CLOSED | 保持关闭 | 历史已有处理记录；未见新增待处理回复。本轮未逐项重跑外部环境。 |
| [#4 微小BUG，在打开该软件设置页后点击开始菜单无反应](https://github.com/LingCore/WinShun/issues/4) | OPEN | 重开：待原环境复现 | 关闭后补充的 MyDockFinder 场景仍未验证解决；需要当前版本及复现信息。 |
| [#5 这个文件内容搜索能不能设置独立的开关，我看了一下，目前好像要把文件类型删了才行](https://github.com/LingCore/WinShun/issues/5) | CLOSED | 保持关闭 | 历史已有处理记录；未见新增待处理回复。本轮未逐项重跑外部环境。 |
| [#6 需要支持wps 对话框](https://github.com/LingCore/WinShun/issues/6) | CLOSED | 保持关闭 | 历史已有处理记录；未见新增待处理回复。本轮未逐项重跑外部环境。 |
| [#7 0.60版本：无法自动跳转目录](https://github.com/LingCore/WinShun/issues/7) | CLOSED | 保持关闭 | 历史已有处理记录；未见新增待处理回复。本轮未逐项重跑外部环境。 |
| [#8 希望大佬增加剪切板置顶功能](https://github.com/LingCore/WinShun/issues/8) | CLOSED | 保持关闭 | 历史已有处理记录；未见新增待处理回复。本轮未逐项重跑外部环境。 |
| [#9 一些功能建议：剪贴板预览记忆、预览快捷键、图片搜索；搜索框操作快捷键自定义；Everything 服务模拟](https://github.com/LingCore/WinShun/issues/9) | CLOSED | 保持关闭，拆分跟踪 | 已有剪贴板、预览等历史闭环；Everything 兼容服务继续由 #40 跟踪。 |
| [#10 快捷键冲突问题](https://github.com/LingCore/WinShun/issues/10) | CLOSED | 保持关闭 | 历史已有处理记录；未见新增待处理回复。本轮未逐项重跑外部环境。 |
| [#11 快捷键无法设置为 alt + space](https://github.com/LingCore/WinShun/issues/11) | CLOSED | 保持关闭 | 历史已有处理记录；未见新增待处理回复。本轮未逐项重跑外部环境。 |
| [#12 开启云母效果时，搜索框字体渲染会出现彩色边缘](https://github.com/LingCore/WinShun/issues/12) | CLOSED | 保持关闭 | 历史已有处理记录；未见新增待处理回复。本轮未逐项重跑外部环境。 |
| [#13 希望能发布 便携版](https://github.com/LingCore/WinShun/issues/13) | CLOSED | 保持关闭 | 历史已有处理记录；未见新增待处理回复。本轮未逐项重跑外部环境。 |
| [#14 0.7.0版：关于“目录自动跳转 ”行为的建议](https://github.com/LingCore/WinShun/issues/14) | CLOSED | 保持关闭 | 历史已有处理记录；未见新增待处理回复。本轮未逐项重跑外部环境。 |
| [#15 安装报错](https://github.com/LingCore/WinShun/issues/15) | CLOSED | 保持关闭 | 历史已有处理记录；未见新增待处理回复。本轮未逐项重跑外部环境。 |
| [#16 0.7.3：关于搜索界面按tab健切换逻辑“反直觉”的问题](https://github.com/LingCore/WinShun/issues/16) | CLOSED | 保持关闭 | 历史已有处理记录；未见新增待处理回复。本轮未逐项重跑外部环境。 |
| [#17 自动备份功能+网盘挂载扫描](https://github.com/LingCore/WinShun/issues/17) | CLOSED | 保持关闭 | 备份和网络位置已有实现与历史回复；本轮不声称重新实测每一种网盘。 |
| [#19 便携版建议](https://github.com/LingCore/WinShun/issues/19) | CLOSED | 保持关闭 | 历史已有处理记录；未见新增待处理回复。本轮未逐项重跑外部环境。 |
| [#20 右键 搜索指定目录(类似 Everything)，便于缩小范围](https://github.com/LingCore/WinShun/issues/20) | CLOSED | 保持关闭 | 历史已有处理记录；未见新增待处理回复。本轮未逐项重跑外部环境。 |
| [#21 [Feature Request] 系统超管权限与GUI权限分离](https://github.com/LingCore/WinShun/issues/21) | CLOSED | 保持关闭，关联回归 | 服务与普通权限 GUI 已分离；高权限向日葵安装器回归另见 #104。 |
| [#22 【Feature request】双击桌面和explorer弹出快捷收藏文件夹菜单](https://github.com/LingCore/WinShun/issues/22) | CLOSED | 保持关闭 | 历史已有处理记录；未见新增待处理回复。本轮未逐项重跑外部环境。 |
| [#23 期望能支持DO及TC以外的第三方文件管理器](https://github.com/LingCore/WinShun/issues/23) | CLOSED | 保持关闭 | 历史已有处理记录；未见新增待处理回复。本轮未逐项重跑外部环境。 |
| [#24 提一个用户体验上的小问题](https://github.com/LingCore/WinShun/issues/24) | CLOSED | 保持关闭 | 历史已有处理记录；未见新增待处理回复。本轮未逐项重跑外部环境。 |
| [#25 关于双击空白处，弹出文件夹菜单](https://github.com/LingCore/WinShun/issues/25) | CLOSED | 保持关闭 | 历史已有处理记录；未见新增待处理回复。本轮未逐项重跑外部环境。 |
| [#26 搜索结果页，支持按住 Shift键，显示系统原生右键菜单](https://github.com/LingCore/WinShun/issues/26) | CLOSED | 保持关闭 | 历史已有处理记录；未见新增待处理回复。本轮未逐项重跑外部环境。 |
| [#27 搜索结果页，图片、文本 等预览，尤其是 图片，命名不规范不预览根本不知道是什么](https://github.com/LingCore/WinShun/issues/27) | CLOSED | 保持关闭 | 历史已有处理记录；未见新增待处理回复。本轮未逐项重跑外部环境。 |
| [#28 文件内容搜索，大小上限 支持 2到3位小数，最低 100KB 还是大了点](https://github.com/LingCore/WinShun/issues/28) | CLOSED | 保持关闭 | 历史已有处理记录；未见新增待处理回复。本轮未逐项重跑外部环境。 |
| [#29 建议：1.按shitf+tab和tab，只在“全部/文件/内容”标签之间切换，与windows的切换行为保持一致。2.顶级标签使用0.5.0的设计，“全部/文件/文件夹/内容”](https://github.com/LingCore/WinShun/issues/29) | CLOSED | 保持关闭 | 历史已有处理记录；未见新增待处理回复。本轮未逐项重跑外部环境。 |
| [#30 Featuring WinShun in Devs Helping Devs (Fri Oct 16)](https://github.com/LingCore/WinShun/issues/30) | CLOSED | 保持关闭 | 历史已有处理记录；未见新增待处理回复。本轮未逐项重跑外部环境。 |
| [#31 在微信获得焦点时，双击ctrl无法弹出搜索对话框。](https://github.com/LingCore/WinShun/issues/31) | CLOSED | 保持关闭 | 历史已有处理记录；未见新增待处理回复。本轮未逐项重跑外部环境。 |
| [#32 粘贴板历史记录给个无限制吧！](https://github.com/LingCore/WinShun/issues/32) | CLOSED | 保持关闭 | 历史已有处理记录；未见新增待处理回复。本轮未逐项重跑外部环境。 |
| [#33 建议：修改按ESC键返回的行为逻辑。](https://github.com/LingCore/WinShun/issues/33) | CLOSED | 保持关闭 | 历史已有处理记录；未见新增待处理回复。本轮未逐项重跑外部环境。 |
| [#34 占用内存有点大](https://github.com/LingCore/WinShun/issues/34) | CLOSED | 保持关闭 | 历史已有处理记录；未见新增待处理回复。本轮未逐项重跑外部环境。 |
| [#35 在任务栏使用win顺可能会导致卡死](https://github.com/LingCore/WinShun/issues/35) | CLOSED | 保持关闭 | 历史已有处理记录；未见新增待处理回复。本轮未逐项重跑外部环境。 |
| [#36 内容索引有点慢](https://github.com/LingCore/WinShun/issues/36) | OPEN | 保留：待诊断 | 旧版机械硬盘内容索引性能反馈，尚缺新版耗时和诊断；不以文件名搜索测试代替内容索引吞吐验证。 |
| [#37 Windows资源管理器，右键 文件夹 或 文件，已经打开菜单并且有快捷键时，不要弹出搜索框](https://github.com/LingCore/WinShun/issues/37) | CLOSED | 保持关闭 | 历史已有处理记录；未见新增待处理回复。本轮未逐项重跑外部环境。 |
| [#38 改进建议：剪贴板布局自定义和窗口弹出位置优化](https://github.com/LingCore/WinShun/issues/38) | CLOSED | 保持关闭 | 历史已有处理记录；未见新增待处理回复。本轮未逐项重跑外部环境。 |
| [#39 开启 在资源管理器里直接打字搜索，匹配到 当前目录的直接子目录，是否可以把 当前窗口跳转到子目录，而不是开新窗口，类似 Listary](https://github.com/LingCore/WinShun/issues/39) | CLOSED | 保持关闭 | 历史已有处理记录；未见新增待处理回复。本轮未逐项重跑外部环境。 |
| [#40 能否增加一个可选的 Everything 兼容服务，方便 Directory Opus 等调用？](https://github.com/LingCore/WinShun/issues/40) | OPEN | 保留：未实现需求 | 没有 Everything IPC 兼容服务；现有 CLI/MCP 不能冒充这项兼容协议。 |
| [#41 建议：增加开源文件资源管理器pulse的快速文件夹跳转ctrl+G的支持，或可以自定义](https://github.com/LingCore/WinShun/issues/41) | CLOSED | 保持关闭 | 历史已有处理记录；未见新增待处理回复。本轮未逐项重跑外部环境。 |
| [#42 0.10.0：在剪贴板窗口无法输入中文](https://github.com/LingCore/WinShun/issues/42) | CLOSED | 保持关闭 | 历史已有处理记录；未见新增待处理回复。本轮未逐项重跑外部环境。 |
| [#43 0.10.0：在剪贴板窗口搜索，出现重叠的文字。](https://github.com/LingCore/WinShun/issues/43) | CLOSED | 保持关闭 | 历史已有处理记录；未见新增待处理回复。本轮未逐项重跑外部环境。 |
| [#44 提几个使用建议，大佬看下哈](https://github.com/LingCore/WinShun/issues/44) | CLOSED | 保持关闭 | 历史已有处理记录；未见新增待处理回复。本轮未逐项重跑外部环境。 |
| [#45 资源管理器鼠标左边双击没有生效](https://github.com/LingCore/WinShun/issues/45) | CLOSED | 保持关闭 | 历史已有处理记录；未见新增待处理回复。本轮未逐项重跑外部环境。 |
| [#46 功能建议：截图鼠标快速操作](https://github.com/LingCore/WinShun/issues/46) | CLOSED | 保持关闭 | 历史已有处理记录；未见新增待处理回复。本轮未逐项重跑外部环境。 |
| [#47 功能建议：鼠标指向剪贴板里面的内容时可以直接预览](https://github.com/LingCore/WinShun/issues/47) | CLOSED | 保持关闭 | 历史已有处理记录；未见新增待处理回复。本轮未逐项重跑外部环境。 |
| [#48 截图 支持 框选、写文字 等常见操作](https://github.com/LingCore/WinShun/issues/48) | CLOSED | 保持关闭 | 历史已有处理记录；未见新增待处理回复。本轮未逐项重跑外部环境。 |
| [#49 同一目录有 F1、F11 两个文件夹，右键 F1文件夹 选择 “用Win顺搜索此处”，会把 F11的文件也显示出来](https://github.com/LingCore/WinShun/issues/49) | CLOSED | 保持关闭 | 历史已有处理记录；未见新增待处理回复。本轮未逐项重跑外部环境。 |
| [#50 安装后桌面进程树继承 Inno Setup 6.7 的 RedirectionGuard，导致 Scoop / WSL 等所有 junction 失效](https://github.com/LingCore/WinShun/issues/50) | CLOSED | 保持关闭 | 历史已有处理记录；未见新增待处理回复。本轮未逐项重跑外部环境。 |
| [#51 功能优化建议：关于双击空白处弹出文件夹菜单](https://github.com/LingCore/WinShun/issues/51) | CLOSED | 保持关闭 | 历史已有处理记录；未见新增待处理回复。本轮未逐项重跑外部环境。 |
| [#52 建议增加无管理员权限的“普通用户模式”：支持手动指定文件夹与定时扫描](https://github.com/LingCore/WinShun/issues/52) | CLOSED | 保持关闭 | 历史已有处理记录；未见新增待处理回复。本轮未逐项重跑外部环境。 |
| [#53 打开Win顺的快捷键希望能改成双击Alt](https://github.com/LingCore/WinShun/issues/53) | CLOSED | 保持关闭 | 历史已有处理记录；未见新增待处理回复。本轮未逐项重跑外部环境。 |
| [#54 剪贴板功能建议：可以设置隐藏或者清理已不存在的文件操作记录](https://github.com/LingCore/WinShun/issues/54) | CLOSED | 保持关闭 | 历史已有处理记录；未见新增待处理回复。本轮未逐项重跑外部环境。 |
| [#55 剪贴板用起来不顺手](https://github.com/LingCore/WinShun/issues/55) | CLOSED | 保持关闭 | 历史已有处理记录；未见新增待处理回复。本轮未逐项重跑外部环境。 |
| [#56 能支持从 Ditto、txt、csv 等导入 剪贴板 么](https://github.com/LingCore/WinShun/issues/56) | OPEN | 重开：本地已修，未发布 | 导入 lDontAutoDelete 到固定分组；普通 sticky 排序不等同禁止删除。真实 Ditto 表结构夹具、持久化、重复导入已测。 |
| [#57 剪贴板和截图功能建议](https://github.com/LingCore/WinShun/issues/57) | CLOSED | 保持关闭 | 历史已有处理记录；未见新增待处理回复。本轮未逐项重跑外部环境。 |
| [#58 希望能复制、粘贴wps、word文档中的公式](https://github.com/LingCore/WinShun/issues/58) | CLOSED | 保持关闭 | 历史已有处理记录；未见新增待处理回复。本轮未逐项重跑外部环境。 |
| [#59 截图建议](https://github.com/LingCore/WinShun/issues/59) | CLOSED | 保持关闭 | 历史已有处理记录；未见新增待处理回复。本轮未逐项重跑外部环境。 |
| [#60 FEATURE: `Ctrl+Enter` open file location should open it in the opened explorer](https://github.com/LingCore/WinShun/issues/60) | CLOSED | 保持关闭 | 历史已有处理记录；未见新增待处理回复。本轮未逐项重跑外部环境。 |
| [#61 请教是否有自定义命令功能](https://github.com/LingCore/WinShun/issues/61) | OPEN | 重开：未完成需求 | 自定义命令已存在，但文件右键菜单传入所选文件的入口仍未实现；本轮的文件夹菜单入口不能代替它。 |
| [#62 这种安装界面能想办法加上搜索框吗？](https://github.com/LingCore/WinShun/issues/62) | CLOSED | 保持关闭 | 历史已有处理记录；未见新增待处理回复。本轮未逐项重跑外部环境。 |
| [#63 询问](https://github.com/LingCore/WinShun/issues/63) | OPEN | 保留：未实现需求 | Flow Launcher 插件兼容层尚未实现；原生插件库是另一种接口，不关闭为已兼容。 |
| [#64 弹出安全警告：这是正常的情况么？目前只有打开福昕出现。](https://github.com/LingCore/WinShun/issues/64) | CLOSED | 保持关闭 | 历史已有处理记录；未见新增待处理回复。本轮未逐项重跑外部环境。 |
| [#65 请问能否像 Lertaro 或 Everything 有支持 Directory Opus 的文件（文件夹）大小脚本显示对应的列字段？](https://github.com/LingCore/WinShun/issues/65) | OPEN | 保留：未实现需求 | Directory Opus 大小列脚本接口缺失；搜索界面能计算文件夹大小不能代替外部接口。与 #40 关联。 |
| [#66 在开始菜单里打字，也用Win顺搜索如果打开开始菜单马上输入文字，文字顺序会错乱](https://github.com/LingCore/WinShun/issues/66) | OPEN | 保留：待原场景复测 | 0.16 已含开始菜单输入顺序调整；本轮相关输入测试通过，仍缺原输入法/开始菜单快速输入复测。 |
| [#67 开机自启怎么不能关？](https://github.com/LingCore/WinShun/issues/67) | CLOSED | 保持关闭 | 历史已有处理记录；未见新增待处理回复。本轮未逐项重跑外部环境。 |
| [#68 字体安装问题](https://github.com/LingCore/WinShun/issues/68) | CLOSED | 保持关闭 | 历史已有处理记录；未见新增待处理回复。本轮未逐项重跑外部环境。 |
| [#69 增加自动升级功能](https://github.com/LingCore/WinShun/issues/69) | CLOSED | 保持关闭 | 历史已有处理记录；未见新增待处理回复。本轮未逐项重跑外部环境。 |
| [#70 文字变了！](https://github.com/LingCore/WinShun/issues/70) | OPEN | 保留：待原环境复现 | 长时间运行后字形错误，重启恢复；没有可证明根因的现场诊断，不关闭为字体问题已修复。 |
| [#71 FEATURE: 新的截图建议](https://github.com/LingCore/WinShun/issues/71) | CLOSED | 保持关闭 | 历史已有处理记录；未见新增待处理回复。本轮未逐项重跑外部环境。 |
| [#72 建议增加搜索某种类型结果的快捷键](https://github.com/LingCore/WinShun/issues/72) | CLOSED | 保持关闭 | 历史已有处理记录；未见新增待处理回复。本轮未逐项重跑外部环境。 |
| [#73 插件生态](https://github.com/LingCore/WinShun/issues/73) | OPEN | 保留：持续需求 | 原生插件库、SDK 与投稿流程已有；生态建设和独立商店网页不等于一次缺陷修复。 |
| [#74 在多屏多缩放倍率情况下，电脑休眠唤醒后软件窗口可能会越来越小](https://github.com/LingCore/WinShun/issues/74) | OPEN | 保留：待硬件复测 | 几何与缩放测试通过；无法代替原多屏、不同 DPR、休眠唤醒的长期实机验证。 |
| [#75 文件列表 可选带时间；大小排序 时显示大小；展开为 搜索面板 时，带 排序 设置过去](https://github.com/LingCore/WinShun/issues/75) | CLOSED | 保持关闭 | 历史已有处理记录；未见新增待处理回复。本轮未逐项重跑外部环境。 |
| [#76 FEATURE: 磁盘索引范围](https://github.com/LingCore/WinShun/issues/76) | CLOSED | 保持关闭 | 历史已有处理记录；未见新增待处理回复。本轮未逐项重跑外部环境。 |
| [#77 搜索范围中可以自行添加要索引的范围吗](https://github.com/LingCore/WinShun/issues/77) | CLOSED | 保持关闭 | 历史已有处理记录；未见新增待处理回复。本轮未逐项重跑外部环境。 |
| [#78 管理员权限运行时，无法添加“索引的文件夹”，之前添加的也没有用](https://github.com/LingCore/WinShun/issues/78) | CLOSED | 保持关闭 | 历史已有处理记录；未见新增待处理回复。本轮未逐项重跑外部环境。 |
| [#79 关于截图与剪贴板模块：建议参考成熟开源方案及架构轻量化](https://github.com/LingCore/WinShun/issues/79) | OPEN | 保留：工作区已有改造 | 任务开始前已有截图/扩展外移及剪贴板释放改造。本轮复核构建、生命周期测试和隔离核心运行；尚未发布。 |
| [#80 建议为双击 Ctrl 搜索窗口增加“显示文件大小”](https://github.com/LingCore/WinShun/issues/80) | CLOSED | 保持关闭 | 历史已有处理记录；未见新增待处理回复。本轮未逐项重跑外部环境。 |
| [#81 建议：设置收藏夹后，搜索框界面tab键功能保持一致。](https://github.com/LingCore/WinShun/issues/81) | CLOSED | 保持关闭 | 历史已有处理记录；未见新增待处理回复。本轮未逐项重跑外部环境。 |
| [#82 在Directory Opus中直接打字，不会弹出win顺的文件搜索框，而是使用的Directory Opus的内部搜索功能](https://github.com/LingCore/WinShun/issues/82) | OPEN | 重开：仍有后续需求 | DO 直接打字已实现；新回复中的全局搜索行为与屏幕右下方位置要求没有全部交付。 |
| [#83 截图建议](https://github.com/LingCore/WinShun/issues/83) | CLOSED | 保持关闭 | 历史已有处理记录；未见新增待处理回复。本轮未逐项重跑外部环境。 |
| [#84 插件库 连接 GitHub 超时 在哪里设置代理啊](https://github.com/LingCore/WinShun/issues/84) | CLOSED | 保持关闭 | 历史已有处理记录；未见新增待处理回复。本轮未逐项重跑外部环境。 |
| [#85 期望可以增加计算器功能](https://github.com/LingCore/WinShun/issues/85) | CLOSED | 保持关闭 | 历史已有处理记录；未见新增待处理回复。本轮未逐项重跑外部环境。 |
| [#86 剪切板搜索异常](https://github.com/LingCore/WinShun/issues/86) | OPEN | 保留：待原环境复现 | Win10/微信/网页偶发按键转发失效仍需现场诊断；新一轮通用路由测试不证明原故障消失。 |
| [#87 外观里的“圆角”和“界面大小”选项好像没用](https://github.com/LingCore/WinShun/issues/87) | CLOSED | 保持关闭 | 历史已有处理记录；未见新增待处理回复。本轮未逐项重跑外部环境。 |
| [#88 Search panel: configurable columns, and dates/sizes for plugin rows · 搜索面板：可配置的列，插件结果也能有日期和大小](https://github.com/LingCore/WinShun/issues/88) | CLOSED | 保持关闭 | 历史已有处理记录；未见新增待处理回复。本轮未逐项重跑外部环境。 |
| [#89 【建议】截图工具希望支持gif录制](https://github.com/LingCore/WinShun/issues/89) | CLOSED | 保持关闭 | 历史已有处理记录；未见新增待处理回复。本轮未逐项重跑外部环境。 |
| [#90 【优化】收藏夹面板中“打开文件”等选项，支持“文件夹菜单”功能](https://github.com/LingCore/WinShun/issues/90) | CLOSED | 保持关闭 | 历史已有处理记录；未见新增待处理回复。本轮未逐项重跑外部环境。 |
| [#91 剪贴板合并复制bug](https://github.com/LingCore/WinShun/issues/91) | CLOSED | 保持关闭 | 历史已有处理记录；未见新增待处理回复。本轮未逐项重跑外部环境。 |
| [#92 【建议】增加收藏夹中“添加命令”中图标的功能](https://github.com/LingCore/WinShun/issues/92) | CLOSED | 保持关闭 | 历史已有处理记录；未见新增待处理回复。本轮未逐项重跑外部环境。 |
| [#93 FEATURE: 支持高级搜索语法](https://github.com/LingCore/WinShun/issues/93) | OPEN | 重开：本地已修，未发布 | 补 re: 正则别名，保留引号、排除、大小写与错误反馈；查询语法测试通过。 |
| [#94 希望支持大图文件显示模式！！！](https://github.com/LingCore/WinShun/issues/94) | CLOSED | 保持关闭 | 历史已有处理记录；未见新增待处理回复。本轮未逐项重跑外部环境。 |
| [#95 对话框的搜索框可能未置顶&搜索框位置不固定](https://github.com/LingCore/WinShun/issues/95) | CLOSED | 保持关闭 | 历史已有处理记录；未见新增待处理回复。本轮未逐项重跑外部环境。 |
| [#96 BUG: 检查更新后界面卡死](https://github.com/LingCore/WinShun/issues/96) | CLOSED | 保持关闭 | 历史已有处理记录；未见新增待处理回复。本轮未逐项重跑外部环境。 |
| [#97 功能改进：安装包安装前的MCP运行状态检测；更新时文件被占用的用户友好提示](https://github.com/LingCore/WinShun/issues/97) | OPEN | 重开：本地已修，未发布 | 增加重新检测/尝试安装/取消，非交互安装不自动忽略；Inno 6.7 编译及独立原生对话框返回值 4/5/3 已验。未在用户杀软环境执行更新。 |
| [#98 功能建议：录屏时的键盘/鼠标输入提示](https://github.com/LingCore/WinShun/issues/98) | CLOSED | 保持关闭 | 历史已有处理记录；未见新增待处理回复。本轮未逐项重跑外部环境。 |
| [#99 功能建议：截图保存后点击系统通知可前往对应目录](https://github.com/LingCore/WinShun/issues/99) | CLOSED | 保持关闭 | 历史已有处理记录；未见新增待处理回复。本轮未逐项重跑外部环境。 |
| [#100 Press the shortcut twice to open the search panel directly · 连按两次快捷键直接打开搜索面板](https://github.com/LingCore/WinShun/issues/100) | CLOSED | 保持关闭 | 历史已有处理记录；未见新增待处理回复。本轮未逐项重跑外部环境。 |
| [#101 FEATURE: 截图控件中添加文字描边功能](https://github.com/LingCore/WinShun/issues/101) | CLOSED | 保持关闭 | 历史已有处理记录；未见新增待处理回复。本轮未逐项重跑外部环境。 |
| [#102 Win+Shift+S stays taken from Windows after "Win+S" is off: `S` is never removed from DisabledHotkeys · 关掉 Win+S 后 Win+Shift+S 仍不归还系统：DisabledHotkeys 里的 S 不会被删掉](https://github.com/LingCore/WinShun/issues/102) | CLOSED | 保持关闭 | 历史已有处理记录；未见新增待处理回复。本轮未逐项重跑外部环境。 |
| [#103 搜狗输入法在资源管理器里直接打字搜索的bug](https://github.com/LingCore/WinShun/issues/103) | CLOSED | 已回复并关闭 | 用户明确确认 0.16 修复搜狗首字母问题；按其确认关闭。 |
| [#104 安装向日葵时搜索框无法正常跳转路径](https://github.com/LingCore/WinShun/issues/104) | OPEN | 重开：回归未解决 | 用户明确反馈安装版 0.16 普通权限仍不跳转、提升 GUI 权限才成功；不能以管理员运行作为修复证据。 |
| [#105 功能建议: 剪贴板永久跟随光标位置](https://github.com/LingCore/WinShun/issues/105) | OPEN | 保留：本地已修，未发布 | 新增每次在鼠标指针旁打开的独立选项；配置往返与真实设置 UI 通过。 |
| [#106 CTRL+E搜索界面右键调整界面选择显示列的弹出窗口被挡住了](https://github.com/LingCore/WinShun/issues/106) | CLOSED | 保持关闭 | 历史已有处理记录；未见新增待处理回复。本轮未逐项重跑外部环境。 |
| [#107 剪贴板功能建议：持续按快捷键选择并粘贴](https://github.com/LingCore/WinShun/issues/107) | CLOSED | 保持关闭，合并 #137 | 尾行回到首行已补修；本机窗口中的继续浏览与新增菜单需求统一在 #137 跟踪。 |
| [#108 在文件管理器中输入直接进行搜索](https://github.com/LingCore/WinShun/issues/108) | CLOSED | 保持关闭 | 历史已有处理记录；未见新增待处理回复。本轮未逐项重跑外部环境。 |
| [#109 双击ctrl搜索页面logo美化建议](https://github.com/LingCore/WinShun/issues/109) | CLOSED | 保持关闭 | 历史已有处理记录；未见新增待处理回复。本轮未逐项重跑外部环境。 |
| [#110 文件管理器中直接键入搜索也能搜到当前目录之外的内容](https://github.com/LingCore/WinShun/issues/110) | CLOSED | 保持关闭 | 历史已有处理记录；未见新增待处理回复。本轮未逐项重跑外部环境。 |
| [#111 响应很慢！](https://github.com/LingCore/WinShun/issues/111) | OPEN | 保留：本地已修，未发布 | 移除鼠标位移对双击键盘修饰键的否决；Ctrl/Alt/Shift 左右两侧六种测试通过，按键/鼠标点击/滚轮/组合键干扰仍排除。其他偶发延迟不据此宣称全修。 |
| [#112 自定义命令能否添加"静默执行"选项？](https://github.com/LingCore/WinShun/issues/112) | CLOSED | 保持关闭 | 历史已有处理记录；未见新增待处理回复。本轮未逐项重跑外部环境。 |
| [#113 功能建议：能不能加一个局域网文件传输功能](https://github.com/LingCore/WinShun/issues/113) | CLOSED | 已回复并关闭 | 0.16 已交付 LocalSend 兼容收发入口；最新回复讨论另一工具，没有新的 WinShun 收发故障。 |
| [#114 建议增加“网络与WSL”的内容索引功能](https://github.com/LingCore/WinShun/issues/114) | CLOSED | 保持关闭 | 历史已有处理记录；未见新增待处理回复。本轮未逐项重跑外部环境。 |
| [#115 启用索引后检索速度也很慢](https://github.com/LingCore/WinShun/issues/115) | CLOSED | 已回复并关闭 | 用户明确确认 0.16 恢复搜索速度；按其确认关闭。 |
| [#116 BUG: 网络与WSL 显示离线](https://github.com/LingCore/WinShun/issues/116) | OPEN | 保留：本地部分修复 | 实际打开优先采用活动映射盘，最长根路径匹配；索引标识仍为 UNC，属性查询移出 UI 线程。原 NAS 重启/恢复后的延迟仍待实机复测。 |
| [#117 希望翻译功能能多加一些服务](https://github.com/LingCore/WinShun/issues/117) | CLOSED | 保持关闭 | 历史已有处理记录；未见新增待处理回复。本轮未逐项重跑外部环境。 |
| [#118 功能建议：新增休息功能（定时弹窗](https://github.com/LingCore/WinShun/issues/118) | CLOSED | 保持关闭 | 历史已有处理记录；未见新增待处理回复。本轮未逐项重跑外部环境。 |
| [#119 功能建议: 最近打开项目数量&文件类型&合并"全部"和"文件"](https://github.com/LingCore/WinShun/issues/119) | OPEN | 保留：本地部分完成 | 最近打开数量 1–200，类型条件支持前后冒号及词间位置，保护引号/盘符；全部/文件合并及全部标签常驻类型筛选仍未实现。 |
| [#120 [Bug] 剪贴板面板首次唤醒白屏（暗色模式）](https://github.com/LingCore/WinShun/issues/120) | CLOSED | 保持关闭 | 历史已有处理记录；未见新增待处理回复。本轮未逐项重跑外部环境。 |
| [#121 功能建议：截屏与剪贴板也增加全屏时屏蔽快捷键功能](https://github.com/LingCore/WinShun/issues/121) | OPEN | 保留：本地已实现，未发布 | 剪贴板和截图分别新增默认关闭的全屏暂停热键选项；持久化、前台规则、真实截图设置开关已验。具体游戏/全屏应用仍需复测。 |
| [#122 希望增加剪切板的功能](https://github.com/LingCore/WinShun/issues/122) | CLOSED | 保持关闭 | 历史已有处理记录；未见新增待处理回复。本轮未逐项重跑外部环境。 |
| [#123 计算器插件减法bug](https://github.com/LingCore/WinShun/issues/123) | OPEN | 重开：本地已修，未发布 | 触发器补连续减法和连续除法；500-498-1、1/2/2 等结果与日期/电话排除测试通过。 |
| [#124 截图编辑界面的黑屏问题](https://github.com/LingCore/WinShun/issues/124) | OPEN | 保留：待原环境复现 | 截图标注黑块缺硬件与诊断，未能复现；空格直接编辑建议亦不能算已实现。 |
| [#125 BUG: 减法算数无法直接触发](https://github.com/LingCore/WinShun/issues/125) | CLOSED | 保持关闭，合并 #123 | 原减法触发及新连续算式统一由 #123 跟踪，避免重复关闭后漏回复。 |
| [#126 FEATURE: 更多的贴图快捷键](https://github.com/LingCore/WinShun/issues/126) | CLOSED | 保持关闭 | 历史已有处理记录；未见新增待处理回复。本轮未逐项重跑外部环境。 |
| [#127 BUG: 修改收藏圆盘的文字描述](https://github.com/LingCore/WinShun/issues/127) | CLOSED | 保持关闭 | 历史已有处理记录；未见新增待处理回复。本轮未逐项重跑外部环境。 |
| [#128 功能建议：资源管理器右键图片贴图](https://github.com/LingCore/WinShun/issues/128) | OPEN | 保留：未实现需求 | 资源管理器图片右键直接贴图的入口尚未实现；复制到剪贴板再贴图不等同此入口。 |
| [#129 功能建议：基于OCR文字定位的截图快速打码](https://github.com/LingCore/WinShun/issues/129) | OPEN | 保留：未实现需求 | 按 OCR 文字框逐块点击切换打码尚未实现；与自动判断敏感信息、整体 OCR 精度是不同需求。应留在可选截图插件。 |
| [#130 【优化】搜索到的图片，复制之后，在WPS里面优先粘贴为路径，而不是图片](https://github.com/LingCore/WinShun/issues/130) | CLOSED | 保持关闭 | 历史已有处理记录；未见新增待处理回复。本轮未逐项重跑外部环境。 |
| [#131 关于 Seer 预览的 版本支持？](https://github.com/LingCore/WinShun/issues/131) | CLOSED | 保持关闭 | 历史已有处理记录；未见新增待处理回复。本轮未逐项重跑外部环境。 |
| [#132 建议：搜索范围中可以排除文件的名称](https://github.com/LingCore/WinShun/issues/132) | CLOSED | 保持关闭 | 历史已有处理记录；未见新增待处理回复。本轮未逐项重跑外部环境。 |
| [#133 截图希望能增加对hdr的支持](https://github.com/LingCore/WinShun/issues/133) | CLOSED | 保持关闭 | 历史已有处理记录；未见新增待处理回复。本轮未逐项重跑外部环境。 |
| [#134 快键键呼出面板和剪贴板卡顿](https://github.com/LingCore/WinShun/issues/134) | OPEN | 保留：部分关联修复 | 鼠标移动导致双击修饰键失效归 #111；剪贴板首次打开/直接粘贴的独立迟滞不能据此关闭。 |
| [#135 希望搜索支持模糊匹配；收藏内容可选是否显示](https://github.com/LingCore/WinShun/issues/135) | OPEN | 保留：本地已实现，未发布 | 英文程序词首/驼峰首字母匹配与文件索引预筛选均支持 hsm；空输入时收藏显示可独立关闭，关键词仍可找到。 |
| [#136 建议和BUG: 默认以管理员权限运行 & 安装版不应该再显示"便携版的索引服务"](https://github.com/LingCore/WinShun/issues/136) | OPEN | 保留：待用户补充 | 已有管理员直读、自启后台与 data 目录解释；尚无对安装目录 data 和弹窗类型的后续确认，不删除用户数据。 |
| [#137 希望增强与 listary 类似的鼠标中键功能](https://github.com/LingCore/WinShun/issues/137) | OPEN | 保留：本地改进，未发布 | 增加中键空白处菜单、当前目录终端/PowerShell、收藏命令和设置入口；尾行循环及本机窗口只浏览路径已补。真实 QML 循环测试通过，未声称已逐个实测所有第三方窗口。 |
| [#138 大哥, 少即是多!](https://github.com/LingCore/WinShun/issues/138) | OPEN | 保留：工作区已有改造 | 预先存在的主程序/插件瘦身改造未发布。本轮补全相关回归，核心隔离 App::start 仅一个主进程，启动 593 ms、私有内存约 76 MiB；单次观察不是性能承诺。 |
| [#139 不能正常启动命令行程序](https://github.com/LingCore/WinShun/issues/139) | OPEN | 保留：本地补修，未发布 | 补 AppsFolder 控制台目标解析与启动；系统 PowerShell 的真实 AppsFolder 入口父进程/工作目录测试通过，原版 Surge 最终行为待用户复测。 |

## 本轮回复链接

- [#4](https://github.com/LingCore/WinShun/issues/4#issuecomment-6525908996)：reopen
- [#56](https://github.com/LingCore/WinShun/issues/56#issuecomment-6525910564)：reopen
- [#61](https://github.com/LingCore/WinShun/issues/61#issuecomment-6525912107)：reopen
- [#79](https://github.com/LingCore/WinShun/issues/79#issuecomment-6525931813)：keep
- [#82](https://github.com/LingCore/WinShun/issues/82#issuecomment-6525913688)：reopen
- [#93](https://github.com/LingCore/WinShun/issues/93#issuecomment-6525915496)：reopen
- [#97](https://github.com/LingCore/WinShun/issues/97#issuecomment-6525917206)：reopen
- [#104](https://github.com/LingCore/WinShun/issues/104#issuecomment-6525918686)：reopen
- [#105](https://github.com/LingCore/WinShun/issues/105#issuecomment-6525920274)：keep
- [#107](https://github.com/LingCore/WinShun/issues/107#issuecomment-6525921231)：keep
- [#111](https://github.com/LingCore/WinShun/issues/111#issuecomment-6525922095)：keep
- [#113](https://github.com/LingCore/WinShun/issues/113#issuecomment-6525923097)：close
- [#116](https://github.com/LingCore/WinShun/issues/116#issuecomment-6525924504)：keep
- [#119](https://github.com/LingCore/WinShun/issues/119#issuecomment-6525925354)：keep
- [#121](https://github.com/LingCore/WinShun/issues/121#issuecomment-6525926124)：keep
- [#123](https://github.com/LingCore/WinShun/issues/123#issuecomment-6525926963)：reopen
- [#134](https://github.com/LingCore/WinShun/issues/134#issuecomment-6525928391)：keep
- [#135](https://github.com/LingCore/WinShun/issues/135#issuecomment-6525929104)：keep
- [#137](https://github.com/LingCore/WinShun/issues/137#issuecomment-6525929936)：keep
- [#138](https://github.com/LingCore/WinShun/issues/138#issuecomment-6525930824)：keep
- [#139](https://github.com/LingCore/WinShun/issues/139#issuecomment-6525932734)：keep
- [#103](https://github.com/LingCore/WinShun/issues/103#issuecomment-6524318499)：用户确认后关闭
- [#115](https://github.com/LingCore/WinShun/issues/115#issuecomment-6524320469)：用户确认后关闭
