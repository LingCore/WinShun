# #153：搜索时的 CPU 占用

验证日期：2026-10-31。基于 `d6dea42` 的本地修改，Windows 11、Qt 6.12.0、MSVC Release、D3D11。

## 原因和修复

[报告视频](https://github.com/LingCore/WinShun/issues/153)里，持续占用 CPU 的主要是多个 `WinShunExtract.exe`。生产路径是 `Launcher::onResults` → 300 ms 防抖 → `startContentSearch` → `SearchEngine::runContentSearch`：即使用户只是搜名字，**全部** 也会读取未索引的文本和文档；文档提取器允许同时解析 4 个文件。内容索引默认关闭、文件多或文档复杂时，这个阶段可以持续很久。本地静置检查没有发现同一个查询不断重新启动，问题不是靠增加刷新间隔解决的。

- **全部** 设置 `Request::contentIndexedOnly`，只用一个读取线程核对已有索引筛出的候选。索引还不知道的文件、已修改的文件、无法用 gram 缩小范围的查询，留给主动内容搜索。
- 文档使用已保存的文字；缓存缺失／不可读取时也不会自动退回提取进程。未搜索的数量通过 `ContentLeftOut::deferred` 返回，不当作“没有匹配”。
- 现有提示提供“搜索内容”，保留搜索词并切到 **内容**。面板的表格和大图视图也显示入口；支持键盘操作，中英文文案已补齐。
- **内容** 和现有的临时补搜保留直接读取、筛选、大小上限、进度和取消能力。内容索引关闭时仍可主动搜索；没有改变用户的索引开关。

## 性能对照

`--preview-results <目录> --search-workload` 使用真实 `Main.qml`、`Launcher`、`SearchEngine` 和 `WinShunExtract.exe`，创建 64 份无命中的 OOXML 文档，再加上缓存命中、未索引、已变更和缺失文字缓存的文件。查询为 `needle`。目录、文件索引、内容索引和剪贴板存储均隔离，不启动真实剪贴板监听。

| 自动搜索指标 | 旧逻辑 | 最终修复构建 |
|---|---:|---:|
| 启动的文档提取进程 | 4 | 0 |
| 提取进程 CPU 时间 | 1125 ms | 0 ms |
| 主程序 CPU 时间 | 578.125 ms | 328.125 ms |
| 两者合计 | 1703.125 ms | 328.125 ms |
| 查询到自动内容阶段结束 | 971 ms | 333 ms |

该组文件的合计 CPU 时间约下降 **80.7%**。CPU 时间使用 Windows `GetProcessTimes` 的用户态／内核态累计值；持有被采样的子进程句柄，避免进程退出后丢失统计。测量期间使用有等待的事件循环，截图所需的布局／绘制发生在测量之后。不能把这个比例外推为报告者电脑上或所有文件集合的固定收益。

自动搜索仍找到已索引的文本和文档，并明确提示其余 68 个文件未搜索内容；主动搜索找到未索引文本、未索引文档和已变更文档。静置两秒没有再次发起查询，最终样本主程序消耗 62.5 ms CPU；光标闪烁等界面开销仍然存在。

证据保存在 `build/issue153/baseline/search-workload.json`、`build/release/tests/search-workload/search-workload.json` 及相邻 PNG；英文深色 150% 在 `build/issue153/final-en-dark150/`。

## 验证结果

- Release 构建通过：`WinShun`、`WinShunExtract`、`tst_core`、`tst_remotecontent`、`tst_cli`。编译器仍有既有的 Qt 生成代码／历史测试警告，不宣称整个项目零警告。
- 5 项相关 CTest 全部通过：`tst_core`、`tst_remotecontent`、`tst_cli`、`tst_searchnavigation`、`tst_searchworkload`。
- 最终界面改动后再次通过搜索导航和资源回归；资源回归随后补充了实际绘制检查，最终 CTest 通过。
- `tst_searchworkload` 覆盖缓存命中、未索引、已变更、缓存缺失、短词、文件范围、静置、QML 继续搜索动作、面板 Enter 操作及查询保留。
- 中文浅色 100% 与英文深色 150% 均通过，人工检查搜索框、面板和大图截图；入口文字完整，面板按钮可见。离屏测试显式完成视口布局，并检查按钮区域确实绘制了内容，防止把空白截图当成通过。
- `--probe-runtime` 的真实 `App::start` 隔离启动通过，退出码 0、Qt 警告 0。此启动检查与上面的受控搜索数据集是两项独立证据。
- `git diff --check` 通过。

## 边界

已定位并验证报告视频对应的自动文档解析路径，没有在报告者原机和原始文件集合上复测。主动全文搜索和首次建立内容索引仍需要计算和磁盘读取；此次修复控制自动搜索的资源边界，不承诺这些主动任务零 CPU 占用。

工作区另有并行修改，已保留，未纳入本问题的验收结论。本次未提交、推送、发布或关闭 GitHub Issue。
