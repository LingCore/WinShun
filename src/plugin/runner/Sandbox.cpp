#include "Sandbox.h"

#include "ServiceLocation.h"

#include <shlobj.h>
#include <userenv.h>

#include <algorithm>
#include <cstring>
#include <cwchar>
#include <cwctype>
#include <string_view>

namespace ws::plugin::sandbox {

using lite::Handle;

// `entries` with `extra` put in (replacing any of the same name), sorted, as one block.
std::vector<wchar_t> withExtra(std::vector<std::wstring> entries, const std::vector<std::pair<std::wstring, std::wstring>>& extra);

namespace {

template <typename T>
std::vector<char> tokenInfo(HANDLE token, TOKEN_INFORMATION_CLASS what, T** out)
{
    DWORD size = 0;
    ::GetTokenInformation(token, what, nullptr, 0, &size);
    std::vector<char> buffer(size ? size : 1);
    if (size == 0 || !::GetTokenInformation(token, what, buffer.data(), size, &size)) {
        *out = nullptr;
        return buffer;
    }
    *out = reinterpret_cast<T*>(buffer.data());
    return buffer;
}

Handle ownToken(DWORD access)
{
    HANDLE token = nullptr;
    ::OpenProcessToken(::GetCurrentProcess(), access, &token);
    return Handle(token);
}

// "enabled", "deny-only", "disabled" or "absent": how the token holds the
// Administrators group.
std::string administratorsGroup(HANDLE token)
{
    BYTE admins[SECURITY_MAX_SID_SIZE];
    DWORD size = sizeof admins;
    if (!::CreateWellKnownSid(WinBuiltinAdministratorsSid, nullptr, admins, &size))
        return "?";
    TOKEN_GROUPS* groups = nullptr;
    const auto buffer = tokenInfo(token, TokenGroups, &groups);
    if (!groups)
        return "?";
    for (DWORD i = 0; i < groups->GroupCount; ++i) {
        if (!::EqualSid(groups->Groups[i].Sid, admins))
            continue;
        const DWORD attributes = groups->Groups[i].Attributes;
        if (attributes & SE_GROUP_USE_FOR_DENY_ONLY)
            return "deny-only";
        return attributes & SE_GROUP_ENABLED ? "enabled" : "disabled";
    }
    return "absent";
}

int privilegeCount(HANDLE token)
{
    TOKEN_PRIVILEGES* privileges = nullptr;
    const auto buffer = tokenInfo(token, TokenPrivileges, &privileges);
    return privileges ? static_cast<int>(privileges->PrivilegeCount) : -1;
}

std::string tokenUserName(HANDLE token)
{
    TOKEN_USER* user = nullptr;
    const auto buffer = tokenInfo(token, TokenUser, &user);
    if (!user)
        return "?";
    wchar_t name[256], domain[256];
    DWORD nameSize = 256, domainSize = 256;
    SID_NAME_USE use;
    if (!::LookupAccountSidW(nullptr, user->User.Sid, name, &nameSize, domain, &domainSize, &use))
        return "?";
    return lite::utf8(std::wstring(domain) + L"\\" + name);
}

// "format:size:hash" of the first clipboard format whose data can be read,
// or "" when none can; `formats` is how many the clipboard holds.
std::string clipboardHash(DWORD* error, int* formats)
{
    *error = 0;
    *formats = 0;
    for (int attempt = 0; attempt < 5; ++attempt) {
        if (::OpenClipboard(nullptr))
            break;
        *error = ::GetLastError();
        ::Sleep(20);
        if (attempt == 4)
            return {};
    }
    *formats = ::CountClipboardFormats();
    std::string result;
    for (UINT f = ::EnumClipboardFormats(0); f; f = ::EnumClipboardFormats(f)) {
        if (f == CF_BITMAP || f == CF_PALETTE || f == CF_METAFILEPICT || f == CF_ENHMETAFILE || f == CF_DSPBITMAP
            || f == CF_DSPENHMETAFILE || f == CF_DSPMETAFILEPICT)
            continue; // GDI handles, not memory
        const HANDLE data = ::GetClipboardData(f);
        if (!data) {
            *error = ::GetLastError();
            continue;
        }
        const SIZE_T size = ::GlobalSize(data);
        if (const auto* bytes = static_cast<const unsigned char*>(::GlobalLock(data))) {
            unsigned long long h = 14695981039346656037ull;
            for (SIZE_T i = 0; i < size; ++i) {
                h ^= bytes[i];
                h *= 1099511628211ull;
            }
            ::GlobalUnlock(data);
            result = lite::format("%u:%zu:%llx", f, static_cast<std::size_t>(size), h);
            *error = 0;
            break;
        }
        *error = ::GetLastError();
    }
    if (result.empty() && *error == 0)
        *error = ERROR_NO_DATA;
    ::CloseClipboard();
    return result;
}

} // namespace

DWORD integrityRid(HANDLE token)
{
    TOKEN_MANDATORY_LABEL* label = nullptr;
    const auto buffer = tokenInfo(token, TokenIntegrityLevel, &label);
    if (!label)
        return 0;
    return *::GetSidSubAuthority(label->Label.Sid, *::GetSidSubAuthorityCount(label->Label.Sid) - 1);
}

bool tokenElevated(HANDLE token)
{
    TOKEN_ELEVATION elevation {};
    DWORD size = 0;
    return ::GetTokenInformation(token, TokenElevation, &elevation, sizeof elevation, &size) && elevation.TokenIsElevated;
}

std::string authenticationId(HANDLE token)
{
    TOKEN_STATISTICS statistics {};
    DWORD size = 0;
    if (!::GetTokenInformation(token, TokenStatistics, &statistics, sizeof statistics, &size))
        return "?";
    return lite::format("%08lx:%08lx", statistics.AuthenticationId.HighPart, statistics.AuthenticationId.LowPart);
}

Handle restrictedToken(DWORD integrity, DWORD* error)
{
    *error = 0;
    const Handle own = ownToken(TOKEN_DUPLICATE | TOKEN_QUERY | TOKEN_ASSIGN_PRIMARY | TOKEN_ADJUST_DEFAULT);
    if (!own.valid()) {
        *error = ::GetLastError();
        return {};
    }
    BYTE administrators[SECURITY_MAX_SID_SIZE];
    BYTE localAdministrator[SECURITY_MAX_SID_SIZE];
    DWORD size = sizeof administrators;
    ::CreateWellKnownSid(WinBuiltinAdministratorsSid, nullptr, administrators, &size);
    size = sizeof localAdministrator;
    ::CreateWellKnownSid(WinLocalAccountAndAdministratorSid, nullptr, localAdministrator, &size);
    SID_AND_ATTRIBUTES deny[] = {{administrators, 0}, {localAdministrator, 0}};
    HANDLE restricted = nullptr;
    if (!::CreateRestrictedToken(own.get(), DISABLE_MAX_PRIVILEGE, 2, deny, 0, nullptr, 0, nullptr, &restricted)) {
        *error = ::GetLastError();
        return {};
    }
    Handle token(restricted);
    BYTE label[SECURITY_MAX_SID_SIZE];
    size = sizeof label;
    ::CreateWellKnownSid(integrity == SECURITY_MANDATORY_LOW_RID ? WinLowLabelSid : WinMediumLabelSid, nullptr, label, &size);
    TOKEN_MANDATORY_LABEL mandatory {};
    mandatory.Label.Attributes = SE_GROUP_INTEGRITY;
    mandatory.Label.Sid = label;
    if (!::SetTokenInformation(restricted, TokenIntegrityLevel, &mandatory, sizeof mandatory + ::GetLengthSid(label))) {
        *error = ::GetLastError();
        return {};
    }
    // Default DACL: the user and the system (an elevated token's names only
    // Administrators, which the child is not).
    BYTE userBuffer[sizeof(TOKEN_USER) + SECURITY_MAX_SID_SIZE];
    BYTE system[SECURITY_MAX_SID_SIZE];
    size = sizeof system;
    DWORD got = 0;
    if (::GetTokenInformation(own.get(), TokenUser, userBuffer, sizeof userBuffer, &got)
        && ::CreateWellKnownSid(WinLocalSystemSid, nullptr, system, &size)) {
        const PSID user = reinterpret_cast<TOKEN_USER*>(userBuffer)->User.Sid;
        BYTE aclBuffer[256];
        auto* acl = reinterpret_cast<PACL>(aclBuffer);
        if (::InitializeAcl(acl, sizeof aclBuffer, ACL_REVISION) && ::AddAccessAllowedAce(acl, ACL_REVISION, GENERIC_ALL, user)
            && ::AddAccessAllowedAce(acl, ACL_REVISION, GENERIC_ALL, system)) {
            TOKEN_DEFAULT_DACL dacl {acl};
            ::SetTokenInformation(restricted, TokenDefaultDacl, &dacl, sizeof dacl);
        }
    }
    return token;
}

Handle makeJob(bool limited, const JobLimits& limits, DWORD* error)
{
    *error = 0;
    Handle job(::CreateJobObjectW(nullptr, nullptr));
    if (!job.valid()) {
        *error = ::GetLastError();
        return {};
    }
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION extended {};
    extended.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
    if (limited) {
        extended.BasicLimitInformation.LimitFlags |= JOB_OBJECT_LIMIT_PROCESS_MEMORY | JOB_OBJECT_LIMIT_DIE_ON_UNHANDLED_EXCEPTION;
        extended.ProcessMemoryLimit = static_cast<SIZE_T>(limits.memoryMB) * 1024 * 1024;
        if (!limits.processes) {
            extended.BasicLimitInformation.LimitFlags |= JOB_OBJECT_LIMIT_ACTIVE_PROCESS;
            extended.BasicLimitInformation.ActiveProcessLimit = 1;
        }
    }
    if (!::SetInformationJobObject(job.get(), JobObjectExtendedLimitInformation, &extended, sizeof extended)) {
        *error = ::GetLastError();
        return {};
    }
    if (limited) {
        JOBOBJECT_BASIC_UI_RESTRICTIONS ui {};
        if (!limits.clipboardRead)
            ui.UIRestrictionsClass |= JOB_OBJECT_UILIMIT_READCLIPBOARD;
        if (!limits.clipboardWrite)
            ui.UIRestrictionsClass |= JOB_OBJECT_UILIMIT_WRITECLIPBOARD;
        if (!limits.ui) {
            ui.UIRestrictionsClass |= JOB_OBJECT_UILIMIT_DESKTOP | JOB_OBJECT_UILIMIT_EXITWINDOWS | JOB_OBJECT_UILIMIT_SYSTEMPARAMETERS
                | JOB_OBJECT_UILIMIT_DISPLAYSETTINGS;
            // UILIMIT_HANDLES keeps it from USER handles of processes outside
            // the job -- the clipboard's data among them (GetClipboardData
            // fails with ERROR_INVALID_HANDLE even with READCLIPBOARD
            // allowed; tst_plugin found it). So only without the clipboard.
            if (!limits.clipboardRead && !limits.clipboardWrite)
                ui.UIRestrictionsClass |= JOB_OBJECT_UILIMIT_HANDLES;
        }
        if (ui.UIRestrictionsClass && !::SetInformationJobObject(job.get(), JobObjectBasicUIRestrictions, &ui, sizeof ui)) {
            *error = ::GetLastError();
            return {};
        }
    }
    return job;
}

std::vector<wchar_t> environmentFor(HANDLE token, const std::vector<std::pair<std::wstring, std::wstring>>& extra)
{
    std::vector<std::wstring> entries;
    void* block = nullptr;
    if (token && ::CreateEnvironmentBlock(&block, token, FALSE)) {
        for (const wchar_t* p = static_cast<wchar_t*>(block); *p; p += std::wcslen(p) + 1)
            entries.emplace_back(p);
        ::DestroyEnvironmentBlock(block);
    } else {
        wchar_t* env = ::GetEnvironmentStringsW();
        for (const wchar_t* p = env; *p; p += std::wcslen(p) + 1)
            entries.emplace_back(p);
        ::FreeEnvironmentStringsW(env);
    }
    return withExtra(std::move(entries), extra);
}

bool loadsCode(std::wstring_view name)
{
    // Whole names, then prefixes: the CLR's and .NET's hooks and profilers,
    // node's, Python's, Perl's, Ruby's, Java's, Qt's plugin paths, the
    // debugger's symbol servers (dbghelp loads symsrv.dll from them).
    static constexpr std::wstring_view kNames[] = {L"PATH", L"NODE_OPTIONS", L"NODE_PATH", L"JAVA_TOOL_OPTIONS",
        L"_JAVA_OPTIONS", L"JDK_JAVA_OPTIONS", L"PERL5LIB", L"PERL5OPT", L"PERLLIB", L"RUBYOPT", L"RUBYLIB",
        L"PSModulePath", L"QT_PLUGIN_PATH", L"QML2_IMPORT_PATH", L"QML_IMPORT_PATH", L"QT_QPA_PLATFORM_PLUGIN_PATH",
        L"_NT_SYMBOL_PATH", L"_NT_ALT_SYMBOL_PATH", L"_NT_SYMCACHE_PATH", L"LUA_PATH", L"LUA_CPATH", L"GIT_EXEC_PATH",
        L"TEMP", L"TMP"};
    static constexpr std::wstring_view kPrefixes[] = {L"COR_", L"CORECLR_", L"COMPLUS_", L"DOTNET_", L"PYTHON",
        L"NODE_", L"MONO_", L"QT_", L"__COMPAT_LAYER", L"LD_"};
    const auto same = [](std::wstring_view a, std::wstring_view b) {
        return a.size() == b.size() && _wcsnicmp(a.data(), b.data(), a.size()) == 0;
    };
    for (const std::wstring_view n : kNames) {
        if (same(name, n))
            return true;
    }
    for (const std::wstring_view p : kPrefixes) {
        if (name.size() >= p.size() && same(name.substr(0, p.size()), p))
            return true;
    }
    return false;
}

namespace {

std::wstring knownFolder(REFKNOWNFOLDERID id)
{
    PWSTR path = nullptr;
    std::wstring out;
    if (SUCCEEDED(::SHGetKnownFolderPath(id, KF_FLAG_DONT_VERIFY, nullptr, &path)) && path)
        out = path;
    if (path)
        ::CoTaskMemFree(path);
    return out;
}

// The system's own PATH (HKLM, its %vars% expanded against the system block).
std::wstring systemPath(const std::vector<std::wstring>& system)
{
    std::wstring raw;
    HKEY key = nullptr;
    if (::RegOpenKeyExW(HKEY_LOCAL_MACHINE, L"SYSTEM\\CurrentControlSet\\Control\\Session Manager\\Environment", 0, KEY_QUERY_VALUE, &key)
        == ERROR_SUCCESS) {
        DWORD type = 0;
        DWORD bytes = 0;
        if (::RegQueryValueExW(key, L"Path", nullptr, &type, nullptr, &bytes) == ERROR_SUCCESS && bytes > 0 && bytes < (1u << 20)) {
            raw.resize(bytes / sizeof(wchar_t) + 1);
            if (::RegQueryValueExW(key, L"Path", nullptr, &type, reinterpret_cast<BYTE*>(raw.data()), &bytes) != ERROR_SUCCESS)
                raw.clear();
            raw.resize(std::wcslen(raw.c_str()));
        }
        ::RegCloseKey(key);
    }
    // %SystemRoot% and the like, from the system block (not this process's).
    std::wstring out;
    for (std::size_t i = 0; i < raw.size();) {
        const std::size_t open = raw.find(L'%', i);
        const std::size_t close = open == std::wstring::npos ? std::wstring::npos : raw.find(L'%', open + 1);
        if (close == std::wstring::npos) {
            out += raw.substr(i);
            break;
        }
        out += raw.substr(i, open - i);
        const std::wstring name = raw.substr(open + 1, close - open - 1) + L'=';
        bool found = false;
        for (const std::wstring& e : system) {
            if (e.size() >= name.size() && _wcsnicmp(e.c_str(), name.c_str(), name.size()) == 0) {
                out += e.substr(name.size());
                found = true;
                break;
            }
        }
        if (!found)
            out += raw.substr(open, close - open + 1);
        i = close + 1;
    }
    if (out.empty()) {
        const std::wstring windows = knownFolder(FOLDERID_Windows);
        out = windows + L"\\system32;" + windows + L";" + windows + L"\\System32\\Wbem";
    }
    return out;
}

} // namespace

std::vector<wchar_t> elevatedEnvironment(const std::vector<std::pair<std::wstring, std::wstring>>& extra)
{
    std::vector<std::wstring> system;
    void* block = nullptr;
    if (::CreateEnvironmentBlock(&block, nullptr, FALSE)) { // no token: the system's variables only
        for (const wchar_t* p = static_cast<wchar_t*>(block); *p; p += std::wcslen(p) + 1)
            system.emplace_back(p);
        ::DestroyEnvironmentBlock(block);
    }
    std::vector<std::wstring> entries;
    for (const std::wstring& e : system) {
        const std::size_t eq = e.find(L'=', 1);
        if (eq != std::wstring::npos && !loadsCode(std::wstring_view(e).substr(0, eq)))
            entries.push_back(e);
    }
    entries.push_back(L"Path=" + protectedPath(systemPath(system)));
    const std::wstring profile = knownFolder(FOLDERID_Profile);
    const auto add = [&](const wchar_t* name, const std::wstring& value) {
        if (!value.empty())
            entries.push_back(std::wstring(name) + L'=' + value);
    };
    add(L"USERPROFILE", profile);
    add(L"APPDATA", knownFolder(FOLDERID_RoamingAppData));
    add(L"LOCALAPPDATA", knownFolder(FOLDERID_LocalAppData));
    if (profile.size() > 2 && profile[1] == L':') {
        add(L"HOMEDRIVE", profile.substr(0, 2));
        add(L"HOMEPATH", profile.substr(2));
    }
    wchar_t user[257] {};
    DWORD size = static_cast<DWORD>(std::size(user));
    if (::GetUserNameW(user, &size))
        add(L"USERNAME", user);
    // TEMP and TMP: not the user's (code unpacked there and loaded could be
    // put there first); Host gives the plugin a protected one in `extra`.
    add(L"TEMP", knownFolder(FOLDERID_Windows) + L"\\Temp");
    add(L"TMP", knownFolder(FOLDERID_Windows) + L"\\Temp");
    return withExtra(std::move(entries), extra);
}

std::wstring protectedPath(std::wstring_view path)
{
    std::wstring out;
    std::vector<std::wstring> kept;
    for (std::size_t i = 0; i <= path.size();) {
        std::size_t end = path.find(L';', i);
        if (end == std::wstring_view::npos)
            end = path.size();
        std::wstring dir(path.substr(i, end - i));
        i = end + 1;
        while (dir.size() > 3 && (dir.back() == L'\\' || dir.back() == L'/'))
            dir.pop_back();
        // "C:\..." only: no relative folder, no share, no %var% left unexpanded.
        const bool local = dir.size() >= 3 && std::iswalpha(dir[0]) && dir[1] == L':' && dir[2] == L'\\'
            && dir.find(L'%') == std::wstring::npos;
        if (!local)
            continue;
        if (std::any_of(kept.begin(), kept.end(), [&](const std::wstring& k) { return _wcsicmp(k.c_str(), dir.c_str()) == 0; }))
            continue;
        const DWORD attributes = ::GetFileAttributesW(dir.c_str());
        if (attributes == INVALID_FILE_ATTRIBUTES || !(attributes & FILE_ATTRIBUTE_DIRECTORY))
            continue;
        if (!svcloc::folderProtected(dir))
            continue;
        if (!out.empty())
            out += L';';
        out += dir;
        kept.push_back(std::move(dir));
    }
    return out;
}

std::vector<wchar_t> withExtra(std::vector<std::wstring> entries, const std::vector<std::pair<std::wstring, std::wstring>>& extra)
{
    for (const auto& [name, value] : extra) {
        const std::wstring prefix = name + L'=';
        std::erase_if(entries, [&](const std::wstring& e) {
            return e.size() >= prefix.size() && _wcsnicmp(e.c_str(), prefix.c_str(), prefix.size()) == 0;
        });
        entries.push_back(prefix + value);
    }
    std::sort(entries.begin(), entries.end(), [](const std::wstring& a, const std::wstring& b) {
        return _wcsicmp(a.c_str(), b.c_str()) < 0;
    });
    std::vector<wchar_t> flat;
    for (const auto& e : entries) {
        flat.insert(flat.end(), e.begin(), e.end());
        flat.push_back(L'\0');
    }
    flat.push_back(L'\0');
    return flat;
}

std::wstring commandLine(const std::wstring& exe, const std::vector<std::wstring>& args)
{
    const auto quoted = [](const std::wstring& arg) {
        if (!arg.empty() && arg.find_first_of(L" \t\n\v\"") == std::wstring::npos)
            return arg;
        std::wstring out = L"\"";
        std::size_t backslashes = 0;
        for (const wchar_t c : arg) {
            if (c == L'\\') {
                ++backslashes;
                continue;
            }
            if (c == L'"') {
                out.append(backslashes * 2 + 1, L'\\');
                out += L'"';
            } else {
                out.append(backslashes, L'\\');
                out += c;
            }
            backslashes = 0;
        }
        out.append(backslashes * 2, L'\\');
        out += L'"';
        return out;
    };
    std::wstring line = L"\"" + exe + L"\"";
    for (const std::wstring& arg : args) {
        line += L' ';
        line += quoted(arg);
    }
    return line;
}

void applyOwnMitigations()
{
    PROCESS_MITIGATION_EXTENSION_POINT_DISABLE_POLICY extension {};
    extension.DisableExtensionPoints = 1;
    ::SetProcessMitigationPolicy(ProcessExtensionPointDisablePolicy, &extension, sizeof extension);
    PROCESS_MITIGATION_IMAGE_LOAD_POLICY imageLoad {};
    imageLoad.NoRemoteImages = 1;
    imageLoad.NoLowMandatoryLabelImages = 1;
    ::SetProcessMitigationPolicy(ProcessImageLoadPolicy, &imageLoad, sizeof imageLoad);
}

json::Value collectFacts(const FactsOptions& options)
{
    json::Value::Object j;
    j["pid"] = static_cast<unsigned long>(::GetCurrentProcessId());
    DWORD session = 0;
    ::ProcessIdToSessionId(::GetCurrentProcessId(), &session);
    j["session"] = static_cast<unsigned long>(session);

    const Handle token = ownToken(TOKEN_QUERY);
    j["integrity"] = static_cast<unsigned long>(integrityRid(token.get()));
    j["elevated"] = tokenElevated(token.get());
    j["admins"] = administratorsGroup(token.get());
    j["authId"] = authenticationId(token.get());
    j["privileges"] = privilegeCount(token.get());
    j["user"] = tokenUserName(token.get());

    // The job: ours, with our limits, or some other?
    BOOL inJob = FALSE;
    ::IsProcessInJob(::GetCurrentProcess(), nullptr, &inJob);
    j["inJob"] = inJob != FALSE;
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits {};
    JOBOBJECT_BASIC_UI_RESTRICTIONS ui {};
    if (inJob && ::QueryInformationJobObject(nullptr, JobObjectExtendedLimitInformation, &limits, sizeof limits, nullptr)) {
        ::QueryInformationJobObject(nullptr, JobObjectBasicUIRestrictions, &ui, sizeof ui, nullptr);
        j["jobLimitFlags"] = static_cast<unsigned long>(limits.BasicLimitInformation.LimitFlags);
        j["jobKillOnClose"] = (limits.BasicLimitInformation.LimitFlags & JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE) != 0;
        j["jobActiveProcessLimit"] = static_cast<unsigned long>(
            (limits.BasicLimitInformation.LimitFlags & JOB_OBJECT_LIMIT_ACTIVE_PROCESS) ? limits.BasicLimitInformation.ActiveProcessLimit : 0);
        j["jobMemoryMB"] = static_cast<unsigned long long>(
            (limits.BasicLimitInformation.LimitFlags & JOB_OBJECT_LIMIT_PROCESS_MEMORY) ? limits.ProcessMemoryLimit / (1024 * 1024) : 0);
        j["jobUiRestrictions"] = static_cast<unsigned long>(ui.UIRestrictionsClass);
    } else {
        j["jobLimitFlags"] = nullptr;
        j["jobKillOnClose"] = false;
        j["jobActiveProcessLimit"] = -1;
        j["jobMemoryMB"] = -1;
        j["jobUiRestrictions"] = nullptr;
    }

    PROCESS_MITIGATION_ASLR_POLICY aslr {};
    PROCESS_MITIGATION_EXTENSION_POINT_DISABLE_POLICY extension {};
    PROCESS_MITIGATION_IMAGE_LOAD_POLICY imageLoad {};
    PROCESS_MITIGATION_DYNAMIC_CODE_POLICY dynamicCode {};
    ::GetProcessMitigationPolicy(::GetCurrentProcess(), ProcessASLRPolicy, &aslr, sizeof aslr);
    ::GetProcessMitigationPolicy(::GetCurrentProcess(), ProcessExtensionPointDisablePolicy, &extension, sizeof extension);
    ::GetProcessMitigationPolicy(::GetCurrentProcess(), ProcessImageLoadPolicy, &imageLoad, sizeof imageLoad);
    ::GetProcessMitigationPolicy(::GetCurrentProcess(), ProcessDynamicCodePolicy, &dynamicCode, sizeof dynamicCode);
    j["aslrBottomUp"] = aslr.EnableBottomUpRandomization != 0;
    j["aslrHighEntropy"] = aslr.EnableHighEntropy != 0;
    j["extensionPointsDisabled"] = extension.DisableExtensionPoints != 0;
    j["noRemoteImages"] = imageLoad.NoRemoteImages != 0;
    j["noLowLabelImages"] = imageLoad.NoLowMandatoryLabelImages != 0;
    j["dynamicCodeProhibited"] = dynamicCode.ProhibitDynamicCode != 0;

    j["consoleWindow"] = ::GetConsoleWindow() != nullptr;
    j["stdinType"] = static_cast<unsigned long>(::GetFileType(::GetStdHandle(STD_INPUT_HANDLE)));
    j["stdoutType"] = static_cast<unsigned long>(::GetFileType(::GetStdHandle(STD_OUTPUT_HANDLE)));

    // The environment: WS_* received; the parent's own variables not.
    json::Value::Array wsEnv;
    int envCount = 0;
    if (wchar_t* env = ::GetEnvironmentStringsW()) {
        for (const wchar_t* p = env; *p; p += std::wcslen(p) + 1) {
            ++envCount;
            if (_wcsnicmp(p, L"WS_", 3) == 0) {
                std::wstring entry(p);
                wsEnv.push_back(lite::utf8(entry.substr(0, entry.find(L'='))));
            }
        }
        ::FreeEnvironmentStringsW(env);
    }
    j["wsEnv"] = std::move(wsEnv);
    j["envCount"] = envCount;
    j["parentEnvLeaked"] = !options.parentMark.empty() && !lite::environmentVariable(options.parentMark.c_str()).empty();
    j["userprofile"] = lite::utf8(lite::environmentVariable(L"USERPROFILE"));
    j["username"] = lite::utf8(lite::environmentVariable(L"USERNAME"));

    if (!options.spawnExe.empty()) {
        std::wstring line = commandLine(options.spawnExe, {L"--noop"});
        STARTUPINFOW startup {sizeof startup};
        PROCESS_INFORMATION info {};
        if (::CreateProcessW(options.spawnExe.c_str(), line.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr,
                &startup, &info)) {
            ::WaitForSingleObject(info.hProcess, 3000);
            BOOL childInJob = FALSE;
            ::IsProcessInJob(info.hProcess, nullptr, &childInJob);
            ::CloseHandle(info.hThread);
            ::CloseHandle(info.hProcess);
            j["spawnOk"] = true;
            j["spawnError"] = 0;
            j["spawnedInJob"] = childInJob != FALSE;
        } else {
            j["spawnOk"] = false;
            j["spawnError"] = static_cast<unsigned long>(::GetLastError());
            j["spawnedInJob"] = nullptr;
        }
    }
    if (options.clipboard) {
        DWORD error = 0;
        int formats = 0;
        const std::string hash = clipboardHash(&error, &formats);
        j["clipRead"] = !hash.empty();
        j["clipError"] = static_cast<unsigned long>(error);
        j["clipFormats"] = formats;
    }
    if (!options.writeDirs.empty()) {
        json::Value::Array writes;
        for (const std::wstring& dir : options.writeDirs) {
            const std::wstring path = dir + L"\\winshun-plugin-write-test-" + std::to_wstring(::GetCurrentProcessId()) + L".tmp";
            const HANDLE file = ::CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                FILE_ATTRIBUTE_NORMAL | FILE_FLAG_DELETE_ON_CLOSE, nullptr);
            json::Value::Object w;
            w["dir"] = lite::utf8(dir);
            w["ok"] = file != INVALID_HANDLE_VALUE;
            w["error"] = static_cast<unsigned long>(file != INVALID_HANDLE_VALUE ? 0 : ::GetLastError());
            if (file != INVALID_HANDLE_VALUE)
                ::CloseHandle(file);
            writes.push_back(std::move(w));
        }
        j["writes"] = std::move(writes);
    }
    return json::Value(std::move(j));
}

} // namespace ws::plugin::sandbox
