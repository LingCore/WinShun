# 自定义文件对话框兼容：#143

此记录区分上游资料、原生协议测试与实际软件验证。AutoCAD 没有安装，也没有为本次验证安装；不把模拟窗口的通过结果写成 AutoCAD 实机通过。

## AutoCAD

[Issue #143](https://github.com/LingCore/WinShun/issues/143) 的截图是自定义文件选择窗口，包含文件列表、文件名组合框和文件类型组合框。Autodesk 的 [CNavDialog 文档](https://help.autodesk.com/cloudhelp/2019/ENU/OARX-RefGuide/files/OREF-CNavDialog.html) 表明它是 `CAdUiDialog` 派生类，不能假设它必定提供标准 Windows 文件对话框的地址栏、控件 ID 或 Shell 视图。

[Listary FAQ](https://help.listary.com/faq) 明确将 AutoCAD 列为特殊适配的软件，但没有公开实现。[Quick Access Popup 作者的变更记录](https://www.quickaccesspopup.com/change-log/) 记载过严格识别条件导致 AutoCAD/Revit 失效后回退的修正；[其公开源码](https://github.com/JnLlnd/QuickAccessPopup/blob/prod/QuickAccessPopup.ahk) 使用文件名输入框切换目录。上述证据支持补充自定义控件路径，不能证明某个固定控件 ID 对所有 CAD 版本有效。

Win顺保留标准识别优先级，增加结构匹配：`#32770`、确定/取消、可见文件列表、列表下方唯一可编辑的文件名组合框，以及其下方唯一不可编辑的文件类型列表。缺少角色或存在歧义就不接管。使用实际组合框 ID 发送 `CBN_EDITCHANGE`，让宿主模型收到输入变化；目录带尾部反斜杠，防止 `foo.dwg` 这样的目录被当成文件打开；跳转后恢复原文件名并再次通知宿主。

验证：`tst_filedialog` 用真实原生组合框、非标准控件 ID 和只在 `CBN_EDITCHANGE` 更新的宿主模型，覆盖普通目录、含扩展名目录、中文空格目录、恢复文件名、明确确认、缺失控件、多个输入框与不存在目录。**尚缺 AutoCAD 实机确认**。结构不同的版本仍可能需要依据诊断补适配。

管理员权限的旧式/自定义窗口仍沿用既有复制路径回退；本次没有放宽服务端辅助程序「不发送 Open/Save」的权限约定。

需要真实 CAD 控件证据时，在 CAD 文件窗口打开后运行：

```powershell
./tools/diagnose-file-dialog.ps1 -DelaySeconds 3 -OutputFile ./build/cad-dialog.json
```

倒计时内切回 CAD 文件窗口。脚本只读取窗口类、控件 ID、样式和相对位置，不读取窗口标题、文件名、文本框内容或用户路径，不修改目标窗口。

AutoCAD 适配另有限制：只为 `acad.exe` / `acadlt.exe` 启用上述额外结构识别，避免把其他软件外观相似的设置窗口当成文件窗口。这两个进程名见 [Autodesk 支持文档](https://www.autodesk.com/support/technical/article/caas/sfdcarticles/sfdcarticles/Error-Windows-cannot-find-filename-dwg-when-a-DWG-file-is-double-clicked.html)。`ctest -R tst_filedialog` 在构建目录创建名为 `acad.exe` 的**测试程序副本**以覆盖此条件，另用原名执行否定用例；该副本不包含 AutoCAD。

## Blender

Blender 的独立文件窗口是自绘 `GHOST_WindowClass`，不能套用 Win32 输入框路线。曾验证过按键方案：目录可切换，但 5.2 中地址输入仍持有焦点，连续动作可能拼接路径或触发内部弹窗；该实现已移除，不能作为交付方案。参见 [5.2 文件浏览器源码](https://github.com/blender/blender/blob/blender-v5.2-release/source/blender/editors/space_file/file_ops.cc)。

采用官方扩展机制安装本仓库的 [Win顺文件对话框扩展](../integrations/blender/winshun_file_dialog/README.md)，通过公开 Python API 修改 `FileSelectParams.directory` / `filename`，在 Blender 主线程刷新。打开动作交给 `bpy.ops.file.execute('INVOKE_DEFAULT')`。保存、导出仍由用户在 Blender 确认，不自动覆盖文件。

打开/保存分类读取操作器 RNA 的 `check_existing` **默认值**，不使用可被调用者改写的当前值。Blender 的 [`WM_operator_properties_filesel`](https://github.com/blender/blender/blob/blender-v5.2-release/source/blender/windowmanager/intern/wm_operator_props.cc) 按 `FILE_SAVE` 定义这个默认值；取值位置是 `active_operator.properties.bl_rna`。

独立文件窗口以同进程、原生 owner、文件操作上下文和客户区尺寸唯一对应；扩展发布窗口标记与随机生命周期令牌。Win顺只连接目标 Blender PID 的本地命名管道，并核验管道服务端 PID；权限限本账户与 SYSTEM，拒绝远程连接。后台检查路径并设置请求期限，主线程拒绝过期、失效或已不在前台的操作。停用扩展时移除标记，Win顺不再识别这些窗口。没有模拟按键、剪贴板替换或注入代码。

适用独立打开、另存为、导入窗口；不接管嵌入编辑器或资产浏览器。同尺寸多窗口无法唯一匹配时跳过。超过 255 个 UTF-16 单元的目录仍通过管道跳转，但因 Windows Atom 长度限制不提供当前目录快捷提示。第三方操作器和 Blender 4.2+ 的其他版本尚未全部实测。

## 可复现验证

管道服务还核对客户端进程的完整性级别，禁止低权限客户端驱动高权限 Blender。打开请求先确认接收，再由同一个主线程轮询处理已确认的动作；动作有单独的短期时限，执行前再次核对原窗口、选中项和前台状态。这样冷加载 `.blend` 不会阻塞请求回执，过期或上下文已改变的动作也不会稍后冒出来。

先构建 `WinShun`、`tst_filedialog` 等相关目标，再运行：

```powershell
./tools/verify-blender-dialog.ps1 -Language en_US
./tools/verify-blender-dialog.ps1 -Language zh_HANS
./tools/build-blender-extension.ps1
```

验证脚本只使用已安装的 Blender，创建独立实例、工厂场景和提前创建的私有配置/临时目录；不操作用户已运行的 Blender。原生用例覆盖启停扩展、主窗口拒绝、中文目录和文件名、保留原名、选择后打开，以及另存为必须留在原生窗口确认。完整程序探针使用真实 `App`、`DialogJump`、`DialogBar`、搜索引擎和 QML，输出 `dialog-with-search.png`，由 Blender 独立观察器确认最终目录和文件名，避免只看管道返回值。

通信回归可用 Blender 自带 Python 运行 `tests/blender/test_transport.py -v`，覆盖连续 Unicode 请求、延迟读取及非法帧后继续服务；不需要 Blender 界面或用户文件。

### 2026-10-30 本地结果

- Release 构建成功。相关 CTest 共 8 个通过，含 CAD 进程隔离正/反例、对话框导航、Shell 启动、命令图标，以及 GPU/软件两种搜索栏可见性。Blender、真实 Inno 和文件粘贴实窗用例需要显式夹具；普通回归不冒用用户窗口。
- 真实 Windows 命名管道 3 项回归通过，含 30 次连续中文请求与延迟读取。
- Blender 5.2 英文实例通过扩展启停、中文路径、原名保留、文件选择与打开、另存为不自动确认；完整 `App` 搜索/选择流程也通过。中文实例实际显示深色搜索栏和结果列表，独立观察器确认选择变化，QML 运行告警为 0。快捷键路由沿用现有实现，此轮未把模拟 Ctrl+G 按键当成实测证据。
- ZIP 通过 Blender 官方构建及校验，并在私有配置、离线后台实例中通过官方安装器安装、启用、建立通信端点和干净停用。
- AutoCAD 未安装，尚无实机通过结论；没有关闭 #143 或发布新版本。

本地证据保存在未提交的 `build/issue143`：`ctest-final.txt`、`visibility-final.txt`、`transport-tests.txt`、`acceptance-en`、`final-ui-zh/runtime/runtime.json` 与 `dialog-with-search.png`、`package-verification/stdout.txt`。

单独的 `tst_filedialog` 默认跳过 Blender 和真实 Inno 窗口用例；只有提供隔离实例 PID 和观察器文件才执行。未通过的探索记录保留在忽略目录 `build/issue143`，不计为当前实现的通过证据。
