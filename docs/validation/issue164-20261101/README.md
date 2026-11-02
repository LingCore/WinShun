# #164：中文标注与录屏启动失败

2026-11-01，Windows 11 x64，Qt 6.12.0，MSVC 2022，Release。

## 已确认的边界

1. `src/capture-plugin/package/plugin.json` 缺少 `processes`。实际 Host/runner 根据清单创建的作业 `ActiveProcessLimit=1`，子进程探针 `CreateProcess` 返回 1816，与报告中的录屏工作进程启动错误一致。生产 `RecordingWorker.cpp` 使用 QProcess 再起独立工作进程，位于此限制之内。
2. runner 对所有插件无条件设置 `EXTENSION_POINT_DISABLE_ALWAYS_ON`。实际 UI 插件探针也报告 `extensionPointsDisabled=true`。[微软文档](https://learn.microsoft.com/en-us/windows/win32/api/processthreadsapi/nf-processthreadsapi-updateprocthreadattribute) 说明该策略阻止传统 IME；这是确定的兼容性问题，但用户尚未提供输入法名称，不能据此证明其中文输入故障只有这一个原因。

## 改动

- UI 插件不再被宿主强制禁用传统扩展点。runner 本身和无 UI 插件保持原策略，其他映像、内存与作业限制保持。
- 截图插件声明 `processes`，包版本更新为 1.0.1；沿用已有新增权限确认流程。单独更新插件不能修复旧宿主的输入法启动策略，两部分需一起交付。
- 增加读取实际截图清单并走真实 Host/runner 的回归；补充插件权限文档和踩坑记录。

## 验证

- [修改前日志](red-tests.txt)：两项新增测试都失败；实际清单对应 `jobActiveProcessLimit=1`、`spawnError=1816`，UI 插件扩展点被禁用。
- [修改后聚焦测试](green-tests.txt)：两项新测试和既有无 UI 权限限制测试均通过。验证子进程仍在作业中、退出清理和内存限制继续生效，UI 权限本身不授予起子进程的权限。
- [完整插件测试](plugin-full.txt)：147 通过、0 失败、1 跳过（需要提权的令牌验证）。
- [构建日志](build.log)：`WinShunPluginHost`、`plugin_fake`、`tst_plugin` 编译通过。
- `git diff --check` 通过。

复用 `build/search-improvements-20261101/` 隔离源码和构建目录，避免干扰正在开发的窗口改动及用户运行中的 Win顺；本次测试的 runner、清单和测试文件与工作区对应文件相同。没有运行桌面录屏或向用户窗口发送按键；探针只验证子进程创建边界，没有验证视频编码输出。中文验证也未覆盖报告者的真实输入法。

## 已索取信息

**2026-11-02 更新**：报告者已提供诊断包和三种输入法版本；以下“等待用户反馈”为当日历史状态。已补充真实微信输入法的旧 runner 失败 / 修复 runner 中文提交对照，见 [新验证记录](../issue164-20261102/README.md)。修复仍未发布，且未验证报告者全部输入法版本。

已在 [Issue 回复](https://github.com/LingCore/WinShun/issues/164#issuecomment-6557843153) 向 @Eryue9527 请求复现后的 `WinShun-diagnostics.zip`、输入法名称/版本、Windows 版本，并请 @ra9fael 补充输入法。等待用户反馈；目前未发布、未关闭 Issue。

复现测试（Qt bin 在 PATH）：

```powershell
cmake --build build/search-improvements-20261101/bin --target tst_plugin
build/search-improvements-20261101/bin/tst_plugin.exe uiPluginKeepsInputMethodExtensions captureManifestAllowsRecordingWorker jobLimitsUnelevatedToo -o build/issue164-green-tests.txt,txt
build/search-improvements-20261101/bin/tst_plugin.exe -o build/issue164-plugin-full.txt,txt
```
