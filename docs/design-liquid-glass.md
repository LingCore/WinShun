# 液态玻璃主题（#235）

2026-11-07。作为第四个可选风格 `liquid`，默认仍为 `fluent`；不改变现有窗口尺寸和操作流程。

## 设计决定

Win顺是键盘和鼠标驱动的高频桌面工具。玻璃感放在窗口背景；中性卡片、输入表面和文字层级组织内容，强调色标记选中、焦点和主操作。浅深色、圆角设置、界面大小、强调色、系统透明开关和减少动画设置继续生效。设置的四张风格卡片在窄窗口换成两列，保留方向键和 Enter / Space 操作。

- 背景：Windows 11 22H2 以上通过公开 DWM API 使用 Desktop Acrylic，模糊由系统完成；切换风格即时更新已注册背景的窗口，新窗口继承当前材质。其余风格仍用 Mica。
- 控件：共用 `GlassSurface.qml` 绘制单层平面和按需细边，不复制文字、不创建离屏纹理、不采样桌面。主搜索栏不再套输入框；搜索范围、排序和设置页签使用短强调色标记，实际选项组保留分段底座。普通工具按钮仅在交互时显示底色；强调色实心按钮保持白字；独立输入框聚焦时用完整强调色描边，错误用错误色。列表和导航使用中性选中底色与左侧标记，不叠加整圈强调色边框。
- 回退：应用关闭透明、系统禁止材质、软件绘制、较早 Windows 保留实色玻璃表面；菜单始终使用足够不透明的阅读底色。外框、阴影和窗口裁剪仍由 DWM 管理。
- 失焦：液态玻璃保持窗口背景材质；标题栏继续按实际激活状态弱化。沿用现有 Qt 无边框窗口的 `WM_NCACTIVATE` 处理，给已登记的 Acrylic 背景保留视觉激活状态，并在实际失焦时返回 TRUE，允许焦点正常交给其他窗口；不调用抢焦点 API，也不把 Qt 的 `active` 改成 true。其他风格保持原有失焦回退。系统材质与电源策略仍由 DWM 执行，透明关闭和软件绘制仍由 QML 实色覆盖。
- 复用：主题仍从现有 `Theme` / `Hook` 加载，提示键、分隔线和滚动条复用 fluent 部件并注入本主题颜色。资源沿用现有 QML module 的目录收集，不新增 shader 构建依赖。

## 材质参数

开启液态玻璃的透明效果后提供两项 0–100 的相对强度，默认均为 50：背景透明程度与卡片清晰度。前者调整窗口底色遮罩，后者调整卡片和工具栏的阅读底色；中点保留原先浅深色的不同配比。文字、按钮与输入框不做整层透明度处理，菜单继续保持实色。关闭透明或系统禁止材质时使用原有实色回退。

滑动中通过 SystemTheme 预览，松手才由 SettingsEditor 保存；键盘调整立即保存。两项进入 Settings / AI 设置目录和剪贴板显示进程的外观同步，一键恢复默认。使用现有 DWM 原生模糊，不提供该接口没有公开的模糊半径或噪点滑块。

## 与 issue 建议的差异

2026-11-07 视觉修订：初版的蓝灰底色、控件渐变与双层亮边被用户反馈为不好看。本次参考 PowerToys 的 CmdPal 搜索样式、语义列表状态和设置卡片组织，移除这些装饰，采用中性表面与更克制的强调色。此修订替代初版的控件高光方案；原验证截图保留为历史证据。Win顺继续使用现有 Qt/QML 主题架构，外框仍由 DWM 管理。

不接入桌面捕获和窗口内折射。Qt 的 `ShaderEffectSource` 只能渲染给定 QML Item，增加视频内存并通常降低性能；直接采样整个窗口还会把文字和前景一并采进纹理，产生递归依赖或重复绘制问题。当前窗口布局没有可独立提供、值得折射的背景内容层，因此先保证原生模糊和控件可读性，不给每个列表行堆模糊与折射通道。

这是一套玻璃材质主题，不宣称是 Apple Liquid Glass 的桌面像素折射实现。Acrylic 仍消耗 GPU 资源，不能把“交给 DWM”写成“零开销”；不声明未经测量的帧率或续航收益。

## 官方参考

- [DWM_SYSTEMBACKDROP_TYPE](https://learn.microsoft.com/en-us/windows/win32/api/dwmapi/ne-dwmapi-dwm_systembackdrop_type)：`DWMSBT_TRANSIENTWINDOW` 在 Windows 11 对应 Desktop Acrylic，最低 build 22621；使用系统公开材质 API。
- [Acrylic material](https://learn.microsoft.com/en-us/windows/apps/design/style/acrylic)：背景与应用内材质的用途、避免重叠多层背景 Acrylic、GPU 与省电模式代价，以及透明和高对比度回退。
- [Qt ShaderEffectSource](https://doc.qt.io/qt-6/qml-qtquick-shadereffectsource.html)：源为 QML Item、额外纹理内存及性能成本，不能当成桌面采样接口。
- [PowerToys CmdPal 搜索框](https://github.com/microsoft/PowerToys/blob/ba946c9885d189dda9b40e2990c5cbf3b4835bbc/src/modules/cmdpal/Microsoft.CmdPal.UI/Styles/TextBox.xaml)：主搜索输入使用透明背景、透明边框与零边框宽度。
- [PowerToys CmdPal 主题](https://github.com/microsoft/PowerToys/blob/ba946c9885d189dda9b40e2990c5cbf3b4835bbc/src/modules/cmdpal/Microsoft.CmdPal.UI/Styles/Theme.Normal.xaml)：中性材质底色与语义交互颜色。
- [PowerToys AlwaysActiveDesktopAcrylicBackdrop](https://github.com/microsoft/PowerToys/blob/ba946c9885d189dda9b40e2990c5cbf3b4835bbc/src/common/Common.UI.Controls/Backdrops/AlwaysActiveDesktopAcrylicBackdrop.cs)：视觉材质状态与实际输入激活状态分开；该组件供不抢焦点的表面使用。Win顺沿用 Qt/Win32 的现有外框处理，没有复制 WinUI 组件或新增 Windows App SDK 依赖。
- [WM_NCACTIVATE](https://learn.microsoft.com/en-us/windows/win32/winmsg/wm-ncactivate)：非客户区的视觉激活消息；失焦时须返回 TRUE 以允许系统完成焦点切换。

参考项目仅用于理解视觉意图，未复制第三方 shader 或新增其代码依赖。
