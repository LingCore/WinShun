# 2026-11-07 GitHub Issues 核查与收尾

初始逐条核查 **58** 条未关闭 Issue，已先回复再以 `completed` 关闭 **23** 条，其余 **35** 条本轮保留。收尾时 GitHub 实际未关闭数量为 **35**。

2026-11-07 后续经维护者确认，将 #200 归并到 #170，将 #224 归并到 #186。两条原帖以 `DUPLICATE` 关闭，主帖的汇总评论继续跟踪剩余需求。归并后 GitHub 实际未关闭数量为 **33**，具体回复见本文末尾的归并记录。

## 判断依据与验证边界

- 阅读各 Issue 的正文及全部后续评论，特别核对重新打开原因和最新补充需求。没有仅凭标题、旧的“已改好”回复或缺少新回复就关闭。
- 主仓库核查时 HEAD 与远端 main 均为 `c4381ddc4d2e2edddeb9b89a1199174946602e3f`。公开 [v0.22.0 Release](https://github.com/LingCore/WinShun/releases/tag/v0.22.0) 与标签均指向 `a7696f48028a0a0c898b506deed09846624af050`，发布时间为 2026-11-07 10:44:30（UTC+8）。
- 对涉及的修复提交逐一检查其属于上述发布提交的祖先，并阅读受影响的生产路径、定向测试与归档验证。插件库原生包规则另核对公开提交 `65cf74ea2330e9441fa868725bcb87274116354d`。
- 复用了 [0.22.0 发布验证](../release-validation-v0.22.0.md) 及各批次记录；本轮没有构建、重新跑测试或在报告者原机复现问题。发布归档的 38 项 CTest 中 37 项首轮通过，粘贴项遇并行前台干扰后单独复测连续三次通过；这不是本轮新运行的结果。
- #219 按请求中允许的社区外部插件方案收尾，仅核查功能文档与公开发布附件，没有运行第三方命令或声称完成宿主 GUI 实测。#159 另有用户明确确认新版恢复。
- 每次写入前重新读取在线正文与评论，检查有无并发变化；先验证回复实际存在，再关闭，随后逐项复查 `CLOSED / COMPLETED`。评论均保留 AI triage 声明。没有为本轮重新设计标签体系。

## 已回复并关闭

| Issue | 收尾依据 | 实际回复 |
| --- | --- | --- |
| [#119](https://github.com/LingCore/WinShun/issues/119) | 最近项目数量、类型条件与全部/文件合并已交付 [验证](issues-119-204-217-20261106.md) | [评论](https://github.com/LingCore/WinShun/issues/119#issuecomment-6701395218) |
| [#136](https://github.com/LingCore/WinShun/issues/136) | 手动启动遵循管理员直读；服务文案已修正，后台启动已有入口 [验证](issues-136-208-213-20261106.md) | [评论](https://github.com/LingCore/WinShun/issues/136#issuecomment-6701396339) |
| [#138](https://github.com/LingCore/WinShun/issues/138) | 截图、LocalSend 等可选插件化已发布；内存具体问题另由 #227 跟踪 | [评论](https://github.com/LingCore/WinShun/issues/138#issuecomment-6701397359) |
| [#159](https://github.com/LingCore/WinShun/issues/159) | 提问者已确认新版正常 | [评论](https://github.com/LingCore/WinShun/issues/159#issuecomment-6701399861) |
| [#161](https://github.com/LingCore/WinShun/issues/161) | 跨词学习及常用结果超过闲置收藏均已发布 [验证](issues-161-236-237-20261106.md) | [评论](https://github.com/LingCore/WinShun/issues/161#issuecomment-6701400992) |
| [#174](https://github.com/LingCore/WinShun/issues/174) | 独立大小设置与 200ms 悬停预览全部交付 [验证](issues-174-176-186-20261106.md) | [评论](https://github.com/LingCore/WinShun/issues/174#issuecomment-6701401940) |
| [#197](https://github.com/LingCore/WinShun/issues/197) | 键盘分组查找、进入分组和组内搜索已发布 [验证](issues-197-200-206-20261106.md) | [评论](https://github.com/LingCore/WinShun/issues/197#issuecomment-6701402799) |
| [#204](https://github.com/LingCore/WinShun/issues/204) | 100 条搜索词历史、快捷键翻阅和历史列表已发布 [验证](issues-119-204-217-20261106.md) | [评论](https://github.com/LingCore/WinShun/issues/204#issuecomment-6701403728) |
| [#205](https://github.com/LingCore/WinShun/issues/205) | 菜单密度与文件夹打字搜索的紧凑布局均已发布 [验证](issues-205-215-231-20261106.md) | [评论](https://github.com/LingCore/WinShun/issues/205#issuecomment-6701404639) |
| [#206](https://github.com/LingCore/WinShun/issues/206) | 独立完整面板快捷键及可记忆侧栏开关已发布 [验证](issues-197-200-206-20261106.md) | [评论](https://github.com/LingCore/WinShun/issues/206#issuecomment-6701405614) |
| [#208](https://github.com/LingCore/WinShun/issues/208) | 内置功能区域默认折叠并保存偏好已发布 [验证](issues-136-208-213-20261106.md) | [评论](https://github.com/LingCore/WinShun/issues/208#issuecomment-6701415267) |
| [#213](https://github.com/LingCore/WinShun/issues/213) | 缩略图基准、编号留白与条目分隔已发布 [验证](issues-136-208-213-20261106.md) | [评论](https://github.com/LingCore/WinShun/issues/213#issuecomment-6701416301) |
| [#215](https://github.com/LingCore/WinShun/issues/215) | 双击修饰键可绑定剪贴板及支持宿主通知的插件动作 [验证](issues-205-215-231-20261106.md) | [评论](https://github.com/LingCore/WinShun/issues/215#issuecomment-6701417324) |
| [#217](https://github.com/LingCore/WinShun/issues/217) | 应用内插件说明阅读器已发布 [验证](issues-119-204-217-20261106.md) | [评论](https://github.com/LingCore/WinShun/issues/217#issuecomment-6701418248) |
| [#219](https://github.com/LingCore/WinShun/issues/219) | 社区 Shell 插件已发布，覆盖原需求及外部插件替代方案 | [评论](https://github.com/LingCore/WinShun/issues/219#issuecomment-6701419280) |
| [#222](https://github.com/LingCore/WinShun/issues/222) | 社区原生包收录规则已上线，0.22.0 已提供安装/更新确认 [验证](issues-185-222-239-20261106.md) | [评论](https://github.com/LingCore/WinShun/issues/222#issuecomment-6701420349) |
| [#229](https://github.com/LingCore/WinShun/issues/229) | 滚动条交互与窗口缩放边缘冲突已修复发布 | [评论](https://github.com/LingCore/WinShun/issues/229#issuecomment-6701421335) |
| [#232](https://github.com/LingCore/WinShun/issues/232) | RPC 日志内外滚动及跟随末尾逻辑已修复发布 | [评论](https://github.com/LingCore/WinShun/issues/232#issuecomment-6701424160) |
| [#234](https://github.com/LingCore/WinShun/issues/234) | 紧凑的文件夹打字搜索及右下定位已发布 [验证](issues-227-233-234-20261106.md) | [评论](https://github.com/LingCore/WinShun/issues/234#issuecomment-6701425083) |
| [#236](https://github.com/LingCore/WinShun/issues/236) | 短子菜单的长路径提示裁切已修复发布 [验证](issues-161-236-237-20261106.md) | [评论](https://github.com/LingCore/WinShun/issues/236#issuecomment-6701426081) |
| [#237](https://github.com/LingCore/WinShun/issues/237) | 界面字体选择及独立剪贴板字体同步已发布 [验证](issues-161-236-237-20261106.md) | [评论](https://github.com/LingCore/WinShun/issues/237#issuecomment-6701427041) |
| [#239](https://github.com/LingCore/WinShun/issues/239) | 版本化共享 Qt 契约、运行环境与文档已发布 [验证](issues-185-222-239-20261106.md) | [评论](https://github.com/LingCore/WinShun/issues/239#issuecomment-6701427923) |
| [#241](https://github.com/LingCore/WinShun/issues/241) | Windows 用户目录重定向修复已随 0.22.0 发布 | [评论](https://github.com/LingCore/WinShun/issues/241#issuecomment-6701428860) |

## 首轮保留与后续归并

| Issue | 保留原因 |
| --- | --- |
| [#40](https://github.com/LingCore/WinShun/issues/40) | Everything 兼容 IPC/HTTP 服务尚未实现；Opus 直接打字搜索不是同一能力。 |
| [#61](https://github.com/LingCore/WinShun/issues/61) | 文件级右键菜单把所选文件交给自定义命令尚未交付，曾因此重新打开。 |
| [#70](https://github.com/LingCore/WinShun/issues/70) | 重启后暂时恢复，字体显示偶发错误根因尚未确认，缺复现/诊断。 |
| [#74](https://github.com/LingCore/WinShun/issues/74) | 0.14.0 有修补，但多屏混合缩放休眠唤醒的原始环境尚未确认恢复。 |
| [#82](https://github.com/LingCore/WinShun/issues/82) | 提问者在 0.21.0 发布后仍明确反馈 Opus 搜索框位置不对。 |
| [#86](https://github.com/LingCore/WinShun/issues/86) | 剪贴板按键转发偶发失效根因未定；恢复入口和诊断不代表修复完成。 |
| [#111](https://github.com/LingCore/WinShun/issues/111) | 多种延迟已改进，但未确认原始偶发迟滞及物理双屏跨屏卡顿均消失。 |
| [#124](https://github.com/LingCore/WinShun/issues/124) | 截图编辑黑块未复现/定位，另有贴图进入编辑快捷键请求。 |
| [#128](https://github.com/LingCore/WinShun/issues/128) | 资源管理器图片右键直接贴图入口尚未实现。 |
| [#129](https://github.com/LingCore/WinShun/issues/129) | 基于 OCR 文字区域的手动快速打码尚未实现。 |
| [#143](https://github.com/LingCore/WinShun/issues/143) | CAD 普通跳转已获确认，但管理员 CAD 联动及 ArcGIS 等自定义规则仍未覆盖。 |
| [#150](https://github.com/LingCore/WinShun/issues/150) | 用户仍反馈收藏圆盘无法弹出，新增诊断不能等同根因修复。 |
| [#157](https://github.com/LingCore/WinShun/issues/157) | 仅有标题，缺使用场景、开关、版本和复现信息。 |
| [#160](https://github.com/LingCore/WinShun/issues/160) | 浏览器历史搜索尚未加入书签插件。 |
| [#170](https://github.com/LingCore/WinShun/issues/170) | 高度自定义快速导航的悬停展开、模块排序与层级配置仍未完整提供。 |
| [#171](https://github.com/LingCore/WinShun/issues/171) | 带 TTL 的剪贴板自动填入及网址/网页搜索路由尚未交付。 |
| [#175](https://github.com/LingCore/WinShun/issues/175) | 快捷键抢占/滚动编号/钉钉粘贴已完成，但单手字母提示/vi 模式仍未交付。 |
| [#176](https://github.com/LingCore/WinShun/issues/176) | 0.22.0 已提供 Tessoa 协议适配，仍缺真实 Tessoa 本体 Ctrl+G 兼容性验证。 |
| [#180](https://github.com/LingCore/WinShun/issues/180) | LocalSend 自动接收与资源管理器右键发送仍未实现。 |
| [#181](https://github.com/LingCore/WinShun/issues/181) | 只过滤乱码历史，系统剪贴板正文被改坏的 UU 冲突仍未解决。 |
| [#185](https://github.com/LingCore/WinShun/issues/185) | 纯净模式已发布，但完整快捷键编辑页和后续空状态微组件仍未完成。 |
| [#186](https://github.com/LingCore/WinShun/issues/186) | 边缘摆放/范围切换已修复，但最新回复继续要求提速，物理多屏尚待确认。 |
| [#191](https://github.com/LingCore/WinShun/issues/191) | 社区已有截图翻译 0.3.3（插件库已收录），但补充的截图改字未交付，尚未确认整条收尾范围。 |
| [#200](https://github.com/LingCore/WinShun/issues/200) | 首轮保留管理员打开命令提示符/PowerShell 的补充要求。后续经维护者确认归并到 [#170](https://github.com/LingCore/WinShun/issues/170)，该要求已列入主帖待办，原帖以 `DUPLICATE` 关闭。 |
| [#210](https://github.com/LingCore/WinShun/issues/210) | Defender 检出问题缺准确样本哈希、检出名称与核验，不能认定已排除。 |
| [#211](https://github.com/LingCore/WinShun/issues/211) | 桌面待办便签和提醒插件仍未交付。 |
| [#220](https://github.com/LingCore/WinShun/issues/220) | 磁盘占用分析及相关扩展尚未实现。 |
| [#224](https://github.com/LingCore/WinShun/issues/224) | 首轮保留预览大小可调、AI/PSD 支持等补充要求。后续经维护者确认归并到 [#186](https://github.com/LingCore/WinShun/issues/186)，相关要求和交互一致性建议已列入主帖待办，原帖以 `DUPLICATE` 关闭。 |
| [#227](https://github.com/LingCore/WinShun/issues/227) | 分配策略与统计口径已改进，但原机 1.81 亿条索引的峰值/冷搜索效果未复验。 |
| [#230](https://github.com/LingCore/WinShun/issues/230) | 0.22.0 增加失败原因与重试，尚未确认 envhosts 原始管理员授权失败已解决。 |
| [#231](https://github.com/LingCore/WinShun/issues/231) | 顶层窗口切换已交付，浏览器后台标签及文件管理器隐藏标签仍不支持。 |
| [#233](https://github.com/LingCore/WinShun/issues/233) | 0.22.0 增加发键失败处理，但原目标软件未知，Win10 原始粘贴失败根因未确认。 |
| [#235](https://github.com/LingCore/WinShun/issues/235) | 液态玻璃仍是提案，已发布风格只有 fluent/brand/tactile。 |
| [#242](https://github.com/LingCore/WinShun/issues/242) | Python 安装管理器误识别问题已本地修复，真实插件启动验证通过，继续跟踪发布与用户侧复验。 |
| [#243](https://github.com/LingCore/WinShun/issues/243) | 已检查快捷键、全屏暂停与截图入口，并回复追问具体软件、版本和入口差异，等待复现资料。 |

## 已发现的后续入口

- #191 的主要截图翻译诉求已有社区方案：[截图翻译插件 0.3.3](https://github.com/iuuuuuuuu/winshun-screenshot-translate/releases/tag/v0.3.3)，核查时插件库条目也已收录。该方案不等同于评论中追加的截图改字，暂不把整条视为已完成。
- #176 的 Tessoa 协议适配与 #239 的共享 Qt 平台能力都已发布；前者还缺 Tessoa 本体兼容性确认，后者已有真实 Host → Runner → 共享 Qt 子进程路径回归，因此收尾结论不同。
- #185、#231 仍有新增/剩余范围。#200 和 #224 的后续需求已分别归入 #170 和 #186。#82 有新版后的负面反馈。#227、#233、#230 继续跟踪原始场景的复验结果。
- 插件库规则已经上线，但其 README / CONTRIBUTING 仍残留“待配套 0.22.0 发布”的旧文案；本轮核查记录该差异，没有改写插件库文档。

本轮未修改业务代码，没有 commit 或 push；只新增本核查记录。临时抓取、评论草稿和操作脚本在核验结果归档后清理。

## 后续需求归并（2026-11-07）

维护者确认归并方向后，先在主帖发布已交付能力及后续待办清单，再回复并关闭原帖。原帖均已复查为 `CLOSED / DUPLICATE`，两个主帖均为 `OPEN`。归并后的未关闭数量为 **33**。

| 原帖与主帖 | 转入的后续要求 | 主帖汇总与原帖回复 |
| --- | --- | --- |
| [#200](https://github.com/LingCore/WinShun/issues/200) → [#170](https://github.com/LingCore/WinShun/issues/170) | 管理员身份在当前目录打开 CMD/PowerShell，原反馈中的组合键作为交互候选。主帖继续跟踪悬停展开、一级菜单配置、模块显隐及排序。 | [主帖清单](https://github.com/LingCore/WinShun/issues/170#issuecomment-6701511943)、[归并回复](https://github.com/LingCore/WinShun/issues/200#issuecomment-6701523330) |
| [#224](https://github.com/LingCore/WinShun/issues/224) → [#186](https://github.com/LingCore/WinShun/issues/186) | 预览大小可调、AI/PSD 支持及清晰度、文件与剪贴板预览交互一致性。主帖继续跟踪加载速度和物理多屏摆放复验。 | [主帖清单](https://github.com/LingCore/WinShun/issues/186#issuecomment-6701512706)、[归并回复](https://github.com/LingCore/WinShun/issues/224#issuecomment-6701524695) |

## #242 与 #243 的后续诊断及验证

### #242 的原因与修复

- 提交者的两个插件均启动了 `C:\Program Files\PyManager\python.exe`。日志中的实际解释器启动错误为 `0x80070718`，进程退出码为 `2147944216`。Windows 将该错误映射为 `ERROR_NOT_ENOUGH_QUOTA`。
- 在真实 Windows Job 中复验，`ActiveProcessLimit=1` 时启动子解释器触发同一错误，允许子进程时成功。两个插件的清单均采用 512 MiB 内存上限且没有 `processes` 权限。
- `Programs.cpp` 通过 PE 版本信息识别 Python Install Manager，读取其全局别名的 UTF-8 `.__target__` 路径，继续解析到实际解释器。PATH 首个候选为管理器或失效启动器时，会继续检查后续候选。缺少可用解释器时展示安装或刷新注册的提示。
- 原有的注册表优先级、venv 的 `__PYVENV_LAUNCHER__`、uv 启动器解析和五秒后的缺失重查仍由既有代码处理。

### 验证结果

- 新回归 `pythonInstallManager` 在修改前失败，旧实现接受了管理器。修改后，相关九个 Python 测试方法及初始化、清理均通过，共 11 项通过，零失败、零跳过。覆盖管理器识别、PATH 回退、中文解释器路径、别名缺失、相对路径、超长路径、循环目标和脚本启动器，以及已有 venv、uv、注册表与 Runner 回归。
- 使用 Python 官方 26.4 MSIX 中的真实 `python.exe` 和 `templates/launcher-64.exe` 复验。MSIX 的 SHA256 为 `002CFE4F4B63E322E9C8170CEA4D7D97E3E2D18246887325F969BB75F482F4DC`，与官方发布页一致。管理器被排除，别名解析到真实解释器。[Python 官方发布页](https://www.python.org/downloads/release/pymanager-264/)
- 从插件库下载 `day.ocr 1.0.4`、`day.rename 1.0.2` 的已发布包，校验值与线上条目一致。使用任务目录内的 Python 3.14.10、Tcl/Tk 9.0.4 及插件要求的 Pillow、CustomTkinter 等依赖。两个插件独立 UI 自检均通过。
- 使用当前 `Host`、`Programs` 和真实 `WinShunPluginHost.exe`，在隔离的数据目录中依次查询 `ocr` 与 `pl`。两个插件均返回 `open` 结果，进程进入 Running，崩溃计数为零，并通过对应 PID 的 Windows 窗口可见性检查。原始清单的单进程约束在验证中生效。
- 使用本机 Python 3.10 再走同一 Host → Runner 路径，两个插件均返回 `dependencies` 提示项，进程保持运行。该场景覆盖版本或依赖不足时的降级提示。
- 原始提交者机器的 Python 3.15.2 环境仍待包含宿主修复的版本发布后复验。本次验证覆盖启动与窗口显示，OCR 模型调用与实际文件改名属于后续功能操作。
- 相关构建和探针均在 `build/issues-242-243-20261107/` 中执行。清理可再生成产物，保留提交者的 `plugins.log`、`WinShun.log`、`events.zip` 原始诊断材料。原始日志含个人路径，本文仅记录脱敏结论。

### #243 的排查进度

- 截图插件当前采用普通用户权限。宿主注册并转发系统快捷键，转发前已有 `AllowSetForegroundWindow` 授权。代码还存在可选的全屏暂停判断，默认关闭。现有报告无法区分快捷键未送达、全屏暂停、截图遮罩交互等失效环节。
- 已请提交者补充目标软件与版本、Windows 和宿主/插件版本、快捷键及全屏暂停设置，并对比托盘“截图 → 开始截图”的结果，同时提供带复现时间的诊断包。[排查回复](https://github.com/LingCore/WinShun/issues/243#issuecomment-6701639268)

已将原因、本地修复范围、验证结果和现有版本的刷新注册办法回复到 #242。[修复进展回复](https://github.com/LingCore/WinShun/issues/242#issuecomment-6701780896)

当前两条 Issue 均继续跟踪。本次源码修复尚未发布。

## 自包含分发后续

按后续确认，已准备 OCR Studio 和批量重命名 1.1.0 自包含候选包，固定 Python 3.15.2 及界面依赖，并接入 Windows 自动构建和未审核原生发布流程。两包均通过 Python 不可用场景下的真实首次启动、旧脚本版升级、关键词和模型设置传递、数据保留及更新确认验证。共 49 个插件测试通过，包含可见 DPI 检查。插件库实现已保存于提交 `31b3e1a`，候选包尚未发布，详细产物校验值和验证范围见[自包含验证记录](issue242-desktop-20261107/README.md)。

提交者随后确认，Win顺 0.22 执行 `py install --refresh` 后两个插件已不再崩溃，目前显示缺少依赖。[提交者复测](https://github.com/LingCore/WinShun/issues/242#issuecomment-6702188846)。已回复说明环境检查与原崩溃的区别、自包含候选包进展、验证范围及尚未发布的状态。[后续回复](https://github.com/LingCore/WinShun/issues/242#issuecomment-6702982251)。#242 仍保持开放；#243 尚无补充信息，未重复追问。
