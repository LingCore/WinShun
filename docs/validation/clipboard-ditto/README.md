# 剪贴板：保留重复项、单条内容的快捷键（0.47.0）

用户反馈：“如果剪贴板功能能够支持复制重复项以及自定义粘贴快捷键，那么连 Ditto 也可以替换掉了”。做了两件事：

- **保留重复项**（`[Clipboard] KeepDuplicates`，默认关）：再复制一次已有的内容也记成新的一条；紧接着重复复制（连按 `Ctrl+C`）仍只记一条，从剪贴板粘贴出去不算复制。
- **单条内容的快捷键**（`clips.hotkey`）：右键一条 →“设置快捷键…”→ 设置窗口打开到 **剪贴板 › 快捷键 › 单条内容的快捷键** 的这一条并开始录制；之后在任何程序里按下就粘贴这一条。设了的放进“固定”；一键一条；和 Win顺 自身快捷键冲突、被他程序占用时在该行下用红字说明；按键一览里列出。

## 验证

| 项目 | 结果 |
|---|---|
| `ctest -R "^(tst_clipboard\|tst_aisettings\|tst_keycaps\|tst_settings\|tst_cliplifecycle\|tst_mcp\|tst_clipselect\|tst_clipboarddisplay)$"` | 全部通过。`tst_clipboard` 新增 `duplicatesKeptWhenAsked`、`entryHotkeys`（含“让出的键、去掉的键重开后仍为空”）、`hotkeyColumnAddedToAnOldHistory`。 |
| `WinShun.exe --probe-runtime <目录> --entry-hotkey-check`（`WINSHUN_TEST_NO_CLIP_HISTORY=1`，隔离配置，真实 App） | 通过。启动时从历史里注册了 `Ctrl+Alt+Shift+F20`；把另一个进程的文本框切到前台后用 `SendInput` 真按这个键，文本框拿到的是较旧那一条（不是最新一条）；开着保留重复项，粘贴后条目仍是 2 条，快捷键仍在原条目上；系统剪贴板原样还原（6 种格式）。 |
| `WinShun.exe --preview-results <目录> --clipboard-entry-hotkey`（真实 QML 剪贴板窗口，临时历史） | 通过。行右侧徽标显示快捷键（键盘图标）；菜单：没设的有“设置快捷键…”，设了的有“更改快捷键（Ctrl+Alt+1）…”和“去掉快捷键”，多选时没有；“设置快捷键…”发出 `hotkeyRequested`；“去掉快捷键”后键清空、仍在固定、待注册列表少一条。系统剪贴板写入线程未启动。 |
| `WinShun.exe --preview-settings <目录> --entry-hotkey-settings --scale 1.5` | 通过。`showClipboardEntryHotkey` 把该行滚到可见并让它的录制器获得焦点；按 `Ctrl+Alt+K` 后 `clipboardEntryHotkeyRequested(47, "Ctrl+Alt+K")` 发出，录制器停止。 |
| `--preview-settings --page 7`、`--page 1`（150%，中文和 `--english`） | 布局与其他分区一致；英文无截断。 |
| 发现并修好 | 去掉快捷键时空 `QString` 绑成 NULL，撞 `NOT NULL` 约束，重启后快捷键又回来 → `COALESCE(?, '')`，并补了重开断言。`lupdate` 把 QML 的 `"D:\\Work"` 读成 `D:Work` → 恢复原条目并记入 pitfalls。 |

截图（150%）：

- [settings-entry-hotkeys.png](settings-entry-hotkeys.png)：设置里的分区（已设、被占用、刚选中待录制三种状态）
- [clipboard-badge.png](clipboard-badge.png)：剪贴板行上的快捷键徽标
- [menu.png](menu.png)：右键菜单
- [from-menu.png](from-menu.png)：从“设置快捷键…”跳到设置，该行在录制
