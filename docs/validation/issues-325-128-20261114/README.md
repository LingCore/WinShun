# #325 界面字体、#128 图片右键贴图的验证记录（2026-11-14）

分支 `issues-1114`（基于 `fcf0acf4`），Qt 6.12.0 / MSVC 2022 Release，构建目录 `build/issues-1114`。实测在 Win10 虚拟机（22H2，19045，中文）里做，装了霞鹜文楷 v1.530 的 Regular 和 Light。

## #325 换成“LXGW WenKai”重启后没变

**原因。** “这个字体装没装”原来是查 `QFontDatabase::families()` 里有没有这个名字。一个字体有中英文两个名字，这份名单里只有其中一个，有时一个都没有，随字体引擎和已经画过什么字而变。在虚拟机里用一个小程序（只有 QtGui）问过：

| 引擎 | `families()` 里有的 | 直接要 “LXGW WenKai” |
|---|---|---|
| FreeType（显卡绘制用） | 只有“霞鹜文楷 Light” | 画出来是 LXGW WenKai Regular |
| GDI（软件绘制用） | “霞鹜文楷”“霞鹜文楷 Light” | 画出来是 LXGW WenKai |

也就是 Qt 认得这个名字、也画得出来，只是名单里没有，Win顺 就当它没装，退回了默认字体。

**改法。** 字体名单和“存的名字对应哪个字体”都直接问 Windows（`EnumFontFamiliesEx`），两种引擎下一样；设置字体时用 Windows 列出来的那个名字。

**结果。**

- 改前（0.28.0 的代码，`UiFont=LXGW WenKai`）：默认的阿里巴巴普惠体，`325-before.png`（这张是软件绘制下拍的）。
- 改后（同一个设置，`Renderer=d3d11`，FreeType）：界面是霞鹜文楷，`325-after.png`。
- `tst_uifont` 在 FreeType 和 GDI 下都通过，新增 `eitherNameOfAFont`：“微软雅黑”和 “Microsoft YaHei” 两个名字得到同一个字体，画出来的不是后备字体；名单里没有竖排（`@`）字体、没有重复。

没测：报告人机器上的原样（真显卡 + 字体由系统安装器安装）；改后软件绘制下的截图（只有单测）。字体选择框本身没改。

## #128 资源管理器里右键图片 → 贴到屏幕

截图插件开着时，图片文件的右键菜单多一项“贴到屏幕（Win顺）”（Windows 11 在“显示更多选项”里）。

- 注册表：`HKCU\Software\Classes\SystemFileAssociations\image\shell\WinShun.PinPicture`，`MultiSelectModel=Document`，命令 `"…\WinShunSearch.exe" --pin-picture "%1"`。
- 菜单：`128-menu.png`（PNG 文件上，带 Win顺 图标）。
- 运行这条命令后图片贴在屏幕中间：`128-pinned.png`。
- `tst_pastefile` 通过（菜单的中英文标题）。

没测：真的点菜单项（测的是菜单项显示出来，以及它要运行的那条命令）；一次选多张；Windows 11；关掉截图插件后菜单项消失（走的是和“粘贴为文件”同一段删除代码）。
