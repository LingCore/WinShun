#pragma once

// The tools Win顺 offers AI clients (MCP "tools"): what each takes and
// gives (JSON Schema), what it may do (annotations), and the switch in the
// settings that allows it (Gate). ToolHost runs them on the search engine
// and the index, one search at a time.
//
// This is the layer a plugin system can share later: a tool is a name, a
// schema, a gate and a handler; what carries the call (MCP over the pipe
// today) is McpServer's business.

#include "ContentIndex.h"
#include "SearchTypes.h"
#include "WorkerPool.h"

#include <QJsonArray>
#include <QJsonObject>
#include <QObject>
#include <QString>
#include <QStringList>
#include <QTimer>

#include <windows.h>

#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <stop_token>
#include <vector>

namespace ws {

class DocExtractor;
class IndexService;
class SearchEngine;

namespace mcp {

class AsideWorkers;
struct AsideContext;
class Visibility;

// Whether AI may change Win顺's settings (change_settings): not at all, each
// change asked of the user on the screen, or at once with the user told
// afterwards (and able to undo it). The settings' [Mcp] Settings.
enum class SettingsMode { Off, Ask, Auto };
inline const char* settingsModeName(SettingsMode mode) noexcept
{
    return mode == SettingsMode::Auto ? "auto" : mode == SettingsMode::Ask ? "ask" : "off";
}

// What the user allows and how searches are set up (from the settings).
struct Policy {
    bool allowOpen = false; // open_item, show_in_folder
    bool allowClipboard = false; // clipboard_history, clipboard_get, screenshot_history
    // Win顺 keeps a clipboard history at all (设置 → 剪贴板 → 记录剪贴板历史):
    // without it the clipboard tools are off too, whatever allowClipboard says.
    bool clipboardHistory = true;
    // take_screenshot; even so every call waits for the user to allow it on
    // the screen (Services::takeScreenshot).
    bool allowScreenshot = false;
    // Win顺's screen capture is on: screenshot_history lists nothing while
    // it is off (and says why).
    bool captureEnabled = false;
    bool allowDocuments = false; // list_documents: for plugins (winshun:content), not AI clients
    SettingsMode settingsMode = SettingsMode::Off; // change_settings
    QStringList excludedPaths; // folders AI never sees (%VARS% expanded by setPolicy)
    bool content = true; // search_content: off with the launcher's content search
    QStringList contentExtensions; // as the launcher's content search
    ContentSizeLimits contentSizeLimits;
    bool contentDocuments = true;
    bool contentInLowPriority = false;
    QStringList excludedTypes; // file types left out of searches, as the launcher's (Settings::excludedTypes)
    KindOrder kindOrder = KindOrder::FilesFirst;

    bool operator==(const Policy&) const = default;
};

// The clipboard history, as the app answers for it (GUI thread).
struct ClipQuery {
    QString query; // words, all of them; "@program" for the source
    QString kind; // all | text | link | path | files | image
    int limit = 20;
    int maxChars = 500; // of each text
};
struct ClipSummary {
    qint64 id = 0;
    QString kind; // text | link | path | files | image
    QString text; // the first maxChars characters; Files: one path per line
    qint64 chars = 0; // of the whole text
    qint64 created = 0; // ms since the epoch
    qint64 used = 0;
    QString source; // the program it was copied from
    QString group; // the group it is in; empty for none
    bool pinned = false;
    int width = 0; // Image
    int height = 0;
};

// A picture Win顺's screen capture took, from the clipboard history (the
// screenshot history: the launcher's jt). GUI thread.
struct ScreenshotSummary {
    qint64 id = 0;
    qint64 created = 0; // ms since the epoch: when it was taken
    int width = 0;
    int height = 0;
    QString ocr; // the words read in it; empty: none, or not read yet
    QString imagePath; // its PNG in the history; empty while still being written
};

// take_screenshot: asked of the user on the screen (App::confirmScreenshot).
struct ScreenshotRequest {
    QString client; // who asks, as it named itself
    int monitor = 0; // 1: the primary monitor, then the others; 0: the one with the mouse pointer
};
struct ScreenshotTaken {
    QString error; // empty: taken
    QString path; // a PNG of its own in the temporary folder, never in the clipboard history
    int width = 0;
    int height = 0;
    QString monitor; // which, in words
};

struct ToolResult {
    QJsonObject structured; // the result (structuredContent), also serialized as the text
    bool isError = false;
    QString errorText; // isError: what went wrong, for the model
    // isError: the kind of failure, for a program that acts on it (ws's exit
    // codes; its sessions get it as structuredContent.error). One of
    // kErrorNotFound, kErrorOff, kErrorTooLarge; empty for the others.
    QString errorCode;
    // The MCP result as it goes out (callToolResult as compact JSON), made
    // where the work was done, off the GUI thread, for a call that asked
    // for it (CallOptions::encode); `structured` then keeps only what the
    // call's log line reads. Empty: McpServer makes it.
    QByteArray encoded;
    // The answer would have been this many bytes, more than one message
    // holds: this is the too_large error in its place (tooLargeResult).
    qint64 tooLargeBytes = 0;
};

// No such path, or one the caller may not see (not listable, or excluded for
// AI): to the caller these are the same.
inline constexpr const char* kErrorNotFound = "not_found";
// The tool's switch in the settings is off.
inline constexpr const char* kErrorOff = "off";
// The answer would not fit in one message (pipe::kMaxLineBytes).
inline constexpr const char* kErrorTooLarge = "too_large";

// A tool's result as a session gets it (MCP's CallToolResult). An AI client
// gets the words of an error, and a result also as text; ws (`cli`) gets an
// error's kind (structuredContent.error) and a result once, without the text.
QJsonObject callToolResult(const ToolResult& result, bool cli);
// The error in place of an answer of `bytes`, more than one message holds.
ToolResult tooLargeResult(qint64 bytes);

// ---- the settings tools (the app answers: src/app/AiSettings) ---------------------------------

// get_settings: what to list.
struct SettingsQuery {
    QStringList sections; // ids; empty: all
    QStringList keys; // empty: all
    QString words; // all of them in the key, title or description
    bool changedOnly = false; // only what differs from the defaults
    bool facts = false; // and what Win顺 knows of this PC that bears on the settings
};

// One change as the app worked it out.
struct SettingsPlanItem {
    QString key;
    bool changed = false; // false: it is so already
    bool askAlways = false; // the user is asked even when AI may change settings by itself
    QJsonValue newValue; // what the setting becomes
    QJsonObject shown; // for the confirmation card: title, old_text, new_text, removed_text, added_text …
};

// What the app made of the changes asked for.
struct SettingsPlan {
    QStringList problems; // any: nothing is to be applied
    std::vector<SettingsPlanItem> items; // in the order asked
};

// How the user answered the card that asks (App::confirmSettingsChange). The
// values are those of the card's own signal (SettingsConfirm.qml).
enum class SettingsAnswer : int {
    Busy = -3, // another card is waiting
    Unavailable = -2, // it could not be shown
    Timeout = -1, // no answer in time
    Refused = 0,
    Allowed = 1,
};

// What the user was shown for one setting, and so what is allowed to happen.
struct ApprovedChange {
    QString key;
    QJsonValue value;
};

// The card that asks the user.
struct SettingsAsk {
    QString client; // who asks, as it named itself
    QString reason; // the AI's own words, one line, at most 200 characters
    QJsonArray items; // the changes that are to be made (the plan's `shown` of those that changed)
    bool askAlways = false; // one of them asks even in auto mode: no "from now on" box
};

// To apply, once the user has agreed (or needs not): the app plans again
// against the settings as they are by then, so the card may have waited, and
// applies only what comes to what `approved` says.
struct SettingsApply {
    QString client;
    QString reason;
    QJsonArray changes; // as asked
    std::vector<ApprovedChange> approved; // every change that was to be made, as the user was shown it
    bool asked = false; // the user answered a card; else the mode was auto and nothing asked always
    bool alwaysAllow = false; // the user ticked "from now on" on a card with nothing that asks always
};

// Text of AI's (a reason, a client's name) as it may be shown to the user: on
// one line, without control or invisible formatting characters (line breaks,
// direction marks), cut to `maxChars`.
QString cleanShownText(const QString& text, qsizetype maxChars);

// What the host needs from the app. Those left empty take their tools away
// (wsbench has no shell and no clipboard).
struct Services {
    IndexService* index = nullptr; // required
    SearchEngine* engine = nullptr; // required, and the host's own: an engine runs only its newest request
    // Content searches' own engine (the AI host's), also the host's alone: a
    // name search (milliseconds) then never waits behind a content search
    // (seconds). Null: content searches queue on `engine` with the rest
    // (the plugins' host).
    SearchEngine* contentEngine = nullptr;
    QString version; // of Win顺, for index_status
    std::function<QStringList()> history; // recent items, newest first, for the ranking; may be empty
    // Opens a file or folder with the user's normal rights, shows one in its
    // folder; `done` may run on any thread.
    std::function<void(const QString& path, std::function<void(bool)> done)> open;
    std::function<void(const QString& path, std::function<void(bool)> done)> reveal;
    std::function<std::vector<ClipSummary>(const ClipQuery&)> clipboard;
    std::function<std::optional<QString>(qint64 id)> clipboardText; // the whole text; nothing for no such entry
    // The screenshot history, newest first, `query` in the words read in
    // them (pinyin too), at most `limit`.
    std::function<std::vector<ScreenshotSummary>(const QString& query, int limit)> screenshots;
    // Shows the user who asks and for which monitor; only 允许 takes the
    // picture. `done` once, on any thread: the PNG, or why not (refused,
    // no answer in time, another request waiting). Only the AI host's:
    // plugins never take screenshots.
    std::function<void(const ScreenshotRequest&, std::function<void(ScreenshotTaken)> done)> takeScreenshot;
    // The settings tools, the AI's alone (plugins never change settings).
    // getSettings, planSettings and openSettings answer at once, on the GUI
    // thread. confirmSettings shows the card; `done` once, with an answer
    // and whether the user ticked "from now on";
    // cancelSettings takes the card away without an answer (the client
    // gave the call up). applySettings makes the changes and says what came
    // of each; `done` once, on the GUI thread.
    std::function<ToolResult(const SettingsQuery&)> getSettings;
    std::function<SettingsPlan(const QJsonArray& changes)> planSettings;
    std::function<void(const SettingsAsk&, std::function<void(SettingsAnswer answer, bool always)> done)> confirmSettings;
    std::function<void()> cancelSettings;
    std::function<void(const SettingsApply&, std::function<void(ToolResult)> done)> applySettings;
    // Opens the settings window at a setting (its key) or a section (its
    // id) and brings it to the front.
    std::function<ToolResult(const QString& key, const QString& section)> openSettings;
    // Where screenshot_history puts its copies of the pictures; empty:
    // %TEMP%\WinShun\mcp.
    QString tempFolder;
    std::function<void(const QString& line)> log; // one line per call
    // Whether a path is on a network share that cannot be reached now; any
    // thread. Empty: the index's shares say (ShareIndexer::offline).
    std::function<bool(const QString& path)> offline;
    // Tests: called on the thread of each job aside as it begins (the tool's
    // name, and the job's stop token), which may hold it there.
    std::function<void(const QString& tool, const std::stop_token& stop)> asideStarting;
};

// A tool from outside the host: a plugin's command (plugin::AiBridge, the
// plugin design §6.2). Run by its own handler, not on the engine; listed
// while `offered`, else known but off with `offReason` for the model.
struct ExternalTool {
    QString name; // [a-z0-9_]
    QString title;
    QString description;
    QJsonObject inputSchema;
    QJsonObject outputSchema; // may be empty
    bool readOnly = true; // annotations: readOnlyHint; else destructiveHint
    bool openWorld = false; // annotations: openWorldHint (it may reach the network)
    bool offered = true;
    QString offReason; // offered = false: what to tell the model ("the plugin is turned off…")
    // Runs it; `done` once, on the host's thread, unless cancelled first.
    std::function<void(std::uint64_t callId, const QJsonObject& arguments, std::function<void(ToolResult)> done)> run;
    std::function<void(std::uint64_t callId)> cancel; // may be empty
};

// Who makes a call, as far as the answer depends on it.
struct CallOptions {
    // The caller's token, for what it may see (Visibility); null for the
    // default, the user's normal rights. Not owned: the host copies it for
    // work it does on other threads, which may outlive the caller's.
    HANDLE token = nullptr;
    // ws, the command line (its answers go to a program, not into a model's
    // context): higher limits (50 000 names), and items without the name the
    // path ends with. Formats and limits only, never what may be seen.
    bool cli = false;
    // The answer goes out as it is (McpServer): made into its JSON where the
    // work is done aside (ToolResult::encoded), not on this thread.
    bool encode = false;
};

class ToolHost : public QObject {
    Q_OBJECT

public:
    enum class Known { No, Off, Yes }; // no such tool; one its switch keeps off; one to call

    explicit ToolHost(Services services, QObject* parent = nullptr);
    ~ToolHost() override;

    void setPolicy(Policy policy); // toolsChanged() when the tools on offer differ
    const Policy& policy() const noexcept { return m_policy; }
    bool excluded(const QString& path) const; // under a folder AI never sees

    QJsonArray tools() const; // MCP tool objects, the ones on offer
    QStringList toolNames() const; // likewise, the names
    Known known(const QString& name) const;
    // Known::Off: the tool's own words on why, when it has any (an external
    // tool's offReason, change_settings' switch); empty for the others.
    QString offReason(const QString& name) const;
    // The external tools, all of them at once (the earlier ones go; a call
    // under way on one of them finishes). toolsChanged() when what is on
    // offer differs.
    void setExternalTools(std::vector<ExternalTool> tools);

    // Runs a tool. `done` runs on this object's thread, once, unless the call
    // is cancelled first. Searches of a kind run on their engine one at a
    // time (names on the engine, content on its own, Services::contentEngine,
    // so neither waits for the other); what comes of each is then made aside
    // while the next one searches, so answers may come in another order than
    // the calls. `client`: for the log.
    using Done = std::function<void(ToolResult)>;
    void call(std::uint64_t id, const QString& client, const QString& tool, const QJsonObject& arguments, Done done,
        CallOptions options = {});
    void cancel(std::uint64_t id); // queued or running: done() is not called
    int pendingCalls() const;
    void log(const QString& line) const; // a line of the server's own in the call log

    // For the initialize result: how to search with Win顺.
    static QString instructions();
    // The prompt "setup" (prompts/get): asks the model to find out how the
    // user works and propose settings; `focus` narrows it to one area.
    static QString setupPrompt(const QString& focus);

signals:
    void toolsChanged();

private:
    struct Tool;
    struct Call;
    struct Active;
    struct Lane;
    // Work of a call off this thread.
    using AsideWork = std::function<ToolResult(AsideContext& aside)>;
    void addTools();
    std::unique_ptr<Lane> makeLane(SearchEngine* engine);
    Lane& laneOf(const Tool& tool);
    void start(std::shared_ptr<Call> call);
    void finish(const std::shared_ptr<Call>& call, ToolResult result);
    void next(Lane& lane); // the next queued search, if any
    // Runs `work` on m_workers, then `back` here with what came of it.
    // `answer`: that is the call's answer (made into JSON there when the
    // call asked for it, CallOptions::encode); false: back() goes on with it.
    void aside(const std::shared_ptr<Call>& call, AsideWork work, std::function<void(ToolResult)> back, bool answer = true);
    void runAside(const std::shared_ptr<Call>& call, AsideWork work); // answered by `work`, among m_others meanwhile
    void finishSearch(Lane& lane, AsideWork work);
    void expireQueued(Lane& lane, const std::weak_ptr<Call>& call); // search_content's timeout ran out while it waited
    void onResults(Lane& lane, quint64 requestId, const SearchResults& results, qint64 totalMatches, qint64 elapsedUs);
    void onContentResults(Lane& lane, quint64 requestId, const SearchResults& results);
    void onContentProgress(Lane& lane, quint64 requestId, int scanned, int total, bool finished);
    void finishContent(Lane& lane, bool complete);
    void logCall(const Call& call, const ToolResult& result) const;

    // The tools.
    void searchFiles(const std::shared_ptr<Call>& call);
    void searchContent(const std::shared_ptr<Call>& call);
    void recentFiles(const std::shared_ptr<Call>& call);
    void listFolder(const std::shared_ptr<Call>& call);
    void fileInfo(const std::shared_ptr<Call>& call);
    void openItem(const std::shared_ptr<Call>& call, bool reveal);
    void clipboardHistory(const std::shared_ptr<Call>& call);
    void clipboardGet(const std::shared_ptr<Call>& call);
    void screenshotHistory(const std::shared_ptr<Call>& call);
    void takeScreenshot(const std::shared_ptr<Call>& call);
    QString tempFolder() const;
    void indexStatus(const std::shared_ptr<Call>& call);
    void readFileText(const std::shared_ptr<Call>& call);
    void listDocuments(const std::shared_ptr<Call>& call);
    void getSettings(const std::shared_ptr<Call>& call);
    void changeSettings(const std::shared_ptr<Call>& call);
    void applySettings(const std::shared_ptr<Call>& call, SettingsApply apply);
    void openSettings(const std::shared_ptr<Call>& call);

    const Tool* find(const QString& name) const; // among the host's own and the external ones

    Services m_services;
    Policy m_policy;
    std::vector<QString> m_excluded; // case-folded, native, with a trailing separator
    std::vector<Tool> m_tools;
    std::vector<std::shared_ptr<Tool>> m_external; // shared: a call keeps its tool while the list is replaced
    WorkerPool m_pool {WorkerPool::defaultThreadCount()}; // recent_files scans the index
    // The access checks of many folders at once (AsideContext::seeAhead).
    // Not m_pool: a scan waits for that one holding the index's lock.
    WorkerPool m_checks {WorkerPool::defaultThreadCount()};
    // One scan of the index at a time (recent_files), taken before the
    // index's lock: none waits for the pool (one loop at a time) holding it.
    std::mutex m_scanMutex;
    std::unique_ptr<Lane> m_names; // name searches, and content searches too without an engine of their own
    std::unique_ptr<Lane> m_contents; // content searches on Services::contentEngine; null without one
    // Calls under way off the engines: opening, reading, and every call's
    // work aside, a search's once its engine answered.
    std::vector<std::shared_ptr<Call>> m_others;
    std::unique_ptr<DocExtractor> m_extractor; // read_file_text: documents the content index has no text of
    // The work aside (aside()). Last: stopped and joined before the rest goes.
    std::unique_ptr<AsideWorkers> m_workers;
};

} // namespace mcp
} // namespace ws
