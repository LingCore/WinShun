# PR #336、#345、#347 的验证记录（2026-11-16）

分支 `prs-1116`（基于 0.32.0 之后的 `e0471df6`），Qt 6.12.0 / MSVC 2022 Release，构建目录 `build/run-latest`。

## 自动测试（只跑相关的）

`ctest -R '^(tst_tools|tst_plugin|tst_pluginsdk)$'`：3 个全部通过。

## #345 小工具（插件这一侧）

用 stdio 直接驱动新构建的 `WinShunTools.exe`（JSON-RPC，一行一条），12 项全过：

| 检查 | 结果 |
|---|---|
| `md5 hello.world`、`md5 a/b`、`md5 1.5` | 按文字算摘要（PR 原版答“没有这个文件”，已改：只有 `C:\…`、`C:/…`、`\服务器\共享\…` 才当文件） |
| `sha256 C:\no\such\file.bin`、不存在的 UNC 路径 | “没有这个文件” |
| 真实文件 / 文件夹 | 摘要正确 / “这是一个文件夹” |
| 文件菜单 `hash-sha256` | `copy` 摘要 |
| 计算记录：清空 → 确认行 → `Shift+Enter` | 记录还在（PR 原版这里也会清空，已改） |
| 确认行 → `Enter` | 清空 |

## #345 文件右键菜单（真实界面）

Win10 虚拟机（22H2），先用真实安装包装 0.32.0，再换上本分支的 `WinShun.exe`、`WinShunPluginHost.exe`（小工具在它里面跑）和 `plugins\winshun.tools\plugin.json`；Hyper-V 合成鼠标右键文件结果 `C:\ws\files\hashme.txt`（内容 `abc`）：

| 设置 | 菜单 | 点击后 |
|---|---|---|
| 默认 | 只有“复制 SHA-256”（PH 指纹图标），见 `menu-sha256.png` | 剪贴板 `ba7816bf…f20015ad`（`abc` 的 SHA-256），底栏“已复制 SHA-256”，见 `copied.png` |
| `menuMd5=true`、`menuSha256=false` | 只有“复制 MD5”，见 `menu-md5.png` | — |

卡片上拨开关后立即生效走的是 `Host::setPluginSettings → Catalog`，由 `tst_plugin filesTriggerSetting` 覆盖，没有在界面里点卡片开关。

## #347 C++ SDK 设置接口

`tst_pluginsdk`：`updateSettings()` 发出的通知、`settingsChanged`、覆写和默认的 `showSettings()` 都回空结果、之后协议照常、未知方法 `-32601`。官方插件里没有用 SDK 声明 `settingsWindow` 的，行为不变。

## #336 WinGet 文档

只改文档。`winget show LingCore.WinShun` 当天是 0.31.1、离线安装包，和文中“比 Releases 晚、装的是离线安装版”一致。
