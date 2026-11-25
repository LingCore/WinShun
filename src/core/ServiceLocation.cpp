#include "ServiceLocation.h"

#include <aclapi.h>
#include <sddl.h>
#include <shlobj.h>

#include <algorithm>
#include <cstdio>
#include <string>
#include <vector>

namespace ws::svcloc {
namespace {

// Rights that make the executable itself tamperable: write/append its data,
// change its EAs or attributes, delete it, change its DACL or owner, or any
// generic/all form of those.
constexpr DWORD kObjectWrite = FILE_WRITE_DATA | FILE_APPEND_DATA | FILE_WRITE_EA | FILE_WRITE_ATTRIBUTES
    | FILE_DELETE_CHILD | DELETE | WRITE_DAC | WRITE_OWNER | GENERIC_WRITE | GENERIC_ALL;
// Rights on an ancestor directory that let a non-admin swap the child below
// it: delete a child, rename / re-permission / re-own the directory itself.
// Note the absence of FILE_ADD_FILE / FILE_ADD_SUBDIRECTORY (creating a
// sibling) and of GENERIC_WRITE (which, for a directory, is add-file, not
// delete-child) — including them would call the default C:\ unsafe.
constexpr DWORD kAncestorWrite = FILE_DELETE_CHILD | DELETE | WRITE_DAC | WRITE_OWNER | GENERIC_ALL;
// Rights on the executable's own folder: those, plus adding a file or a
// subfolder (and the generic write that includes them). The loader searches a
// program's folder for its DLLs before System32, so a file a non-admin may
// put there is code a SYSTEM service would run.
constexpr DWORD kParentWrite = kAncestorWrite | FILE_ADD_FILE | FILE_ADD_SUBDIRECTORY | GENERIC_WRITE;

// The folder created for a portable copy's service: Administrators own it,
// Administrators and SYSTEM have full control (inherited by the exe inside),
// ordinary users may only read and run, and nothing is inherited from above.
constexpr wchar_t kProtectedSddl[] = L"O:BAD:P(A;OICI;FA;;;SY)(A;OICI;FA;;;BA)(A;OICI;0x1200a9;;;BU)";

bool equalSid(PSID a, PSID b) noexcept
{
    return a && b && ::IsValidSid(a) && ::IsValidSid(b) && ::EqualSid(a, b);
}

bool isWellKnown(PSID sid, WELL_KNOWN_SID_TYPE kind) noexcept
{
    BYTE buffer[SECURITY_MAX_SID_SIZE] {};
    DWORD size = sizeof buffer;
    return ::CreateWellKnownSid(kind, nullptr, buffer, &size) && equalSid(sid, buffer);
}

// SYSTEM, the Administrators group, or TrustedInstaller: the three principals
// a protected location may let write. Everything else (Everyone, Users,
// Authenticated Users, INTERACTIVE, CREATOR OWNER, a named user...) is not.
bool isAdministrativeSid(PSID sid) noexcept
{
    if (!sid || !::IsValidSid(sid))
        return false;
    if (isWellKnown(sid, WinLocalSystemSid) || isWellKnown(sid, WinBuiltinAdministratorsSid))
        return true;
    // TrustedInstaller is a service SID, with no WELL_KNOWN_SID_TYPE of its own.
    static const wchar_t kTrustedInstaller[]
        = L"S-1-5-80-956008885-3418522649-1831038044-1853292631-2271478464";
    PSID trusted = nullptr;
    bool match = false;
    if (::ConvertStringSidToSidW(kTrustedInstaller, &trusted)) {
        match = equalSid(sid, trusted);
        ::LocalFree(trusted);
    }
    return match;
}

std::wstring fullPath(const std::wstring& path)
{
    wchar_t buffer[32768] {};
    const DWORD n = ::GetFullPathNameW(path.c_str(), static_cast<DWORD>(std::size(buffer)), buffer, nullptr);
    std::wstring full = n > 0 && n < std::size(buffer) ? std::wstring(buffer, n) : path;
    // Drop a trailing backslash, but keep the one in a drive root ("C:\").
    while (full.size() > 3 && full.back() == L'\\')
        full.pop_back();
    return full;
}

// The parent directory, or an empty string once at a root ("C:\", "\\srv\share").
std::wstring parentOf(const std::wstring& path)
{
    if (path.size() <= 3) // "C:\" or shorter: nothing above it
        return {};
    const std::size_t slash = path.find_last_of(L'\\');
    if (slash == std::wstring::npos)
        return {};
    if (slash <= 2) // "C:\x" -> "C:\"
        return path.substr(0, 3);
    return path.substr(0, slash);
}

std::wstring sidText(PSID sid)
{
    if (!sid || !::IsValidSid(sid))
        return L"<invalid SID>";
    LPWSTR raw = nullptr;
    if (!::ConvertSidToStringSidW(sid, &raw) || !raw)
        return L"<unknown SID>";
    std::wstring out(raw);
    ::LocalFree(raw);
    return out;
}

DWORD watchedRights(Role role) noexcept
{
    return role == Role::Object ? kObjectWrite : role == Role::Parent ? kParentWrite : kAncestorWrite;
}

// The first thing wrong with this owner and DACL: what, whose, and the rights
// in question. No allocation: writableByNonAdmin stays noexcept.
struct Finding {
    Weakness::Kind kind = Weakness::Kind::None;
    PSID sid = nullptr;
    DWORD mask = 0;
};

Finding find(PSID owner, PACL dacl, Role role) noexcept
{
    using Kind = Weakness::Kind;
    // The owner can always rewrite the DACL, so a non-admin owner is unsafe.
    if (!isAdministrativeSid(owner))
        return {Kind::Owner, owner, 0};
    if (!dacl)
        return {Kind::NoAcl, nullptr, 0}; // a NULL DACL grants everyone every right
    const DWORD watched = watchedRights(role);
    for (WORD i = 0; i < dacl->AceCount; ++i) {
        void* raw = nullptr;
        if (!::GetAce(dacl, i, &raw))
            return {Kind::Unreadable, nullptr, 0}; // cannot read an ACE: assume the worst
        auto* header = static_cast<ACE_HEADER*>(raw);
        if (header->AceType != ACCESS_ALLOWED_ACE_TYPE && header->AceType != ACCESS_ALLOWED_CALLBACK_ACE_TYPE)
            continue; // deny and audit aces do not grant anything
        if (header->AceFlags & INHERIT_ONLY_ACE)
            continue; // applies to children, not to this object
        auto* ace = static_cast<ACCESS_ALLOWED_ACE*>(raw); // SidStart is at the same offset for the callback form
        if ((ace->Mask & watched) && !isAdministrativeSid(&ace->SidStart))
            return {Kind::Rights, &ace->SidStart, ace->Mask};
    }
    return {};
}

Weakness named(const Finding& found, const std::wstring& path)
{
    Weakness weak;
    weak.kind = found.kind;
    if (found.kind == Weakness::Kind::None)
        return weak;
    weak.path = path;
    weak.mask = found.mask;
    if (found.kind == Weakness::Kind::Owner || found.kind == Weakness::Kind::Rights)
        weak.sid = sidText(found.sid);
    return weak;
}

// For the log: "C:\ can be changed by a non-administrator (owner S-1-5-21-...)".
std::wstring describe(const Weakness& weak, const wchar_t* verb)
{
    std::wstring detail;
    switch (weak.kind) {
    case Weakness::Kind::Owner:
        detail = L"owner " + weak.sid;
        break;
    case Weakness::Kind::Rights: {
        wchar_t mask[16] {};
        ::swprintf_s(mask, L"0x%08lX", static_cast<unsigned long>(weak.mask));
        detail = L"ACE " + weak.sid + L" mask " + mask;
        break;
    }
    case Weakness::Kind::NoAcl:
        detail = L"null DACL";
        break;
    case Weakness::Kind::Unreadable:
        detail = L"cannot read its security, error " + std::to_wstring(weak.error);
        break;
    case Weakness::Kind::None:
        break;
    }
    return weak.path + L" " + verb + (detail.empty() ? std::wstring() : L" (" + detail + L")");
}

Weakness unreadable(const std::wstring& path, DWORD error)
{
    Weakness weak;
    weak.kind = Weakness::Kind::Unreadable;
    weak.path = path;
    weak.error = error;
    return weak;
}

// `path` as an Object, the folder it is in as `parentRole`, and every
// directory above that as an Ancestor: the first of them that is weak.
Weakness chainWeakness(const std::wstring& path, Role parentRole)
{
    if (path.empty())
        return unreadable(L"(empty path)", ERROR_INVALID_PARAMETER);
    const std::wstring full = fullPath(path);
    if (Weakness weak = weaknessOf(full, Role::Object))
        return weak;
    Role role = parentRole;
    for (std::wstring dir = parentOf(full); !dir.empty(); dir = parentOf(dir)) {
        if (Weakness weak = weaknessOf(dir, role))
            return weak;
        role = Role::Ancestor;
    }
    return {};
}

// Every directory strictly above `path` as an Ancestor; `path` itself need
// not exist. A root has nothing above it to vouch for it: weak.
Weakness ancestorWeakness(const std::wstring& path)
{
    if (path.empty())
        return unreadable(L"(empty path)", ERROR_INVALID_PARAMETER);
    const std::wstring full = fullPath(path);
    std::wstring dir = parentOf(full);
    if (dir.empty())
        return unreadable(full, ERROR_PATH_NOT_FOUND);
    for (; !dir.empty(); dir = parentOf(dir)) {
        if (Weakness weak = weaknessOf(dir, Role::Ancestor))
            return weak;
    }
    return {};
}

bool report(const Weakness& weak, std::wstring* why)
{
    if (weak && why)
        *why = weak.path;
    return !weak;
}

// "C:\Program Files" in quotes, C:\ bare: a root's closing backslash would
// escape the quote after it, and a root has no spaces to need one.
std::wstring commandArgument(const std::wstring& path)
{
    return path.find(L' ') == std::wstring::npos ? path : L"\"" + path + L"\"";
}

} // namespace

bool writableByNonAdmin(PSID owner, PACL dacl, Role role) noexcept
{
    return find(owner, dacl, role).kind != Weakness::Kind::None;
}

Weakness weakness(PSID owner, PACL dacl, Role role)
{
    return named(find(owner, dacl, role), {});
}

Weakness weaknessOf(const std::wstring& path, Role role)
{
    PSECURITY_DESCRIPTOR descriptor = nullptr;
    PSID owner = nullptr;
    PACL dacl = nullptr;
    const DWORD error = ::GetNamedSecurityInfoW(path.c_str(), SE_FILE_OBJECT,
        OWNER_SECURITY_INFORMATION | DACL_SECURITY_INFORMATION, &owner, nullptr, &dacl, nullptr, &descriptor);
    if (error != ERROR_SUCCESS)
        return unreadable(path, error); // unknown is treated as unsafe
    Weakness weak = named(find(owner, dacl, role), path);
    ::LocalFree(descriptor);
    return weak;
}

std::wstring accountName(const std::wstring& sid)
{
    PSID raw = nullptr;
    if (sid.empty() || !::ConvertStringSidToSidW(sid.c_str(), &raw))
        return sid;
    wchar_t name[256] {};
    wchar_t domain[256] {};
    DWORD nameSize = static_cast<DWORD>(std::size(name));
    DWORD domainSize = static_cast<DWORD>(std::size(domain));
    SID_NAME_USE use {};
    const bool found = ::LookupAccountSidW(nullptr, raw, name, &nameSize, domain, &domainSize, &use) && name[0];
    ::LocalFree(raw);
    return found ? std::wstring(name) : sid; // a deleted account, or a domain's with no controller in reach
}

std::vector<std::wstring> repairCommands(const Weakness& weak)
{
    const std::wstring target = commandArgument(weak.path);
    switch (weak.kind) {
    case Weakness::Kind::Owner:
        return {L"takeown /f " + target + L" /a"};
    case Weakness::Kind::Rights:
        // Its inherited entries made its own (the same rights, now editable),
        // then that account's replaced by read and run.
        return {L"icacls " + target + L" /inheritance:d", L"icacls " + target + L" /grant:r *" + weak.sid + L":(OI)(CI)(RX)"};
    case Weakness::Kind::NoAcl:
    case Weakness::Kind::Unreadable:
    case Weakness::Kind::None:
        break;
    }
    return {};
}

namespace {

std::wstring knownServiceDir(REFKNOWNFOLDERID id)
{
    PWSTR folder = nullptr;
    std::wstring dir;
    if (SUCCEEDED(::SHGetKnownFolderPath(id, KF_FLAG_DEFAULT, nullptr, &folder)) && folder)
        dir = std::wstring(folder) + L"\\WinShun Service";
    if (folder)
        ::CoTaskMemFree(folder);
    return dir;
}

} // namespace

bool pathProtected(const std::wstring& path, std::wstring* why)
{
    return report(chainWeakness(path, Role::Parent), why);
}

bool folderProtected(const std::wstring& dir, std::wstring* why)
{
    return report(chainWeakness(dir, Role::Ancestor), why);
}

bool ancestorsProtected(const std::wstring& path, std::wstring* why)
{
    return report(ancestorWeakness(path), why);
}

std::vector<std::wstring> protectedServiceDirs()
{
    std::vector<std::wstring> dirs;
    const auto add = [&](REFKNOWNFOLDERID id) {
        std::wstring dir = knownServiceDir(id);
        if (dir.empty())
            return;
        const bool seen = std::any_of(dirs.begin(), dirs.end(), [&](const std::wstring& existing) {
            return existing.size() == dir.size()
                && ::CompareStringOrdinal(existing.c_str(), -1, dir.c_str(), -1, TRUE) == CSTR_EQUAL;
        });
        if (!seen)
            dirs.push_back(std::move(dir));
    };
    add(FOLDERID_ProgramFiles);
    add(FOLDERID_ProgramData);
    add(FOLDERID_ProgramFilesX86);
    return dirs;
}

std::wstring protectedServiceDir()
{
    const std::vector<std::wstring> dirs = protectedServiceDirs();
    return dirs.empty() ? std::wstring() : dirs.front();
}

bool inProtectedServiceDir(const std::wstring& path)
{
    for (const std::wstring& dir : protectedServiceDirs()) {
        if (parentIs(path, dir))
            return true;
    }
    return false;
}

bool anyProtectedDirUsable(const std::vector<std::wstring>& candidates, Weakness* firstWeak)
{
    Weakness first;
    for (const std::wstring& dir : candidates) {
        if (dir.empty())
            continue;
        // There already: itself and all above it. Not yet: all above it.
        const DWORD attributes = ::GetFileAttributesW(dir.c_str());
        const bool absent = attributes == INVALID_FILE_ATTRIBUTES;
        Weakness weak = absent ? ancestorWeakness(dir) : chainWeakness(dir, Role::Ancestor);
        if (!weak && !absent && !(attributes & FILE_ATTRIBUTE_DIRECTORY))
            weak = unreadable(dir, ERROR_DIRECTORY); // a file where the folder would be
        if (!weak)
            return true;
        if (!first)
            first = std::move(weak);
    }
    if (firstWeak)
        *firstWeak = std::move(first);
    return false;
}

bool ensureProtectedDir(const std::wstring& dir, std::wstring* error)
{
    if (dir.empty()) {
        if (error)
            *error = L"no folder was given";
        return false;
    }
    const DWORD attributes = ::GetFileAttributesW(dir.c_str());
    if (attributes != INVALID_FILE_ATTRIBUTES) {
        if (!(attributes & FILE_ATTRIBUTE_DIRECTORY)) {
            if (error)
                *error = dir + L" exists but is not a folder";
            return false;
        }
        // It was there already: trust it only if it is actually protected.
        if (const Weakness weak = chainWeakness(dir, Role::Ancestor)) {
            if (error)
                *error = describe(weak, L"can be changed by a non-administrator");
            return false;
        }
        return true;
    }
    // Check ancestors first so we never create a stray folder under an unsafe
    // parent (e.g. a tweaked C:\Program Files).
    if (const Weakness weak = ancestorWeakness(dir)) {
        if (error)
            *error = describe(weak, L"can be changed by a non-administrator");
        return false;
    }
    bool created = false;
    if (::CreateDirectoryW(dir.c_str(), nullptr)) {
        created = true;
    } else if (const DWORD err = ::GetLastError(); err != ERROR_ALREADY_EXISTS) {
        if (error)
            *error = dir + L" could not be created (error " + std::to_wstring(err) + L")";
        return false;
    }
    const auto cleanupIfCreated = [&] {
        if (created)
            ::RemoveDirectoryW(dir.c_str());
    };
    // Set the owner (Administrators) and a protected DACL in one step, rather
    // than trusting what the directory inherited.
    PSECURITY_DESCRIPTOR descriptor = nullptr;
    if (!::ConvertStringSecurityDescriptorToSecurityDescriptorW(kProtectedSddl, SDDL_REVISION_1, &descriptor, nullptr)) {
        if (error)
            *error = L"could not build the protected descriptor (error " + std::to_wstring(::GetLastError()) + L")";
        cleanupIfCreated();
        return false;
    }
    BOOL ownerDefaulted = FALSE;
    PSID owner = nullptr;
    BOOL daclPresent = FALSE;
    BOOL daclDefaulted = FALSE;
    PACL dacl = nullptr;
    ::GetSecurityDescriptorOwner(descriptor, &owner, &ownerDefaulted);
    ::GetSecurityDescriptorDacl(descriptor, &daclPresent, &dacl, &daclDefaulted);
    const DWORD set = ::SetNamedSecurityInfoW(const_cast<wchar_t*>(dir.c_str()), SE_FILE_OBJECT,
        OWNER_SECURITY_INFORMATION | DACL_SECURITY_INFORMATION | PROTECTED_DACL_SECURITY_INFORMATION, owner, nullptr,
        dacl, nullptr);
    ::LocalFree(descriptor);
    if (set != ERROR_SUCCESS) {
        if (error)
            *error = dir + L": could not set its security (error " + std::to_wstring(set) + L")";
        cleanupIfCreated();
        return false;
    }
    if (const Weakness weak = chainWeakness(dir, Role::Ancestor)) {
        if (error)
            *error = describe(weak, L"is still not protected after creating the folder");
        cleanupIfCreated();
        return false;
    }
    return true;
}

bool ensureAnyProtectedDir(
    const std::vector<std::wstring>& candidates, std::wstring& chosen, std::wstring* error)
{
    chosen.clear();
    if (candidates.empty()) {
        if (error)
            *error = L"no protected folder candidates were given";
        return false;
    }
    std::wstring combined;
    for (const std::wstring& dir : candidates) {
        std::wstring oneError;
        if (ensureProtectedDir(dir, &oneError)) {
            chosen = dir;
            return true;
        }
        if (!combined.empty())
            combined += L"; ";
        combined += oneError;
    }
    if (error)
        *error = std::move(combined);
    return false;
}

namespace {

bool readWholeFile(const std::wstring& path, std::string& bytes, std::wstring* error)
{
    const HANDLE file = ::CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        if (error)
            *error = path + L": cannot open it (error " + std::to_wstring(::GetLastError()) + L")";
        return false;
    }
    bytes.clear();
    char buffer[64 * 1024];
    bool ok = true;
    for (;;) {
        DWORD read = 0;
        if (!::ReadFile(file, buffer, sizeof buffer, &read, nullptr)) {
            ok = false;
            break;
        }
        if (read == 0)
            break; // end of file
        bytes.append(buffer, read);
    }
    const DWORD lastError = ::GetLastError();
    ::CloseHandle(file);
    if (!ok && error)
        *error = path + L": cannot read it (error " + std::to_wstring(lastError) + L")";
    return ok;
}

bool writeWholeFile(const std::wstring& path, const std::string& bytes, std::wstring* error)
{
    // CREATE_NEW: the folder is protected, so no one can have pre-planted this.
    const HANDLE file = ::CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW,
        FILE_ATTRIBUTE_NORMAL | FILE_FLAG_WRITE_THROUGH, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        if (error)
            *error = path + L": cannot create the temporary copy (error " + std::to_wstring(::GetLastError()) + L")";
        return false;
    }
    bool ok = true;
    std::size_t offset = 0;
    while (offset < bytes.size()) {
        const DWORD chunk = static_cast<DWORD>((std::min)(bytes.size() - offset, std::size_t {1} << 20));
        DWORD written = 0;
        if (!::WriteFile(file, bytes.data() + offset, chunk, &written, nullptr) || written != chunk) {
            ok = false;
            break;
        }
        offset += written;
    }
    ::FlushFileBuffers(file);
    ::CloseHandle(file);
    if (!ok) {
        ::DeleteFileW(path.c_str());
        if (error)
            *error = path + L": cannot write the temporary copy (error " + std::to_wstring(::GetLastError()) + L")";
    }
    return ok;
}

} // namespace

bool copyExeInto(const std::wstring& dir, const std::wstring& name, const std::wstring& sourceExe,
    std::wstring* dest, std::wstring* error)
{
    std::string bytes;
    if (!readWholeFile(sourceExe, bytes, error))
        return false;
    if (bytes.empty()) {
        if (error)
            *error = sourceExe + L" is empty";
        return false;
    }
    const std::wstring target = dir + L"\\" + name;
    const std::wstring temp = target + L".new";
    ::DeleteFileW(temp.c_str()); // a leftover from an interrupted run, in this admin-only folder
    if (!writeWholeFile(temp, bytes, error))
        return false;
    // The service was just stopped, but its image (or the antivirus scanning
    // the old or the new file) can hold the target a moment longer: replacing
    // a file still in use fails with ACCESS_DENIED or SHARING_VIOLATION. Try
    // again for a few seconds before giving up.
    bool moved = false;
    DWORD moveError = 0;
    for (int attempt = 0; attempt < 20 && !moved; ++attempt) {
        moved = ::MoveFileExW(temp.c_str(), target.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != FALSE;
        if (moved)
            break;
        moveError = ::GetLastError();
        if (moveError != ERROR_ACCESS_DENIED && moveError != ERROR_SHARING_VIOLATION && moveError != ERROR_LOCK_VIOLATION)
            break; // not a transient hold: no point waiting
        ::Sleep(250);
    }
    if (!moved) {
        if (error)
            *error = target + L": cannot replace it (error " + std::to_wstring(moveError) + L")";
        ::DeleteFileW(temp.c_str());
        return false;
    }
    std::wstring why;
    if (!pathProtected(target, &why)) {
        if (error)
            *error = why + L" is writable by a non-administrator after the copy";
        return false;
    }
    std::string back;
    if (!readWholeFile(target, back, error))
        return false;
    if (back != bytes) {
        if (error)
            *error = target + L": its contents do not match the source";
        return false;
    }
    if (dest)
        *dest = target;
    return true;
}

namespace {

// The index service runs as SYSTEM and --install runs elevated: both report an
// elevated token. An ordinary-user process (a unit test writing its own logs)
// does not, and must not be locked out of a folder it creates for itself.
bool elevatedOrSystem() noexcept
{
    HANDLE raw = nullptr;
    if (!::OpenProcessToken(::GetCurrentProcess(), TOKEN_QUERY, &raw))
        return false;
    TOKEN_ELEVATION elevation {};
    DWORD size = 0;
    const bool ok = ::GetTokenInformation(raw, TokenElevation, &elevation, sizeof elevation, &size);
    ::CloseHandle(raw);
    return ok && elevation.TokenIsElevated;
}

// A DACL (no owner) only administrators and SYSTEM may write to, users read.
constexpr wchar_t kProtectedDaclOnly[] = L"D:P(A;OICI;FA;;;SY)(A;OICI;FA;;;BA)(A;OICI;0x1200a9;;;BU)";

} // namespace

bool createProtectedDir(const std::wstring& dir)
{
    if (dir.empty())
        return false;
    if (!elevatedOrSystem()) // an ordinary user's own folder: a plain one it can write to
        return ::CreateDirectoryW(dir.c_str(), nullptr) || ::GetLastError() == ERROR_ALREADY_EXISTS;
    PSECURITY_DESCRIPTOR descriptor = nullptr;
    if (!::ConvertStringSecurityDescriptorToSecurityDescriptorW(kProtectedDaclOnly, SDDL_REVISION_1, &descriptor, nullptr))
        return ::CreateDirectoryW(dir.c_str(), nullptr) || ::GetLastError() == ERROR_ALREADY_EXISTS;
    SECURITY_ATTRIBUTES attributes {sizeof attributes, descriptor, FALSE};
    const bool ok = ::CreateDirectoryW(dir.c_str(), &attributes) || ::GetLastError() == ERROR_ALREADY_EXISTS;
    ::LocalFree(descriptor);
    return ok;
}

void secureLogFolder(const std::wstring& dir)
{
    if (dir.empty())
        return;
    createProtectedDir(dir);
    if (!elevatedOrSystem() || folderProtected(dir))
        return; // not our boundary to set, or already safe (we own it, or just made it)
    // There already and weak: set a protected DACL (and the owner, if we may).
    PSECURITY_DESCRIPTOR descriptor = nullptr;
    if (!::ConvertStringSecurityDescriptorToSecurityDescriptorW(kProtectedSddl, SDDL_REVISION_1, &descriptor, nullptr))
        return;
    BOOL ownerDefaulted = FALSE;
    PSID owner = nullptr;
    BOOL daclPresent = FALSE;
    BOOL daclDefaulted = FALSE;
    PACL dacl = nullptr;
    ::GetSecurityDescriptorOwner(descriptor, &owner, &ownerDefaulted);
    ::GetSecurityDescriptorDacl(descriptor, &daclPresent, &dacl, &daclDefaulted);
    if (::SetNamedSecurityInfoW(const_cast<wchar_t*>(dir.c_str()), SE_FILE_OBJECT,
            OWNER_SECURITY_INFORMATION | DACL_SECURITY_INFORMATION | PROTECTED_DACL_SECURITY_INFORMATION, owner, nullptr,
            dacl, nullptr)
        != ERROR_SUCCESS) {
        // Could not set the owner (no take-ownership right): the DACL alone
        // still shuts ordinary users out, and we keep the current owner.
        ::SetNamedSecurityInfoW(const_cast<wchar_t*>(dir.c_str()), SE_FILE_OBJECT,
            DACL_SECURITY_INFORMATION | PROTECTED_DACL_SECURITY_INFORMATION, nullptr, nullptr, dacl, nullptr);
    }
    ::LocalFree(descriptor);
}

bool parentIs(const std::wstring& path, const std::wstring& dir)
{
    if (path.empty() || dir.empty())
        return false;
    const std::wstring parent = parentOf(fullPath(path));
    return parent.size() == dir.size() && ::CompareStringOrdinal(parent.c_str(), -1, dir.c_str(), -1, TRUE) == CSTR_EQUAL;
}

namespace {
bool samePath(std::wstring_view a, std::wstring_view b) noexcept
{
    return a.size() == b.size()
        && ::CompareStringOrdinal(a.data(), static_cast<int>(a.size()), b.data(), static_cast<int>(b.size()), TRUE)
        == CSTR_EQUAL;
}
} // namespace

bool isWinShunBeside(std::wstring_view clientImage, std::wstring_view serviceImage) noexcept
{
    const auto folderOf = [](std::wstring_view path) {
        const std::size_t slash = path.find_last_of(L'\\');
        return slash == std::wstring_view::npos ? std::wstring_view() : path.substr(0, slash);
    };
    const std::wstring_view clientFolder = folderOf(clientImage);
    const std::wstring_view serviceFolder = folderOf(serviceImage);
    return !clientFolder.empty() && samePath(clientImage.substr(clientFolder.size() + 1), L"WinShun.exe")
        && samePath(clientFolder, serviceFolder);
}

bool servesDialogNav(std::wstring_view clientImage, std::wstring_view serviceImage, std::wstring_view installedFrom) noexcept
{
    return isWinShunBeside(clientImage, serviceImage)
        || (!installedFrom.empty() && !clientImage.empty() && samePath(clientImage, installedFrom));
}

} // namespace ws::svcloc
