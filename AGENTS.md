# Win顺 项目约定

## 文档路由（按需阅读）

先按任务领域读对应入口，避免通读无关文档：

| 需求 | 入口文档 |
|---|---|
| 系统拓扑、模块边界与专题架构 | [docs/architecture.md](docs/architecture.md)（路由至 `docs/architecture/` 下 6 份专题） |
| 高频红线与按领域拆分的踩坑指南 | [docs/pitfalls.md](docs/pitfalls.md)（路由至 `docs/pitfalls/` 下 6 份专题） |
| 色彩规范与 4 套主题开发 | [docs/colors.md](docs/colors.md)、[src/app/qml/themes/README.md](src/app/qml/themes/README.md) |
| 插件协议与官方 SDK | [docs/plugins.md](docs/plugins.md)、[src/plugins-sdk/DEVELOPMENT.md](src/plugins-sdk/DEVELOPMENT.md) |
| 命令行 `ws`、手势、对话框兼容 | [docs/design-cli.md](docs/design-cli.md)、[docs/design-gestures.md](docs/design-gestures.md)、[docs/dialog-compatibility.md](docs/dialog-compatibility.md) |
| 完整发版流程 | [docs/releasing.md](docs/releasing.md) |

## 工作方式与安全边界

- **端到端推进**：对已授权的实现、修复与整理任务，直接推进到构建与对应验证完成，不在常规可逆决策上停顿反问。
- **高风险先确认**：退出或替换用户运行中的 Win顺、改动用户 `data`、执行破坏性清理，或对外推送与发版前先确认。
- **不干扰运行中的实例**：UI 验证优先用 `--preview-* --renderer d3d11` 离屏出图；临时实跑使用隔离 `--profile <dir>`（并在 `[Launcher]` 设 `ExplorerMenu=false`、`UriProtocol=false`）及独立 `-OutputDir`。
- **按需测试**：只运行本次改动涉及的 `ctest -R` 或 `scripts/build.ps1 -Tests` 套件（发布也一样），不跑全量测试。

## 工作区整洁与文档卫生

- **产物目录**：日常构建放 `build/<配置>`；任务级构建、探针、日志和临时配置放 `build/<任务名>/`；安装包与测试部署放 `dist/<任务名>/`；独立源码用 `.worktrees/<任务名>/`（仅在成果已保存且工作树干净时用 `git worktree remove` 回收）。
- **临时目录自清理**：由创建者用 `try/finally` 覆盖成功与失败路径回收；确需写入项目外时记录完整路径并在收尾清理。
- **清理前核对**：核对绝对路径、目录链接目标、运行中程序与用户数据；只清本项目的可再生成产物，保留源码、未提交修改、诊断原件、截图原始素材与正式文档。收尾检查 `git status --short`。
- **验证证据精选**：需保留的验证放 `docs/validation/<任务名>/`（不平铺在 `docs/` 根目录），仅放精选 README、关键截图（单图 ≤ 300 KB，必要时裁剪或转 JPEG）、日志关键片段与小型结果文件，单目录 ≤ 5 MB。原始截图包、录屏 GIF、全尺寸原图和整份日志不进仓，放素材项目或 issue 附件并在 README 注明；已入库历史不重写。
- **现行状态文档**：`docs/architecture/` 与 `docs/pitfalls/` 只维护当前有效的设计与避雷要点；改实现时原地更新，不往文档顶部或末尾追加按日期记录的排障日记或原始压测表。

## 编码（不出乱码）

- 文本文件一律 UTF-8；含中文的 PowerShell 脚本（`.ps1`/`.psm1`/`.psd1`）带 UTF-8 BOM；`.bat`/`.cmd` 仅用 ASCII。
- C++ 字符串走 `QString`（`u"..."_s`）与 `W` 后缀 Win32 API，不用 `A` 后缀调用。确属系统代码页的 `fromLocal8Bit`、`CP_ACP`、`CP_OEMCP` 等例外须登记到 `tests/tst_encoding.cpp`，改动涉及时跑 `ctest -R ^tst_encoding$`。详见 [乱码与编码](docs/pitfalls/installer-build-and-testing.md#乱码与编码)。
- 始终在 PowerShell 中编译，进 MSVC 环境用 `scripts/devshell.ps1`；命令输出乱码时先按正确编码重读再判断。

## 界面与图标

- 界面保持紧凑、现代化、简洁，与 Win顺 现有 4 套主题（浅/深）风格一致；颜色统一取自 `Theme.qml`，按 100%–175% 缩放检查实际像素。
- 自有功能图标统一优先复用 Phosphor（PH）图标库（主程序、官方插件与各主题通用）；“复制完整路径”保留现有自定义设计。
- PH 无合适图标时，按 PH 的网格、线宽、端点、转角、留白与视觉重心在统一图标入口补绘，保持 Regular/Fill 一致，不在不同页面或主题中各画一套。品牌 Logo、系统文件/应用图标和用户图片保留原来源；历史自绘或标题栏字形不构成新增例外，涉及时按本规则收敛（源码中 Segoe 编号可保留兼容别名，以映射后渲染的 PH 图标为准）。

## 发布新版本

用户说“发布新版本”“发新版”等即执行完整发版流程，**文档与新版截图更新均为必做项**（明确的范围限制优先）。完整步骤见 [docs/releasing.md](docs/releasing.md)，核心门禁如下：

- **提交与代码核对**：先查未提交修改、近期提交与最新 Release；按功能拆分有意义的 commit，只提交已授权改动，保护无关编辑与用户数据。
- **文档与截图**：按实际实现与验证结果同步中英文 README、[docs/使用说明.md](docs/使用说明.md)、[docs/features.en.md](docs/features.en.md)、发布说明及涉及的插件协议/SDK 文档。用 [tools/screenshots/README.md](tools/screenshots/README.md) 实拍受影响界面，保留品牌封面规范（Logo、LingCore 署名、标题、副标题、背景及底部文字；1440×1440 JPEG 质量 96 关闭色度抽样；README 只放含“关注我”的 `cover.jpg`，其余放 [docs/截图.md](docs/截图.md) 分组 `width="49%"` 两列排列）。
- **构建、实测与安装验证**：执行相关构建、按需测试及新功能实测，生成安装版、便携版与 SHA256。改过 `installer/` 或应用内更新启动代码，提交前及发版前必须跑 `tools/verify-installer.ps1`（`scripts/release.ps1` 自动执行，失败不出包）；发版时征得用户同意后在本机跑 `tools/verify-installed-upgrade.ps1 -Mode InApp`，未跑项在验证记录如实说明。验证失败或存在阻塞时不发布。
- **推送方式**：默认正常推送（分批）；用户明确要求时用 `scripts/release.ps1 -Push Now`（立即重要更新提示），汇报中写明本次方式。
- **同步官方插件库**：逐个比对官方可选插件源码（含编译进去的 `src/app`、`src/capture`、`src/core` 共享文件）与插件库已发布版本；有变化就升版、从干净提交构建并发布到 `LingCore/WinShun-plugins`，更新 `plugins/<id>.json` 并核对线上 index（新增权限须注明需用户确认）。
- **主仓发版核验**：发布到 `LingCore/WinShun`，核对附件、SHA256、版本与标签提交一致；用户单次要求只发布一次。
