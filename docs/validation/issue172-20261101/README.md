# #172：收藏圆盘普通权限辅助进程未启动

2026-11-01，Windows 11 x64，Qt 6.12.0。

## 已核对的生产路径

- `App::applyDropPie()` 仅在开启收藏圆盘且主程序以管理员权限运行时创建 `DropPieHelper`；普通权限模式在主进程内创建圆盘。
- `DropPieHelper::update()` 写入独立临时配置，再调用 `shell::startUnelevated()` 启动同一 exe 的 `--drop-pie-helper` 模式。
- 主进程已提权时，`startUnelevated()` 从桌面 shell 取普通权限令牌，经 `CreateProcessWithTokenW` 启动。当前接口只返回句柄，没有向调用者返回失败阶段/错误码。
- `poll()` 把无进程句柄、进程已退出、5 秒内没有 ready 文件统一显示成“请确认资源管理器正在运行”。因此这句提示不能直接证明资源管理器没有运行。
- 子进程还可能因仍处于提权状态、无法打开父进程、QML 加载失败、OLE 放下目标或输入监听器未就绪而退出。尚无证据确认报告者落在哪个分支。

## 本机检查及边界

复用隔离构建 `build/search-improvements-20261101/bin/WinShun.exe`，未覆盖用户程序或配置，未停止运行中的安装版。

```powershell
WinShun.exe --preview-results build/issue172-probe --drop-pie-check --renderer d3d11
```

当前测试进程不是管理员。真实辅助进程的 `ready=true`、`running=true`、`elevated=false`，可读到初始两个收藏夹；关闭时 `stopped=true`。这仅证明本机普通权限创建路径能启动，**未验证报告者的管理员降权路径**。

完整既有回归返回失败，原因是后续配置/收藏刷新出现“无法创建圆盘的临时配置”，`updatedFavorites=false`。这与 #172 的“无法启动普通权限的圆盘进程”是不同阶段，不能据此宣称复现了用户问题，也不能把整项回归记作通过。保留 [状态](drop-pie-helper.json) 和 [日志](preview.log)，后续可单独定位该更新失败。

## 已请求的信息

[已在 Issue 回复](https://github.com/LingCore/WinShun/issues/172#issuecomment-6557984363)，请求：

- Win顺 版本、安装版/便携版、Windows 版本。
- 开机自启、手动打开、右键管理员启动、兼容性强制管理员，以及是否使用另一账户的管理员凭据。
- 复现后、退出程序前导出的 `WinShun-diagnostics.zip`。

本次未修改启动策略，未宣称修复，未关闭 Issue。等待报告者信息后再确定失败分支，避免凭通用提示猜测原因。
