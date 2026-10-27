#include "Swap.h"

#include <windows.h>

#include <algorithm>

namespace ws::update {

namespace {

std::wstring join(const std::wstring& folder, const std::wstring& name)
{
    return folder + L'\\' + name;
}

bool exists(const std::wstring& path)
{
    return ::GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES;
}

// A file just written is read by the antivirus for a moment, and a program
// that just ended may still be letting go of its files: tried for a while.
bool move(const std::wstring& from, const std::wstring& to, const Log& log)
{
    DWORD error = 0;
    for (int attempt = 0; attempt < 40; ++attempt) {
        if (::MoveFileExW(from.c_str(), to.c_str(), MOVEFILE_WRITE_THROUGH))
            return true;
        error = ::GetLastError();
        if (error != ERROR_ACCESS_DENIED && error != ERROR_SHARING_VIOLATION && error != ERROR_LOCK_VIOLATION)
            break;
        ::Sleep(250);
    }
    if (log)
        log(L"move failed (" + std::to_wstring(error) + L"): " + from + L" -> " + to);
    ::SetLastError(error);
    return false;
}

} // namespace

std::vector<std::wstring> stagedNames(const std::wstring& stage)
{
    std::vector<std::wstring> names;
    WIN32_FIND_DATAW found;
    const HANDLE find = ::FindFirstFileW(join(stage, L"*").c_str(), &found);
    if (find == INVALID_HANDLE_VALUE)
        return names;
    do {
        const std::wstring name = found.cFileName;
        if (name == L"." || name == L".." || ::CompareStringOrdinal(name.c_str(), -1, L"data", -1, TRUE) == CSTR_EQUAL)
            continue;
        names.push_back(name);
    } while (::FindNextFileW(find, &found));
    ::FindClose(find);
    std::sort(names.begin(), names.end());
    return names;
}

bool swapIn(const SwapPlan& plan, SwapMoves& moves, const Log& log, int failAfter)
{
    if (!::CreateDirectoryW(plan.backup.c_str(), nullptr) && ::GetLastError() != ERROR_ALREADY_EXISTS) {
        if (log)
            log(L"cannot create the backup folder " + plan.backup);
        return false;
    }
    int done = 0;
    const auto step = [&](const std::wstring& from, const std::wstring& to) {
        if (failAfter >= 0 && done >= failAfter) {
            if (log)
                log(L"move failed (test): " + from);
            return false;
        }
        ++done;
        return move(from, to, log);
    };
    for (const std::wstring& name : stagedNames(plan.stage)) {
        const std::wstring current = join(plan.app, name);
        if (exists(current)) {
            if (!step(current, join(plan.backup, name))) {
                rollBack(plan, moves, log);
                return false;
            }
            moves.backedUp.push_back(name);
        }
        if (!step(join(plan.stage, name), current)) {
            rollBack(plan, moves, log);
            return false;
        }
        moves.placed.push_back(name);
    }
    if (log)
        log(L"swapped in " + std::to_wstring(moves.placed.size()) + L" entries, backed up "
            + std::to_wstring(moves.backedUp.size()));
    return true;
}

bool rollBack(const SwapPlan& plan, const SwapMoves& moves, const Log& log)
{
    bool ok = true;
    for (auto it = moves.placed.rbegin(); it != moves.placed.rend(); ++it)
        ok = move(join(plan.app, *it), join(plan.stage, *it), log) && ok;
    for (auto it = moves.backedUp.rbegin(); it != moves.backedUp.rend(); ++it)
        ok = move(join(plan.backup, *it), join(plan.app, *it), log) && ok;
    if (log)
        log(ok ? L"rolled back" : L"rollback incomplete");
    return ok;
}

bool removeTree(const std::wstring& folder)
{
    const DWORD attributes = ::GetFileAttributesW(folder.c_str());
    if (attributes == INVALID_FILE_ATTRIBUTES)
        return true;
    if (!(attributes & FILE_ATTRIBUTE_DIRECTORY) || (attributes & FILE_ATTRIBUTE_REPARSE_POINT)) {
        ::SetFileAttributesW(folder.c_str(), FILE_ATTRIBUTE_NORMAL);
        return (attributes & FILE_ATTRIBUTE_DIRECTORY) ? ::RemoveDirectoryW(folder.c_str())
                                                       : ::DeleteFileW(folder.c_str());
    }
    bool ok = true;
    WIN32_FIND_DATAW found;
    const HANDLE find = ::FindFirstFileW(join(folder, L"*").c_str(), &found);
    if (find != INVALID_HANDLE_VALUE) {
        do {
            const std::wstring name = found.cFileName;
            if (name != L"." && name != L"..")
                ok = removeTree(join(folder, name)) && ok; // a junction is removed, not followed
        } while (::FindNextFileW(find, &found));
        ::FindClose(find);
    }
    ::SetFileAttributesW(folder.c_str(), FILE_ATTRIBUTE_DIRECTORY);
    return ::RemoveDirectoryW(folder.c_str()) && ok;
}

} // namespace ws::update
