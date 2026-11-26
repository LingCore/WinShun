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

- `--preview-results <dir> --file-metadata`：`#479 Shift+Tab on "sum" alone (footer: Tab 补全): the box reads "sum" `、`#479 Shift+Tab on "calc" alone (footer: Shift+Tab 改搜文件名): the box reads "calc" `，引号后没有插件行、范围仍是“全部”；只打 `su`（关键字的开头）时不拦 Shift+Tab。同一探针当时另有两条 #454 警告（文件夹窗口里 Tab 补全路径），已发布的 0.49.0 上同样出现，与本次无关，随后补修，见文末。
- `ctest -R ^tst_plugin$`：通过（含 `loneKeyword` 新用例）。
- `ctest -R ^tst_searchnavigation$`：功能检查 126 + 54 + 60 项全过，但探针以 2 条警告失败，0.49.0 上同样出现；当时误记为预览读 `.md` 的两条，实际是另外两条，随后补修，见文末。

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

## 补修：#454 文件夹窗口里打的子路径补全不了

原因：在文件夹窗口里打 `合同\20`，搜索框按“从这个文件夹起的路径”补全（`navigation::typedIn`），搜索引擎却拿整串 `合同\20` 去窗口文件夹里按名字列磁盘，一行都列不出；只有已进索引的文件夹能靠索引结果补上，没进索引或刚建的文件夹 Tab 无行可补。

改法：`SearchEngine::runNameSearch` 的 `listedPath`：词里带 `\` 或 `/`（非正则）时，从磁盘列出它指向的子文件夹；索引结果和“只搜这个文件夹”的范围仍按窗口文件夹，不带路径的词照旧。

- `--preview-results <dir> --file-metadata`：改前两条警告“#454 Tab did not complete a path typed from the window's folder”和“#454 stepOut lost the path typed from the window's folder”（临时打印：输入 `ordinary\内` 后结果 0 行），改后 0 条警告、退出码 0。
- `tst_core` 新用例 `folderTypedPathFromIt`（真实临时目录，索引里没有它）：`合同\20`、`合同/20`、只搜此文件夹，结果都是 `合同\2024 年`、`合同\2024 说明.txt`；`合同\` 列出整个文件夹（自己排第一）；只打 `合同` 仍是窗口文件夹里的两项；指向不存在的文件夹或正则模式时磁盘不列。`ctest -R ^tst_core$` 通过（305 项）。

同时修了探针的一个隐患：`--clipboard-sizing` 用真实鼠标拖窗口右下角，原先角落被别的置顶窗口挡住时也照样按下拖动（会按在那个窗口上）；现在挡住就不按，并在 `clipboard-sizing.json` 里用 `corner_covered_by` 记下挡住它的窗口。单独跑 `--clipboard-sizing` 一次，角落归属、真实拖动、不抢前台均通过。

## 补修：tst_searchnavigation 的两条警告

探针打印的 WARNING 里，读 `.md` 卡住的两条是它故意制造、检查完就从计数里扣掉的；算进 `2 warning(s)` 的是另外两条：

1. `Window frame: the window has a new native handle (预览), hooked again`：`WindowFrame::setWindow` 先装事件过滤器、再第一次取 `winId()` 建原生窗口，建窗本身被当成“换了句柄”，误报并重复挂钩。预览窗口由工厂现建、交出时还没有句柄，就会触发；正式程序的窗口大多先 `create()` 再交出，所以少见。改为先挂钩再装过滤器；`SavedWindowSize::setWindow` 同样的顺序一并改（它不报警告，但会重复挂钩），`Placement::setWindow` 原本就对。
   - 复现测试 `tst_appearance` 的 `windowFrameTakesAWindowNotYetMade`：改前“first handle”失败（收到警告），改后两种情况都过，真正销毁重建时仍报并重新挂钩。
2. `WinShun: new rows 812 ms after the list asked for them`：搜索 2 ms 就答了，迟的是测试自己。临时计时显示进入 `checkFileGallery` 时启动器已在搜索（搭启动器时发起），接着在界面线程上生成 18 张图片、建 Main.qml、首帧渲染，结果排不上；时长随机器忙闲在 365–812 ms 之间，过 300 ms 才报。改为先等在途搜索落地、图片和索引备好、窗口画完首帧，再进入面板发起要测的搜索。产品里这条日志是对的，不改。

改后：`--preview-results <dir> --search-navigation`：126 + 54 + 60 项全过，`0 warning(s)`，退出码 0；`ctest -R '^(tst_appearance_.*|tst_displaygeometry|tst_foldermenu|tst_settingsnav|tst_searchnavigation)$'` 8 项全过。
