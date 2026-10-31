# 首次引导验证（2026-10-31）

## 当前版本：功能区域层次

在上一轮简洁排版上恢复功能卡片，但继续保留短说明和两页结构：窗口底色、卡片表面、固定操作栏分别使用现有 Theme 的 page、card、background；卡片使用统一图标底座、轻微底部投影和随开关变化的边框。标题、说明与输入共用对齐线，顶部增加两步进度条，不增加新的操作。默认尺寸仍为 720×560。

当前构建 SHA256：`87716549D64B81850BE94D6DB044525E9050153806D90F6FC8E8943BA41BA964`。4 项引导 CTest 通过（27.80 秒，`layers-tests.log`）；随后仅增加页面布局树诊断输出，再跑空配置翻页测试通过。中文 125%、175% 各运行空配置真实 App/QML 探测通过；`capture`、`english`、`english-capture` 三个安装/界面场景全部通过且无非预期警告，涵盖英文深色 150%、展开选项、最小窗口滚动及权限确认。

产物均在 `build/onboarding-two-pages/`：`layers-build.log`、`layers-tests.log`、`layers-final/`、`layers-scale-1.25/`、`layers-scale-1.75/`、`layers-screenshots/`；部署程序仍为 `run/WinShun.exe`。下文尺寸与哈希是历史验证，以本节为准。

## 简化布局后的复验

按界面反馈再次调整：默认窗口为 720×560，移除卡片边框和数字编号；功能标题与开关共用一行，两组之间只保留一条分隔线。缩短说明，快捷键选择和试用共用 Flow，长译文自动换行；“稍后设置”改为次要文字按钮。仍保留索引成本、本机存储、剪贴板保留期限和截图下载授权说明。

本轮实际构建 SHA256：`01C29C317248396E804047737602AE531614BA1B6D2ABE2DE7A32953A067F860`，独立部署仍在 `build/onboarding-two-pages/run/WinShun.exe`。4 项 `tst_onboarding_*` 全部通过（26.95 秒），包括连续 8 轮翻页、草稿、提交与旧配置路径；另补充第二页两个主开关的可见性与布局宽度检查。日志为 `build/onboarding-two-pages/simple-tests.log`。

本轮重跑 `capture`、`english`、`english-capture` 三个场景，覆盖真实组件安装、英文深色 150%、660×480 滚动和权限确认。截图和报告位于 `build/onboarding-two-pages/simple-final/`，中文两页截图另存 `build/onboarding-two-pages/simple-screenshots/`。以下为前一轮两页功能验证记录，哈希及默认尺寸以本节为准。

## 实现范围

全新配置分为两页：第一页是搜索与索引、打开方式，第二页是剪贴板历史、截图。默认窗口由 800×790 改为 800×640，每页只呈现两项。下一步仅验证并翻页，上一步保留全部选择并停止按键捕获；最后确认、同步保存成功后才创建搜索运行时、索引与剪贴板监听。选择“稍后设置”使用文件名搜索和双击 Ctrl，关闭内容索引、剪贴板历史和截图。确认后直接打开搜索框，包括原本通过 `--background` 启动的安装场景。

`Setup/Completed` 在其他默认值写入前区分空配置和旧配置。未完成时退出，下次继续引导；旧配置升级跳过引导。保存失败保留页面、选择和错误提示。

截图复用现有插件库下载、校验与权限确认，不另建安装协议。快捷键写入该插件自己的配置；只有真实进程进入 Running 才显示已启用。下载失败、版本不兼容及拒绝授权时可重试或稍后安装，搜索继续可用。未完成的安装意图保留在 `Setup/CapturePending`，随后从插件页启用成功会清除。

## 环境与产物

- Windows x64，Qt 6.12.0 / MSVC 2022，D3D11；中文浅色 100%，英文深色 150%。
- 使用当前工作区构建，包含此前 #153 及工作区原有的并行改动；本记录只说明首次引导及其相关回归的覆盖范围。
- 构建程序：`build/release/WinShun.exe`；独立部署程序：`build/onboarding-two-pages/run/WinShun.exe`，包含应用配套字体。
- 两份程序 SHA256 一致：`BD5B54279872A3295D350632CDEFD1798174D5E31D85576824D3A4EEA22FDF39`。
- `library_plugins` 重新构建、整理真实截图组件，测试安装包来自该构建。

## 最终结果

本轮两页引导的 5 项 CTest 全部通过（26.26 秒）：

| 测试 | 覆盖 |
| --- | --- |
| `tst_settings` | 新配置未完成标记、加载默认值后重进、完成与待安装状态持久化、旧配置兼容、磁盘保存失败 |
| `tst_onboarding_fresh` | 真正的空配置进入实际 App/QML，引导前未创建搜索窗口、剪贴板数据库 |
| `tst_onboarding_later` | 稍后设置的安全默认值、完成标记、完成后显示搜索 |
| `tst_onboarding_apply` | 内容索引选项、打开方式、草稿与提交边界、保存失败留在引导 |
| `tst_onboarding_legacy` | 没有新完成标记的旧配置直接启动原运行时 |

CLI、搜索导航和 #153 搜索负载没有相关变化，本轮未重复运行，保留上一轮 8 项通过记录 `build/onboarding-20261031/regression-final.log`。

真实应用和截图组件的 7 个独立场景全部通过：

| 场景 | 结果 |
| --- | --- |
| `capture` | 本地包经过插件库下载/校验，实际权限弹窗确认，真实组件运行且截图快捷键注册成功 |
| `decline` | 拒绝权限后保持未启用，可重试或继续搜索 |
| `offline` | 连接失败显示原因，可稍后安装，保留待安装状态 |
| `retry` | 连接失败后通过真实“重试”入口恢复安装、确认权限并运行组件 |
| `incompatible` | 插件库要求更高主程序版本时给出解释，继续使用搜索 |
| `english` | 英文深色 150%，首次引导与提交 |
| `english-capture` | 英文深色 150%，真实安装、授权、运行和快捷键注册 |

所有场景均无非预期警告。`offline` 和 `retry` 各保留一条插件库连接失败的预期诊断；测试只精确识别本机 `127.0.0.1:1` 夹具对应日志，其余警告仍使测试失败。

共同检查还包括两页只显示各自的两项功能、翻页不保存或启动运行时、返回保留四项选择、无效打开快捷键阻止下一步、最终确认时重新发现打开快捷键失效会返回第一页；Ctrl / Alt / Shift 双击试用、自定义与截图组合键的窗口内识别、缺失或重复快捷键被拒绝、剪贴板及 Win+V 的明确选择持久化也继续通过。第二页在 660×480 最小窗口中可滚动到截图设置，权限弹窗可滚动且键盘焦点能到确认按钮。

开发过程中一次失败的转储调用栈位于测试辅助函数 `settle → QQuickWindow::grabWindow`，已增加 `QPointer` 窗口存活保护，意外关闭时终止测试并输出报告；翻页和确认改为两个独立按钮及固定动作，避免共用按钮随页码改变职责。最终空配置测试额外连续往返 8 轮，全部验证完成前没有保存或启动运行时；最终 5 项回归及 7 个安装/界面场景均退出 0，未再出现该崩溃。

## 视觉检查

已打开最终程序生成的原始 PNG 逐项检查：中文浅色的两页引导、截图授权及成功状态、最小窗口的权限确认；英文深色 150% 的两页引导、第二页的最小窗口与权限确认。补充第二页底部说明的真实像素检查，避免仅通过布局属性而遗漏截图中的文字；离屏切页后补充渲染一帧，未改动生产渲染流程。文字、开关、底部动作均可读，无按钮重叠或窗口外的确认操作。原生窗口截图关闭 Mica，避免 `grabWindow()` 无法捕获 DWM 背景形成透明黑底；未改动用户的系统主题。

最终图片位于：

- `build/release/tests/onboarding-fresh/onboarding.png`、`onboarding-page2.png`
- `build/onboarding-two-pages/verified-integration/capture/onboarding-{choices,permissions,permissions-compact,capture}.png`
- `build/onboarding-two-pages/verified-integration/english/onboarding.png`、`onboarding-page2.png`、`onboarding-compact.png`
- `build/onboarding-two-pages/verified-integration/english-capture/onboarding-permissions-compact.png`

此前单页截图和初版两页测试结果由本轮记录替代。

## 复现与证据

在项目 MSVC / Qt 构建环境中：

```powershell
cmake --build build/release --target WinShun library_plugins tst_settings -j 2
ctest --test-dir build/release -R '^(tst_settings|tst_onboarding_(fresh|later|apply|legacy))$' --output-on-failure -j 2

# 使用独立部署目录，包含 Qt DLL、WinShunPluginHost.exe 和 fonts；选择新的输出目录。
./tools/verify-onboarding.ps1 `
    -AppExe build/onboarding-two-pages/run/WinShun.exe `
    -OutputDir build/onboarding-check-new
```

最终日志：`build/onboarding-two-pages/verified-tests.log`、`verified-integration.log`；逐项断言见 `build/onboarding-two-pages/verified-integration/*/onboarding.json`，汇总见该目录的 `summary.json`。

## 验证边界

使用独立临时配置、测试文件和进程内 HKCU 重定向，不退出用户程序，不修改系统剪贴板，不重启 Explorer。剪贴板同意及 Win+V 选择通过实际控制器验证保存，未在用户桌面接管 Win+V；监听和接管仍复用原有实现。

截图安装使用真实本地构建包和测试目录，不代表 GitHub 公网下载速度或可用性。连接失败由本机拒绝连接模拟，版本不兼容由测试索引模拟；没有断开用户网络。截图组件的进程启动和快捷键注册已验证，本轮未重新测试底层框选、标注、贴图或录屏的全部行为。

本次没有提交、推送或发布版本。
