#include "Unc.h"

#include "WebDav.h"
#include "Win32Util.h"

#include <windows.h>

#include <winnetwk.h>

#include <algorithm>
#include <cstdint>
#include <cwctype>
#include <optional>

namespace ws::unc {

bool isUnc(std::wstring_view path) noexcept
{
    return path.size() > 2 && path.starts_with(L"\\\\") && path[2] != L'?' && path[2] != L'.' && path[2] != L'\\';
}

bool isWslHost(std::wstring_view name) noexcept
{
    return win32::equalsIgnoreCase(name, L"wsl.localhost") || win32::equalsIgnoreCase(name, L"wsl$");
}

std::wstring_view wslDistribution(std::wstring_view path) noexcept
{
    if (!isWslHost(host(path)))
        return {};
    const std::size_t slash = path.find(L'\\', 2);
    if (slash == std::wstring_view::npos)
        return {};
    const auto end = path.find(L'\\', slash + 1);
    return path.substr(slash + 1, end == std::wstring_view::npos ? end : end - slash - 1);
}

std::wstring normalize(std::wstring_view path)
{
    std::wstring out;
    out.reserve(path.size());
    for (const wchar_t c : path) {
        const wchar_t s = c == L'/' ? L'\\' : c;
        if (s == L'\\' && out.size() >= 2 && out.back() == L'\\')
            continue; // "\\host\\share" (a path copied from source code)
        out.push_back(s);
    }
    while (out.size() > 2 && out.back() == L'\\')
        out.pop_back();
    if (!isUnc(out))
        return {};
    const std::size_t slash = out.find(L'\\', 2);
    if (slash == std::wstring::npos || slash == 2 || slash + 1 >= out.size())
        return {}; // no share
    if (std::any_of(out.begin(), out.end(), [](wchar_t c) { return c < 32 || std::wstring_view(L"<>\"|?*").find(c) != std::wstring_view::npos; }))
        return {};
    if (isWslHost(host(out)))
        out.replace(2, slash - 2, L"wsl.localhost");
    return out;
}

std::wstring canonical(std::wstring_view path)
{
    std::wstring out = normalize(path);
    if (out.empty() && isUnc(path)) {
        std::wstring_view server = path.substr(2);
        while (!server.empty() && server.back() == L'\\')
            server.remove_suffix(1);
        if (server.find(L'\\') == std::wstring_view::npos && webdav::parseHost(server))
            out = normalize(L"\\\\" + std::wstring(server) + L"\\DavWWWRoot");
    }
    if (out.empty() || !webdav::isWebDavPath(out))
        return out;
    const std::wstring_view server = host(out);
    std::optional<webdav::Host> parsed = webdav::parseHost(server);
    if (!parsed && server.find(L'@') != std::wstring_view::npos)
        return out; // "\\nas@0\DavWWWRoot": no server in WebClient's form, kept as written
    // No '@': "\\server\DavWWWRoot", plain http on 80 as WebClient takes it.
    const webdav::Host h = std::move(parsed).value_or(webdav::Host {std::wstring(server), 80, false});
    std::wstring canon = L"\\\\";
    for (const wchar_t c : h.name)
        canon.push_back(static_cast<wchar_t>(std::towlower(c)));
    if (h.tls)
        canon += L"@SSL";
    if (!h.tls || h.port != 443)
        canon += L'@' + std::to_wstring(h.port);
    std::wstring_view rest = std::wstring_view(out).substr(2 + server.size() + 1); // normalize(): a share follows
    // The root goes before a folder, but not before one of that name: in
    // "\\h@80\DavWWWRoot\DavWWWRoot\x" the second is the folder /DavWWWRoot.
    const auto rooted = [](std::wstring_view s) { return win32::equalsIgnoreCase(s.substr(0, s.find(L'\\')), L"DavWWWRoot"); };
    if (rooted(rest)) {
        const std::size_t slash = rest.find(L'\\');
        const std::wstring_view after = slash == std::wstring_view::npos ? std::wstring_view() : rest.substr(slash + 1);
        if (!rooted(after))
            rest = after;
    }
    canon += L'\\';
    canon += rest.empty() ? std::wstring_view(L"DavWWWRoot") : rest; // its folders keep their case
    return canon;
}

std::wstring_view host(std::wstring_view path) noexcept
{
    if (!isUnc(path))
        return {};
    const std::size_t slash = path.find(L'\\', 2);
    return path.substr(2, slash == std::wstring_view::npos ? std::wstring_view::npos : slash - 2);
}

std::wstring_view shareRoot(std::wstring_view path) noexcept
{
    if (!isUnc(path))
        return {};
    const std::size_t slash = path.find(L'\\', 2);
    if (slash == std::wstring_view::npos)
        return {};
    return path.substr(0, path.find(L'\\', slash + 1));
}

bool contains(std::wstring_view folder, std::wstring_view path) noexcept
{
    const auto distro = wslDistribution(folder);
    if (!distro.empty()) {
        const auto other = wslDistribution(path);
        if (!win32::equalsIgnoreCase(distro, other))
            return false;
        folder.remove_prefix(static_cast<std::size_t>(distro.data() - folder.data()) + distro.size());
        path.remove_prefix(static_cast<std::size_t>(other.data() - path.data()) + other.size());
        return path.starts_with(folder)
            && (path.size() == folder.size() || path[folder.size()] == L'\\');
    }
    if (path.size() < folder.size() || (path.size() > folder.size() && path[folder.size()] != L'\\'))
        return false;
    return win32::equalsIgnoreCase(path.substr(0, folder.size()), folder);
}

bool equal(std::wstring_view a, std::wstring_view b) noexcept
{
    return contains(a, b) && contains(b, a);
}

namespace {

std::wstring readString(HKEY key, const wchar_t* subKey, const wchar_t* value)
{
    DWORD bytes = 0;
    if (::RegGetValueW(key, subKey, value, RRF_RT_REG_SZ, nullptr, nullptr, &bytes) != ERROR_SUCCESS || bytes < 2)
        return {};
    std::wstring text(bytes / sizeof(wchar_t), L'\0');
    if (::RegGetValueW(key, subKey, value, RRF_RT_REG_SZ, nullptr, text.data(), &bytes) != ERROR_SUCCESS)
        return {};
    text.resize(::wcsnlen(text.c_str(), text.size()));
    return text;
}

// The names of a key's subkeys, the most recently written first.
std::vector<std::wstring> subKeys(HKEY parent, const wchar_t* path)
{
    win32::UniqueKey key;
    if (::RegOpenKeyExW(parent, path, 0, KEY_READ, key.out()) != ERROR_SUCCESS)
        return {};
    struct Sub {
        std::wstring name;
        std::int64_t written;
    };
    std::vector<Sub> subs;
    wchar_t name[256];
    for (DWORD i = 0;; ++i) {
        DWORD length = static_cast<DWORD>(std::size(name));
        FILETIME written {};
        const LSTATUS status = ::RegEnumKeyExW(key.get(), i, name, &length, nullptr, nullptr, nullptr, &written);
        if (status == ERROR_NO_MORE_ITEMS)
            break;
        if (status == ERROR_SUCCESS)
            subs.push_back({std::wstring(name, length), win32::ticks(written)});
    }
    std::stable_sort(subs.begin(), subs.end(), [](const Sub& a, const Sub& b) { return a.written > b.written; });
    std::vector<std::wstring> names;
    for (Sub& s : subs)
        names.push_back(std::move(s.name));
    return names;
}

} // namespace

namespace {

DWORD readDword(HKEY key, const wchar_t* subKey, const wchar_t* value)
{
    DWORD data = 0;
    DWORD bytes = sizeof data;
    return ::RegGetValueW(key, subKey, value, RRF_RT_REG_DWORD, nullptr, &data, &bytes) == ERROR_SUCCESS ? data : 0;
}

bool isWebAddress(std::wstring_view text) noexcept
{
    const auto starts = [&](std::wstring_view prefix) {
        return text.size() > prefix.size() && win32::equalsIgnoreCase(text.substr(0, prefix.size()), prefix);
    };
    return starts(L"http://") || starts(L"https://");
}

// A WebDAV folder on port 80 in a form that says so: "\\server\dav" -> "\\server@80\dav".
std::wstring withWebDavPort(std::wstring path)
{
    if (path.empty() || webdav::parseHost(host(path)))
        return path;
    const std::size_t slash = path.find(L'\\', 2);
    path.insert(slash == std::wstring::npos ? path.size() : slash, L"@80");
    return path;
}

// WebClient's network provider, by name ("Web Client Network" in English).
bool isWebDavProvider(const wchar_t* provider)
{
    if (!provider || !*provider)
        return false;
    wchar_t name[256];
    DWORD size = static_cast<DWORD>(std::size(name));
    return ::WNetGetProviderNameW(WNNC_NET_DAV, name, &size) == NO_ERROR && win32::equalsIgnoreCase(provider, name);
}

// Whether a connected drive letter is WebClient's, from Windows' list of
// connections (kept on this computer: nothing is asked of a server).
bool connectedToWebDav(wchar_t letter)
{
    HANDLE e = nullptr;
    if (::WNetOpenEnumW(RESOURCE_CONNECTED, RESOURCETYPE_DISK, 0, nullptr, &e) != NO_ERROR)
        return false;
    std::vector<std::uint64_t> buffer(16 * 1024 / sizeof(std::uint64_t));
    bool dav = false;
    for (bool more = true; more && !dav;) {
        DWORD count = static_cast<DWORD>(-1);
        DWORD bytes = static_cast<DWORD>(buffer.size() * sizeof(std::uint64_t));
        const DWORD result = ::WNetEnumResourceW(e, &count, buffer.data(), &bytes);
        if (result == ERROR_MORE_DATA) {
            buffer.resize(bytes / sizeof(std::uint64_t) + 1);
            continue;
        }
        more = result == NO_ERROR;
        const auto* found = reinterpret_cast<const NETRESOURCEW*>(buffer.data());
        for (DWORD i = 0; more && i < count; ++i) {
            if (found[i].lpLocalName && std::towupper(found[i].lpLocalName[0]) == letter)
                dav = isWebDavProvider(found[i].lpProvider);
        }
    }
    ::WNetCloseEnum(e);
    return dav;
}

} // namespace

std::wstring fromMappedDrive(std::wstring_view path)
{
    if (path.size() < 2 || path[1] != L':' || !std::iswalpha(path[0]) || (path.size() > 2 && path[2] != L'\\'))
        return {};
    const auto letter = static_cast<wchar_t>(std::towupper(path[0]));
    const std::wstring key = L"Network\\" + std::wstring(1, letter);
    // Persistent: as Windows remembers it. A WebDAV drive keeps its web address
    // ("http://nas:5244/dav/"), and on port 80 a UNC path that does not say so.
    const std::wstring remembered = readString(HKEY_CURRENT_USER, key.c_str(), L"RemotePath");
    std::wstring remote = isWebAddress(remembered) ? webdav::uncFromUrl(remembered) : normalize(remembered);
    if (!remote.empty() && readDword(HKEY_CURRENT_USER, key.c_str(), L"ProviderType") == WNNC_NET_DAV)
        remote = withWebDavPort(std::move(remote));
    if (remote.empty()) {
        // Mapped for this sign-in only ("net use Z: ... /persistent:no"): the
        // connection itself, which an elevated process does not see.
        const wchar_t drive[] {letter, L':', L'\0'};
        wchar_t name[MAX_PATH + 1];
        DWORD size = static_cast<DWORD>(std::size(name));
        if (::WNetGetConnectionW(drive, name, &size) == NO_ERROR) {
            remote = normalize(name);
            if (!remote.empty() && !webdav::isWebDavPath(remote) && connectedToWebDav(letter))
                remote = withWebDavPort(std::move(remote));
        }
    }
    if (remote.empty())
        return {};
    return canonical(remote + std::wstring(path.substr(2)));
}

std::vector<Known> knownShares()
{
    std::vector<Known> known;
    const auto add = [&](std::wstring path, std::wstring drive) {
        path = canonical(path); // a WebDAV folder as a drive's and as Explorer's: once
        if (path.empty())
            return;
        const auto same = [&](const Known& k) { return win32::equalsIgnoreCase(k.path, path); };
        if (const auto it = std::find_if(known.begin(), known.end(), same); it != known.end()) {
            if (it->drive.empty())
                it->drive = std::move(drive);
            return;
        }
        known.push_back({std::move(path), std::move(drive)});
    };
    // Mapped drives: "Network\Z" with RemotePath (a web address for WebDAV).
    for (const std::wstring& letter : subKeys(HKEY_CURRENT_USER, L"Network")) {
        if (letter.size() != 1)
            continue;
        const std::wstring key = L"Network\\" + letter;
        std::wstring remote = readString(HKEY_CURRENT_USER, key.c_str(), L"RemotePath");
        if (isWebAddress(remote))
            remote = webdav::uncFromUrl(remote);
        else if (readDword(HKEY_CURRENT_USER, key.c_str(), L"ProviderType") == WNNC_NET_DAV)
            remote = withWebDavPort(normalize(remote));
        add(std::move(remote), letter + L':');
    }
    // Shares Explorer has opened: "##host#share" ('#' for '\').
    for (std::wstring name : subKeys(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\MountPoints2")) {
        if (!name.starts_with(L"##"))
            continue;
        std::replace(name.begin(), name.end(), L'#', L'\\');
        add(std::move(name), {});
    }
    return known;
}

} // namespace ws::unc
