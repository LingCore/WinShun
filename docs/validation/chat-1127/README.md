# 群聊反馈（2026-11-27）

| 反馈 | 改动 | 验证 |
|---|---|---|
| 公式识别窗口没有关闭按钮，只能按 Esc | 关闭按钮一直在，但图标字符丢了（`glyph: ""`），所以看不见。补回图标；`tst_encoding` 新增 `namedGlyphsAreNotEmpty`，以后 QML 里带名字注释的空图标会被查出来 | `WinShunCapture.exe --preview-formula`：右上角出现关闭按钮（[formula-close.png](formula-close.png)）；`ctest -R ^tst_encoding$` 通过 |
| 插件库写着 44 个插件，只显示 1 个 | 从别处跳到插件库时会带上那个插件的筛选词，留在筛选框里不容易发现。筛选框有字时右端出现清空按钮（已安装的筛选框也有）；有筛选词时标题下写“N 个插件，筛选后显示 M 个” | `WinShun.exe --preview-plugins <png> --filter capture`：按钮显示、点击后全部列回（[library-filter.png](library-filter.png)） |
| 开着“始终模糊匹配”搜 tim，System Informer 排在 TIM 前面 | 模糊猜中的应用也拿了“常用加分”（最多 80 分），能超过名字里真有这几个字母的应用。现在模糊猜中的只有“以前就是用这几个字打开它”才加分 | `tst_fuzzymatch` 新增 `appNamedAsTypedComesFirstWithFuzzyOn`：修复前 System Informer 89 分、RealTimeSync 24 分，修复后通过；`tst_core` 通过 |
| 便携版 Total Commander 必须先开着，Win顺 才能用它打开文件夹 | 便携版在注册表里没有记录，只有运行时才认得出。现在把运行时看到的程序位置记在 `state.ini` 的 `[FileManagers]` 里，关着也能用；XYplorer、Pulse 同样处理 | `tst_shelllaunch` 新增 `portableTotalCommanderIsRemembered`：本机装了 Total Commander，只验证到“运行时看到的位置被记下”，“关着时读回来用”这一半被跳过 |
| 剪贴板想默认钉住，连续粘贴多条 | 设置 › 剪贴板 新增“打开时钉住”（默认关） | `tst_settings`、`tst_aisettings` 通过；设置导航探针通过。没有实际打开剪贴板连续粘贴 |

## 没有改的

- “记住窗口大小对设置界面不起作用”：没有查。按设计只记拖边框拖出来的大小（贴靠到半屏、打开时为了放进小屏幕而缩小的不记），需要对方说明是怎么改的大小。
- 小键盘数字键粘贴：剪贴板面板开着时，小键盘数字和主键盘数字一样可用；“不打开面板直接粘贴”（Ctrl/Alt + 数字）目前只注册了主键盘的数字键，小键盘没做，留作下一轮。
- Edge 无痕窗口复制的内容不进历史：是 Edge 自己标了“不要记进剪贴板历史”，Windows 自带的 Win+V 也不记，保持不变。
- `tst_shelllaunch` 里原有的 `realDialogFocusAfterGoingToAFolder(the user goes into the list meanwhile)` 这次运行失败（找不到对话框的文件列表），没有查。
