# 截图工具

README 里的图（`docs/images/*.jpg`）和发帖用的图（小红书 3:4、1:1、X、GitHub 社交预览）都由这里生成：用演示数据启动 Win顺，用真实键鼠输入把窗口摆好，黑白两次背板抠出带真实透明度的窗口，再合成到生成的背景上。图里不会出现自己电脑上的文件。

README 必须保持原有视觉格式：`square` 方形海报模板，保留 Win顺 Logo、LingCore 署名、彩色标题、副标题、背景及模板自带的底部文字；图片在 README 中以 `width="49%"` 排成两列。`readme` 预设只是工具中的纯窗口合成选项，不用于 README 这套海报。修复清晰度时只更换截图来源和导出质量，不删设计元素，不移动图内文案，不改变双列布局。

海报保持原来的 JPEG 文件格式和路径，使用模板原始的 1440 × 1440 分辨率，不再缩到 1080；导出质量为 96，关闭色度抽样，减少文字边缘的压缩损失，点击可查看原图。发帖的 `pages`、`square`、`covers` 保持各自的尺寸与排版；`F:\Win顺宣传图` 是之前导出的发帖成品目录。

更新海报后，先提交图片，再把 README 对应引用指向该提交 SHA 的 `raw.githubusercontent.com/LingCore/WinShun/<提交>/docs/images/<名字>.jpg`。仓库里的文件仍沿用原 `.jpg` 路径，引用使用不可变地址避免旧图缓存。0.15.0 已观察到：相对引用加 `.jpg?v=0.15.0`、刷新页面后，GitHub 的 raw 跳转仍显示旧图，因此不要再把查询参数当成可靠修复。核对仓库 blob、固定提交的图片与实际页面三者一致。

## 目录

| 文件 | 作用 |
| --- | --- |
| `shots.py` | 命令行入口：`scene` / `restore` / `profile` / `shoot` / `matte` / `compose` / `cover` / `taskbar` / `sheet` / `render`，各命令的说明见文件开头 |
| `art.py` | 出图：背景、去掉 Windows 自带阴影（`lifted`）、三层阴影、README 图、帖子内页、封面、任务栏那一页 |
| `capture.py` | 抠图：窗口后面放黑、白背板各拍一张算出透明度，第三张黑的用来发现光标闪烁和没停的动画 |
| `stage.py` | 布景：只索引演示文件夹的配置（`--profile`，每次 `profile start` 重新生成：演示文件的修改时间、收藏夹和关键词、剪贴板历史都直接写进配置），临时换应用主题和壁纸、拍完换回 |
| `agent.ps1` | 提权运行的输入代理：执行场景文件里的按键、点击、拖动（`dragws`，`dragfg` 从前台窗口的边框量起）、抠图命令，每次按键前确认前台是 Win顺 |
| `scenes/` | 场景文件（代理的命令序列）：`all.txt` 拍全部，`light.txt`/`dark.txt` → `windows.txt`（也拍 README 另有的系统开关那三个窗口），`taskbar.txt` 任务栏搜索框，`post.txt` 帖子另有的几张（“全部”里的序号、最新的在前、网站搜索、剪贴板搜图里的字、设置搜索、设置和 AI 页），`more.txt` README 另有的七张（按意思找、只搜一个文件夹、外观设置，再跑 `features.txt`：搜索面板和预览，`foldermenu.txt`：资源管理器里双击空白处的菜单，`edit.txt`：截图编辑窗口，`edit-one.txt` 是它的一遍，`edit-style.txt` 是文字样式那几下点击）；`edit-dark.txt` 拍深色的编辑窗口，要单独跑一次（编辑窗口在同一次运行里第二次打开，没有阴影和圆角，抠出来是不透明的）；`区域销售额.png` 是剪贴板历史里那张图（数字用普惠体，Windows 的文字识别才不会把 371 认成 571） |
| `demo_docs.py` | 生成演示文件夹里的 Word、Excel、PowerPoint、PDF（有内容、有大小，面板里不显示 0 B，PDF 有缩略图，内容搜索搜得到） |
| `demo/` | 演示文件夹的内容，`profile` 发现 `F:\Win顺演示` 不在时复制过去（这个路径会出现在对话框截图里） |
| `work/` | 不进 git：`shots/` 原始抠图，`profile/` 演示配置，`out/` 帖子用图，`agent.txt` 代理日志 |

需要 Python 3.10+、Pillow、numpy。

## 只重新出图

0.15.0 的补拍入口是 `release015.py`。它启动已构建的真实程序，使用现有的 `--preview-*` 模式和临时配置，导出新版搜索面板、可调宽的剪贴板、外观预览、截图工具条及 MP4 / GIF 控制窗口，再交给同一个 `art.compose(..., "square", ...)` 模板。录屏验证会短暂显示它自己的演示窗口，只录这个窗口的客户区；不退出正在运行的 Win顺，不监听或改写系统剪贴板，不改系统主题和缩放。

```powershell
python tools/screenshots/release015.py --exe dist/release015/WinShun/WinShun.exe --capture-only
# 先逐张检查 work/release015 里的原图和 preview.log，再导出：
python tools/screenshots/release015.py --exe dist/release015/WinShun/WinShun.exe --export-only
```

原图按 2 倍像素密度真实渲染，来自生产 QML、控制器和渲染器，内容是演示索引及临时剪贴板历史；不是重新画一套界面。搜索列菜单先在原生弹出窗口里验证，再用同一菜单的窗口内模式导出到一张图上。PNG 原图、录屏验证文件、程序 SHA256 和无损海报保存在忽略的 `work/release015/`；成品沿用 `docs/images/*.jpg`。未改界面的功能保留已核验的旧图；长截图海报的说明已改为默认手动滚动。

0.16.0 的补拍入口是 `release016.py`，用法同上，多一个 `--work`：2 倍像素的原图很占地方，放到空间够的盘上（比如 `--work C:\wsrel016\shots`），海报照常写进 `docs/images`。这一版补拍的是能用独立预览拍到的界面（图标都换成了 Phosphor，设置里的版本号也要跟着变）：搜索面板（右下角多了列表 / 大图切换）、剪贴板、外观、收藏夹与圆盘、插件库、备份、文件类型、搜索范围、网络与 WSL、录屏、截图工具条。启动器、资源管理器、对话框那些图要用下面的代理流程整套重拍，会退出并重启 Win顺、换主题和壁纸，等用户不用电脑时再拍。

下面的 `agent.ps1` / `profile start` 流程会切换或退出完整程序。用户正在运行自己的 Win顺 时，优先用上面的独立预览；不要为了拍图直接执行会退出用户程序的历史场景。

0.14.0 已补拍搜索结果、剪贴板 GIF 预览、收藏夹与圆盘设置，以及插件库、备份迁移、自定义类型、搜索范围和 WSL 页面。运行的是 `dist/WinShun/WinShun.exe` 0.14.0，使用独立演示配置，只索引演示文件夹；剪贴板内容直接写入演示数据库，排除实际运行程序的复制记录。未改界面的功能图保留原有截图。

清晰度修正：最初 `work/release014/raw` 的 Computer Use 截图按逻辑像素返回，再缩成 1080 宽 JPEG 海报，新增页面的文字不够清楚。新版原图在 `work/readme-hd/raw`，由这里的 `capture.py` 在每显示器 DPI 感知模式下按物理像素捕获，整理到 `work/shots/L-*.png`，再沿用原有方形海报模板导出。补拍时只给演示进程设置 Qt 缩放，使窗口以 2 倍像素密度真实绘制，不放大已有位图，不改系统缩放。动态 GIF 先用界面按钮暂停，再拍黑、白、黑三次背板，避免合成错帧。`shoot --margin 24` 可以避免大窗口周围的留白碰到任务栏。

普通截图时，工具会临时隐藏 Computer Use 自己的鼠标提示浮层，拍完无焦点恢复；否则即使真实鼠标已移出窗口，箭头和蓝色光晕仍可能被拍进来。`--hold` 保留拖动过程的指针提示。

改了背景、标题、排版，抠图不用重拍：

```
python tools/screenshots/shots.py render            # 全部
python tools/screenshots/shots.py render readme     # README 功能图与封面（原方形海报模板，写进 docs/images）
python tools/screenshots/shots.py render pages square covers   # 帖子用图，写进 work/out
```

README 的图是帖子各页加上 `shots.py` 的 `README_ONLY`（帖子最多 18 张，放不下的只进 README）；帖子各页的标题、副标题，封面用哪张抠图、哪种背景，都在 `PAGES`、`TASKBAR_PAGE`、`COVERS` 里。几张矮窗口可以用 `+` 连起来叠成一张（`L-plug1+L-plug2+L-plug3`）。挑背景：`shots.py sheet work/shots/L-search.png -o sheet.png`。

## 重拍

1. 构建并部署到 `dist\WinShun`（代理启动的就是那里的 `WinShun.exe`）。
2. 在**提权**的 PowerShell 里：

   ```
   tools\screenshots\agent.ps1 scenes\all.txt
   ```

   依次：用演示配置重启 Win顺 → 删掉 Win顺 启动时记下的系统剪贴板那一条 → 浅色主题 + dawn 壁纸拍 14 张窗口 → 任务栏搜索框 → 深色主题 + aurora 壁纸再拍 14 张 → 换回原来的主题和壁纸 → 用自己的配置重启 Win顺。抠图写进 `work/shots/`。拍的时候别动键鼠；哪一步前台不对，代理就停下，原因写在 `work/agent.txt`。
3. 逐张看 `work/shots/` 的抠图：范围标签对不对、查询文字有没有被选中、有没有自己的文件。
4. `shots.py render`。

只补拍一部分：照 `all.txt` 写一个场景文件（先 `profile start`，拍剪贴板就照它先删掉最上面那一条，最后 `restore`、`profile stop`）。不带参数运行 `agent.ps1` 时，它执行每次写进 `work\cmd.txt` 的一批命令，适合边看边拍；`profile start` 后面可以跟 `shots.py profile` 的参数，比如 `--number-keys off`（图标上就没有序号）；`quit` 关掉 Win顺（原来就没在运行时用它代替 `profile stop`）。README 另有的七张：`agent.ps1 scenes\more.txt`。预览开在启动器旁边，是另一个窗口：`matte <名字> beside` 把贴着前台窗口的 Win顺 窗口也抠进来；演示文件夹里的 `年度报告 封面.png` 是真的图片，预览和结果里的缩略图才有东西可看。按意思找用的是自己设置里的模型服务和密钥（密钥在 Windows 凭据管理器里，不写进配置），演示配置把它指向演示文件夹，拍的时候会把这几个演示文件的文字发给那个服务算向量；拍双击菜单前关掉自己的资源管理器和 Directory Opus 窗口，菜单会列出所有开着的文件夹；`profile start` 启动 Win顺 时带 `WINSHUN_RECENT=own`，最近的文件夹只取演示配置自己的记录，不取 Windows 的。帖子最后一页（`17-follow`）不用截图，由 `art.ending` 画出来。

## 0.13.0 的六张新图（`scenes/release013.txt`，历史场景）

**0.14.0 起圆盘已改成按数字 1 左边的键（`）呼出，旧场景里的晃动和内外圈操作不再适用。** 不要用下面的旧圆盘步骤拍新版；新版 README 改用真实的收藏夹与圆盘设置页，说明当前的触发方式。

拖放圆盘（`droppie`）、长截图（`longshot`）、智能打码（`redact`）、贴图（`pins`）、框选截图（`capture`）、资源管理器里直接打字（`typing`）。一条命令，在**普通权限**的 PowerShell 里：

```
tools\screenshots\run.ps1 -Exe <要拍的 WinShun.exe，发布版，带拖放圆盘>
```

`run.ps1` 等用户 20 秒没动键鼠，拿 `F:\wswt\ui.lock`，用提权的代理跑场景文件（约 3 分钟），完了不管成败都换回主题和壁纸、关掉遮罩、用普通权限重启用户自己的 `dist` 那份、放掉锁。结果是 `work/shots/L-<名字>.png`（六张都进 `README_ONLY`；没有抠图时 `render` 跳过它们），再 `python tools/screenshots/shots.py render readme`。拍之前：

- PixPin 之类退掉（快捷键会在启动器里被划掉）；
- 屏幕 2560×1600、缩放 150%；
- Win顺 **不能提权**：从资源管理器拖过来的东西进不了提权的程序，所以场景里 `profile start --unelevated`，代理开的资源管理器窗口也是普通权限的（`unelevated on`）。

每张怎么拍的：

| 图 | 做法 |
| --- | --- |
| `typing` | 资源管理器开着演示文件夹，打 `2026`（数字：中文输入法会把字母收进自己的拼音框），启动器带着文件夹出来，两个窗口一起抠（`matte … winshun with CabinetWClass`）。钩子只在“前台换成这个窗口”时才装上，所以先弹出启动器再 Esc、`front` 回资源管理器；刚开的窗口第一次试不上，场景里先打一个 `z`（资源管理器里不起作用的键）再来一遍 |
| `droppie` | `pie start 年度报告 数据附录.xlsx 900 575`：在资源管理器里按住这个文件、挪到窗口中间、三次来回晃，等圆盘出来；`pie hover 0 8 right 10 215`：越过内圈右半（复制），停在第 0 块外圈的外沿（文件的图标挂在指针上方，停在外沿，这块的名字在指针下面，不被盖住）；`matte … hold` 指针不动（抠图时不把指针挪开）；`pie end`：先 Esc 再松键，**永远不在圆盘上松开**。收藏夹用 `profile --pie-favorites`（`stage.PIE_FAVORITES`，都在演示文件夹里，其中一个长名字显示两行） |
| `capture`、`redact` | 截图的遮罩是全屏的，拍的是整个屏幕，所以先用 `cover on`（一个满屏的窗口，里面是壁纸）盖住用户的窗口，要指的程序再放到它上面（`front`）；拍完裁成需要的一块（`shotabs`，任务栏和别的都在外面）。`capture` 指着 Windows 设置的 个性化 › 颜色（中文界面的 WinUI 程序；计算器是英文界面，画图、控制面板只框得出整个窗口），框住“最近使用的颜色”的第一块蓝，放大镜里有颜色值。设置窗口先记下位置和是否最大化（`winstate save`），拍完放回去、关掉。`redact` 是浏览器（Edge 访客模式，应用窗口：不登录、不带这个用户的东西）里的 `scenes/客户联系单.html`，全是编的数据；点一下窗口选中，等“发现 5 处”，`Shift+B` 盖上；指针挪到最左边的暗处，放大镜别压在数据上 |
| `longshot` | 同样的 Edge 访客窗口打开 `scenes/季度经营简报.html`（长页面），悬停、快捷键、`Ctrl+L`，自动滚动拼好（约 5 秒，1420×5369），编辑窗口打开后 `Ctrl`+滚轮缩到 50%，抠编辑窗口。页面要在最顶上：上一次长截图“自动滚动不起作用”（`NothingWorks`）时面板还开着，要按 Esc 才结束 |
| `pins` | 经营简报页（Edge 访客窗口）拖框选出四张数字卡片，`F3` 贴图，再把贴图拖到资源管理器窗口上面（`dragpin`：在贴图上按下，在别的程序上松开，只是挪动贴图），一张图悬在两个窗口上。放在整次拍摄的最后：贴图一直在最上面，到 `profile stop` 重启 Win顺 才消失 |

## 换了屏幕要改的

这几处是按 2560×1600 的屏幕量的：

- `scenes/windows.txt` 里点对话框下搜索框的 `clickws 400 1013`（搜索框紧贴在对话框下边：对话框的位置和大小变了，这个点也要跟着改）；
- `scenes/taskbar.txt` 里点任务栏搜索框的 `clickbox 192 1564`（框在任务栏最左端，一个固定的位置）；
- `scenes/more.txt` 里点设置“外观”页的 `clickfg 120 252`，`scenes/edit-one.txt` 里编辑窗口的画框、画箭头、点文字面板（`dragfg` / `clickfg`，图片 100% 显示，左上角在窗口边框内 (81,174)；换了屏幕缩放要重量），`scenes/foldermenu.txt` 里资源管理器窗口的位置（`explorer place`）和双击的点；
- `art.py` 的 `TASKBAR`：全屏截图 `screen-taskbar.png` 上启动器、任务栏、搜索框的位置。

## 注意

- 启动器记住上次的范围和排序，Win顺 刚启动时在“全部”、最匹配。`Tab` 换范围：全部 → 文件 → 内容（`Shift+Tab` 是文件类型那一排，GitHub #16）；选中的是文件夹时 `Tab` 会进入它，所以换范围前让列表里没有文件夹（先打一个什么都搜不到的词）。`windows.txt` 从“全部”开始，也停在“全部”，顺序改了按键就要跟着改。
- 代理会丢掉参数开头的空格：要打“关键词 + 空格 + 文字”，先 `tap 23`（End）、`tap 20`（空格）再 `type`。
- 开拍前系统剪贴板里要有东西（比如复制一段字）：场景开头按 Delete 删掉 Win顺 启动时记下的那一条，剪贴板是空的时它什么都没记，删掉的就是演示历史里最新的那张图片，剪贴板搜图里的字那张就没了。
- Windows 11 的文本光标指示器（品红色）可能被抠进图里；演示配置只排除了各盘的文件夹，盘根目录下的文件仍能搜到，查询别碰上它们。
- `profile start` 现在会把 `demo/` 里新加的文件补进 `F:\Win顺演示`（只加不改不删）；新文件的修改时间写在 `stage.CHANGED` 里（要老，别让“最新的在前”那张图里冒出它们）。
- 资源管理器里别连点同一行：第二下是改名，打进去的字就改了演示文件夹里的名字（`Esc` 取消）。用 `front` 把窗口放前面，只点一次没选过的行。
- `profile stop` 现在用普通权限启动用户自己的那份（以前是提权的，要自己 `--quit` 再启动）；`QuitWs` 也会退掉 `dist` 里正在跑的那份，所以用 `-Exe` 拍别的构建时不用先退它。
- 代理一次只能开一个：`exit` 批处理里第一行被守卫挡住时没有退出，再开一个就是两个都在抢 `work\cmd.txt`。
- 坑和原因见 `docs/pitfalls.md` 的“README 和帖子的截图”一节：Mica 取壁纸颜色、提权后置顶失效、剪贴板窗口不在前台、演示配置混进自己的东西、去阴影时窗口和 logo 分开算。
