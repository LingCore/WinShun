# Win顺 架构总览

面向开发与维护者的系统总览与按需路由。用户使用说明见 [README.md](../README.md) 与 [使用说明.md](使用说明.md)；高频陷阱见 [pitfalls.md](pitfalls.md)；发布流程见 [releasing.md](releasing.md)。

## 1. 进程拓扑与权限边界

Win顺 采用多进程、最小权限设计：日常界面保持普通用户权限，高权限卷访问、不可信文档解析与可选插件均隔离到独立进程。

| 可执行文件 | 运行权限 / 依赖 | 职责与隔离边界 |
|---|---|---|
| `WinShun.exe` | 普通用户 / Qt 6 Quick | 主程序：文件与内容搜索 UI、剪贴板历史、文件对话框集成、设置、插件宿主与内置 MCP 服务。含专用入口 `--clipboard-display`（由服务以非管理员 UIAccess 拉起，仅在原生 Start/Search 上方做剪贴板独立显示）。 |
| `WinShunService.exe` | Windows 服务（SYSTEM）/ 无 Qt | 可选后台服务：以只读方式打开 NTFS 卷句柄供主程序读取 MFT/USN；按白名单与租约以同会话非管理员 UIAccess 令牌启动受限助手（管理员窗口双击监听、提权对话框导航、管理员代按粘贴、剪贴板 UIAccess 显示）。 |
| `WinShunPluginHost.exe` | 低完整性 / Job / 无 Qt 依赖 UI | 插件 runner：负责拉起与托管脚本/进程插件、限额管道 JSON 通信、超时清理与沙箱隔离。 |
| `WinShunExtract.exe` | 受限工作进程 | 文档正文提取：隔离 PDFium、OOXML、旧版 Office 等解析，崩溃或超时不影响主程序。 |
| `WinShunMcp.exe` | 普通用户 / 无 Qt | MCP stdio ↔ 主程序本地管道中继，供外部 AI 客户端接入。 |
| `bin/ws.exe` | 普通用户 / 无 Qt | 命令行客户端，通过本地命名管道调用主程序搜索、剪贴板、插件与设置。 |
| `WinShunSearch.exe` | 普通用户 / 无 Qt | 可固定到任务栏的轻量搜索入口存根，通知主程序显隐。 |
| `WinShunUpdate.exe` | 普通用户 / 无 Qt | 后台增量更新器：流式下载、校验与差分重建，避免拖慢主界面。 |
| `WinShun*.exe`（插件） | 普通用户（部分共享 Qt） | 14 个插件库可选 C++ 插件与内置工具插件，按清单声明权限独立运行，不注入主进程。 |

## 2. 仓库目录地图

| 路径 | 内容 |
|---|---|
| `src/core/` | 纯 Qt Core 核心库：内存文件索引（`FileIndex`）、MFT/USN 读取、内容索引、排序匹配、剪贴板存储（`ClipStore`）、镜像下载（`Download`）与诊断日志（`DiagnosticLog`）。 |
| `src/app/` | 主程序与 Windows 集成：`qml/`（界面与 4 套主题 `qml/themes/`）、`platform/`（Win32/DWM/UIA/对话框/托盘/热键）、`i18n/`（中英文翻译）。 |
| `src/service/` | `WinShunService.exe` 与 UIAccess 会话助手实现。 |
| `src/extract/` | `WinShunExtract.exe` 受限文档提取进程与解析器。 |
| `src/plugin/` | 插件宿主（`Host`）、清单权限校验、`WinShunPluginHost.exe` 与插件市场客户端。 |
| `src/plugins-sdk/` | 官方插件开发 SDK 与示例（见 [DEVELOPMENT.md](../src/plugins-sdk/DEVELOPMENT.md)）。 |
| `src/capture/` | 截图、标注、智能打码、长截图、录屏与演示共享库（供 `capture-plugin` 使用）。 |
| `src/*-plugin/` | 官方 C++ 插件源码（15 个目录，连同 `plugins/` 下脚本/内置清单共 18 个官方插件：4 个随包 + 14 个插件库）。 |
| `src/mcp/`、`src/cli/` | 内置 MCP 服务、`WinShunMcp.exe` 中继与 `bin/ws.exe` 命令行工具。 |
| `src/stub/`、`src/updater/` | `WinShunSearch.exe` 任务栏存根与 `WinShunUpdate.exe` 增量更新器。 |
| `integrations/`、`plugins/` | 外部程序集成扩展（如 Blender 文件对话框扩展）与内置脚本插件清单。 |
| `skills/` | Agent 技能包：`winshun`（嵌入 `ws.exe`）、`winshun-dev`（本体开发）、`winshun-plugin-dev`（插件开发）。 |
| `tests/`、`tools/` | Qt Test 套件（手工登记于 `tests/CMakeLists.txt`）、探针/压测、安装升级验真与截图生成工具（`tools/screenshots/`）。 |
| `scripts/`、`installer/`、`cmake/` | 构建/发布脚本（`build.ps1`、`release.ps1`、`devshell.ps1`）、Inno Setup 安装脚本与依赖下载规则。 |
| `docs/` | 架构与踩坑路由、专题规范、用户文档、发版说明（`release-notes/`）与验证证据（`validation/`）。 |

## 3. 跨子系统硬约束

- **权限最小化**：主进程始终保持普通用户权限；NTFS 原始读取限制在服务进程，不可信文件解析限制在 `WinShunExtract`，会话 UIAccess 助手以非管理员令牌和白名单环境执行固定白名单动作。
- **界面线程非阻塞**：主线程仅做轻量 UI 调度与内存快照查询；扫描、SQLite 落盘、OCR、缩略图、网络下载与诊断导出全部异步执行。所有 `std::jthread` 线程体需捕获异常防止未捕获异常触发 `abort()`。
- **单实例与隔离验证**：数据目录优先级 `--profile` > 便携版 `data/` > `%APPDATA%\WinShun`。自动化测试与 UI 检查优先用 `--preview-*` 离屏预览或隔离 `--profile`（并在 `[Launcher]` 关闭 `ExplorerMenu=false`、`UriProtocol=false`），不干扰正在运行的用户实例。
- **隐私与诊断边界**：诊断 JSONL 与导出包只记录结构化状态、指标、硬件/驱动配置和模块偏移调用栈，绝不包含文档正文、搜索词、剪贴板内容、用户设置或敏感路径。
- **按现行状态维护文档**：新增架构变更应直接更新对应专题文档的现行描述，不在文档顶部或末尾追加日期流水账、一次性压测表或未清理的草案。

## 4. 专题架构路由（按需阅读）

| 开发领域 | 专题文档 | 涵盖范围 |
|---|---|---|
| 索引与搜索 | [indexing-and-search.md](architecture/indexing-and-search.md) | 内存索引、压缩快照、NTFS MFT/USN 与权限分离、SMB/WebDAV/WSL、搜索语法与排序、内容索引与详情窗格 |
| 界面与系统集成 | [ui-and-windows.md](architecture/ui-and-windows.md) | 渲染器/字体/4 套主题/中英文、主搜索框与面板/大图/预览、首次引导、收藏/命令/网页搜索、文件对话框集成、资源管理器/拖放圆盘/任务栏搜索 |
| 剪贴板与全局输入 | [clipboard-and-input.md](architecture/clipboard-and-input.md) | Raw Input 双击检测、服务 UIAccess 监听与 `TapTeller`、`GameGuard`、`win::bringToFront`、剪贴板 SQLite/图片/公式/OCR、`KeyRouter`、`ClipboardDisplay`、管理员代按粘贴与 `HoldToStep` |
| 插件、MCP 与 CLI | [plugins-and-ai.md](architecture/plugins-and-ai.md) | 宿主/插件边界、沙箱与共享 Qt 运行时、18 个官方插件总表、`AiBridge` 与 MCP 服务、`ws.exe` 命令行与 Skills |
| 截图、录屏与演示 | [capture-and-recording.md](architecture/capture-and-recording.md) | `winshun.capture` 进程结构、多屏/HDR 捕获与遮罩、标注/智能打码/贴图/编辑器、长截图、Media Foundation/WASAPI 录屏、演示与激光笔 |
| 诊断、配置、更新与验证 | [diagnostics-and-updates.md](architecture/diagnostics-and-updates.md) | 配置/便携目录与 `WinShun.ini`、JSONL 诊断与卡顿/崩溃采样、增量更新与推送链路、离屏预览/探针/压测入口与参考基线 |

### 其他专项规范

- 视觉与主题：[colors.md](colors.md)、[主题开发说明](../src/app/qml/themes/README.md)
- 插件协议与 SDK：[plugins.md](plugins.md)（[English](plugins.en.md)）、[插件 SDK 开发指南](../src/plugins-sdk/DEVELOPMENT.md)
- 命令行与外部协议：[design-cli.md](design-cli.md)、[design-gestures.md](design-gestures.md)、[dialog-compatibility.md](dialog-compatibility.md)、[shunsync-protocol.md](shunsync-protocol.md)
- 早期历史设计草案与过往版本验证记录已由 Git 历史保存，不再保留于当前工作树。
