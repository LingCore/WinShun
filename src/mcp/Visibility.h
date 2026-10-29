#pragma once

// What a viewer may see. The AI clients connect with their own rights, and
// the tools show only what that viewer could list, read or open itself. On
// this computer's drives every decision is an AccessCheck of the file's
// security descriptor against the viewer's token: the pipe client's own
// (McpServer takes it with ImpersonateNamedPipeClient), or, for a session
// without one, this process's linked token (the user's unelevated rights,
// should Win顺 run elevated) — never an elevated token.
//
// On a server or in WSL (GitHub #114) no AccessCheck is made: the server
// would be asked for the descriptor (one gone away holds the caller up for
// seconds a path, on the GUI thread), and could not be weighed here anyway
// (a NAS names its own accounts, Samba S-1-22 ids for its Unix ones, a
// WebDAV server has no descriptor at all). What the index has under one of
// its roots there was listed, and read, by Win顺 with this user's own
// credentials on that server: the user could list the root when it was
// indexed, and lists what is in it. So under such a root the viewer sees and
// reads what the index has if it is this process's user, and nothing if it
// is anyone else (another account let in by the pipe, which has other
// credentials there, or none). Any other network path is not there: no
// server the AI names is asked anything, which would sign in to it and hand
// an attacker's server the user's NTLM answer.

#include <QHash>
#include <QString>

#include <windows.h>

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace ws::mcp {

class Visibility {
public:
    // `token`: an impersonation token at identification level or higher;
    // not owned. Null: defaultToken(). `remoteRoots`: the index's roots on
    // servers and in WSL (ShareConfig::path, canonical).
    explicit Visibility(HANDLE token, std::vector<std::wstring> remoteRoots = {});

    // The folder can be listed (FILE_LIST_DIRECTORY): the names in it are
    // the viewer's to see. Cached by folder.
    bool canList(const QString& folder);
    // The file's data (FILE_READ_DATA), its attributes (FILE_READ_ATTRIBUTES).
    bool canRead(const QString& file);
    bool canReadAttributes(const QString& path);
    // The entry is there for the viewer: its folder can be listed (a volume
    // root: itself).
    bool sees(const QString& path);

    int checks() const noexcept { return m_checks; }
    std::int64_t microseconds() const noexcept { return m_us; }

    // Where a path is, for what may be done with it: on a drive by its
    // letter ("C:\…", a mapped drive too), under one of `remoteRoots`
    // (written in any of the forms that name the same folder: unc::canonical,
    // webdav::rootHolds), or anywhere else: another server, a device path
    // ("\\?\…", "\\.\…"), no full path. Reads nothing: no file system call.
    enum class Place : std::uint8_t { Local, Remote, Elsewhere };
    static Place placeOf(const QString& path, const std::vector<std::wstring>& remoteRoots);

    // The token for sessions that bring none: the linked token of this
    // process (the user's normal rights, when it is elevated), else the
    // process's own as an impersonation token. Kept for the process's life;
    // null if neither can be had (then nothing is visible).
    static HANDLE defaultToken();
    // "C:\dir" of "C:\dir\file"; "C:\" of "C:\" and of "C:\dir".
    static QString parentOf(const QString& path);
    // Whether `token` is of this process's own user (its TokenUser SID).
    static bool sameUser(HANDLE token);

private:
    bool check(const QString& path, DWORD desired);

    HANDLE m_token;
    std::vector<std::wstring> m_roots;
    std::optional<bool> m_sameUser; // asked once: the viewer is this process's user
    QHash<QString, bool> m_folders; // case-folded, native
    int m_checks = 0;
    std::int64_t m_us = 0;
};

} // namespace ws::mcp
