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
#include <list>
#include <memory>
#include <optional>
#include <string>
#include <thread>
#include <vector>

namespace ws {

class DocExtractor;
class IndexService;
class SearchEngine;

namespace mcp {

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
    QStringList excludedFileNames; // ... and file names (Settings::excludedFileNames)
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
};

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
    // is cancelled first. Searches run one at a time: a call waits for the
    // one before it. `client`: for the log. `token`: the caller's, for what
    // it may see (Visibility); null for the default, the user's normal rights.
    using Done = std::function<void(ToolResult)>;
    void call(std::uint64_t id, const QString& client, const QString& tool, const QJsonObject& arguments, Done done,
        HANDLE token = nullptr);
    void cancel(std::uint64_t id); // queued or running: done() is not called
    int pendingCalls() const;

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
    struct Mailbox;
    void addTools();
    // The index's roots on servers and in WSL, canonical (Visibility).
    std::vector<std::wstring> remoteRoots() const;
    // A call's work with the disk or a server, off the GUI thread: one slow
    // to answer (a share gone away: half a minute) holds up the call, not
    // Win顺. `work(closing)` runs on a thread of its own that nothing waits
    // for, holding only what it is given; then `then(answer)` runs here,
    // unless the call was cancelled. The call is among m_others meanwhile.
    template <typename Work, typename Then> void offThread(const std::shared_ptr<Call>& call, Work work, Then then);
    void start(std::shared_ptr<Call> call);
    void finish(const std::shared_ptr<Call>& call, ToolResult result);
    void next(); // the next queued search, if any
    void onResults(quint64 requestId, const SearchResults& results, qint64 totalMatches, qint64 elapsedUs);
    void onContentResults(quint64 requestId, const SearchResults& results);
    void onContentProgress(quint64 requestId, int scanned, int total, bool finished);
    void finishContent(bool complete);
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
    std::deque<std::shared_ptr<Call>> m_queue; // searches waiting for the engine
    std::unique_ptr<Active> m_active; // the search running on the engine
    std::vector<std::shared_ptr<Call>> m_others; // calls under way that need no engine (opening, reading)
    QTimer m_contentTimeout;
    // read_file_text: documents the content index has no text of. Shared
    // with the reads under way, which outlive the host (offThread).
    std::shared_ptr<DocExtractor> m_extractor;
    std::shared_ptr<Mailbox> m_mailbox;
};

} // namespace mcp
} // namespace ws
