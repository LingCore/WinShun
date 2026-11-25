# 自定义文件对话框兼容：#143

此记录区分上游资料、原生协议测试与实际软件验证。AutoCAD 没有安装，也没有为本次验证安装；不把模拟窗口的通过结果写成 AutoCAD 实机通过。

## AutoCAD

[Issue #143](https://github.com/LingCore/WinShun/issues/143) 的截图是自定义文件选择窗口，包含文件列表、文件名组合框和文件类型组合框。Autodesk 的 [CNavDialog 文档](https://help.autodesk.com/cloudhelp/2019/ENU/OARX-RefGuide/files/OREF-CNavDialog.html) 表明它是 `CAdUiDialog` 派生类，不能假设它必定提供标准 Windows 文件对话框的地址栏、控件 ID 或 Shell 视图。

[Listary FAQ](https://help.listary.com/faq) 明确将 AutoCAD 列为特殊适配的软件，但没有公开实现。[Quick Access Popup 作者的变更记录](https://www.quickaccesspopup.com/change-log/) 记载过严格识别条件导致 AutoCAD/Revit 失效后回退的修正；[其公开源码](https://github.com/JnLlnd/QuickAccessPopup/blob/prod/QuickAccessPopup.ahk) 使用文件名输入框切换目录。上述证据支持补充自定义控件路径，不能证明某个固定控件 ID 对所有 CAD 版本有效。

Win顺保留标准识别优先级，增加结构匹配：`#32770`、确定/取消、可见文件列表、列表下方唯一可编辑的文件名组合框，以及其下方唯一不可编辑的文件类型列表。缺少角色或存在歧义就不接管。使用实际组合框 ID 发送 `CBN_EDITCHANGE`，让宿主模型收到输入变化；目录带尾部反斜杠，防止 `foo.dwg` 这样的目录被当成文件打开；跳转后恢复原文件名并再次通知宿主。

验证：`tst_filedialog` 用真实原生组合框、非标准控件 ID 和只在 `CBN_EDITCHANGE` 更新的宿主模型，覆盖普通目录、含扩展名目录、中文空格目录、恢复文件名、明确确认、缺失控件、多个输入框与不存在目录。**尚缺 AutoCAD 实机确认**。结构不同的版本仍可能需要依据诊断补适配。

管理员权限的旧式/自定义窗口：2026-10-30 这一版仍沿用复制路径回退；2026-11-11 起由服务的助手代为跳转，见下文“管理员权限的 CAD 与旧式窗口”。

需要真实 CAD 控件证据时，在 CAD 文件窗口打开后运行：

```powershell
./tools/diagnose-file-dialog.ps1 -DelaySeconds 3 -OutputFile ./build/cad-dialog.json
```

倒计时内切回 CAD 文件窗口。脚本只读取窗口类、控件 ID、样式和相对位置，不读取窗口标题、文件名、文本框内容或用户路径，不修改目标窗口。

AutoCAD 适配另有限制：只为 `acad.exe` / `acadlt.exe` 启用上述额外结构识别，避免把其他软件外观相似的设置窗口当成文件窗口。这两个进程名见 [Autodesk 支持文档](https://www.autodesk.com/support/technical/article/caas/sfdcarticles/sfdcarticles/Error-Windows-cannot-find-filename-dwg-when-a-DWG-file-is-double-clicked.html)。`ctest -R tst_filedialog` 在构建目录创建名为 `acad.exe` 的**测试程序副本**以覆盖此条件，另用原名执行否定用例；该副本不包含 AutoCAD。

## 管理员权限的 CAD 与旧式窗口（2026-11-11）

[#143 的后续反馈](https://github.com/LingCore/WinShun/issues/143#issuecomment-6681348712)：装了某些 CAD 插件后 AutoCAD 以管理员身份运行，这时 Ctrl+G、切回自动跳转都不动，把 Win顺 也用管理员身份运行才行。

原因：管理员窗口的跳转由索引服务的助手代做（`core/DialogNav`，[#104](https://github.com/LingCore/WinShun/issues/104)）。它原先只认有地址栏的对话框、Inno 选择器和“浏览文件夹”；没有地址栏的旧式对话框（`Kind::Legacy`）和 AutoCAD 的窗口一律返回“没跳成”，因为那时约定助手从不发确定。界面端（普通权限的 AutoCAD）早就用“文件名框填文件夹 + 确定”跳转，报告者确认过可用。

改动：助手也认这两类窗口并用同一办法跳转，识别条件与界面端 `platform/FileDialog.cpp` 保持一致（AutoCAD 仍只认 `acad.exe` / `acadlt.exe`，角色不全或有歧义就不认）。安全上的收口：

- 填进文件名框的只会是以 `\` 结尾的文件夹路径；发确定之前再查一次它确实是目录，并且框里确实是刚填的这一串。
- 确定只发一次；原来框里的文件名（长度不超过编辑框上限时）事后放回，放不回的不动手。
- 确定按钮是灰的不发；发完窗口没了按“没跳成”上报。
- 服务端对请求方和文件夹字符串的校验、助手对会话和完整性级别的校验都没有变。

验证：

- `tst_dialognav`：旧式窗口三种文件夹名（普通、形似文件名、中文加空格）跳转成功、文件名放回、未被当成回答；文件夹不存在、确定是灰的、没有文件名框都不动手；把确定当回答的窗口上报失败。`ctest -R tst_dialognav_cad` 用改名为 `acad.exe` 的测试副本覆盖 AutoCAD 替身窗口的正例和四个歧义反例，原名运行时同一窗口不被识别。
- 实测（`docs/validation/issue143-20261111`）：管理员权限的真实旧式“打开”对话框（WinForms 关掉 `AutoUpgradeEnabled`，即带钩子的 `GetOpenFileName`）+ 隔离的 SYSTEM 测试服务，普通权限的 `tst_servicedialognav` 发请求。不存在的文件夹被拒且对话框原样；目标文件夹（名字形似文件、含中文和空格）跳转成功，“查找范围”从 `ws143-from` 变为目标文件夹，文件名框仍是 `keep me.txt`，对话框没有被关闭。
- **仍缺管理员权限下的 AutoCAD 实机确认**：本机没有 AutoCAD，AutoCAD 这一路只有替身窗口的结果。

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

单独的 `tst_filedialog` 默认跳过 Blender 和真实 Inno 窗口用例，仅在提供隔离实例 PID 和观察器文件时执行。

## 程序自己画的选文件窗口：用户规则（#272，2026-11-12）

[Issue #272](https://github.com/LingCore/WinShun/issues/272) 要的是 ArcMap、ArcGIS Pro 的“添加数据”窗口，并希望做成用户能自己适配别的窗口的办法。截图表明 ArcMap 的是带自绘内容视图的 `#32770`（不是系统文件对话框，没有地址栏和标准控件 ID），ArcGIS Pro 的是 WPF 窗口（类名 `HwndWrapper[ArcGISPro;;<每次不同>]`，没有子窗口）。**本机没有 ArcGIS，也没有为这次验证安装；下面的通过结果都来自替身窗口，不是 ArcGIS 实机结果。**

没有为 ArcGIS 写死适配，做的是通用规则（`platform/DialogRules`、`DialogRuleStore`）：

- 规则写在数据文件夹的 `dialogs.json`：哪个程序（`program`）、哪个窗口（`class`、`title`，`*` 通配）、路径填进哪个输入框（`box`）、怎么确认（`confirm`：输入框里回车，或一个按钮）。输入框和按钮按 UI Automation 的控件类型、AutomationId、名称、类名来指，所以同一套规则适用于 Win32、WPF、WinForms、Qt 窗口。
- 设置里的“识别窗口”倒数五秒后看前台窗口，自动写出一条规则：输入框取有键盘焦点的那个（它能输入文字时），否则取位置最靠下的可输入框；窗口类名里每次运行都变的部分换成 `*`。看到的所有输入框和按钮记在规则的 `seen` 里，供认错时手工改。
- 驱动方式和旧式对话框一样：文件夹带尾部反斜杠填进输入框并确认（只可能被当成文件夹）；文件填整条路径，要“直接打开”时才确认。回车是发给刚拿到焦点的输入框（有自己窗口句柄时）或顶层窗口（WPF、Qt）的窗口消息，不合成按键，不往目标进程里装东西。
- 规则只对 Win顺 本来不认识的窗口生效；文件一保存就重新读，当前在前台的窗口立刻重新判断。管理员权限的程序不支持（服务的助手不认规则，退回复制路径）。

前提是那个窗口本身支持“在文件名框里填一条路径、回车就过去”。ArcGIS 的两个窗口是否如此、识别出的输入框对不对，要等报告者实测。

验证：

- `tst_dialogrules`：规则文件的读写和报错；通配；只认规则指明的程序、类名、标题；原生替身窗口（独立线程，编辑框 + 默认按钮）分别用回车和按钮确认，覆盖形似文件名的中文文件夹、不存在的文件夹、窗口不清空输入框时放回原文字、文件整条路径、明确确认；找不到输入框时不动手；识别出的规则能直接驱动窗口；另一个进程里的 WPF 替身窗口（PowerShell，`tests/fixtures/wpf-file-window.ps1`，两个输入框）识别到下方的名称框并跳转成功。
- 真实主程序的离屏检查 `WinShun --preview-settings <目录> --dialog-rule-settings --fixture tests\fixtures\wpf-file-window.ps1`（中、英文各跑通过，20 项）：设置行和两个按钮在行内；点“识别窗口”倒数、不能重复点；切到替身窗口后写出规则；真实的 `DialogJump` 在没有规则时不跟随、规则写入后立刻跟随仍在前台的窗口；`DialogJump::go` 把文件夹和文件送进窗口；在文件里删掉规则后立刻不再跟随。
- 没有覆盖：搜索框在这类窗口旁边的实际摆放（走的是与其他对话框相同的代码）、ArcGIS 实机。

## 自带路径框的选文件夹窗口：Bandizip、7-Zip、WinRAR（2026-11-13）

用户反馈 Bandizip 的“选择解压路径”窗口下面没有搜索框。这类窗口不是系统对话框：没有地址栏，文件夹写在窗口自己的一个输入框里，按它的“确定”时程序读这个框。此前只认了同类里的 Inno Setup 选目录窗口。

先查了别的项目怎么做，再动手：

| 项目 | 做法 | 对我们有用的结论 |
|---|---|---|
| [Listary 的 WinRAR 插件](https://github.com/listary/Listary.FileAppPlugin.WinRAR)（开源） | `#32770` + 进程 `WinRAR.exe` + 标签页控件 `0x3020`；路径组合框 `0x65`（在第一个子页上），写入后点“显示”按钮 `0x66`，不按确定 | WinRAR 的控件 ID 与驱动办法照此实现 |
| Listary 本体（闭源） | [官方页面](https://www.listary.com/feature/quick-save-and-open)点名支持 Bandizip，实现未公开 | 只说明值得做 |
| [JoyHak/QuickSwitch](https://github.com/JoyHak/QuickSwitch)（[#43](https://github.com/JoyHak/QuickSwitch/issues/43)） | 按结构认“按钮 + 输入框 + 树”，写入后回车；回车在 Bandizip 上等于确定，窗口直接关闭 | 结构识别可行；**不能发回车** |
| [Quick Access Popup](https://www.quickaccesspopup.com/can-i-change-folders-in-file-dialog-boxes-open-save-as-etc/) | 规则最宽（任意带输入框的 `#32770`），因误改设置窗口而默认关闭 | 识别条件要收紧 |
| Flow Launcher、gepruts/QuickSwitch、PathWrap | 只支持系统对话框 | 无 |
| [7-Zip 源码](https://github.com/ip7z/7zip/blob/main/CPP/7zip/UI/GUI/ExtractDialog.cpp) | 解压窗口：路径组合框 `100`、“...”按钮 `101`、两个下拉列表 `102`/`103`；确定时**先读列表选中项，没有选中项才读文字**；“...”按钮自己的做法是先 `SetCurSel(-1)` 再写文字 | 写入前必须取消列表选中项 |

没有一个项目靠遍历文件夹树（`TVM_*`）来跳转。

实现（`core/PathBoxDialog`，界面端 `platform/FileDialog` 和管理员窗口的助手 `core/DialogNav` 共用，Inno 的识别也并了进来）：

- **Bandizip 这一类按结构认，不限程序**：`#32770`，有确定和取消，直接子控件里恰好一棵可见的文件夹树、恰好一个可输入的框（组合框或单行输入框），并且框里当前是一条绝对路径。出现列表、标签页、第二个输入框、第二棵树、只读/密码/数字框，或树是“浏览文件夹”自己的那棵（它另有处理），都不认。界面端认过一次的窗口在它关闭前一直算数，用户在框里手改路径时搜索框不会跟着消失；助手不记，框里不是路径就不动。
- **7-Zip 和 WinRAR 按控件 ID 认，只在各自的程序里生效**（`7zG.exe`／`7zFM.exe`、`WinRAR.exe`）。
- **跳转 = 写框**：取消组合框的列表选中项，文字写进框并读回核对，再把“用户改了文字”（`CBN_EDITCHANGE`）告诉窗口；WinRAR 再按一下它自己的“显示”。**不发回车、不按确定**，解压由用户自己点。
- 通知是投递的，不等对方处理完（见下面的实测）。

Bandizip 7.36 实测（本机已装；7-Zip、WinRAR 本机没有，也没有为此安装）：

- 窗口结构：路径组合框 ID 1021、树 ID 1024、确定、取消，另有换肤的位置栏（不是标准控件）。
- 只 `WM_SETTEXT` 时树不动；收到 `CBN_EDITCHANGE` 后树跟到该文件夹；确定读的是框里的文字。
- 树跟过去的耗时取决于沿途文件夹的大小：普通文件夹 10 毫秒级，`System32\drivers` 0.7 秒，有 1700 项的 `Temp` 下面的文件夹 5 秒以上，这期间 Bandizip 不回应任何消息。用户自己在框里打字也一样，所以通知改为投递，Win顺 不陪着等。

验证：

- `tst_pathbox`（24 项）：路径形状判断；替身窗口（真实控件，行为按上面的实测和源码）覆盖组合框与单行输入框、形似文件名的文件夹、中文加空格、共享路径、列表选中项被取消、没有被确认；十几种不认的情形。`ctest -R tst_pathbox_7zip` 和 `tst_pathbox_winrar` 用改名为 `7zG.exe`、`WinRAR.exe` 的测试副本覆盖两个替身窗口（7-Zip 替身的确定按源码先读选中项，写入后它的回答就是写入的文件夹；WinRAR 替身收到“显示”时框里已是新文件夹，第一页被隐藏时也可以），原名运行时同样的窗口不被识别。副本里没有这两个软件的任何东西。
- `tst_pathbox` 的 `realBandizip`（设 `WINSHUN_TEST_BANDIZIP=<Bandizip.exe>` 才跑）：启动真实 Bandizip 解压这里现做的压缩包，窗口被认出，写入中文加空格的文件夹，树的选中项随之改变，窗口还开着且什么都没解压；再按它的确定，文件出现在写入的文件夹里。
- `tst_filedialog`、`tst_dialognav` 各加一项：界面端识别为选文件夹窗口、当前位置取自框里、跳转、不给文件名也不确认、认过之后保持；助手对不存在的文件夹和相对路径不动手。
- 真实主程序：`./tools/verify-bandizip-dialog.ps1 -BuildDir build/dialog-pathbox -OutputDir build/dialog-pathbox/bandizip`。真实 `App`、`DialogJump`、`DialogBar` 和搜索引擎对着真实 Bandizip 窗口：搜索框出现在窗口下方并拿到焦点，搜到文件夹，选中后窗口的框里是该文件夹，窗口没有被关闭；随后取消窗口，确认什么都没解压。2026-11-13 通过，QML 运行告警 0。
- **没有实机结果的部分**：7-Zip 和 WinRAR 只有替身窗口的结果（依据分别是 7-Zip 源码和 Listary 插件源码）；管理员权限下的 Bandizip 没有实测（助手一侧只有替身窗口）。这两个软件的“复制到/移动到”等其他窗口没有处理。

## Tessoa 自己的打开、另存为窗口（2026-11-24）

用户反馈：文件管理器用 Tessoa，WPS、CAD 另存为能自动转到 Tessoa 当前的文件夹；微信另存为弹出的却是 Tessoa 自己的窗口（下面有“改用系统窗口”），停在上次保存的文件夹，点“改用系统窗口”后才转过去。

原因在 Tessoa 0.31 起的实验功能“接管打开 / 另存为窗口”（Tessoa 设置的安装页，只有安装版有）。打开后它在 HKCU 下把系统的 FileSaveDialog、FileOpenDialog 换成自己的 DLL（`HKCU\Software\Classes\CLSID\{C0B4E2F3-BA21-4773-8DBA-335EC946EB8B}` 与 `{DC1C5A9C-E88A-4DDE-A5A1-60F82A20AEF7}` 的 `InprocServer32` 指向 `%LOCALAPPDATA%\tessoa-file-dialog\tessoa-dialog-x64-*.dll`；32 位程序的在 `WOW6432Node\CLSID` 下，指向 x86 版）。用这两个系统接口的程序（微信等）弹出的就是 Tessoa 的窗口；WPS、CAD 用自己的窗口，没被接管，仍是 Win顺 认识的窗口。它另有“不接管的程序”名单和窗口里的“改用系统窗口”按钮。

Tessoa 0.32.0 实测（本机临时装上并打开接管，测完关掉、核对注册表已还原）：

- 窗口属于 tessoa.exe，类名 `Window Class`（winit），标题是调用方给的对话框标题，owner 是调用方的窗口（跨进程）。全部自绘：没有子控件，也没有 UI Automation。
- Tessoa 给其他程序的[接口](https://github.com/Tessoa/Listary.FileAppPlugin.Tessoa/blob/main/docs/launcher-integration.zh-CN.md)不包括这个窗口：查询管道 `query-cwd` 对它回空，`query-tabs` 只回 `api 2`；`--open-window=<文件夹> --in-tab=<这个窗口>` 退出码为 0，文件夹却开在 Tessoa 主窗口的新标签里，这个窗口不动。
- 它的文件名框不接受文件夹（提示“文件名与现有文件夹重名”），接受文件的完整路径；它的地址栏（点路径条，输入，回车）能转到文件夹。

所以这个窗口 Win顺 转不过去，是 Tessoa 一侧的限制。Win顺 的做法（`filedialog::isTessoaPicker`：tessoa.exe 的顶层窗口，owner 在别的进程；不属于任何 `kind`）：

- 不注册 `Ctrl+G`（按键留给 Tessoa），不自动转，管理员窗口的助手也不试。
- 窗口下的搜索框照常出现，只复制路径。提示语：“Tessoa 的窗口不能自动跳转：搜索后复制路径，或点‘改用系统窗口’”；复制后改为“路径已复制：文件夹粘贴到 Tessoa 的地址栏，文件粘贴到文件名框”。
- 每次运行、每个调用程序记一行日志（开了 `Ctrl+G` 或自动跳转时为警告级，默认就写进 `WinShun.log`）和一条诊断记录 `tessoa-picker`（调用程序、是否开了跳转、搜索框是否显示）。
- 开了 `Ctrl+G` 或自动跳转，而这个窗口下又没有搜索框（关了搜索框，或把 tessoa.exe 加进了“不在这些程序的对话框下显示”）时：窗口在前台 1 秒后发一次通知，说明原因和办法，以后不再发（`Launcher/TessoaPickerHinted`）。

用户的办法：在 Tessoa 的窗口里点“改用系统窗口”；或在 Tessoa 设置的安装页把这个程序（新版微信 `Weixin.exe`，旧版 `WeChat.exe`）加入“不接管的程序”；或关掉 Tessoa 的接管。之后弹出的是系统窗口，`Ctrl+G` 和自动跳转照常可用。

验证：

- `tst_filedialog` 的 `tessoaPicker`：测试副本改名 tessoa.exe，建一个隐藏窗口，owner 是测试进程的窗口：认出，`kind` 为 None，`goTo`、`setFileName` 不动手；没有 owner、owner 在 tessoa.exe 自己进程里、换成别的程序名、本进程里的同类窗口、空句柄，都不认。
- 真实主程序（隔离 `--profile`，`Ctrl+G` 与自动跳转都开）对真实 Tessoa 0.32.0 窗口（WinForms 另存为被接管后弹出），2026-11-24：
  - 搜索框开着：窗口在前台时搜索框出现在它正下方，显示 Tessoa 的提示语，窗口没被移动或改尺寸；此时 `Ctrl+G` 仍可被别的程序注册（Win顺 没占）；`WinShun.log` 默认就有一行警告，诊断记录 `tessoa-picker` 为 `bar: true, following: true`。
  - 搜索框关着：同样的日志与记录（`bar: false`），1 秒后系统通知“Tessoa 的窗口不能自动跳转”，正文带上调用程序名，完整显示（通知正文只有约四行，原先较长的一版被截断，已压短），`TessoaPickerHinted` 写为 true。
  - 没实测：选中结果后的“路径已复制”提示（复制会进 Windows 剪贴板历史；只复制的逻辑与管理员窗口共用）；没有托盘图标时不发通知、不记为已提示（按代码）。

## WPS 自己的打开、另存为窗口（2026-11-24 加固）

WPS 文字、表格、演示和 PDF（`wps.exe`、`et.exe`、`wpp.exe`、`wpspdf.exe`、`wpsoffice.exe`）的打开、另存为窗口是 WPS 用 Qt 自己画的（`KcfdFileDialog`），没有系统对话框的地址栏和控件 ID，只能通过 UI Automation 认（`filedialog::kingsoft`）。地址栏平时是一串路径按钮，点它后面的空白（`KcfdLocSpaceButton`）才换成输入框（`KcfdLocNavigationLineEdit`），在里面回车就过去，和资源管理器一样。Win顺 的跳转就是这样做的：把文件夹写进这个输入框，确认焦点在它上面才投回车，以路径按钮重新出现为跳转完成；不读 WPS 的当前文件夹和文件类型。

这次加固（3a1e3e7c）解决了三种“有时自动跳、有时不跳”的情况：

- **第一次打开不跳，之后这个窗口一直不跳**：Qt 的无障碍要等第一次被问到才打开，在那之前窗口只报 `Qt5QWindowIcon` 这类 `Qt*` 类名或空类名。原先等 500 毫秒还没认出就记成“不是选文件窗口”，这个窗口从此不再被认。现在冷启动最多等 1 秒；还没认出就不记结论，500 毫秒后在它仍在前台时再认一次（`DialogJump::probeKingsoft`）。
- **关掉再打开，第二次不跳**：Qt 的对话框关闭时常常只是隐藏，下次打开还是同一个窗口句柄，而 Win顺 记着“这个窗口已经跳过了”。现在窗口隐藏时（`EVENT_OBJECT_HIDE`）就忘掉它，已跳过、刚从文件管理器回来、已发过的路径一并清掉，再打开按新窗口处理。
- **偶尔不跳，或差点把文件名框里的名字直接保存**：刚打开时 `SetFocus` 要过一会儿才在 UI Automation 里生效，焦点也可能还在落向文件名框；原先设焦后马上判断，判断落空就放弃。回车总是发给当时有焦点的控件，落在文件名框就等于保存。现在设焦后最多等 250 毫秒，没落上就在窗口仍在前台时再设一次、再等 250 毫秒；写入前后都核对焦点在地址输入框、窗口仍在前台，才投回车。

没变的约束：

- 以管理员身份运行的 WPS 对普通权限的 UI Automation 不可见（UIPI），需要 Win顺 同为管理员。
- WPS 转不到的文件夹会弹它自己的“输入位置有误”，所以跳之前先确认目标确实是存在的文件夹。

验证：`tst_filedialog` 里没有 WPS 替身窗口（它的识别要真实的 Qt 无障碍树）；这次加固没有留下真实 WPS 的测试记录。本机装有 WPS 12.1，日后改这段时用它实测三种情况：冷启动后第一次打开、关掉再开、刚打开就跳。

## XYplorer 的当前文件夹（2026-11-24 加固）

文件管理器选 XYplorer 时，对话框跳到它当前的文件夹，搜索框的建议里也列出它打开的标签页（`filemanager::Kind::XYplorer`，#23）。正常途径是它的脚本接口：用 `WM_COPYDATA` 发一段脚本，问当前和另一个窗格的标签页（`pathreal()` 把“文档”这类特殊位置换成真实路径），它用 `WM_COPYDATA` 回答。

这次加固（3a1e3e7c）改了这几处：

- **认不出 XYplorer**：原先只认 `XYplorer.exe`，找它时还要求运行库的隐藏窗口 `ThunderRT6Main` 标题正好是“XYplorer”。现在认 `XYplorer*.exe`（Free 版 `XYplorerFree.exe`、`XYplorer64.exe`、便携版改名的 `XYplorerPortable.exe`），排除 `XYcopy.exe` 和 `XY64*.exe` 这类辅助程序。32 位（VB6，27.x）和 64 位（twinBASIC，28.x）的主窗口都是 `ThunderRT6FormDC`，64 位的没有 owner；XYplorer 自己的对话框 owner 是主窗口，不算主窗口。
- **打开对话框时偶尔读错或读空**：搜索框（`DialogBar`）和自动跳转（`DialogJump`）在两个工作线程里同时问，而 XYplorer 的脚本引擎是单线程的，会被重入。现在 `askXYplorer` 加锁，一次只问一个。
- **64 位下偶尔没有回答**：回信地址是 Win顺 窗口的句柄，64 位里句柄按有符号数扩展，转成十进制会超出它能接的长度。现在用 `HandleToULong` 截成 32 位再写。
- **长路径和网络路径被截掉**：标签页路径里的 `?` 表示快速搜索（`C:\Windows?*.exe`），原先先按 `?` 截断，`\\?\D:\…`、`\\?\UNC\…` 这类前缀会被整条截没。现在先去掉前缀，再截 `?`。
- **脚本不回答时读不到**：Free 版没有脚本接口；XYplorer 开着弹窗时脚本回空；XYplorer 以管理员身份运行时 UIPI 同时挡住 `WM_COPYDATA` 和 `WM_GETTEXT`。现在依次退回：
  1. 读可见的主地址栏（`ThunderRT6FormDC` › `ThunderRT6PictureBoxDC` › `Edit`）。取最宽的那个，不会误读右边的实时筛选框和隐藏的框；地址栏里只有“桌面”“文档”这类特殊位置名时，换成它的真实文件夹。
  2. 读主窗口标题（`GetWindowTextW` 读的是系统保存的标题，不发跨进程消息，管理员身份的 XYplorer 也能读）。默认格式是 `<路径> - XYplorer 28.50 - (Academic)`，取最后一个 ` - XYplorer` 之前的部分，路径里含“ - XYplorer”的文件夹也不会读错；把程序名放在前面的格式 `XYplorer 28.50 - <路径>` 也认。“此电脑”这类虚拟位置读不出路径，就不给。

这两种退路只拿得到当前窗格的一个文件夹，没有另一个窗格和后台标签页。

验证：

- `tst_core`：XYplorer 标签页回答里的 `\\?\`、`\\?\UNC\` 和快速搜索；地址栏文字和窗口标题两种格式（版本号、Free 版、路径含“ - ”或“ - XYplorer”、网络路径、虚拟位置、试用版标题）。
- `tst_shelllaunch` 的 `xyplorerDiscoveryAndFallbacks`：另一个进程里的替身 XYplorer（改名为 `XYplorerFree.exe`，`ThunderRT6Main` 标题不是默认的，主窗口加一个属于它的对话框）依次覆盖三条路：脚本回答当前、另一个窗格和后台标签页，两个线程同时问时回答不串；脚本不答时读宽地址栏，不读较窄的筛选框和隐藏框，“Documents”换成真实的文档文件夹；地址栏也隐藏时读窗口标题。属于主窗口的对话框不被当成主窗口。0.45.0 发布前单独跑过，通过。
- 没有覆盖：真实 XYplorer（本机装有 32 位版）、管理员身份的 XYplorer。
