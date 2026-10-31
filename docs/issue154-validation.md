# #154：内容搜索详情面板

验证日期：2026-10-31。基于 `d6dea42` 的本地修改；Windows 11、Qt 6.12.0、MSVC 2022 x64 Release、D3D11。需求见 [GitHub #154](https://github.com/LingCore/WinShun/issues/154)。

## 实现范围

- 内容范围的右侧详情可关闭、拖动或用键盘调整宽度，保存开关、宽度及标签页。窄窗口改为单区阅读，返回后保留结果选择。
- 摘要、匹配、文本、原生预览、报告五页按需加载。匹配逐处计数，支持关键词开关、进一步筛选、大小写／全词／正则、0–3 行上下文及 F3 导航。上下文不会增加命中计数。
- 文本模型保留文档区段、工作表和真实行号，虚拟化显示；缓存按文件身份、大小、修改时间区分版本。选择和查询变化撤销旧任务，回到界面时再次核对代次。
- 完整统计由用户启动，使用独立搜索，不替换主结果。报告可选当前结果、所选文件、完整搜索，支持文件列表、逐处匹配和上下文。
- 文本、CSV、HTML、真实 XLSX 共用不可变报告快照；复用 miniz 写 OOXML，不依赖 Excel COM。保存使用 `QSaveFile`，复制格式化、导出及原生打印在后台执行。
- 复用现有 `PreviewHost` 和主题；详情关闭或切到其他范围时取消读取并卸载预览。普通名称搜索不新增文档解析。

## 验证结果

| 项目 | 当前结果与覆盖 |
|---|---|
| Release 构建 | `WinShun`、`tst_contentdetails`、`tst_core`、`tst_remotecontent`、`tst_cli` 构建成功。最终界面调整后再次构建 `WinShun` 成功。仍有 Qt 头文件及既有代码警告，不宣称整个项目零编译警告。 |
| 新核心测试 | `tst_contentdetails`：11 passed，0 failed，0 skipped（包含初始化／清理）。覆盖多次命中、Unicode 偏移、跨行词条、筛选与上下文、页／稀疏工作表边界、部分内容、取消、UTF-8／UTF-16、读失败、格式转义和原子保存。 |
| 实际界面与搜索 | `tst_contentpane` 使用真实 `Main.qml`、`Launcher`、`SearchEngine`、提取进程和临时 TXT／XLSX。输出 52 项状态全部为 true；覆盖五页、筛选与词条开关、工作表与真实行号、快速切换 25 次、文件修改后刷新、配置恢复、关闭无额外读取、窄窗口、报告选项失效和取消。 |
| 完整搜索与失败 | 308 个命中文件超过普通列表上限仍完整统计；损坏二进制 XLSX、超过大小上限的文本文件均列入报告。损坏文档夹具采用二进制，避免把扩展名错误但可读取的纯文本当作提取失败。 |
| 导出交叉检查 | 四种格式均实际写入；ZIP 内 XML 可解析。另用 openpyxl 3.1.2 独立打开导出的 XLSX：4 个工作表、9 条匹配，无公式单元格。CSV 防公式前缀，HTML／XML 转义另有恶意文本用例。 |
| 打印 | 实际使用本机 Microsoft Print to PDF 的 HDC 渲染 250 条长文本记录，成功生成 PDF；测试未跳过。 |
| 原生预览 | 本机已注册的 XLSX 预览处理程序实际创建可见 HWND；检查宿主区域宽度和切页后隐藏，100% 与 150% 均通过。 |
| 原有搜索回归 | `tst_searchnavigation`、`tst_searchworkload`、`tst_remotecontent`、`tst_cli` 全部通过。与新增两项合计 6 项 CTest 全通过；最终保存回调及界面调整后新增两项再次通过。 |
| 原有内容逻辑 | 选择运行 `tst_core` 的 29 个内容扫描、索引、缓存和文档提取函数：31 passed，0 failed，0 skipped（包含初始化／清理）。未将其表述为整套核心测试。 |
| 语言与布局 | 中文浅色 100%、英文深色 150% 各完成同一组界面检查，状态全部通过；人工检查实际截图，包括匹配、工作表、窄窗口、摘要和报告。调整报告控件字体与间距后，复制／打印／保存入口在标准窗口内可见，英文标签完整。 |
| 生产启动 | 独立配置运行真实 `App::start` 成功，退出码 0，Qt 警告 0；未开启用户剪贴板监听。此项不替代上面的受控搜索测试。 |
| 差异检查 | `git diff --check` 通过。 |

## 可复核证据

以下是本地构建产物，不属于发布附件：

- `build/issue154-accepted-build.log`、`build/issue154-accepted-tests.log`：构建与 6 项 CTest。
- `build/issue154-build.log`、`build/issue154-tests.log`：最终界面调整后的构建与新增两项复测。
- `build/issue154-core-regression.txt`：所选核心回归函数及结果。
- `build/release/tests/content-pane/`：真实界面的 `hits.png`、`text-sheets.png`、`narrow.png`、`summary.png`、`report.png`、`preview.png`，检查状态 `content-pane.json` 及四种报告文件。
- `build/issue154-en-dark/`：英文深色 150% 的同组检查和截图。
- `build/issue154-independent-check.json`：独立 XLSX 读取与两种界面模式的状态核对。
- `build/issue154-runtime/runtime.json`：隔离生产启动结果。

复跑新增测试：

```powershell
ctest --test-dir build/release -R '^tst_(contentdetails|contentpane)$' --output-on-failure
```

## 明确边界

- 预览从处理程序默认位置打开；尚无各处理程序专用的跳页／跳工作表。实际确认的是本机 XLSX 处理程序，不外推到所有文件格式。Qt 窗口截图不包含独立原生预览 HWND 的像素，预览证据另来自实际 HWND 可见性及几何检查。
- Excel 文本保留工作表、真实行号和单元格分隔，暂不恢复 A/B/C 列位置或原格式。Word 页标记沿用提取器，不代表重新排版后的页码。
- 完整统计最多 20,000 个命中文件；详情最多 250,000 行／处匹配，文字沿用 16 MiB 上限；逐处报告最多 100,000 条、32 MiB 正文，每字段最多 32,767 字符。限制或读取失败会标注，数字不冒充完整总数。
- 已检查 VS Code／Notepad++ 的定位参数生成，未实际启动这两个编辑器。打印验证使用 PDF 驱动，未验证实体打印机；四种保存走实际原子写入函数，未自动操作用户的原生保存／打印对话框或改写系统剪贴板。
- 工作区原有修改和用户数据已保留；本次提交仅包含 #154，未推送、发布或关闭 GitHub Issue。
