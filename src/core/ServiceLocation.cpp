#include "ServiceLocation.h"

#include <aclapi.h>
#include <sddl.h>
#include <shlobj.h>

#include <algorithm>
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

bool checkOne(const std::wstring& path, Role role, std::wstring* why)
{
    PSECURITY_DESCRIPTOR descriptor = nullptr;
    PSID owner = nullptr;
    PACL dacl = nullptr;
    const DWORD error = ::GetNamedSecurityInfoW(path.c_str(), SE_FILE_OBJECT,
        OWNER_SECURITY_INFORMATION | DACL_SECURITY_INFORMATION, &owner, nullptr, &dacl, nullptr, &descriptor);
    if (error != ERROR_SUCCESS) {
        if (why)
            *why = path + L" (cannot read its security, error " + std::to_wstring(error) + L")";
        return false; // unknown is treated as unsafe
    }
    const bool unsafe = writableByNonAdmin(owner, dacl, role);
    ::LocalFree(descriptor);
    if (unsafe && why)
        *why = path;
    return !unsafe;
}

} // namespace

bool writableByNonAdmin(PSID owner, PACL dacl, Role role) noexcept
{
    // The owner can always rewrite the DACL, so a non-admin owner is unsafe.
    if (!isAdministrativeSid(owner))
        return true;
    if (!dacl)
        return true; // a NULL DACL grants everyone every right
    const DWORD watched = role == Role::Object ? kObjectWrite : role == Role::Parent ? kParentWrite : kAncestorWrite;
    for (WORD i = 0; i < dacl->AceCount; ++i) {
        void* raw = nullptr;
        if (!::GetAce(dacl, i, &raw))
            return true; // cannot read an ACE: assume the worst
        auto* header = static_cast<ACE_HEADER*>(raw);
        if (header->AceType != ACCESS_ALLOWED_ACE_TYPE && header->AceType != ACCESS_ALLOWED_CALLBACK_ACE_TYPE)
            continue; // deny and audit aces do not grant anything
        if (header->AceFlags & INHERIT_ONLY_ACE)
            continue; // applies to children, not to this object
        auto* ace = static_cast<ACCESS_ALLOWED_ACE*>(raw); // SidStart is at the same offset for the callback form
        if ((ace->Mask & watched) && !isAdministrativeSid(&ace->SidStart))
            return true;
    }
    return false;
}

namespace {

// `path` as an Object, the folder it is in as `parentRole`, and every
// directory above that as an Ancestor.
bool chainProtected(const std::wstring& path, Role parentRole, std::wstring* why)
{
    if (path.empty()) {
        if (why)
            *why = L"(empty path)";
        return false;
    }
    const std::wstring full = fullPath(path);
    if (!checkOne(full, Role::Object, why))
        return false;
    Role role = parentRole;
    for (std::wstring dir = parentOf(full); !dir.empty(); dir = parentOf(dir)) {
        if (!checkOne(dir, role, why))
            return false;
        role = Role::Ancestor;
    }
    return true;
}

} // namespace

bool pathProtected(const std::wstring& path, std::wstring* why)
{
    return chainProtected(path, Role::Parent, why);
}

bool folderProtected(const std::wstring& dir, std::wstring* why)
{
    return chainProtected(dir, Role::Ancestor, why);
}

std::wstring protectedServiceDir()
{
    PWSTR programFiles = nullptr;
    std::wstring dir;
    if (SUCCEEDED(::SHGetKnownFolderPath(FOLDERID_ProgramFiles, KF_FLAG_DEFAULT, nullptr, &programFiles)) && programFiles)
        dir = std::wstring(programFiles) + L"\\WinShun Service";
    if (programFiles)
        ::CoTaskMemFree(programFiles);
    return dir;
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
        std::wstring why;
        if (!folderProtected(dir, &why)) {
            if (error)
                *error = why + L" can be changed by a non-administrator";
            return false;
        }
        return true;
    }
    if (!::CreateDirectoryW(dir.c_str(), nullptr) && ::GetLastError() != ERROR_ALREADY_EXISTS) {
        if (error)
            *error = dir + L" could not be created (error " + std::to_wstring(::GetLastError()) + L")";
        return false;
    }
    // Set the owner (Administrators) and a protected DACL in one step, rather
    // than trusting what the directory inherited.
    PSECURITY_DESCRIPTOR descriptor = nullptr;
    if (!::ConvertStringSecurityDescriptorToSecurityDescriptorW(kProtectedSddl, SDDL_REVISION_1, &descriptor, nullptr)) {
        if (error)
            *error = L"could not build the protected descriptor (error " + std::to_wstring(::GetLastError()) + L")";
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
        return false;
    }
    std::wstring why;
    if (!folderProtected(dir, &why)) {
        if (error)
            *error = why + L" is still not protected after creating the folder";
        return false;
    }
    return true;
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

bool isWinShunBeside(std::wstring_view clientImage, std::wstring_view serviceImage) noexcept
{
    const auto folderOf = [](std::wstring_view path) {
        const std::size_t slash = path.find_last_of(L'\\');
        return slash == std::wstring_view::npos ? std::wstring_view() : path.substr(0, slash);
    };
    const auto same = [](std::wstring_view a, std::wstring_view b) {
        return a.size() == b.size()
            && ::CompareStringOrdinal(a.data(), static_cast<int>(a.size()), b.data(), static_cast<int>(b.size()), TRUE)
            == CSTR_EQUAL;
    };
    const std::wstring_view clientFolder = folderOf(clientImage);
    const std::wstring_view serviceFolder = folderOf(serviceImage);
    return !clientFolder.empty() && same(clientImage.substr(clientFolder.size() + 1), L"WinShun.exe")
        && same(clientFolder, serviceFolder);
}

} // namespace ws::svcloc
