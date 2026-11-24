# 截图与海报工具（tools/screenshots/）

用于生成 README 封面（`docs/images/cover.jpg`）、[docs/截图.md](../../docs/截图.md) 功能海报（`docs/images/*.jpg`）以及发帖配图。全程使用隔离演示数据，不暴露本机真实文件。

## 海报规范（不可破坏）

- **模板与版式**：沿用 `art.compose(..., "square", ...)` 方形品牌海报，保留 Win顺 Logo、LingCore 署名、彩色标题、副标题、背景与底部文字。`readme` 预设仅作纯窗口合成，**不用于** README/功能海报。
- **格式与尺寸**：`1440 × 1440` JPEG，质量 `96`，关闭色度抽样（`subsampling=0`），沿用 `docs/images/*.jpg` 原路径。原图须按物理像素或 2× 高像素密度真实渲染，不放大低清位图。
- **展示结构**：README 仅放 `cover.jpg`（含“关注我”）；其余功能图放 `docs/截图.md`，按功能分组、`width="49%"` 双列排列。
- **缓存刷新**：先提交更新后的图片，再把 README / `docs/截图.md` 中的引用更新为该提交 SHA 的 `https://raw.githubusercontent.com/LingCore/WinShun/<commit>/docs/images/<name>.jpg`（不要用 `?v=...` 查询参数，GitHub raw 跳转不会可靠刷新）。

## 目录结构

| 文件 / 目录 | 作用 |
| --- | --- |
| `poster_pipeline.py` | 公共离屏预览采集（`capture_previews`）与方形海报导出（`export_posters`）模块 |
| `release044.py` | 最新版本增量补拍脚本示例，仅声明本版 `RUNS`/`SOURCES`/`POSTERS` 并复用 `poster_pipeline.py` |
| `shots.py` | 命令行总入口：`scene` / `restore` / `profile` / `shoot` / `matte` / `compose` / `cover` / `taskbar` / `sheet` / `render` |
| `art.py` | 海报排版、背景、去原生阴影（`lifted`）与三层柔和阴影合成 |
| `capture.py` | 黑/白背板两次抠图还原真实透明通道，第三张黑背板检测闪烁光标或未停止动画 |
| `stage.py` | 演示环境布景（仅索引演示目录的 `--profile` 配置、演示收藏与剪贴板数据） |
| `demo/`、`demo_docs.py` | 演示文件及生成 Word / Excel / PowerPoint / PDF 的脚本（首次运行时同步到 `F:\Win顺演示`） |
| `agent.ps1`、`run.ps1`、`scenes/` | 全桌面真实键鼠录拍脚本与场景指令（仅在不影响用户正常使用时按需运行） |
| `work/`（git 忽略） | 原始抠图、演示配置、预览日志与无损中间产物 |

运行依赖：Python 3.10+、Pillow、numpy。

## 推荐流程：独立预览补拍（不打扰正在运行的 Win顺）

新版发布优先使用 `--preview-*` 离屏渲染脚本（复用 `poster_pipeline.py`，参考 `release044.py`），使用真实新版程序、生产 QML 与隔离配置，不会退出用户正在运行的 Win顺，也不改系统主题、缩放和剪贴板：

```powershell
# 1. 仅渲染原图到新的工作目录，检查原图与 preview.log
python tools/screenshots/release044.py --exe dist\<task>\WinShun\WinShun.exe --work tools\screenshots\work\release044 --capture-only

# 2. 逐张核对标题、裁切、清晰度与隐私信息后，导出到 docs/images/*.jpg
python tools/screenshots/release044.py --exe dist\<task>\WinShun\WinShun.exe --work tools\screenshots\work\release044 --export-only
```

- 涉及截图插件界面（工具条、截图设置、录屏、白板等）时额外传 `--capture <BuildDir>\library-plugins\winshun.capture\WinShunCapture.exe`（与 `--exe` 同源 Qt）。
- 离屏渲染建议带 `--renderer d3d11` 与 2× 缩放（结果页原生几何校验按需使用 150%）。
- 验证可自动调整尺寸的窗口时，须通过生产 `Placement.attach` 布置空间，避免裸 `setPosition` 导致控制器状态失步。
- 仅修改标题、副标题、背景或排版时，无需重拍抠图：

  ```powershell
  python tools/screenshots/shots.py render readme          # 仅重出 docs/images 海报与封面
  python tools/screenshots/shots.py render                 # 全部（含发帖图，输出到 work/out）
  ```

## 全桌面实拍流程（仅在需要系统壳层画面时）

涉及任务栏搜索框、资源管理器窗口内交互、系统文件对话框等离屏模式无法覆盖的场景，才使用 `agent.ps1`：

1. 构建并部署到独立目录（不要覆盖含 `data` 的用户目录）。
2. 在用户无操作时，以**提权** PowerShell 执行场景（如 `tools\screenshots\agent.ps1 scenes\all.txt` 或单场景文件）。该流程会临时切换演示配置、主题与壁纸并在结束时恢复，代理日志写到 `work\agent.txt`。
3. 核对 `work/shots/` 抠图无异常（范围标签、选中状态、无本机私人文件）后运行 `shots.py render`。

## 实拍注意事项

- **代理单实例**：同一时间只能运行一个 `agent.ps1`，否则会争抢 `work\cmd.txt`。
- **剪贴板首条**：场景开头会删掉启动时顺带记下的系统剪贴板条目；开拍前系统剪贴板需非空，否则会误删演示历史中的最新图片。
- **Tab 切范围与进文件夹**：选中行是文件夹时 `Tab` 会进入该文件夹，脚本切范围前应先确保结果列表无文件夹。
- **屏幕坐标依赖**：部分 `scenes/*.txt` 与 `art.py` 的 `TASKBAR` 坐标基于 `2560×1600 @ 150%` 录制；更换屏幕分辨率/缩放时需重新量取。
- **常见坑**：Mica 采样壁纸、提权窗口置顶、去阴影算法等详见 [安装、构建与测试踩坑](../../docs/pitfalls/installer-build-and-testing.md)。
