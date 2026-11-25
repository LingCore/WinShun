# 剪贴板备份与还原：验证记录

2026-11-25，分支 `clip-backup`。

## 自动测试

`ctest -R "^(tst_clipbackup|tst_backup|tst_backupservice|tst_clipimport)$"`：全部通过。`tst_clipbackup` 覆盖：

- 真实 `ClipStore`（带格式文字、分组、备注、图片）在打开状态下备份 → 解包 → 换入另一份历史 → 重新打开，内容、分组、备注、图片逐项一致，原有历史被整体替换；数据文件夹旁不留临时目录。
- 中文路径；没有历史时不产生文件；写不成时保留原来的备份文件。
- 拒收：没有清单、别的程序的 ZIP、设置备份、更新的格式、缺数据库、数据库不是 SQLite、越界路径、`images` 里带子目录、不认识的文件、大小写重名、条目被改坏（校验和）、不是 ZIP。拒收后不留半解开的目录。
- 设置的“从备份恢复”拿到剪贴板备份时，提示去剪贴板页还原。

## 实机（Win10 22H2 虚拟机，独立 `--profile`）

`F:\HyperV\Win10\cb-start.ps1` / `cb-state.ps1` / `cb-clean.ps1`，配合 `steps.ps1` 的真实鼠标键盘。

| 步骤 | 结果 |
|---|---|
| 设置 › 剪贴板 › 保留，点“备份…”，保存对话框回车 | 行内显示“已备份 43 条：C:\Users\ws\Documents\WinShun-clipboard-2026-11-25-171043.zip”；ZIP 内 `clipboard.db`、13 张图片、1 个标注工程、`backup.json`（entries 43，pictures 13） |
| 退出，删掉这份配置的历史，再启动 | “清除剪贴板历史”一行显示“没有记录” |
| 点“还原…”，选刚才的 ZIP | 下方出现确认行“用这份备份替换现在的剪贴板历史？”，带“取消”和“还原并重启” |
| 点“还原并重启” | Win顺 重启并回到同一页，行内显示“剪贴板历史已还原”；历史文件夹里数据库 438272 字节、13 张图片、1 个工程，`clipboard.restore`、`clipboard.replaced` 都不在了 |

诊断事件：`clipboard-backup` 与 `clipboard-restore` 各一条，`ok: true`、`entries: 43`、`pictures: 13`。

截图原件在 `F:\HyperV\Win10\cb-*.png`，未入库。
