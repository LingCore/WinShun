# 电源命令拼音首字母、内容搜索打字筛选文件（2026-11-25）

两条群里的反馈：`关机`、`重启` 希望能用首字母；内容搜索希望同时按文件名、类型筛选。

## 改了什么

- **电源命令**：把整个名字或 places.txt 里的整个关键词拼全（`gj`、`guanji` 关机，`cq` 重启，`sm` 睡眠，`xm` 休眠，`zx` 注销）不再算“猜测”，按正常名次排，只让位给名字里真有这几个字母的项目（`GJ Notes`）。仍排在应用和普通页面后面的：只对上一部分的、几条命令共用的词（`dy` 电源）、一按就执行的锁屏（`sp`）。
- **内容搜索**：词里不带引号的 `ext:py;js`、`*.docx`、`报告*.pdf`、`!*.min.js` 是“读哪些文件”的条件，不是要找的字；搜索框下那一排最前面显示漏斗标签，点它或点“不限”从搜索框里去掉。插件的内容结果、`ws grep`、MCP `search_content` 走同一套解析。正则模式下不认。

## 验证

| 项目 | 结果 |
|---|---|
| `tst_core`：`contentFileWords`、`powerPlaces`、`switchPlaceSearch`、`contentFolderSpecs` 等 | 通过 |
| `tst_core` `relocatedPersonalFoldersKeepPriority`（真实文件，引擎按 `*.txt`、`ext:md`、`!nee*.txt`、带文件夹、带引号各搜一次） | 通过 |
| `tst_plugin`（295 项，含 `Merge::setContentFiles`） | 通过 |
| `tst_mcp` | 通过 |
| `WinShun.exe --preview-results <目录> --content-kinds --renderer d3d11`，fluent 与 tactile 两种风格 | 通过，无警告 |

说明：

- `relocatedPersonalFoldersKeepPriority` 在程序所在目录下建临时树；构建目录位于 `.worktrees` 下时，点开头的文件夹不进内容计划，这条会失败。把 `tst_core.exe` 拷到普通路径下运行通过，与本次改动无关。
- 整套 `tst_core` 在机器同时有别的编译时，`shareCheckFiles`、`learnedHistoryAcrossResultKinds` 各失败过一次，单独重跑通过（计时类用例）。
- 没有做真实键盘输入的实机测试，也没有真的执行关机、重启。

![内容搜索里打 *.md](content-files.png)
