# 发布新版本

“发布新版本”包含代码梳理、文档、截图、验证、打包、提交推送和 GitHub Release。只有上传一个新程序，或者只修改 README 文字，都不算完成。用户明确限制范围时以当次要求为准。

## 先确认发布内容

检查 `git status --short`、最近提交、现有版本与 GitHub 最新 Release。若有协作聊天正在修改同一项目，先确认其结果；不要和另一个发布流程同时写版本、图片或安装包。区分本次改动、无关编辑和用户数据，按功能拆分 commit，不用清理或回滚抹平差异。

版本来自 `CMakeLists.txt` 的 `project(VERSION)`。结合实际增量选择下一个版本，不覆盖已经发布的同版本附件或移动旧标签。主仓库为 [LingCore/WinShun](https://github.com/LingCore/WinShun)，插件库为 [LingCore/WinShun-plugins](https://github.com/LingCore/WinShun-plugins)，不要新建替代仓库。

## 文档与截图

- README 中英文介绍、默认行为、快捷键和功能列表应与最终代码一致。
- 更新 `docs/使用说明.md`；涉及插件数据、权限或 SDK 的改动，同步中英文插件文档和示例。
- 写 `docs/release-notes/v<版本>.md`。首段及 `## 新功能 · What's new` 是更新对话框解析的内容，保持现有格式；写清下载文件名和升级时如何保留数据。
- 先看旧海报及 [截图项目说明](../tools/screenshots/README.md)，再补拍新增和变动的界面。使用真实新版程序、生产 QML / 控制器与隔离演示数据；未变化的功能可沿用已核验的图。不要把伪造界面或旧验证结果当成本次证据。
- 沿用 `art.compose(..., "square", ...)`：Logo、LingCore 署名、彩色标题、副标题、背景、底部文字均保留。README 图使用原 `.jpg` 路径，1440×1440、质量 96、`subsampling=0`，以 `width="49%"` 排成两列。
- 原图按物理像素或高像素密度实际渲染，不放大低清位图。逐张检查标题、裁切、文字、当前界面和私人信息，再检查 GitHub 上的真实双列效果。先提交更新的图片，再引用该提交 SHA 的不可变 raw 地址，避免同名旧图缓存；仓库内的图片路径保持不变。相对引用加版本查询参数已验证不能可靠刷新 GitHub raw 跳转，不再采用。

这些约束针对本项目的品牌海报。清晰度问题应从原图与导出质量解决；把海报换成纯窗口截图，会同时丢掉品牌元素和原有版式，不能算质量修复。

## 验证与打包

优先用临时配置和现有 `--preview-*` 模式；录屏用自建演示窗口，不采集用户文档和声音。不要为了截图或替换 DLL 退出用户程序。

使用新的绝对输出路径，例如：

```powershell
.\scripts\release.ps1 -OutputDir 'F:\电脑便捷工具\dist\release015'
```

脚本构建并运行 CTest，再生成安装包、便携包及 `SHA256SUMS.txt`。输出目录内运行着 Win顺，或部署目录含 `data` 时应换一个新目录。不要删除用户数据以绕过保护。

按改动验证实际生产路径。涉及录屏时，检查真实 MP4 / GIF 能解码、分辨率与帧率、暂停继续、再录一段及旧文件保留；音频未实测就不要写“音频已实测”。明确区分当前系统上的验证与尚未覆盖的系统。

核对便携包只含发布文件与空的 `data` 说明，不能带索引、设置、剪贴板或演示配置。核对安装包版本、部署程序和截图记录的程序 SHA256，以及两种包的校验值。构建、测试或验证失败就先修复，不能跳过失败直接发布。

## 推送与发布

提交所有本次必要改动并推送。保存已验证的提交 SHA，再次检查远端有无同版本 Release。

先创建草稿 Release，使用明确的提交 SHA 为目标，上传安装包、便携包和 SHA256 文件。确认三个附件齐全、大小及远端 SHA256 与本地一致后，才发布草稿并设为最新正式版本。最后确认公开 Release、最新版本接口和标签提交一致；不要在没有核验结果时仅凭上传命令成功报告完成。

软件内更新问的是 lcstp.com，它最多 10 分钟后才从 GitHub 同步到新版本。发布后用 `curl` 查这两个地址，核对 `tag_name`、文件名、`size` 和 `digest` 是否和 `SHA256SUMS.txt` 一致：

```powershell
curl.exe -s 'https://lcstp.com/updates/winshun/stable.json?platform=windows&arch=x64&osVersion=10.0.22631'
curl.exe -s 'https://lcstp.com/updates/winshun/stable.json?platform=windows&arch=x64&osVersion=10.0.22631&package=portable'
```

已发布成功的单次任务不得重复执行；若由一次性自动化触发，成功后关闭它。论坛文案可另行提供，但发布到论坛或向他人发消息需要用户明确要求。
