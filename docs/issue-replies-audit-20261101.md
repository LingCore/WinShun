# Issue 新回复复查（2026-11-01）

本次全量读取主仓库及插件仓库的 issue 和评论，包含已关闭 issue 的后续回复。主仓库 156 条实际 issue（51 条打开）、416 条评论；插件仓库 4 条 issue（3 条打开）、4 条评论。统计不包含 PR，表示本次读取时的快照。没有发布评论、改变 issue 状态、推送或发布版本。

## 有实质后续的回复

| Issue / 回复 | 本次结论和后续 |
| --- | --- |
| [#111 双屏跳位与设置跨屏停顿](https://github.com/LingCore/WinShun/issues/111#issuecomment-6545324877) | 搜索窗口接缝跳位已在真实双屏复现，修复提交 `c0a90f9`，用户确认跳动消失。设置窗口追加真实跨屏复测，用户确认没有明显停顿；报告者 Windows 10 环境仍未复现。详见 [验证记录](issue111-multimonitor-validation.md)。尚未发布。 |
| [#150 普通权限也打不开圆盘](https://github.com/LingCore/WinShun/issues/150#issuecomment-6542380454) | 新回复排除了仅管理员运行导致故障的解释。已有普通权限真实 Explorer → 完整 App 启动链路验证及失败诊断补充，本次核对了生产入口和记录，没有重复改写触发逻辑。仍需报告者的版本、操作来源、输入设备及失败诊断包，详见 [已有复核](issue149-151-validation.md)。 |
| [#69 更新、插件连接失败及 CNB 建议](https://github.com/LingCore/WinShun/issues/69#issuecomment-6545040274) | 新回复仍有独立连接故障，不能沿用“自动更新已实现”就认为已解决。当前源码已具备两个镜像 → GitHub 原站的回退；更新元数据来自 `lcstp.com`。本次实际连接结果见下节。没有建立 CNB 仓库或上传附件；新增发布源需要配套元数据、校验和同步流程，仅改客户端 URL 不足以完成。 |
| [#109 合并 Logo、放大镜和固定入口](https://github.com/LingCore/WinShun/issues/109#issuecomment-6544650085) | 是关闭后的新交互建议，与原“隐藏 Logo”不同。中键切换固定的可发现性、状态表达及既有拖动入口需要单独设计评估；未把建议直接作为已批准的界面变更实施。 |
| [#4 MyDockFinder 组合复测](https://github.com/LingCore/WinShun/issues/4#issuecomment-6540992218) | 报告者明确确认最新版本在其 Windows 25H2 下已正常，符合按用户复测结束跟踪的条件。本次仅记录，没有擅自关闭 issue；本机没有重装 MyDockFinder 复测。 |
| [#145 数字列](https://github.com/LingCore/WinShun/issues/145#issuecomment-6543261039) | 新回复是感谢，没有新增故障。代码已在本地提交，等待发布，不增加无依据的修改。 |

## 旧复查遗漏的已关闭后续

- [#77](https://github.com/LingCore/WinShun/issues/77#issuecomment-6385493784)：报告者补充“管理员运行就不能自定义”，原始需求还涉及 `mklink` 路径。当前生产路径明确区分 MFT 整盘索引与选定文件夹遍历，管理员直读时显示整盘索引；这能解释选项状态，不能证明符号链接搜索问题已解决。仍应针对链接种类、目标位置与索引模式分别验证。
- [#3](https://github.com/LingCore/WinShun/issues/3#issuecomment-6108243819)：关闭后仍有 XYplorer / Tessoa 支持请求。当前代码已有 XYplorer 集成，Tessoa 完整适配仍需单独评估；不能把现有管理器的支持视为所有第三方管理器都已支持。

[2026-10-30 旧复查表](issue-audit-20261030.md) 对 #3、#77 的“未见新增待处理回复”表述不完整，本记录补正；#69、#109、#4 的本次新回复也应以当前记录为准。插件仓库本次未发现需要新增实现的实质后续回复。

## #69 当前网络验证

用当前生产对象中的 `http::get`（WinHTTP）和生产解析、大小 / SHA256 校验函数运行独立无界面入口。仅 GET 公开资源，使用系统代理模式，没有读取或修改用户配置、安装或启用插件，也没有执行下载的程序。

| 入口 | 本次结果 |
| --- | --- |
| `lcstp.com` 安装版 / 便携版版本接口，Windows 10.0.19045 参数 | 两个请求均为 HTTP 200，响应有效，当前版本 0.16.0。 |
| `gh-proxy.com`、`ghfast.top`、GitHub 原站的插件索引 | 三个请求均为 HTTP 200；均为 11,410 字节、9 个插件，三份响应 SHA256 一致。 |
| 三个来源的 `winshun.web` 1.0.0 包 | 三个请求均为 HTTP 200，3,483 字节，均通过生产大小及 SHA256 校验。仅下载校验，没有安装。 |

报告者在 #111 提供的诊断包中，更新日志含错误码 12029 和 12002，但没有与 #69 失败操作一一对应的请求耗时或服务器响应。当前本机网络检查通过不能推断报告者网络正常，也不证明大安装包的持续下载速度；上次完整安装包回归见 [镜像审计](download-mirrors-audit.md)。

追加证据位于忽略目录 `build/issue111-settings-20261101/`：`HttpProbe.cpp`、`build-http.py`、`http-live-with-package.json`、编译和链接日志。入口使用当前主程序对象、Qt 6.12.0 和 MSVC Release，不经过 GUI，退出码 0；它验证当前 WinHTTP 传输和协议解析，不代表完整更新安装或原报告网络环境已验收。
