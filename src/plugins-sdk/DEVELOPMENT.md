# 用 AI 开发 Win顺 插件

先在 Win顺 的“设置 → 插件 → 开发者”创建模板，再把这个文件和同目录的 `plugin.json`、脚本、SDK 一起交给编程助手。模板已经能运行，先保留它的协议接入，再实现功能。

## 给编程助手的任务

请基于当前目录的 Win顺 插件模板实现我描述的功能。先读现有 `plugin.json`、入口文件和 SDK；沿用它们的语言与运行方式。功能描述由我在这段提示之后提供。

- 协议与例子：https://github.com/LingCore/WinShun/blob/main/docs/plugins.md
- JSON Schema：https://raw.githubusercontent.com/LingCore/WinShun-plugins/main/schema/plugin.schema.json
- SDK：https://github.com/LingCore/WinShun/tree/main/src/plugins-sdk
- 投稿与检查：https://github.com/LingCore/WinShun-plugins/blob/main/CONTRIBUTING.md

实现约束：

1. 不猜协议。先核对上述文档和模板；给 `plugin.json` 添加 `$schema` 以获得编辑器校验。零代码能完成的需求优先使用零代码；已有脚本模板不必改写成常驻进程。
2. 查询只查询，打开结果或执行动作由用户触发。查询时不删除文件、不启动程序、不修改剪贴板；不要把每次输入变成副作用。
3. 只申请实际需要的权限，并在 README 说明原因。插件以普通用户身份运行；权限声明不等于操作系统强制隔离，只有 strict 沙箱支持的限制才是强制的。不要要求管理员权限。
4. 数据写入 `WS_DATA_DIR`；密钥使用宿主提供的秘密设置，不写进源码、普通配置或日志。不上传用户数据来替代本地处理。
5. stdout 只输出符合协议的 UTF-8 JSON，诊断写 stderr。常驻插件一行一个 JSON-RPC 消息并及时 flush；使用 SDK 接入初始化、查询、取消和动作。Python 一次性模板按环境变量读取查询。
6. 结果使用稳定 id，中文可读的标题和副标题；提供可复制的结果和清楚的空状态、错误状态。命令必须能仅用键盘完成，避免让用户猜关键字。
7. 网络查询设置超时，处理取消；失败不返回伪造结果。不要在用户每输入一个字符时重新加载大模型或全量扫描目录。
8. 添加针对实际行为的验证，至少覆盖正常输入、空输入、无效输入和依赖/网络不可用。通过 Win顺 的消息记录核对真实往返，不能把独立脚本运行成功当作插件接入成功。
9. 为交付补齐 README：用途、关键字和示例、运行依赖、权限、数据位置、失败处理、许可证。投稿只包含允许的源码和素材，不打包虚拟环境、密钥或运行日志。

完成后报告修改内容、验证结果和仍未验证的部分。

## 本地运行与投稿

1. 在 Win顺 中打开插件开关，输入模板关键字。开发者模式会在脚本修改后重新加载，并显示消息记录。
2. 从 Win顺 源码构建后，可运行：

   ```powershell
   wsbench --plugin <插件目录> --query "<关键字> 示例" --runs 5
   ```

3. Fork [WinShun-plugins](https://github.com/LingCore/WinShun-plugins)，把插件目录放到 `plugins/<作者.名字>/`，运行 `python tools/check.py`，再按该仓库的贡献指南提交 PR。每次更新提升插件版本。

社区库只收支持的脚本、文本和图片，不收 exe 或 DLL。C++ 模板可在自己的仓库发布，用户通过 Win顺 导入插件包。PowerShell 脚本存成带 BOM 的 UTF-8，兼容 Windows PowerShell 5.1。

插件安装后默认关闭，用户开启时才确认权限；开发者模式和模板创建不代替这个授权。

常驻界面插件可在 `initialize.capabilities.hostHotkeys == true` 时通过 `winshun/tool` 调用 `register_hotkeys`，参数 `{keys: ["F1"]}`，返回 `{errors: {}}` 或逐键的 Windows 注册错误。宿主发 `hotkey` 通知 `{key: "F1"}`；插件只执行当前仍绑定的动作，录制期间忽略迟到通知。先发 `winshun/hotkeys` 保留相应键，录制时用空数组暂时释放，关闭或崩溃时由宿主清理。此工具使用已有 `ui` 权限，不提高插件权限；旧宿主无该能力时自行注册。完整协议见[插件文档](../../docs/plugins.md)。
