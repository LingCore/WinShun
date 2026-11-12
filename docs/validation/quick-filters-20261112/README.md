# 快捷筛选（2026-11-12）

论坛用户的需求：像 Everything 的自定义筛选器那样，把常用条件（例如 `!.Recycle.Bin\`）存起来，在搜索面板里一点就套用。

## 做了什么

- 设置 → 结果列表 → 类型与筛选：“快捷筛选”，一条是“名称 + 条件”，条件的写法和搜索框一样。
- 搜索面板侧栏多一组“快捷筛选”小片，可同时开几个（全部满足）；`+` 把输入框里的条件带到设置里。
- 设计取舍见 `docs/architecture.md` 的“搜索面板”一节。

## 验证

构建目录 `build/quick-filters`（Release，Qt 6.12.0）。

| 检查 | 结果 |
|---|---|
| `tst_core`：`quickFilterConditions`、`quickFilterSearch`（匹配器、引擎：排除、多条并且、` \| `、正则模式、不输入浏览、读不了的条件跳过） | 通过 |
| `tst_core`：`relocatedPersonalFoldersKeepPriority` 末尾新增的内容搜索断言（被排除的文件不读） | 通过（把 exe 拷到不带 `.` 的路径下运行；在 `.worktrees` 里这条旧测试本身在更早的一行失败，与本次无关） |
| `tst_settings`：`quickFilterSettings`（读写、手改 ini 的整理） | 通过 |
| `tst_settingsfilters`（新增 ctest：真实设置窗口，增删改排、重名、读不了的条件、`+` 带过来的草稿） | 通过，`quick-filters.png` |
| `--navigation-workflows` 的 `panelQuickFilters`（真实 Main.qml，鼠标事件点小片：开、叠加、关、清除全部、设置里改了立即生效、`+`） | 全部通过，QML 无警告；左侧列表 `panel-quick-filters.png`，顶部一行 `panel-quick-filters-top.png` |
| `tst_querysyntax` | 通过 |
| `tst_searchnavigation` | 失败：1 条“new rows … ms after the list asked for them”计时警告。同一时刻用没有本次改动的构建（`build/press-feedback`）跑同一命令，同样的警告，同样失败，不是本次引入 |

过程中发现并改掉的：点一下小片会让整组小片重建（离屏检查里表现为悬空指针崩溃，实际会是悬停闪一下、焦点丢失）——开关状态改成单独的 `activeQuickFilters`，列表只在设置改动时变。lupdate 会吃掉 QML 字符串里的 `\`，带反斜杠的例子改成 `%1` 参数。

合并了同日的面板改版（b58ddf4b：筛选改成左侧窄列表或结果上方一行）后重新构建、重跑以上检查，两种布局里各有一组快捷筛选。没做实机键鼠测试，没有部署。

## 搜索前缀（同日追加）

每条筛选可以有一个搜索前缀：`:nobin 报告` 在普通搜索框里也套用。构建目录 `build/quick-filter-keywords`。

| 检查 | 结果 |
|---|---|
| `tst_querysyntax`：`quickFilterKeywords`（前后位置、大小写、多个、单独一个、引号、盘符、路径、和类型前缀并用） | 通过 |
| `tst_settings`：`quickFilterSettings`（前缀读写；重复、占用、写错的前缀作废） | 通过 |
| `tst_settingsfilters`（设置窗口：前缀规范化、与类型／文件夹／其他筛选重名被拒，类型也不能占筛选的前缀） | 通过 |
| `--navigation-workflows`：普通搜索框里打前缀、两个前缀、删掉前缀、设置里拿掉／还回前缀、面板里小片加前缀 | 8 项全部通过，QML 无警告 |

英文翻译补齐（含面板改版留下的 8 条），`winshun_en.ts` 未完成项为 0。同样没做实机键鼠测试，没有部署。
