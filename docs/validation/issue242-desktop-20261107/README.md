# #242 自包含插件候选包验证

验证日期为 2026-11-07。本次将 OCR Studio 和批量重命名的 Windows 分发改为自包含目录包，并准备对应的自动发布流程。

## 产物

候选包位于项目的 `dist/issue242-desktop-20261107/`，最低要求 Win顺 0.22.0。每个包附有 SHA256 文件和构建收据。

| 插件 | 版本 | 包大小 | SHA256 |
| --- | --- | --- | --- |
| OCR Studio | 1.1.0 | 21,048,533 字节，约 20.1 MiB | `5dc100bf3f58a8af68b8c43ddfd5464190542a09c342992756e35219c2d60ec8` |
| 批量重命名 | 1.1.0 | 22,199,035 字节，约 21.2 MiB | `14e9bd3ed7cf352fa9c69a8bd8b6c04c4be0b09192c3ae2462ea601fa0b0dd5d` |

本地候选包的插件源码导出自 `LingCore/WinShun-plugins` 提交 `9b13660df5b4a15a6149841ffba772b5e4b47fd3`，构建器将候选分发版本设为 1.1.0。收据记录逐文件源码哈希、工具链版本和最终包校验值。正式发布时，工作流会从包含这些构建与文档变更的干净提交重新生成包及收据。

## 实现与分发

- 使用固定的 CPython 3.15.2、Tcl/Tk 9.0.4、PyInstaller 6.22.3、Pillow 12.3.0 和 CustomTkinter 6.0.0。构建依赖及传递依赖均固定版本并附 Windows wheel 的 SHA256，安装流程启用 `--require-hashes`。
- PyInstaller 使用 onedir 和 console 子系统。Win顺 的 Runner 以 `CREATE_NO_WINDOW` 启动，保留 JSON-RPC 标准输入输出。最终包从中文目录解压后执行自检，自检进程使用空白配置及仅含系统目录的 PATH。
- 插件 ID、命令 ID、设置键和权限沿用脚本版。两份清单保留单进程作业约束及 512 MiB 内存上限，包内解释器由插件分发统一管理。
- 新增 `desktop.json` 分发标记及 Windows 构建任务，普通 legacy 打包器跳过这两个 GUI 插件。原生包按未审核类别发布，线上条目带 `review: "unreviewed-native"`，安装和每次更新均需确认。
- 发布流程检查包、校验附件及收据的一致性，拒绝内容不一致的收据，跳过已经发布的同版或更高版本。原生 Release 标签取收据中的源码提交，防止构建期间 main 前进导致标签与产物来源错位。

## 实际验证

### 最终包及真实 Host → Runner

两个最终 `.wsplugin` 均通过插件库未审核原生包检查和解包后 EXE 自检，结果均为 `ok=true`、`frozen=true`、Python 3.15.2、Tcl/Tk 9.0.4。

本机 Windows 11 x64 上，以真实 `Host`、`install::inspect`、`updates::replace` 和 `WinShunPluginHost.exe` 执行以下场景。每轮使用任务独有的中文配置与数据目录。

1. 首次安装。宿主 PATH 仅包含 Windows 系统目录，并将 Python 解析显式设为不可用。输入 `ocr` 和 `pl` 后，两包均返回 `open` 查询结果，显示可见的主窗口，状态为 Running，崩溃计数为零。
2. 脚本版升级。先安装并启动插件库当前发布的 OCR Studio 1.0.4 和批量重命名 1.0.2，设置独立的模型值、自定义关键词及数据文件。通过设置页实际调用的 `updates::replace` 停止旧进程并替换安装目录，再关闭 Python 解析。两个新包均能使用原自定义关键词启动，初始化请求携带保留的模型设置，INI 文件及数据文件内容保持一致。
3. 更新确认。即使权限集合相同，未审核原生分类在自动更新决策中仍返回 Ask。该行为已针对这两份实际清单验证。

对应机器可读结果见 [host-validation.json](host-validation.json)。逐次启动耗时随机器可读结果记录，属于本机本轮观测。

最初的升级探针在 `Host::stop` 后直接调用 `install::place`，观察到旧进程尚未退出时的目录锁。最终探针改用生产的 `updates::replace` 入口，复用了正式停止与等待流程，升级验证通过。此记录用于区分底层探针与完整安装入口的证据。

### 相关回归

| 验证范围 | 结果 | 证据 |
| --- | --- | --- |
| OCR 插件 | 23 项常规测试及 1 项可见 DPI 测试通过 | [常规日志](ocr-tests.log)、[DPI 日志](ocr-layout-tests.log) |
| 批量重命名插件 | 24 项常规测试及 1 项可见 DPI 测试通过 | [常规日志](rename-tests.log)、[DPI 日志](rename-layout-tests.log) |
| 可见界面缩放 | 100%、150%、200% 下关键控件可见且位于窗口内 | 两份 DPI 日志 |
| 宿主相关回归 | Python 管理器识别、安装、目录及数据保留、原生审核分类、更新决策均通过，共 7 个方法及初始化、清理 | [宿主日志](host-tests.txt) |
| 插件库原生包规则 | 8 项测试通过 | [规则日志](community-package-tests.log) |
| 插件库完整离线检查 | 16 个条目、3 个 legacy 源码，零错误 | [检查日志](registry-check.log) |
| 固定依赖哈希 | 10 个 Windows wheel 通过离线 `--require-hashes` 检查 | [依赖检查日志](locked-wheels.log) |
| 发布配置 | actionlint 1.7.12 校验通过 | 本地构建过程 |

常规插件日志各含一项默认跳过的可见 DPI 测试，该项随后单独显式开启并通过，因此合计为 49 个插件测试通过。插件库离线检查保留了原有 `ocr` 关键词与图片文字搜索插件重名的两条警告。

### 发布步骤演练

直接提取工作流中的 Python 校验步骤，在任务独有的发布目录执行，确认正常候选包通过、同版或较新版已发布时跳过、篡改源码提交的构建收据被拒绝、篡改 SHA256 附件被拒绝。见 [publisher-validation.json](publisher-validation.json)。

同时确认普通 legacy 打包仅生成剩余的脚本插件，候选目录已存在时构建器拒绝覆盖，现有包与收据的哈希保持一致。

### 探针源码

[Host 探针](probe/main.cpp)、[构建文件](probe/CMakeLists.txt)和[发布步骤探针](publisher_probe.py)保留本轮实际验证入口。Host 探针按任务目录布局编译，`host/` 放主仓库 `tst_plugin` 目标的构建结果，`python/` 放固定的 Python 3.15.2 构建环境，两个旧版脚本包分别命名为 `day.ocr-old.wsplugin`、`day.rename-old.wsplugin`。执行时传入该任务目录和候选包目录。

发布步骤探针接收任务目录、插件库源码目录及候选包目录三个参数。运行前使用插件库 `tools/build.py --legacy --out <任务目录>/publish-simulation/dist` 生成脚本包发布清单，然后执行探针。所有测试条目和被篡改的测试附件均位于任务独有目录。

## 当前范围

源码与工作流改动、本地候选包和以上验证已完成，尚未发布。插件库实现已保存于提交 `31b3e1a`，GitHub Actions 远端运行需推送后执行，本次完成的是本地构建、静态校验和发布步骤演练。

本轮临时构建目录 `build/issue242-desktop-20261107/` 已回收；最终候选包保留于上述 `dist` 目录，探针源码及验证记录保留于本目录。此前收到的原始诊断文件仍保留在 `build/issues-242-243-20261107/`。

本轮覆盖启动、窗口、模型设置传递、数据与升级行为，插件的 OCR 服务和 AI 命名测试采用既有模拟响应。提交者原机的发布后复验仍继续跟踪。#243 目前仍待提交者补充具体软件及复现资料。
