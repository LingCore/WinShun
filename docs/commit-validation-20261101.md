# 本次提交的独立验证（2026-11-01）

用户要求提交修改后，从暂存区导出独立源码快照，排除工作区中既有的剪贴板、贴图等未暂存修改。导出时 Git tree 为 `1ec5ae25edfc7554fbf0a37804b1ca6db96579bd`；本页与这次测试日志在验证完成后补入。

- 源码：`D:\WinShun-commit-check-20261101\source`，构建：同目录下 `build`。
- Qt 6.12.0、MSVC x64、Ninja Release；`WinShun` 与下面五个测试目标全部从该快照构建成功。构建有既有编译警告，不声称零警告。
- 启用原生桌面外观、托盘菜单和键盘路由测试，顺序运行，全部退出码为 0，且无失败、无跳过。详情见 [结果](validation/commit-20261101/results.json) 和各测试日志。

| 测试 | 通过 |
| --- | ---: |
| [文件夹菜单](validation/commit-20261101/tst_foldermenu.txt) | 12 |
| [托盘菜单](validation/commit-20261101/tst_traymenu.txt) | 16 |
| [原生外框与外观](validation/commit-20261101/tst_appearance.txt) | 16 |
| [按键路由](validation/commit-20261101/tst_keyrouter.txt) | 6 |
| [Opus 焦点识别](validation/commit-20261101/tst_opusfocus.txt) | 3 |

这次检查验证了待提交代码可独立构建与运行，不依赖其他未提交编辑。测试使用自建窗口或临时数据；#157 报告者的 Windows 10 场景、#159 在真实 Explorer 中隐藏图标后物理双击的完整链路仍未实测，参见各问题验证文档。

此前文档中的“没有执行提交”描述的是各轮验证当时的状态；用户随后已明确授权本次本地提交。未推送、未发布新版本。
