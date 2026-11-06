#pragma once

// How a plugin process is started and kept in bounds (the plugin design,
// §3; measured in stage 0, docs/pitfalls.md "插件系统"): a restricted token
// for the strict sandbox, a job object with the limits the manifest's
// permissions leave, the user's environment plus WS_*, the three standard
// handles, and the facts a process can report about itself (what the
// tests and `hello` read out). No Qt: used by the runner
// (WinShunPluginHost.exe) and the test plugin (plugin_fake.exe).

#include "Json.h"
#include "Lite.h"

#include <windows.h>

#include <string>
#include <utility>
#include <vector>

namespace ws::plugin::sandbox {

// What the job allows a plugin, from the manifest's permissions.
struct JobLimits {
    unsigned memoryMB = 512; // JOB_OBJECT_LIMIT_PROCESS_MEMORY
    bool processes = false; // may start processes (else ActiveProcessLimit = 1)
    bool clipboardRead = false;
    bool clipboardWrite = false;
    bool ui = false; // may own windows and see other programs' (else UILIMIT_DESKTOP | UILIMIT_HANDLES)
};

// Our own token without Administrators and privileges, at
// SECURITY_MANDATORY_MEDIUM_RID or SECURITY_MANDATORY_LOW_RID. Invalid with
// `error` set when it cannot be made.
lite::Handle restrictedToken(DWORD integrity, DWORD* error);

// A job with KILL_ON_JOB_CLOSE and `limits`; or, with `limited` false, the
// runner's own: ending with the job and nothing else (it must start plugins).
lite::Handle makeJob(bool limited, const JobLimits& limits, DWORD* error);

// The user's environment from `token` (CreateEnvironmentBlock: the registry,
// never this process's variables), with `extra` added (replacing any of the
// same name), as the block CreateProcess wants.
std::vector<wchar_t> environmentFor(HANDLE token, const std::vector<std::pair<std::wstring, std::wstring>>& extra);
// Applied to the child's final block only; preserves its PATH and overrides Qt discovery.
std::vector<wchar_t> withHostQt(const std::vector<wchar_t>& environment, const std::wstring& folder);

// For a process running as administrator (an `elevated` plugin, started by
// the elevated runner): never the user's variables (HKCU\Environment and
// this process's are the user's to change, and a variable can make a
// program load code: COR_PROFILER_PATH, DOTNET_STARTUP_HOOKS, NODE_OPTIONS,
// PATH...). The system's variables only (CreateEnvironmentBlock without a
// token: HKLM), less any that loads code, with the system PATH (only its
// folders ordinary users cannot write to: protectedPath); the user's folders
// (USERPROFILE, APPDATA, LOCALAPPDATA, HOMEDRIVE, HOMEPATH, USERNAME) from
// the known folders; then `extra`.
std::vector<wchar_t> elevatedEnvironment(const std::vector<std::pair<std::wstring, std::wstring>>& extra);
// A variable that makes a program load code (the names elevatedEnvironment leaves out).
bool loadsCode(std::wstring_view name);
// The folders of a PATH (`;`-separated) that only administrators can add to,
// down from the volume root (svcloc::folderProtected), in order. An installer
// often puts a folder in the user's profile into the system PATH (Python,
// VS Code, WindowsApps): a program by that name planted there would run as
// administrator. Entries that are not absolute local paths, are missing, or
// cannot be judged are left out.
std::wstring protectedPath(std::wstring_view path);

// `exe` and `args` as one command line, quoted the way the C runtime (and
// CommandLineToArgvW) splits it.
std::wstring commandLine(const std::wstring& exe, const std::vector<std::wstring>& args);

// What a process may set on itself once it runs (the runner: started through
// CreateProcessWithTokenW, it got none at creation): extension points
// disabled, no images from remote or low-label locations.
void applyOwnMitigations();

// The facts about this process the tests assert and `hello` reports.
struct FactsOptions {
    std::wstring spawnExe; // try to start this program (with "--noop"): may the process start one?
    bool clipboard = false; // try to read the clipboard
    std::vector<std::wstring> writeDirs; // try to create a file in each
    std::wstring parentMark; // an environment variable that must NOT have arrived (the parent's own)
};
json::Value collectFacts(const FactsOptions& options);

// "high:low" of a token's logon session (TokenStatistics.AuthenticationId).
std::string authenticationId(HANDLE token);
DWORD integrityRid(HANDLE token);
bool tokenElevated(HANDLE token);

} // namespace ws::plugin::sandbox
