# #357 字母直达：换键、换字母、行里的按钮（2026-11-17）

main，构建目录 `build/run-latest`（Release，Qt 6.12.0）。

- `tst_numberhints` 通过：`letterTap` 加了换成 Shift（Alt 不再算）、换成 Ctrl、自定字母 `QWERT12`（字母顺序、数字、Shift+1 和小键盘 1 都算 1、不在里面的字母不算）、Shift+字母选中后行上没有字母、再轻按退出、窗口不听时退出；`letterJumpSetting` 加了键和字母的保存读取、清理（小写转大写、去掉重复和其他字符、空的回到默认）、非法键回到“跟数字键一样”。
- `tst_aisettings` 通过（新的 `Results/LetterJumpKey` 在设置窗口有同名行）。
- Win10 22H2 虚拟机，真实按键，字母设成 `QWERTASDFGZXCVB12345`：
  - 搜索框轻按 Ctrl，三行是 Q W E（`vm-j2-letters.png`）；Shift+W 选中第二行，它的五个按钮上是 Q W E R T（`vm-j3-picked.png`）；按 R（复制完整路径），客户机剪贴板里是 `C:\ws\files\zzletter-b.txt`。
  - Shift+Q 选中第一行，按 T 删除按钮变成“确认删除”，字母还在（`vm-j5-delete-armed.png`）；再按 T，文件进了回收站。
  - 改成 Shift：轻按 Ctrl 不出字母，轻按 Shift 搜索框出字母（`vm-s1-shift-letters.png`），剪贴板（开在资源管理器上，按键是转发的）也出字母（`vm-s2-clip-shift-letters.png`）。

- 设置 › 结果列表 › 操作习惯：两行新设置跟在“字母直达”下面。在“用哪些字母”里用英文输入 `asd1a` 回车，存成 `ASD1`，出现“恢复默认”（`vm-settings-letters-taken.png`）；中文输入法选了“阿斯顿”再回车，框标红、设置不变（`vm-settings-chinese-not-taken.png`），免得清理后只剩一个 A；“恢复默认”回到 26 个字母，框里从头显示。

没有覆盖：中文输入法下在搜索框轻按 Shift 是否会切中英文（设置里已写明会）；Windows 11；剪贴板里的 Shift+字母（没改，仍是粘贴为纯文本）。
