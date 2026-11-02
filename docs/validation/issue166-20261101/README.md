# Issue #166：网页搜索统一为可选插件

日期：2026-11-01；2026-11-02 补充截图迁移权限回归修正。关联 [issue #166](https://github.com/LingCore/WinShun/issues/166)。

## 实现

- 官方 `winshun.web` 升级为 2.0，源码和插件清单位于 `src/web-plugin/package`，从随包目录移到插件库产物。它声明 `webSearch: true`，没有 `runtime` 或重复静态命令，不启动额外进程。宿主继续提供通用网址解析、名称匹配、结果渲染、搜索建议及编辑控件。
- 设置侧栏移除旧“网页搜索”页面，网站、图标、导入导出和搜索建议统一放在插件卡片中，编辑器按需加载。打开设置时等待卡片展开后定位，保证编辑入口可见；设置搜索能找到刚保存的网站。
- 新配置默认关闭。现有配置通过 `Plugins/WebSearchMigrated` 一次性迁移到 `[Plugin.winshun.web] Setting.shortcuts/Setting.suggestions`，保留网站顺序、主页、本地图标引用及显式关闭的建议。原 `[WebSearch]` 保留为迁移前副本，此后不再用作数据源。
- 已启用旧示例插件的关键词覆盖（包括清空关键词）合并进列表，原内置关键词优先。用户目录里手动修改的旧零代码网址清单也会读取并保留；清单本身不被重写。旧清单由解析器在内存中适配，避免重复索引静态命令。
- 安装、启用状态同时控制搜索结果、关键词占用及建议请求。停用、卸载保留配置和图标，重装恢复。停用时，收藏夹等功能可以使用释放出的关键词。
- 截图插件的 `web_search` 工具使用同一个有效提供者；未启用或没有网站时返回错误并提供可打开插件设置的通知。
- 沿用既有插件恢复机制：旧配置启用但缺包时，从插件库恢复，失败保留数据并允许重试。安装版清理旧随包目录。构建插件库产物时，`minWinShun` 自动写为对应的主程序版本。

这是声明式插件迁移，主程序仍包含宿主通用能力；没有将 Qt 界面或网址逻辑复制到另一个常驻程序，也没有以此宣称文件搜索性能提升。

## 构建与回归

Windows 11、Qt 6.12.0、MSVC 2022 x64 Release。共享 `build/release` 一度被另一份预览占用，最终验证使用独立 `build/issue166`；只复用已有依赖和 OCR 模型，未停止用户程序。

```powershell
cmake --preset release -B build/issue166 -DCMAKE_PREFIX_PATH=C:/Qt/6.12.0/msvc2022_64
cmake --build build/issue166 --target WinShun tst_settings tst_aisettings tst_plugin tst_core tst_appearance tst_movedplugins --parallel 4
ctest --test-dir build/issue166 -R '^(tst_core|tst_settings|tst_aisettings|tst_plugin|tst_movedplugins|tst_appearance_.*)$' --output-on-failure
```

构建成功，见 [独立完整构建](build-isolated.txt)、[回归构建](build-final.txt)、[关键词占用修正构建](build-final-keywords.txt)及[最终验证入口构建](build-preview-final.txt)。

11 月 1 日首次完整回归中，选取的 **9 组 CTest 有 8 组通过、1 组截图迁移权限测试失败**：[历史日志](tests-final.txt)。该失败已按下面的迁移授权契约修正，历史记录保留。

11 月 2 日重新构建相关目标后，**同一组 9 项 CTest 全部通过，0 失败，耗时 129.85 秒**：[最新完整日志](tests-consent-20261102.txt)。

| 检查 | 结果 |
| --- | --- |
| tst_settings | 通过，包含新配置默认关闭、旧数据/图标引用保留、关键词改名/清空、重复启动、空列表、手改清单保留 |
| tst_aisettings | 通过，原 AI 配置接口继续可用，网页设置定位到插件页 |
| tst_core | 通过，覆盖网址编码、默认站点、名称/拼音匹配及便携网站文件等既有回归 |
| tst_plugin | 通过，包含新能力、安装/启用/缺失状态、旧网址清单适配，及既有插件协议回归 |
| tst_appearance × 4 | 100%、125%、150%、200% 均通过 |
| tst_movedplugins | 全部通过，含真实截图清单新增权限确认、迁移持久化、失败恢复和网页插件缺包恢复 |
| 网页插件恢复 | `missingAreTheOnesTurnedOn`、`failedRecoveryCanRetryWithoutLosingData` 通过，见[日志](web-recovery-tests.txt) |

**11 月 2 日修正：** 旧 `capturePermissionsAreTheManifests` 测试错误地要求迁移授权追随最新清单。截图插件 1.0.1 已为独立录屏/GIF 工作进程增加 `processes`，其 README 和宿主已有规则要求新增权限再次确认；迁移函数保留原六项授权是符合该规则的行为。

将此测试改为 `captureMigrationRequiresNewPermissionConsent`：在临时目录迁移旧 `[Capture]`，读取真实保存的 `Confirmed`，安装当前生产 `plugin.json` 并经过真实 `Catalog` 授权检查。验证旧授权保持六项、新增 `processes` 等待确认、等待期间不匹配命令、明确确认后恢复可用，以及重复迁移不改变授权。头文件补充固定迁移范围的说明，生产授权列表没有扩大。相关目标重新构建成功：[构建日志](build-consent-20261102.txt)。

末次修正停用后的关键词占用后，重新运行 `tst_settings`、`tst_aisettings`，全部通过：[最终日志](keyword-tests-final.txt)；同时重跑下面两套完整生产路径验证。

## 真实主程序验证

使用真正的 `App::start`、插件安装器、插件目录监视、Launcher、SettingsEditor 和生产 QML，配置、注册表及文件均在隔离测试环境内。搜索建议由本地 HTTP 服务提供，真实经过现有网络和异步回调链路。

```powershell
python -X utf8 tools/verify_web_plugin.py `
  --exe build/issue166/WinShun.exe `
  --qt-bin C:/Qt/6.12.0/msvc2022_64/bin `
  --archive build/issue166/packages/winshun.web-2.0.0.wsplugin `
  --out docs/validation/issue166-20261101/runtime-light
# 另一套加 --dark --english --scale 1.5，并使用独立输出目录。
```

两套均 **27 项检查通过、0 QML warnings**：

- [浅色 / 中文 / 100%](runtime-light/runtime.json)
- [深色 / 英文 / 150%](runtime-dark-150-en/runtime.json)

覆盖关键词置顶、名称与拼音搜索、实时建议、编辑保存、设置搜索、编辑入口定位、启用时的关键词占用、停用后收藏夹使用该关键词、停用/重启用、卸载/重装、持久化保留。重装使用下列实际 `.wsplugin` 归档，经过真实解压、清单检查、安装和重新发现。

本地服务记录确认停用及卸载期间没有建议请求：[浅色请求](runtime-light/suggestion-requests.json)、[深色请求](runtime-dark-150-en/suggestion-requests.json)。两套最终资源快照都只有主程序一个进程。没有打开浏览器、改用户剪贴板或退出用户正在运行的 WinShun。

| 界面 | 截图 |
| --- | --- |
| 生产设置窗口，单一插件配置入口 | [浅色](runtime-light/web-plugin-settings.png)、[深色英文](runtime-dark-150-en/web-plugin-settings.png) |
| 生产搜索窗口及实时建议 | [浅色](runtime-light/web-plugin-search.png)、[深色英文](runtime-dark-150-en/web-plugin-search.png) |

截图使用不透明背景以便检查；窗口内容截图不能包含桌面的原生 Mica 材质。站点和搜索词是测试数据。

## 表单交互

另用现有 `--preview-settings --web-form` 检查真实 SettingsWindow、SettingsEditor 和编辑表单：添加、校验错误、Enter 保存、修改、取消、焦点恢复、滚动及空列表。该入口的插件列表使用预览数据，不能代替上面的真实 App 验证。

- D3D11 / 中文 / 100%，920×720，另覆盖 760×420：[日志](web-forms-final/preview.log)、[添加](web-forms-final/web-add.png)、[编辑](web-forms-final/web-edit.png)。
- 软件/GDI / 深色英文 / 150%，760×420：[日志](web-forms-software-150-en/preview.log)、[最小窗口](web-forms-software-150-en/web-add-empty-compact.png)。
- 两套退出码均为 0、0 QML warnings。

## 产物与范围

工作区 HEAD：`07f9d3d5065a1e189a0c631cf003786aecc4fefd`，包含其他并行任务及本次未提交修改；该 hash 不是本次补丁提交。

最终 `build/issue166/WinShun.exe` SHA256：`27D247E6A837AC503D69914C538C5079AB6850FDACF3318015ECC2C8E1FC42B9`。

11 月 2 日的修正仅涉及迁移契约注释和回归测试，未改网页搜索生产行为；重新核对以下两个产物的 SHA256 均未变化，沿用上面的真实程序及界面验证证据。

插件包：`build/issue166/packages/winshun.web-2.0.0.wsplugin`，SHA256：`3fcc729a17a643a4b5752fd0ff0d54575c6cf551e94fb1715804079c53ef1258`。ZIP 完整性、扁平文件布局、清单及真实重装已验证。本地开发包的最小主程序版本是当前项目版本 0.17.0；正式打包时自动改为新版主程序版本，须随支持此能力的新版主程序发布。

没有发布插件库、提交或推送，也没有修改 issue。尚未逐一验证所有预置网站的公网接口、Win10 实机，或截图插件中“网页搜索”按钮的原生点击流程；本次网络验证使用本地服务，截图工具的提供者检查与错误提示已接入生产代码。
