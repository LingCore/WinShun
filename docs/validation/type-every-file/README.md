# 类型扩展名填 `*`：只分文件和文件夹

2026-11-26，Windows 11 Pro 26200，工作树 `.worktrees/type-all-files`，`build/type-all-files`（Qt 6.12.0）。群里用户反馈：类型分得太细，只想分成文件夹和文件，问扩展名能不能写 `*`。以前不能：`*` 被当成通配符去掉，提示“填上扩展名”。

现在类型的扩展名填 `*`（`*.*`、输入法打出的 `＊。＊` 也算）就是“所有文件”：选它列出“不限”搜到的文件，不管扩展名，没有扩展名的也算，不列文件夹；`ext:` 照常缩小；数字是搜到的文件数。跳过的文件类型、优先显示的类型这两个列表仍不接受 `*`。

| 检查 | 结果 |
|---|---|
| `tst_settings everyFileKind`（`*`、`*.*`、`＊。＊`、`jpg，* png` 归成 `*`；`*.PNG` 仍是 png；`fileExtensions` 不收 `*`；存盘读回；手改 INI 为 `*.*` 读回） | 通过 |
| `tst_core everyFileType`（索引：只列文件、无扩展名也列、名字像文件的文件夹不列；`ext:pdf` 缩小；跳过的 bak 仍不出现；多选含 `*` 仍是所有文件；单个词 `src` 不列 src 文件夹里的全部文件，`src main` 仍可按文件夹找；`a \| b`；空输入的最近修改只列文件；绑定文件夹的类型列出该文件夹里的文件；计数 `{5, 2, 1}`；不在索引的文件夹走磁盘遍历也只列文件） | 通过 |
| 整份 `tst_settings`、`tst_core` | tst_settings 通过。tst_core 299 过 4 败：3 条网络共享计时/核对在构建机繁忙时失败，单独运行通过；`relocatedPersonalFoldersKeepPriority` 是已知的 `.worktrees` 路径问题（见 [power-initials-content-files](../power-initials-content-files/README.md)），把 `tst_core.exe` 拷到普通路径运行通过 |
| `WinShun.exe --preview-results <dir> --type-strip-every-file --renderer d3d11`（fluent 100%、150%；英文 150%；tactile 深色 150%） | 全部通过。真实 Main.qml、Launcher 和搜索引擎：类型“文件(`*`)”+“文档”，输入“报告”，不限 5 行，选“文件”只剩 3 个文件；类型条数字 不限 5、文件夹 2、文件 3、文档 2；“文件”用 Phosphor `files` 图标，提示“所有文件，不含文件夹”；`报告 ext:pdf` 只剩 pdf；再加选“文档”仍是 3 个文件 |
| `WinShun.exe --preview-settings <dir> --file-types --scale 1.5`（含新增检查） | 通过。SettingsEditor 新增 `＊。＊` 存成 `*`，改成 `jpg *` 仍是 `*`；新类型输入框提示“扩展名，例如 dwg dxf；* 为所有文件”在 150% 下完整显示 |

截图：类型条 [strip-150.png](strip-150.png)，设置页 [settings-150.png](settings-150.png)。

未做：没有退出用户正在运行的 Win顺 做替换实测；保存、读取和界面分别由上面的单元测试和离屏预览覆盖。
