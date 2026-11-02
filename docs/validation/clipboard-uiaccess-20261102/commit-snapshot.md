# 提交快照的独立验证

2026-11-02，将本次剪贴板改动与其他未提交功能分开暂存，再从 Git 暂存区导出独立源树验证。没有使用工作区内未暂存的源码或头文件补齐构建。

- 独立 Release 构建：WinShun、WinShunService、tst_clipboarddisplay、tst_keyrouter、tst_service 成功。
- CTest：tst_clipboarddisplay、tst_keyrouter 两个测试集通过。默认按键路由测试中的桌面 opt-in 项跳过；下述运行时探针另行覆盖真实输入转发。
- [真实 UIAccess 探针](commit-native.json)：在临时受保护目录和独立 SYSTEM 测试服务下运行该源树构建的程序。两个快捷键入口、原生 Start/Search 的焦点、窗口与菜单的 band 2、搜索/选择/删除/撤销、预览重新打开恢复与折叠保持均通过。临时任务和目录已清理。
- [独立显示及回退探针](commit-fallback.json)：同一构建的普通令牌显示进程检查通过；终止探针自己创建的显示进程后，普通窗口恢复可见、无激活样式和原目标的按键转发均保持。

运行时探针使用临时 profile，不写系统剪贴板，不执行粘贴；没有停止或替换用户正在运行的程序。本页的提交快照验证与此前工作区构建的历史记录分开保留。
