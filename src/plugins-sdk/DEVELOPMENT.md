# 用 AI 开发 Win顺 插件

先在 Win顺 的 **设置 › 插件 › 开发者** 创建模板，再把这个文件和同目录的 `plugin.json`、脚本、SDK 一起交给编程助手。模板已经能运行，保留协议接入后直接实现功能。

> 编程助手也可直接通过 `npx skills add LingCore/WinShun --skill winshun-plugin-dev` 安装同一套说明，不必每次粘贴提示词。

## 参考文档

- 协议与示例：https://github.com/LingCore/WinShun/blob/main/docs/plugins.md（英文：`docs/plugins.en.md`）
- 清单 JSON Schema：https://raw.githubusercontent.com/LingCore/WinShun-plugins/main/schema/plugin.schema.json
- SDK（Python / PowerShell / C++）：https://github.com/LingCore/WinShun/tree/main/src/plugins-sdk
- 插件库投稿规则：https://github.com/LingCore/WinShun-plugins/blob/main/CONTRIBUTING.md

## 实现约束

1. **先核对协议再写代码**：给 `plugin.json` 加 `$schema` 获得校验。零代码能完成的需求优先零代码；已有脚本模板不必硬改常驻进程。
2. **查询无副作用**：查询只返回结果，不删除文件、不启动程序、不修改剪贴板；动作只在用户触发 `Enter` 或显式 action 时执行。
3. **最小权限原则**：只申请实际使用的权限并在 README 说明原因。普通插件以标准用户权限运行，权限声明不等于 OS 沙箱（仅 `strict` 沙箱具 OS 级强制限制）；非必要不申请 `elevated`（仅自带 exe 且确实需要操作管理员窗口时使用）。插件安装后默认关闭，首次启用由用户在 Win顺 窗口确认权限。
4. **数据与密钥边界**：运行数据写入 `WS_DATA_DIR`；密钥使用 `password` 型设置项（存 Windows 凭据管理器），不写入源码、普通配置或日志。
5. **洁净协议输出**：`stdout` 只输出 UTF-8 协议 JSON，日志和错误写 `stderr`。常驻插件每行一条 JSON-RPC 消息并及时 flush；PowerShell 脚本保存为带 BOM 的 UTF-8（兼容 PowerShell 5.1）。
6. **可读结果与健壮性**：提供稳定 `id`、清晰标题/副标题、可复制内容以及明确的空状态/错误状态；全程可用键盘完成。网络请求必须设超时并响应取消，失败时不伪造结果，不在每次击键时重载重资源。
7. **真实往返验证**：覆盖正常、空输入、无效输入及依赖/网络不可用场景，并在 Win顺 真实消息链路下验证（不能只测独立脚本）。
8. **完整交付**：补齐 README（用途、关键字示例、依赖、权限、数据位置、失败处理、许可证），不打包虚拟环境、密钥或日志。

## 进阶能力速查

- **内容检索提供者**：命令声明 `contentSearch: true` 即可接收 `trigger: "content"` 与目录/日期/大小/排除等 `filters`，返回原文件 `open.path` 与命中摘要；动作数组严格按 `Enter`、`Ctrl+Enter`、`Shift+Enter` 顺序排列（`key` 仅用于显示）。详见 [内容检索提供者](../../docs/plugins.md#内容检索提供者)。
- **宿主热键注册**：常驻界面插件在 `initialize.capabilities.hostHotkeys == true` 时，可通过 `winshun/tool` 调用 `register_hotkeys`（`{keys: ["F1"]}`）并处理 `hotkey` 通知；先用 `winshun/hotkeys` 声明保留键，录键时发空数组暂释，需 `ui` 权限（不提权）。详见 [插件文档](../../docs/plugins.md)。
- **原生共享 Qt 运行时**（最低 Win顺 0.22.0）：可在 `runtime` 中声明 `"qt": "host", "qtVersion": "6.12.0"` 复用宿主的 Qt Core/Gui/Qml/Quick、Basic Controls 和 Layouts（仅普通权限、MSVC x64 Release，不与 `elevated` 同用，不要混入其他 Qt 运行时）。详见 [共享 Qt 契约](../../docs/plugins.md)。
- **C++ 异步查询**：C++ SDK 支持重写 `queryAsync(const Query&, std::function<void(Json)>)`（默认同步调用 `queryResult`）。异步实现须在协议线程快照设置与语言、保证恰好回复一次、处理 `cancel(queryId)`、在 `shuttingDown()` 及派生类析构中 join 工作线程；工作线程可调用 `partial()` 流式推送结果。

## 本地调试与投稿

1. **开发者模式**：在 Win顺 中启用插件，打开开发者模式可在脚本修改后自动重载并查看实时消息记录。
2. **命令行调试**（需在 **设置 › AI 助手** 启用命令行连接）：

   ```powershell
   ws plugin install <插件目录>     # 安装或覆盖更新（已启用的保持启用）
   ws plugin enable <作者.名字>     # 启用（首次需用户在 Win顺 窗口确认权限，此时退出码为 1）
   ws plugin list --json           # 查看 state、进程 status 与 error
   ```

   插件 `stderr`、协议错误和崩溃记录在 `%LOCALAPPDATA%\WinShun\plugins.log`。源码构建环境下也可用 `wsbench --plugin <插件目录> --query "<关键字> 示例" --runs 5` 基准测试。
3. **发布到插件库**：将插件发布到公开 GitHub 仓库，Release 附 `<作者.名字>-<版本>.wsplugin`；向 [WinShun-plugins](https://github.com/LingCore/WinShun-plugins) 提交仅新增 `plugins/<作者.名字>.json` 的 PR（用该仓库 `tools/entry.py` 生成）。含 exe/dll 的社区包按“未审核原生插件”（`--unreviewed-native`）收录，安装及每次更新需用户确认。
