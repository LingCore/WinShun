# #480 #479 #478 验证记录（2026-11-26）

## #480 密集尺寸下搜索框里的按键被压扁

原因：触感、品牌两种风格的搜索框离窗口边固定 10，框里的键再缩进 7（品牌 8，范围键 4），是按 56 高及以上的搜索行设计的。“密集”尺寸和文件夹窗口的搜索行只有 44 高，触感的范围键只剩 10 高，字被裁掉；品牌的范围条 16 高。

改法：`Theme.fieldInsetYIn(行高)` / `Theme.fieldKeyInsetIn(框高, dpr)`，行矮时先让上下留白，框至少 28、键至少 20；左右仍是 `fieldInset`。启动器搜索栏、搜索面板、剪贴板页都用它，顶部把手跟着框一起上移。

| 检查（`WinShun.exe --preview-results <dir> --apps-grid --style <s> --size <size> --scale <k> --renderer d3d11`） | 结果 |
|---|---|
| 密集 150% 深色，四种风格，改前 | [dense-before.png](dense-before.png)：触感（第 1 行）范围键是细条、字被裁；品牌（第 2 行）范围条过矮 |
| 同上，改后 | [dense-after.png](dense-after.png)：触感键 20 高、指示灯和字完整；品牌舒展；液态玻璃、Fluent 不变；把手不压框 |
| 触感 100% 浅色、125% 深色、175% 浅色，品牌 125% 浅色（密集） | [dense-scales.png](dense-scales.png)：均正常 |
| 触感 紧凑 150% 深色，改前改后像素对比（顶部 100 行） | 0 个像素不同（紧凑、标准、宽松的数值按公式不变） |
| 剪贴板页 `--clipboard-entry-hotkey`，触感、品牌，密集 150% | [clipboard-dense.png](clipboard-dense.png)：第 1、3 行改前（框 24 高），第 2、4 行改后（28 高） |

文件夹窗口的搜索栏固定 44 高，与密集同一套几何，未单独实跑。

## #479 只打插件关键字时 Shift+Tab 跳到“内容”

原因：关键字后面没打空格时不算进入插件（列表型命令只多一行提示，即时型命令的结果排在最前），Shift+Tab 走“上一个范围”，从“全部”绕到“内容”。

改法：`Launcher::keywordAlone()`（`Catalog::loneKeyword`，以及截图历史的关键字）为真时，Shift+Tab 同样给关键字加引号改搜文件名；Tab 没有别的用处时底栏提示“Shift+Tab 改搜文件名”。

- `--preview-results <dir> --file-metadata`：`#479 Shift+Tab on "sum" alone (footer: Tab 补全): the box reads "sum" `、`#479 Shift+Tab on "calc" alone (footer: Shift+Tab 改搜文件名): the box reads "calc" `，引号后没有插件行、范围仍是“全部”；只打 `su`（关键字的开头）时不拦 Shift+Tab。同一探针另有两条 #454 警告（文件夹窗口里 Tab 补全路径），已发布的 0.49.0 上同样出现，与本次无关。
- `ctest -R ^tst_plugin$`：通过（含 `loneKeyword` 新用例）。
- `ctest -R ^tst_searchnavigation$`：功能检查 126 + 54 + 60 项全过；失败来自预览读 `.md` 文件等 5 秒的两条警告，0.49.0 上同样出现。

## #478 压缩包插件一直占 16–17 MB

原因：插件闲置时把内存还给系统，但它用 `PrivateWorkingSetSize` 判断占用，Windows 11 22H2 之前这个值读出来是 0，所以在 Windows 10 上从不归还；另外原门槛 24 MB，小于它的索引也一直留着。“按意思找”插件是同一份代码。

改法：两个插件共用 `src/plugins-common/IdleTrim.h`：读不到私有工作集时用整个工作集代替；闲 20 秒、比上次归还后多出 4 MB 才归还。

`wsbench --archive --data <dir> --idle 60 报价`（真实插件宿主，本机 672 个压缩包）：

```
zip 报价  (12 rows, 8 ms)
plugin memory at +  0 s: private working set 26.2 MB, working set 30.6 MB, private 26.5 MB
plugin memory at + 20 s: private working set 26.2 MB, working set 30.6 MB, private 26.5 MB
plugin memory at + 30 s: private working set 0.1 MB, working set 0.2 MB, private 26.4 MB
plugin memory at + 60 s: private working set 0.1 MB, working set 0.3 MB, private 26.4 MB
zip 报价 again after 60 s idle: 12 rows, 11 ms
```

本机是 Windows 11，Windows 10 的读数回退未实机运行（与主程序 `IdleTrim` 已在 Win10 22H2 验证过的回退相同）；“按意思找”插件只编译通过，未接模型实跑。`ctest -R ^tst_archive$` 通过。
