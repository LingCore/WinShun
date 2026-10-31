# 系统开关 / System switches

Win顺 的可选 C++ 原生插件。在搜索结果中切换深色模式、隐藏文件、文件扩展名、蓝牙、WLAN、静音与音量，以及保持唤醒。Enter 就地操作，Ctrl+Enter 打开 Windows 设置；音量继续支持 Alt+←/→。

在“设置 → 插件”启用“系统开关”。新配置默认关闭；从内置开关升级的配置只迁移一次，保留原来的“搜索系统设置”选择，已有的插件开关优先。停用后结束进程，硬件通知、后台线程和保持唤醒请求一起释放。没有插件时，系统设置搜索仍可打开 Windows 对应页面。

插件以普通用户权限运行，申请 `ui` 和 `system:settings`。前者用于系统通知窗口与硬件接口，后者声明会修改 Windows 用户设置。后者不是 Windows 沙箱保证；它不授予管理员权限。插件不访问剪贴板，也不需要联网。保持唤醒不会跨退出、停用或崩溃恢复。

构建目标为 `WinShunSwitches` 与 `WinShunSwitchesApp`，与主程序一起构建并随包放入 `plugins/winshun.switches/`。小型原生启动器从 `WS_APP_DIR` 加载主程序已有的 Qt，不重复携带 Qt。协议见 [插件文档](https://github.com/LingCore/WinShun/blob/main/docs/plugins.md) 的“系统开关提供者”。

An optional native C++ plugin for theme, Explorer options, Bluetooth/WLAN, default output volume/mute and keep-awake directly in search results. Enable **System switches** in **Settings → Plugins**. Fresh profiles start with it off; existing profiles retain their previous system-search choice once. Disabling it ends its process and releases device listeners, workers and its power request. Windows Settings search still opens the relevant page without it.

The plugin requests `ui` and `system:settings` and runs without administrator rights. System settings access is a declaration, not an OS sandbox guarantee. No network or clipboard access is needed. Keep-awake ends on exit, disable or crash and is never restored automatically. The native launcher shares the host's Qt via `WS_APP_DIR`.
