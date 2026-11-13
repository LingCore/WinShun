// Where the cloud backup plugin (winshun.cloudbackup, GitHub #276) keeps
// Win顺's backups: a folder on a WebDAV server (坚果云, Nextcloud, a NAS,
// alist) or a private GitHub repository, either or both. A backup there is
// one file, "WinShun-2026-11-12-194700-DESKTOP-ABC.zip": the zip Win顺 makes
// (backup_export), named by when it was made and on which computer.
//
// Plain C++ on the SDK's JSON and Http.h; tests/tst_cloudbackup.cpp builds it
// in again and runs it against a local server.
#pragma once

#include "Http.h"
#include "winshun.h"

#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace cb {

struct Settings {
    std::string webdavUrl; // the folder: "https://dav.jianguoyun.com/dav/WinShun/"
    std::string webdavUser;
    std::string webdavPassword;
    std::string githubRepo; // "owner/name", or the repository's address
    std::string githubToken;
    std::string githubApi; // empty: https://api.github.com (GitHub Enterprise: https://host/api/v3)
    int keep = 10; // backups kept at each place; older ones go
    bool automatic = true;

    bool webdav() const { return !webdavUrl.empty(); }
    bool github() const { return !githubRepo.empty() && !githubToken.empty(); }
};
Settings settingsFrom(const winshun::Json& settings);

std::string base64(const std::string& bytes);
// One path segment: everything but unreserved characters as %XX.
std::string urlEncode(const std::string& segment);
std::string urlDecode(const std::string& text);
// An address as typed: spaces and non-ASCII bytes as %XX, the rest as it is.
std::string encodeAddress(const std::string& url);

// "WinShun-2026-11-12-194700-DESKTOP-ABC.zip"; without the computer's name
// when it has no letter or digit that fits a file name everywhere.
std::string backupName(const SYSTEMTIME& local, const std::string& computer);
struct Name {
    std::string when; // "2026-11-12 19:47"
    std::string second; // "03": told only where two of the same minute stand side by side
    std::string computer; // may be empty
};
std::optional<Name> parseName(const std::string& name);

struct Remote {
    std::string name;
    long long size = -1; // bytes; -1: not told
    std::string sha; // GitHub: what a deletion has to name
};

struct Outcome {
    std::string error; // empty: fine; else a sentence for the user
    bool ok() const { return error.empty(); }
};

class Target {
public:
    virtual ~Target() = default;
    virtual std::string key() const = 0; // "webdav" | "github"
    virtual std::string label() const = 0; // "WebDAV" | "GitHub"
    // Which place: another one has none of the backups this one was given.
    virtual std::string id() const = 0;
    // The backups there, newest first (the folder is made when missing).
    virtual Outcome list(std::vector<Remote>& out) = 0;
    virtual Outcome upload(const std::string& name, const std::string& bytes) = 0;
    virtual Outcome download(const Remote& backup, std::string& bytes) = 0;
    virtual Outcome remove(const Remote& backup) = 0;
};

// Null when the settings do not name that place. `canceled` ends a request
// under way (the plugin is told to stop).
std::unique_ptr<Target> webdavTarget(const Settings& settings, bool chinese, const std::atomic_bool* canceled);
std::unique_ptr<Target> githubTarget(const Settings& settings, bool chinese, const std::atomic_bool* canceled);

// A PROPFIND answer's backups (other files and folders are not ours).
std::vector<Remote> webdavListing(const std::string& xml);
struct Repo {
    std::string owner;
    std::string name;
};
// "owner/name", "https://github.com/owner/name", "…/name.git".
std::optional<Repo> parseRepo(const std::string& text);

inline constexpr std::size_t kMaxBackup = 512u << 20; // Win顺's own limit for a backup
inline constexpr std::size_t kMaxGithub = 50u << 20; // the contents API takes the file inside a JSON body

} // namespace cb
