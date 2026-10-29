#pragma once

#include "AppCatalog.h"
#include "ContentIndex.h"
#include "Favorites.h"
#include "LauncherNavigation.h"
#include "ResultModel.h"
#include "PanelColumns.h"
#include "SearchEngine.h"
#include "SearchTypes.h"
#include "WebShortcut.h"
#include "WebSuggester.h"

#include <QElapsedTimer>
#include <QObject>
#include <QPointer>
#include <QRectF>
#include <QSet>
#include <QVariantList>
#include <QTimer>
#include <QWindow>
#include <QtQml/qqmlregistration.h>

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <utility>
#include <vector>

namespace ws {

class FilePreview;
class History;
class IndexService;
class Switches;
class SystemCatalog;
namespace plugin {
class Host;
class Merge;
struct Effect;
struct Item;
struct Open;
}
namespace share {
enum class Reach : std::uint8_t; // ShareReach.h
}

// The view-model behind the search window: query and scope in, results and
// status out, plus the actions on a result (open, reveal, copy, ...).
class Launcher : public QObject {
    Q_OBJECT
    QML_ELEMENT
    QML_UNCREATABLE("Provided by the application")
    Q_PROPERTY(QString query READ query WRITE setQuery NOTIFY queryChanged FINAL)
    Q_PROPERTY(Scope scope READ scope WRITE setScope NOTIFY scopeChanged FINAL)
    Q_PROPERTY(ws::ResultModel* results READ results CONSTANT FINAL)
    Q_PROPERTY(ws::PanelColumns* panelColumns READ panelColumns CONSTANT FINAL)
    Q_PROPERTY(QString statusText READ statusText NOTIFY statusChanged FINAL)
    Q_PROPERTY(bool busy READ busy NOTIFY statusChanged FINAL)
    // Scrolled to the end, there is more to list (loadMore()): names beyond
    // those listed, a content scan stopped at its limit, more files written
    // lately.
    Q_PROPERTY(bool moreAvailable READ moreAvailable NOTIFY statusChanged FINAL)
    // What the search behind the rows passed over, once it is done, each with
    // a one-off action that searches it for this query alone (nothing is
    // saved): [{kind, text, action, running}], for the rows after the last
    // result (or the lines under 没有找到). Files over the size limit (kind
    // "large"), in 内容 the files in system and program folders ("system")
    // and the text of kinds not listed ("types"), and a removable drive the
    // index leaves out ("drive:E:"). While one runs, it alone, with how far
    // it is and 停止 (searchLeftOut, stopLeftOut).
    Q_PROPERTY(QVariantList leftOutHints READ leftOutHints NOTIFY statusChanged FINAL)
    // The rows were found by how their names read, not as typed
    // (SearchResult::approximate): the heading over them, "没有叫“年度报高”的，
    // 按读音找到："; empty otherwise.
    Q_PROPERTY(QString approximateHeading READ approximateHeading NOTIFY statusChanged FINAL)
    // How far the content scan is, 0–1 (the footer's divider fills); -1 while
    // that is not known (busy: the divider shows work going on).
    Q_PROPERTY(qreal progress READ progress NOTIFY statusChanged FINAL)
    Q_PROPERTY(bool searching READ searching NOTIFY statusChanged FINAL)
    Q_PROPERTY(bool stale READ stale NOTIFY statusChanged FINAL)
    Q_PROPERTY(QString placeholder READ placeholder NOTIFY scopeChanged FINAL)
    Q_PROPERTY(QString contentFilesLabel READ contentFilesLabel NOTIFY scopeChanged FINAL)
    // File content search is on (Settings::contentSearch): the 内容 tab is there.
    Q_PROPERTY(bool contentEnabled READ contentEnabled NOTIFY scopeChanged FINAL)
    // The content index is built (Settings::contentIndex): the 内容 strip's switch.
    Q_PROPERTY(bool contentIndex READ contentIndex NOTIFY contentIndexChanged FINAL)
    Q_PROPERTY(bool searchesContent READ searchesContent NOTIFY statusChanged FINAL)
    // History is kept and has something in it (the footer's clear button).
    Q_PROPERTY(bool canClearHistory READ canClearHistory NOTIFY historyChanged FINAL)
    // The apps opened most, for the launcher over the taskbar with nothing
    // typed: for each {name, icon}. Read again each time it is shown; none
    // while history is not kept.
    Q_PROPERTY(QVariantList frequentApps READ frequentApps NOTIFY frequentAppsChanged FINAL)
    // Files and folders, and content matches: best match first, or by when
    // last written (the footer's switch, Ctrl+S). 全部 keeps its apps,
    // places and web shortcuts first.
    Q_PROPERTY(Sort sort READ sort WRITE setSort NOTIFY sortChanged FINAL)
    // With nothing typed: what was opened last, or the files written last
    // (the footer's switch there, Ctrl+S).
    Q_PROPERTY(StartList startList READ startList WRITE setStartList NOTIFY startListChanged FINAL)
    // Rows say when each file or folder was last written.
    Q_PROPERTY(bool showModified READ showModified NOTIFY resultOptionsChanged FINAL)
    Q_PROPERTY(bool showFileSize READ showFileSize NOTIFY resultOptionsChanged FINAL)
    Q_PROPERTY(QString folderKeyword READ folderKeyword NOTIFY fileTypesChanged FINAL)
    // A click on a row only selects it; a double click (or Enter) opens it
    // (Settings::doubleClickToOpen, GitHub #44).
    Q_PROPERTY(bool doubleClickToOpen READ doubleClickToOpen NOTIFY resultOptionsChanged FINAL)
    // The modifier that opens the nth row shown with a digit: "ctrl", "alt",
    // or "" for none (Settings::numberKeys).
    Q_PROPERTY(QString numberKeys READ numberKeys NOTIFY resultOptionsChanged FINAL)
    Q_PROPERTY(QString numberHints READ numberHints NOTIFY resultOptionsChanged FINAL)
    // Bits 1 << n of the digits n left to another program's global hotkey
    // (numberkeys::heldMask): their rows show the chord struck through.
    Q_PROPERTY(int numberKeysLeft READ numberKeysLeft NOTIFY resultOptionsChanged FINAL)
    // Opened over Win顺's search box on the taskbar (taskbar::SearchBox):
    // what is typed shows there, its own field is out of sight (SearchBar.qml
    // reports the field, the app draws it in the box).
    Q_PROPERTY(bool inTaskbarBox READ inTaskbarBox NOTIFY inTaskbarBoxChanged FINAL)
    // The row the keyboard is on (Main.qml's), -1 for none: content matches
    // coming in are placed by how well they match, but only below it.
    Q_PROPERTY(int currentRow READ currentRow WRITE setCurrentRow NOTIFY currentRowChanged FINAL)
    // The footer's key hints for the row the keyboard is on: what Enter
    // does ("打开", a plugin action's name; empty for a row Enter does
    // nothing on), what the second key does with its name ("Ctrl+Enter" /
    // "打开位置"; empty when the row has no location or second action),
    // and what Tab does ("切换范围", "补全"; empty when nothing), with the
    // keys it is written with in that place ("Tab"; on the volume's row
    // "Alt+←→" / "调音量").
    Q_PROPERTY(QVariantMap keyHints READ keyHints NOTIFY keyHintsChanged FINAL)
    // 文件 narrowed to a kind of file, as the strip under the search box
    // shows it: its index in fileTypes, or -1 for any (不限; 文件夹 is the
    // scope Folders). Kept while in another scope: 文件 comes back as it
    // was left, 文件夹 too (fileChoice, remembered between runs).
    Q_PROPERTY(int fileType READ fileType NOTIFY fileTypeChanged FINAL)
    Q_PROPERTY(QList<int> selectedFileTypes READ selectedFileTypes NOTIFY fileTypeChanged FINAL)
    Q_PROPERTY(QString queryError READ queryError NOTIFY queryErrorChanged FINAL)
    Q_PROPERTY(bool gallery READ gallery WRITE setGallery NOTIFY galleryChanged FINAL)
    Q_PROPERTY(int thumbnailSize READ thumbnailSize WRITE setThumbnailSize NOTIFY galleryChanged FINAL)
    // The kinds as the settings have them: [{name, extensions: "jpg png …"}].
    Q_PROPERTY(QVariantList fileTypes READ fileTypes NOTIFY fileTypesChanged FINAL)
    // Plugins (plugin::Host, the plugin design §4.1): a keyword typed first
    // ("kill note") gives the list to that command. The keyword as typed,
    // empty otherwise; the command's title and icon for the chip in the
    // scope tabs; its hint, shown after the keyword while nothing follows it.
    Q_PROPERTY(QString pluginKeyword READ pluginKeyword NOTIFY pluginChanged FINAL)
    Q_PROPERTY(QString pluginLabel READ pluginLabel NOTIFY pluginChanged FINAL)
    Q_PROPERTY(QString pluginIcon READ pluginIcon NOTIFY pluginChanged FINAL)
    Q_PROPERTY(QString pluginHint READ pluginHint NOTIFY pluginChanged FINAL)
    // In keyword mode, once the plugin takes a while: "正在等 结束进程…" (or
    // what it says it is doing); empty before that and otherwise.
    Q_PROPERTY(QString pluginWaiting READ pluginWaiting NOTIFY statusChanged FINAL)
    // The screenshot history's keyword mode (jt, setScreenshots): Delete
    // removes the row, Ctrl+Z puts it back (Main.qml).
    Q_PROPERTY(bool screenshotMode READ screenshotMode NOTIFY pluginChanged FINAL)
    // Its user keeps favorites (Favorites): listed first with nothing typed.
    Q_PROPERTY(bool hasFavorites READ hasFavorites NOTIFY favoritesChanged FINAL)
    // 内容 in some folders, as the strip under the search box shows it (in
    // place of the kinds of 文件): [{type, name, tip, dot}], `type` the key
    // chooseFolderChip takes. With a folder named among the words ("报告
    // abc/"): every folder so named ("所有 abc · 3"), then each of them;
    // otherwise 不限, the favorite folders, the folder chosen. folderChip:
    // the current one's key. folderHint: a few words after them, how to
    // name one.
    Q_PROPERTY(QVariantList folderChips READ folderChips NOTIFY folderChipsChanged FINAL)
    Q_PROPERTY(QString folderChip READ folderChip NOTIFY folderChipsChanged FINAL)
    Q_PROPERTY(QString folderHint READ folderHint NOTIFY folderChipsChanged FINAL)
    // 内容: the words looked for, without the folders named among them, and
    // where it looks ("abc", "D:\Projects"; empty: everywhere), for the
    // empty state; whereMissing: that place is not in the index (or on a share).
    Q_PROPERTY(QString contentWords READ contentWords NOTIFY folderChipsChanged FINAL)
    Q_PROPERTY(QString contentWhere READ contentWhere NOTIFY folderChipsChanged FINAL)
    Q_PROPERTY(QString whereMissing READ whereMissing NOTIFY folderChipsChanged FINAL)
    // The search panel (the forum's #209, Listary's): the same window grown
    // into a panel, filters down its left and the rows in columns (Main.qml,
    // Panel.qml), on Ctrl+E or the button at the box's corner, and back the
    // same way. The app sizes the window (App::setPanel) and then says so
    // here; togglePanel() asks it to.
    Q_PROPERTY(bool panel READ panel NOTIFY panelChanged FINAL)
    // Pinned (Ctrl+T, the pin at the box's corner; linux.do #157): it stays
    // up when it loses the focus or a result is opened, until Esc (or double
    // Ctrl while it is in front) hides it. Kept while hidden, until turned
    // off or the program restarts. The app does the rest (App.cpp).
    Q_PROPERTY(bool pinned READ pinned WRITE setPinned NOTIFY pinnedChanged FINAL)
    // Its filters, applied in the panel alone (the box is the plain quick
    // search): when written (dateChoice: 0 不限, 1 今天, 2 最近 7 天, 3 最近
    // 30 天, 4 今年, 5 自定义 with dateFrom and dateTo as "yyyy-MM-dd", either
    // blank for no bound), how large (sizeChoice: 0 不限, 1 under 1 MB, 2
    // 1–100 MB, 3 100 MB–1 GB, 4 over 1 GB, 5 自定义 with sizeMin and
    // sizeMax in MB, blank for no bound), where (folderFilter: a folder's
    // full path, or empty for anywhere; 内容 looks there too). A bound on
    // the time or the size leaves out apps and places, and what has none.
    Q_PROPERTY(int dateChoice READ dateChoice NOTIFY filtersChanged FINAL)
    Q_PROPERTY(QString dateFrom READ dateFrom NOTIFY filtersChanged FINAL)
    Q_PROPERTY(QString dateTo READ dateTo NOTIFY filtersChanged FINAL)
    Q_PROPERTY(int sizeChoice READ sizeChoice NOTIFY filtersChanged FINAL)
    Q_PROPERTY(QString sizeMin READ sizeMin NOTIFY filtersChanged FINAL)
    Q_PROPERTY(QString sizeMax READ sizeMax NOTIFY filtersChanged FINAL)
    Q_PROPERTY(QString folderFilter READ folderFilter NOTIFY filtersChanged FINAL)
    Q_PROPERTY(bool filtersActive READ filtersActive NOTIFY filtersChanged FINAL)
    // The favorite folders (Favorites), for 位置: [{name, path}].
    Q_PROPERTY(QVariantList favoriteFolders READ favoriteFolders NOTIFY favoritesChanged FINAL)
    // The panel's order: a column (ws::SortKey: 0 as ranked, 1 name, 2 path,
    // 3 size, 4 modified) and its direction (sortBy).
    Q_PROPERTY(int panelSortKey READ panelSortKey NOTIFY panelSortChanged FINAL)
    Q_PROPERTY(bool panelSortDescending READ panelSortDescending NOTIFY panelSortChanged FINAL)
    // How many files and folders the last name search found in all (the
    // panel's footer says so); 0 while not known.
    Q_PROPERTY(int totalMatches READ totalMatches NOTIFY statusChanged FINAL)
    // Typed in a folder's window (setTypingFolder, GitHub #110): that
    // folder's name ("work", "D:"), while the box still starts with it; empty
    // otherwise. typingFolderOnly: only that folder is searched this time
    // (FolderTyping, or switchTypingFolder); else what is in it comes first,
    // then the rest, under a caption (ResultModel::elsewhereRow).
    Q_PROPERTY(QString typingFolder READ typingFolder NOTIFY typingFolderChanged FINAL)
    Q_PROPERTY(bool typingFolderOnly READ typingFolderOnly NOTIFY typingFolderChanged FINAL)

public:
    // As ws::Scope. 文件夹 is shown within 文件, as a chip of its strip
    // (chooseFileType): Tab goes 全部, 文件, 内容 (cycleScope; Shift+Tab back), Ctrl+Tab
    // through the strip under the box (cycleFileType, cycleFolderChip).
    enum Scope { All, Files, Content, Folders };
    Q_ENUM(Scope)
    // Alternate: Shift+Enter, a plugin row's third action (Open elsewhere).
    // FavoriteFolder: the folder a file is in into the favorites.
    // EditFavorite: the settings' 收藏夹 page, at the row's favorite.
    // OpenTerminal: a command prompt in a folder, or in the folder a file is in.
    // PluginAction + n: the nth of a plugin row's actions (its context menu).
    // PluginFileAction + n: the nth of the plugins' `files` commands for a
    // file's or folder's row, or the selection (Catalog::fileCommands; #113).
    enum Action {
        Open,
        Reveal,
        RunAsAdmin,
        CopyPath,
        CopyName,
        CopyItem,
        Recycle,
        ForgetRecent,
        EditWebShortcuts,
        Alternate,
        AddFavorite,
        RemoveFavorite,
        FavoriteFolder,
        EditFavorite,
        OpenTerminal,
        SearchInFolder, // 内容 in a folder's row (the strip shows it chosen)
        SystemMenu, // the shell's own menu for the row (更多选项 in Win顺's; see showSystemMenu)
        Preview, // the file or folder in a preview (Alt+P, togglePreview)
        PinPicture, // 贴到屏幕: a picture file's row, a screenshot's (pinRequested)
        RemoveScreenshot, // 从历史删除: a screenshot's row (Delete)
        SwitchOn, // a system switch's row, set on / off (the menu's two entries; Enter is the row's own)
        SwitchOff,
        EditPicture, // 编辑: a screenshot's row, in the editing window (Shift+Enter)
        BrowseFolder, // list a folder in Win顺, then keep typing to search there
        PluginAction = 100,
        PluginFileAction = 200
    };
    Q_ENUM(Action)
    // The first three as ws::RankBy and ws::StartList; the rest are a
    // ws::SortOrder over the names (#44), the content matches staying as ranked.
    enum Sort { BestMatch, NewestFirst, OldestFirst, ByName, ByFolder, ByType, LargestFirst, SmallestFirst };
    Q_ENUM(Sort)
    enum StartList { RecentlyOpened, RecentlyModified, NoStartList }; // none: only the favorites
    Q_ENUM(StartList)

    Launcher(IndexService* index, AppCatalog* apps, SystemCatalog* places, SearchEngine* engine, History* history,
        QObject* parent = nullptr);
    ~Launcher() override;

    // The plugins (may be none): every query goes to them too (全部).
    void setPluginHost(plugin::Host* host);
    QString pluginKeyword() const;
    QString pluginLabel() const;
    QString pluginIcon() const;
    QString pluginHint() const;
    QString pluginWaiting() const;
    // Esc in keyword mode: the words after the keyword go, the keyword
    // stays (true); nothing to clear: false, and Esc closes the window.
    Q_INVOKABLE bool clearToKeyword();
    // Tab: the row's autocomplete into the box, if it has one; outside
    // keyword mode else the hint row offering a keyword, wherever it is
    // ("uu" → "uuid "; completionRow()). Optionally browses a selected
    // favorite folder in 全部. False leaves Tab for scope switching.
    Q_INVOKABLE bool autocomplete(int row);
    Q_INVOKABLE void leaveKeyword(); // the chip clicked: the box is emptied

    // The screenshot history: the clipboard history's pictures Win顺's
    // capture took (ClipFilter::screenshots), listed after its keyword ("jt ")
    // as a plugin's keyword lists a command's rows. App's, through these.
    struct Screenshot {
        qint64 id = 0; // the clipboard entry
        QString imagePath;
        QString ocr; // the words read in it
        int width = 0;
        int height = 0;
        qint64 created = 0; // ms since the epoch: when it was taken
    };
    struct ScreenshotHooks {
        std::function<std::vector<Screenshot>(const QString& words)> list; // newest first, words in the OCR
        std::function<void(qint64 id)> paste; // Enter: copied, and pasted where the launcher was opened over
        std::function<void(qint64 id)> copy; // Ctrl+C
        std::function<void(qint64 id)> edit; // Shift+Enter: in the editing window
        std::function<void(qint64 id)> remove; // Delete
        std::function<bool()> undo; // Ctrl+Z: false when there is nothing to put back
    };
    // `keyword` empty: no keyword (the capture off, or the word another's).
    void setScreenshots(const QString& keyword, ScreenshotHooks hooks);
    QString screenshotKeyword() const { return m_shotKeyword; }
    bool screenshotMode() const;
    void refreshScreenshots(); // the history changed: listed again while shown
    Q_INVOKABLE bool undoScreenshotRemoval(); // Ctrl+Z in screenshotMode

    QString query() const { return m_query; }
    void setQuery(const QString& query);
    Scope scope() const { return m_scope; }
    void setScope(Scope scope);
    int currentRow() const { return m_currentRow; }
    void setCurrentRow(int row);
    QVariantMap keyHints() const { return m_keyHints; }
    ResultModel* results() { return &m_results; }
    QString statusText() const { return m_flash.isEmpty() ? m_status : m_flash; }
    bool busy() const;
    qreal progress() const;
    bool moreAvailable() const;
    // The list scrolled to its end: the same search again for a page more,
    // what it finds that is not listed yet added after the rows (none of
    // them moves).
    Q_INVOKABLE void loadMore();
    QVariantList leftOutHints() const;
    QString approximateHeading() const;
    // A hint taken up (its kind, as leftOutHints names it): that part
    // searched for this query, and what it holds put among the rows (content
    // matches where they rank; a drive's files after the rows, as found).
    // Not offered again for this query; nothing is written to the settings or
    // the index. stopLeftOut() ends the one running (the rows found stay).
    // A kind may also be a plain action, nothing searched (GitHub #52, only
    // the chosen folders indexed): "folders" the settings' page of them,
    // "folders-dismiss" the note closed for good, "addfolder:<path>" a folder
    // added to them.
    Q_INVOKABLE void searchLeftOut(const QString& kind);
    Q_INVOKABLE void stopLeftOut();
    // Whether only the chosen folders are indexed (`folders`, as IndexService
    // has them; `only`: set), for the notes about what lies outside them
    // (leftOutHints), and whether the user closed the general one for good.
    void setChosenFolders(bool only, const QStringList& folders, bool noteDismissed);
    // Explorer's "search here": the folder is typed; one not indexed is
    // walked at once for what is typed after it, as the user named it.
    void noteSearchHere(const QString& folder);
    // Typed in a folder's window (App::typingFolderKnown, GitHub #110): the
    // box starts with `prefix`, the folder and a space ("D:\work\ "). While
    // it still does, the words after it are looked for in that folder first,
    // then everywhere else, as Listary does it; or only there (the setting,
    // setFolderTypingOnly, or switched for this time: switchTypingFolder).
    // Edited, it is a path typed: only that folder, as ever. Empty: none
    // (the window opened for anything else).
    void setTypingFolder(const QString& prefix);
    // FolderTyping: "only" set (only the folder), else its rows first.
    void setFolderTypingOnly(bool only);
    QString typingFolder() const;
    bool typingFolderOnly() const { return !m_typingPrefix.isEmpty() && m_typingOnly; }
    // Ctrl+D, or the caption's or hint's link: this time only the folder
    // typed in, or its rows first and then the rest (not the setting).
    // False when the box is not for a folder typed in (nothing to switch).
    Q_INVOKABLE bool switchTypingFolder();
    // How long the name search behind the rows shown took (--dump, for tests).
    qint64 elapsedUs() const noexcept { return m_elapsedUs; }
    // More results for the query are on the way. Not while quietly
    // refreshing results that were already complete, nor for a hint's
    // one-off search (searchLeftOut): the rows are the query's, and the hint
    // itself says what is going on (with 停止); the divider shows its progress.
    bool searching() const
    {
        return !m_quiet && m_hintRunning.isEmpty()
            && (m_pending || m_contentRunning || m_contentDebounce.isActive() || !m_pluginPending.isEmpty());
    }
    // The rows on screen are for the previous query; the new one has found
    // nothing yet (内容 takes a while).
    bool stale() const { return m_replacePending && m_results.count() > 0; }
    // A name search for the current query is still running (window reveal
    // waits for it, see App::showLauncher). Never for a plugin's answer.
    bool waitingForNames() const { return m_pending && !m_contentRunning; }
    QString placeholder() const;
    QString contentFilesLabel() const; // ".txt / .md / .log 等文件"
    // Whether the current query also looks inside files: always in 内容,
    // and in 全部 unless the query is name syntax or starts with a web
    // shortcut's keyword (see contentNeedle()).
    bool searchesContent() const { return m_scope == Content || m_withContent; }
    bool contentEnabled() const { return m_contentEnabled; }
    bool contentIndex() const { return m_contentIndex; }
    void setContentIndex(bool on);
    // The strip's switch: contentIndexRequested, the app changes the setting.
    Q_INVOKABLE void requestContentIndex(bool on) { emit contentIndexRequested(on); }

    void setWindow(QWindow* window) { m_window = window; }
    // `enabled` false: no 内容, and 全部 finds names only; a launcher in 内容 goes to 全部.
    void setContentOptions(bool enabled, QStringList extensions, const ContentSizeLimits& sizeLimits,
        bool inLowPriority, bool documents);
    void setWebShortcuts(const WebShortcuts& shortcuts);
    // 网页搜索建议 (Settings::webSuggestions): a site keyword and words typed
    // ("baidu 显卡") list the site's suggestions under its search's row.
    void setWebSuggestions(bool on);
    // Its user's favorites (may be none): with every request, and the
    // rows refreshed when they change.
    void setFavorites(Favorites* favorites);
    bool hasFavorites() const;
    // The system switches (may be none): the rows of one show its state and
    // Enter on them switches it. `stateFile` (state.ini) remembers what was
    // told once.
    void setSwitches(Switches* switches, const QString& stateFile);
    void setResultOptions(KindOrder kindOrder, bool showModified, bool showFileSize, bool thumbnails, const QString& numberKeys);
    // 优先显示的文件类型 (Settings::preferredTypes): searched again in the new order when shown.
    void setPreferredTypes(const QStringList& types);
    // Files with these extensions, or so named (wildcards), are left out of
    // every list and search.
    void setExcludedFiles(const QStringList& types, const QStringList& names);
    Sort sort() const { return m_sort; }
    void setSort(Sort sort);
    StartList startList() const { return m_startList; }
    void setStartList(StartList list);
    // Ctrl+S: with nothing typed, the other start list; else the next order
    // (best match, newest, oldest).
    Q_INVOKABLE void switchOrder();
    bool showModified() const { return m_showModified; }
    bool showFileSize() const { return m_showFileSize; }
    bool doubleClickToOpen() const { return m_doubleClickToOpen; }
    void setDoubleClickToOpen(bool on);
    void setTabEntersFavoriteFolder(bool on);
    QString numberKeys() const { return m_numberKeys; }
    QString numberHints() const { return m_numberHints; }
    void setNumberHints(const QString& hints);
    int numberKeysLeft() const { return static_cast<int>(m_numberKeysLeft); }
    void setNumberKeysLeft(unsigned digits);
    bool inTaskbarBox() const { return m_inTaskbarBox; }
    void setInTaskbarBox(bool on);
    // The search field changed (text, caret, selection, composition): fieldReported.
    Q_INVOKABLE void reportField(
        const QString& text, int cursor, int selectionStart, int selectionEnd, const QString& composition);
    // Ctrl+n (or Alt+n): the nth row shown opens (numberPressed), once the
    // rows on screen are the query's (holdUntilShown).
    Q_INVOKABLE void pressNumber(int number);
    // Enter or a number pressed while the rows on screen are still the
    // previous query's (it was typed a moment ago): true, and heldKey()
    // brings it back once the new rows are shown. Typing on drops it.
    Q_INVOKABLE bool holdUntilShown(int action, int number);
    // Off: nothing opened is remembered, and what was is not shown (it stays
    // until cleared in the settings).
    void setRecordHistory(bool on);
    bool canClearHistory() const;
    Q_INVOKABLE void clearHistory();
    QVariantList frequentApps() const;
    Q_INVOKABLE void openFrequentApp(int index);
    // Nothing typed, 全部, no folder chosen in 内容: as at the first opening
    // (Settings::clearOnOpen); the search follows in handleShown.
    void startOver();
    void handleShown();
    void handleHidden();
    void retranslate(); // the language changed: status, placeholder, rows
    // A few words in the footer for a moment ("已复制"); longer for one that
    // stands until a result replaces it ("正在连接 …").
    void flash(const QString& message, std::chrono::milliseconds shown = std::chrono::seconds(2));
    // An action waiting for a slow server is no longer wanted: by any other
    // action, the launcher going, and its menu opening (ContextMenu.qml).
    Q_INVOKABLE void dropServerWait();
    // "正在连接 …" for the server of `path`, waited for off the UI thread
    // ("正在启动 Windows 的 WebDAV 服务…" while `clientStarting`); the
    // m_serverWaitId its answer must still match.
    quint64 showServerWait(const QString& path, bool clientStarting);
    // For the preview's checks: the server checks started so far (one per
    // files asked about while it is under way), and what copies files in
    // place of shell::copyFiles (the clipboard left be).
    quint64 serverChecksStarted() const { return m_serverChecksStarted; }
    void setFileCopier(std::function<void(const QStringList& files)> copier) { m_fileCopier = std::move(copier); }
    // A plugin's effects from outside a row: a click on its system
    // notification (Host::toastRequested), held to its permissions as an
    // action's are.
    void applyPluginEffects(const QString& pluginId, const QList<ws::plugin::Effect>& effects) { applyEffects(pluginId, effects); }
    // Its list is opening from a click on that plugin's notification: for a
    // moment (kToastOpenMs) a row coming in on top under the keyboard's first
    // row keeps the keyboard there, once (followCurrentRow): it is what the
    // notification was about. Otherwise the keyboard follows its row.
    void openedFromToast(const QString& pluginId);
    static constexpr int kToastOpenMs = 3000;

    // An action on a row; on a row of a selection of several, on all of it.
    Q_INVOKABLE void trigger(int row, int action);
    Q_INVOKABLE void triggerSelection(int action); // the keyboard's, with rows selected
    // A click on a row (or on its switch, `onSwitch`): trigger(row, Open), but on a
    // system switch's row the second click of a double click is no second
    // switch; and its switch is clicked to toggle it, whatever the row asks.
    Q_INVOKABLE void clickRow(int row, bool onSwitch = false);
    // Whether Enter on this row switches something and the window stays (a
    // system switch's row with a backend): a held key must not repeat it.
    Q_INVOKABLE bool isSwitchRow(int row) const;
    // Alt+← → on the volume's row: `steps` steps of 5 down (< 0) or up
    // (Switches::step), the window left as it is. False on any other row:
    // the keys are the box's then.
    Q_INVOKABLE bool stepSwitch(int row, int steps);
    // The row's file or folder dragged out of the window (into a chat, a
    // folder); the selection when the row is in it. Returns when it was let
    // go (see dragout::drag). Apps, places, web pages and plugins' rows stay.
    Q_INVOKABLE void dragOut(int row);
    // Selecting several rows: Ctrl toggles one, Shift takes the rows from an
    // anchor to here (instead of the selection, or with Ctrl added to it).
    Q_INVOKABLE void toggleSelected(int row);
    Q_INVOKABLE void selectRange(int from, int to, bool add);
    Q_INVOKABLE void clearSelection();
    // Context menu (ContextMenu.qml): its entries for a row, the usable
    // screen area around a point, and the native styling of its window.
    Q_INVOKABLE QVariantList menuItems(int row) const;
    Q_INVOKABLE QRectF screenArea(QPointF globalPos) const;
    Q_INVOKABLE void prepareMenuWindow(QWindow* menu) const;
    // The shell's own context menu (platform/ShellContextMenu) for a file's
    // or folder's row, or for the files among a selection it is in:
    // Shift+right-click, Shift+Menu, 更多选项 in Win顺's menu (GitHub #26).
    // hasSystemMenu: the row is such a row (an app, a place, a web page or a
    // plugin's row has none: the caller shows Win顺's menu). showSystemMenu
    // opens it a moment later, from the event loop, at `globalPos` (logical
    // pixels on the screen: the row, or where Win顺's menu was) or, without
    // one, at the pointer. The menu runs its own loop there; the launcher
    // stays open under it, and goes, as for 打开, once a command that opens
    // something was picked (shellcontext::keepsLauncher).
    Q_INVOKABLE bool hasSystemMenu(int row) const;
    Q_INVOKABLE void showSystemMenu(int row, const QVariant& globalPos = {});
    bool systemMenuOpen() const { return m_systemMenuOpen; }
    // Open, or asked for a moment ago: the context-menu event the Menu
    // key's release brings is not for Win顺's menu then (App::eventFilter).
    bool systemMenuBusy() const;
    // A command of that menu that keeps the launcher (复制, 删除…) is
    // running, or just was: whatever it shows (a confirmation, a progress
    // window, which 删除 brings up after the call returns) takes the focus
    // for a moment and gives it back; the launcher is not to go for that.
    // Over once the launcher is active again, or another program's window
    // took the focus (App's activeChanged), or the launcher was hidden.
    bool systemCommandRunning() const { return m_systemCommand; }
    void systemCommandDone() { m_systemCommand = false; }
    Q_INVOKABLE void cycleScope(int delta);
    int fileType() const { return m_fileType; }
    QList<int> selectedFileTypes() const;
    QString queryError() const;
    bool gallery() const { return m_gallery; }
    void setGallery(bool gallery);
    int thumbnailSize() const { return m_thumbnailSize; }
    void setThumbnailSize(int size);
    Q_INVOKABLE void toggleFileType(int type);
    QVariantList fileTypes() const;
    void setFileTypes(const FileTypes& types); // named (Settings::namedFileTypes)
    void setFolderKeyword(const QString& keyword);
    QString folderKeyword() const { return m_folderKeyword; }
    // A chip of the strip: kFoldersChip (文件夹), -1 (不限), or a kind
    // (fileTypes); in 文件 (or 文件夹) from any scope.
    static constexpr int kFoldersChip = -2;
    Q_INVOKABLE void chooseFileType(int type);
    // Ctrl+Tab in 文件: the next chip (Ctrl+Shift+Tab the one before), round (不限, 文件夹, then the kinds).
    Q_INVOKABLE void cycleFileType(int delta);
    Q_INVOKABLE void editFileTypes(); // the strip's +: fileTypeSettingsRequested
    // The strip's choice, to keep between runs: "folders", a kind's id
    // ("pictures") or "name:<its name>" for one of the user's own, or empty
    // (不限). Set after setFileTypes; in 文件 it applies at once.
    QString fileChoice() const;
    void setFileChoice(const QString& choice);
    QVariantList folderChips() const;
    QString folderChip() const;
    QString folderHint() const;
    QString contentWords() const;
    QString contentWhere() const;
    QString whereMissing() const;
    // A chip of the strip in 内容 (folderChips): searched there.
    Q_INVOKABLE void chooseFolderChip(const QString& key);
    // Ctrl+Tab in 内容: the next folder chip (or the one before), round; nothing with none to go to.
    Q_INVOKABLE void cycleFolderChip(int delta);
    Q_INVOKABLE void editFolderChips(); // the strip's +: the favorites, where the folders come from
    // Esc in 内容 with a folder chosen: the words go, the folder stays
    // (true); nothing to clear: false, and Esc closes the window.
    Q_INVOKABLE bool clearContentWords();
    Q_INVOKABLE void dismiss();

    // Alt+P, 预览 in the menu: the row's file or folder in a preview
    // (FilePreview: QuickLook or Seer when running, else Win顺's own window),
    // or, shown already, closed. While it is open the current row's file
    // follows (setCurrentRow); the launcher hidden closes it, but it comes
    // back when the launcher opens again (state.ini Launcher/PreviewOpen),
    // until Alt+P or the × closes it. Esc leaves it alone: it closes the
    // launcher. closePreview: true when there was one to close.
    void setPreview(FilePreview* preview);
    Q_INVOKABLE void togglePreview(int row);
    Q_INVOKABLE bool closePreview();
    // A folder opened by Enter or a click (one row, not as administrator) is
    // offered here first: true when it was taken there instead of a new
    // window of the file manager (App: the Explorer tab the launcher was
    // opened from by typing, GitHub #39). Remembered as opened either way.
    void setFolderOpener(std::function<bool(const QString& folder)> opener) { m_folderOpener = std::move(opener); }
    // The same for Ctrl+Enter on one file or folder (not a drive): true when
    // its folder was shown in that tab with the item selected (GitHub #60).
    void setRevealer(std::function<bool(const QString& path)> revealer) { m_revealer = std::move(revealer); }
    // Which row Alt+P is for: the one under the pointer when the mouse moved
    // there after the keyboard last moved (keyboardMoved, the arrows), else
    // the keyboard's. pointerIfMoved: the pointer (global, logical) in the
    // first case, undefined in the second.
    Q_INVOKABLE void keyboardMoved();
    Q_INVOKABLE QVariant pointerIfMoved() const;

    // The search panel (see the properties above).
    bool panel() const { return m_panel; }
    PanelColumns* panelColumns() { return &m_columns; }
    void setPanel(bool on); // by the app, the window sized for it (App::setPanel)
    Q_INVOKABLE void togglePanel(); // panelToggleRequested: the app does it
    bool pinned() const { return m_pinned; }
    void setPinned(bool on);
    Q_INVOKABLE void togglePin() { setPinned(!m_pinned); } // Ctrl+T
    // The search icon clicked (not dragged, WindowFrame::handleClicked):
    // the settings window (settingsRequested).
    Q_INVOKABLE void openSettings() { emit settingsRequested(); }
    int dateChoice() const { return m_dateChoice; }
    QString dateFrom() const { return m_dateFrom; }
    QString dateTo() const { return m_dateTo; }
    int sizeChoice() const { return m_sizeChoice; }
    QString sizeMin() const { return m_sizeMin; }
    QString sizeMax() const { return m_sizeMax; }
    QString folderFilter() const { return m_folderFilter; }
    bool filtersActive() const;
    QVariantList favoriteFolders() const;
    Q_INVOKABLE void setDateChoice(int choice);
    Q_INVOKABLE void setDateRange(const QString& from, const QString& to); // 自定义
    Q_INVOKABLE void setSizeChoice(int choice);
    Q_INVOKABLE void setSizeRange(const QString& minMb, const QString& maxMb); // 自定义
    Q_INVOKABLE void setFolderFilter(const QString& path);
    Q_INVOKABLE void pickFolderFilter(); // the folder dialog
    Q_INVOKABLE void clearFilters();
    int panelSortKey() const { return static_cast<int>(m_panelSort.key); }
    bool panelSortDescending() const { return m_panelSort.descending; }
    // A column's header clicked: by it (name and path up, size and time
    // down); clicked again, the other way round. 0: as ranked.
    Q_INVOKABLE void sortBy(int key);
    int totalMatches() const { return static_cast<int>(std::min<qint64>(m_totalMatches, 1'000'000'000)); }

signals:
    void queryChanged();
    void scopeChanged();
    void statusChanged();
    void shown();
    void resultsReplaced(); // results for a new query: select the first row
    void namesShown(); // a name search finished and its rows are in the model
    void dismissRequested();
    void escaped(); // Esc closed it (dismiss), before dismissRequested: nothing opened
    // Not started: it would have needed administrator rights to start at all
    // (see shell::open). The launcher is closed by then.
    void openFailed(const QString& name);
    // Menu key / Shift+F10 (arrive as a context-menu event, not a key);
    // `system`: Shift+Menu or Ctrl+Shift+F10, the shell's menu (showSystemMenu).
    void contextMenuKeyPressed(bool system);
    void historyChanged();
    void contentIndexChanged();
    void contentIndexRequested(bool on);
    void frequentAppsChanged();
    void webSettingsRequested(); // the settings window, on its 网页搜索 page
    void fileTypeSettingsRequested(); // ... on its 结果列表 page, at the file types
    void fileTypeChanged();
    void galleryChanged();
    void queryErrorChanged();
    void fileTypesChanged();
    void fileChoiceChanged();
    void favoriteSettingsRequested(const QString& path); // ... on its 收藏夹 page, at this favorite
    void favoritesChanged();
    // The settings window, on the 插件 page at this plugin's card: a plugin
    // row opening "winshun:settings" (its own card only; performPlugin).
    void pluginSettingsRequested(const QString& pluginId);
    void currentRowChanged();
    void keyHintsChanged();
    void sortChanged();
    void startListChanged();
    void resultOptionsChanged();
    void inTaskbarBoxChanged();
    void fieldReported(const QString& text, int cursor, int selectionStart, int selectionEnd, const QString& composition);
    // Pressed in the box on the taskbar: the field's caret goes there, or
    // its word is selected; dragged there, the text from `anchor` is selected.
    void caretRequested(int position, bool word);
    void selectionRequested(int anchor, int position);
    void numberPressed(int number); // 1…9: open that row, counted from the first one shown
    void heldKey(int action, int number); // see holdUntilShown(); `number` 0: the action on the current row
    // A content match (or a plugin's row) better than the first row came in
    // above it while the keyboard was there: the first row is current again
    // (the list moved it down).
    void betterFirstRow();
    void pluginChanged();
    // 贴到屏幕 (PinPicture, Ctrl+Enter on a screenshot): the picture in
    // `path`; `screenshot`: it is one of the history's.
    void pinRequested(const QString& path, bool screenshot);
    void folderChipsChanged();
    void typingFolderChanged();
    void panelChanged();
    void panelToggleRequested();
    void pinnedChanged();
    void settingsRequested();
    void folderSettingsRequested(); // the settings' page of the chosen folders
    void shareSettingsRequested(); // the settings' section of network and WSL folders (GitHub #114)
    void folderAddRequested(const QString& path); // a folder not indexed, to be (the app adds it as the settings do)
    void folderNoteDismissed(); // 不再提示
    void filtersChanged();
    void panelSortChanged();
    void panelRowRestored(int index);
    // A keyword's list changed by itself (a plugin's rerun, a later batch)
    // under the keyboard: the keyboard goes to `index` (its row's new place,
    // or the last row when its place is past the end of a shorter list);
    // -1: it stays where it is (its row went). The menu open for it closes.
    void currentRowMoved(int index);

private:
    class ModelSink; // the result model as plugin::Merge sees it
    // Keyword mode: the command the keyword opens, and what it shows.
    struct KeywordMode {
        QString pluginId;
        QString commandId;
        QString keyword; // as typed
        QString words; // after it
        QString label; // the command's title
        QString hint;
        QString icon; // an image URL
    };

    void search(bool more = false); // more: a page more (loadMore)
    // The command whose keyword opens the text ("kill note"), if any: what
    // keyword mode shows for it. No side effects (search() asks before it
    // decides whether the rows are new ones).
    std::optional<KeywordMode> matchKeyword(const QString& text) const;
    std::optional<KeywordMode> matchScreenshotKeyword(const QString& text) const;
    SearchResults screenshotRows(const QString& words) const;
    void placeScreenshotHint(); // the screenshot keyword typed alone: its hint row first
    void performScreenshot(const SearchResult& result, Action action);
    // Part of search(): what the text triggers; `keyword` as matchKeyword() found it.
    void startPluginQuery(const QString& text, std::optional<KeywordMode> keyword);
    void refreshKeyHints(); // keyHints for the current row; keyHintsChanged when they differ
    const SearchResult* completionRow(int row) const; // what Tab completes from `row`, or null (see autocomplete)
    void onPluginTaken(std::uint64_t queryId, const QString& pluginId);
    void onPluginResults(std::uint64_t queryId, const QString& pluginId, const QString& commandId,
        const QList<ws::plugin::Item>& items, bool partial, bool replace);
    void onPluginFinished(std::uint64_t queryId, const QString& pluginId);
    void onPluginFailed(std::uint64_t queryId, const QString& pluginId, const QString& why);
    void onPluginProgress(std::uint64_t queryId, const QString& pluginId, const QString& text);
    void onPluginActionDone(std::uint64_t requestId, const QString& pluginId, const QList<ws::plugin::Effect>& effects, bool ok, const QString& error);
    void pluginSettled(const QString& pluginId); // answered, or failed
    QString pluginName(const QString& pluginId) const;
    void performPlugin(const SearchResult& result, Action action);
    void applyEffects(const QString& pluginId, const QList<ws::plugin::Effect>& effects);
    // The files a plugin's `files` command gets from the menu of `row`: the
    // selection's when the row is in one, else the row's; false when any of
    // them is not a file or folder that is there (an app, a site, a plugin's
    // row, a screenshot, a file gone or offline).
    bool fileCommandPaths(int row, QStringList* paths, QList<bool>* folders) const;
    void runFileCommand(int row, int index); // PluginFileAction + index into m_menuFileCommands
    // The files commands the last menu offered (menuItems), by their place in
    // it: which plugin's which command, for which files. Looked up by those at
    // the click, never by the place alone (the plugins may have changed).
    struct MenuFileCommand {
        QString pluginId;
        QString commandId;
        QStringList paths;
    };
    mutable std::vector<MenuFileCommand> m_menuFileCommands;
    // After a keyword's list changed by itself (onPluginResults): the
    // keyboard kept on its row (currentRowMoved), and for a moment a click
    // or a number key on a row that changed, or Enter on one that went, does
    // nothing. Forgotten when another list comes or the window hides.
    void followCurrentRow(const QStringList& before, const QString& currentPath);
    bool rowJustChanged(int row) const;
    void forgetRowChanges();
    // A plugin may only have Win顺 carry out a declarative `open` (an item's
    // default action, or an `open` effect) within its permissions: without
    // `processes` it cannot start a program or open a non-web link (OpenPolicy).
    // True when it may go ahead; otherwise shows a plain notice, logs it once,
    // and returns false so the caller does nothing.
    bool pluginOpenAllowed(const QString& pluginId, const ws::plugin::Open& open);
    void showPluginRows(SearchResults rows); // keyword mode: the whole list (showRows)
    RankBy rankBy() const; // as the current scope has it
    // The names' order when it is a column's (the panel's, or the sorts by
    // name, folder, type and size, #44); Rank: rankBy().
    SortOrder nameOrder() const;
    // The panel's filters as the engine takes them (the dates as of now),
    // and what of the panel the rows are for (as m_shownText is): the
    // filters, the order, and whether nothing typed lists everything.
    EntryFilter panelFilter() const;
    QJsonObject pluginFilters() const;
    void sortShownPanel(const QString& currentPath = {});
    QString panelKey() const;
    bool showsStartList() const; // nothing typed, and a list for it
    QString contentNeedle() const;
    QString shownExtensions() const;
    void startContentSearch();
    void showRows(SearchResults rows, QStringList highlights);
    // Web suggestions (WebSuggester). Part of search(): the site's asked for
    // the words typed after its keyword, or the rows go (another site, no
    // words, a plugin's keyword, another scope, the panel). The answer's rows
    // (m_suggestRows) go right under the search's own row, wherever it is
    // (placeSuggestions; showRows puts them back into each new list).
    void suggest();
    void onSuggestions(const QString& endpoint, const QString& words, const QStringList& suggestions);
    void placeSuggestions();
    void dropSuggestions();
    void perform(const SearchResult& result, Action action, bool fileChecked = false);
    void performApp(const SearchResult& app, Action action);
    void performPlace(const SearchResult& place, Action action);
    // A system switch's row with a backend: false for any other, which is an
    // ordinary place. Enter switches (the window stays, the words selected),
    // Ctrl+Enter opens its page.
    bool performSwitch(const SearchResult& row, Action action);
    // What Enter did (Switches::Outcome as an int, from apply() or waitEnded()); `resolved`: what it asked, as the footer said.
    void switchOutcome(const SearchResult& row, const SwitchIntent& resolved, int outcome);
    void openSwitchPage(const SearchResult& row);
    void scheduleAwakeTick(); // m_awakeTick for the next minute that changes in the keep-awake row, if it has one
    bool liveSwitch(const SearchResult& row) const; // a switch row with a backend (not Unsupported)
    bool drawsSwitch(const SearchResult& row) const; // ... that is ready or not read yet: it has its switch
    bool steppable(const SearchResult& row) const; // ... and a level: the volume's, Alt+← →
    // refreshKeyHints() for a switch's row.
    void switchKeyHints(const SearchResult& row, QString& enter, QString& secondKeys, QString& second) const;
    QString switchAlready(const SearchResult& row, const SwitchIntent& resolved) const; // "蓝牙已经是关的"
    void performWeb(const SearchResult& web, Action action);
    void performFavorite(const SearchResult& result, Action action); // AddFavorite … EditFavorite
    // Files on servers looked up off the UI thread: what of each (share::Reach).
    using ServerFound = QHash<QString, share::Reach>;
    // `found`: files on SMB and WebDAV servers among them looked up off the
    // UI thread; the call that does it calls again with what it found.
    void performMany(const SearchResults& items, Action action, const ServerFound* found = nullptr);
    // `paths` (on servers) looked up off the UI thread, then `then` with what
    // was found, unless another action came first or the launcher went
    // (dropServerWait). Asked again while that is under way (Enter held
    // down), the same files join it, the answer going to the newest.
    void checkServersThen(const QStringList& paths, std::function<void(const ServerFound&)> then);
    void copyFiles(const QStringList& files); // m_fileCopier, else shell::copyFiles
    navigation::TabAction tabAction(int row) const;
    void browseFolder(const SearchResult& folder);
    // Opened: into the history, if it is kept. `query`: the text the list it
    // was opened from is for (m_shownText); none when it was not found by typing.
    void remember(const QString& path, const QString& query = {});
    void forgetRecent(const QStringList& paths);
    // showSystemMenu's work, from the event loop; afterwards forgetGone drops
    // the rows whose files the command took away (删除, 剪切 and paste
    // elsewhere), now and a little later (a confirmation, a progress window).
    void runSystemMenu(const QStringList& paths, std::optional<QPointF> globalPos);
    void forgetGone(const QStringList& paths, int attempt);
    bool forgetRecycled(const SearchResult& result); // forgetRow, unless it is still there (then false)
    void forgetRow(const SearchResult& result); // out of the list (its count) and the history
    void forgetMissing(const SearchResult& r, Action action); // an action's file not there: said so, out of the history
    std::function<void(bool)> reportFailure(const QString& name);
    void onRecycled(const SearchResult& result, bool ok);
    void onRecycledMany(const SearchResults& items, bool ok);
    void onResults(quint64 id, const SearchResults& results, qint64 total, qint64 elapsedUs);
    void onContentResults(quint64 id, const SearchResults& results);
    void onContentProgress(quint64 id, int scanned, int total, bool finished);
    void onContentLeftOut(quint64 id, const ContentLeftOut& leftOut);
    void onContentScope(quint64 id, const ContentScope& scope);
    // A content request where the strip in 内容 says to look (m_contentFolder, m_contentPick).
    void setContentPlace(SearchEngine::Request& request) const;
    // The scan's report on the folders typed, if it is for the text there is (else none yet).
    const ContentScope::Place* typedPlace() const;
    static QString folderLabel(const QString& path); // its last names, "项目\abc"
    static QString pickKey(const SearchEngine::Request::ContentPick& pick); // for m_shownPick; empty: none
    void onRemovableDrives(quint64 id, const RemovableDrives& drives);
    void onDriveResults(quint64 id, const SearchResults& results);
    void onDriveProgress(quint64 id, int seen, bool finished);
    // Whether the search behind the rows is done (a quiet refresh keeps them
    // as they are, so it counts as done): only then is what it passed over
    // known, and the hints shown.
    bool hintsSettled() const;
    // The folder typed (a path, then words) when only the chosen folders are
    // indexed and it is not among them, on a disk of this PC that is there;
    // else empty. Asked of the disk once per query.
    QString outsideFolder() const;
    void walkHereSoon(); // noteSearchHere: its folder walked as soon as the search ends
    void finishLeftOut(bool stopped); // the one running is over: the hint goes, the footer says what came of it
    void refreshStatus();
    void refreshFrequentApps();
    void followPreview(); // the current row's file into the open preview

    struct FrequentApp {
        QString name;
        QString path; // launch path
        QString icon;
        bool operator==(const FrequentApp&) const = default;
    };

    IndexService* m_index;
    AppCatalog* m_apps;
    SystemCatalog* m_places;
    SearchEngine* m_engine;
    History* m_history;
    ResultModel m_results;
    QPointer<QWindow> m_window;
    std::function<bool(const QString& folder)> m_folderOpener; // setFolderOpener
    std::function<bool(const QString& path)> m_revealer; // setRevealer

    QString m_query;
    Scope m_scope = All;
    quint64 m_requestId = 0;
    // The last name search's: its report of the removable drives
    // (onRemovableDrives) may arrive after 全部 has gone on to its content
    // scan, under a new m_requestId.
    quint64 m_nameRequestId = 0;
    bool m_pending = false;
    bool m_haveResults = false;
    // What the rows on screen are for. Searching it again (window reopened,
    // index updated) refreshes them in place instead of starting over.
    QString m_shownText;
    Scope m_shownScope = All;
    RankBy m_shownRankBy = RankBy::Match;
    SortOrder m_shownOrder;
    StartList m_shownStartList = RecentlyOpened;
    KindOrder m_shownKindOrder = KindOrder::FilesFirst;
    QStringList m_shownPreferredTypes;
    QStringList m_shownExtensions; // of the kind of file chosen (fileExtensions())
    // The command whose list the rows are ("<plugin>/<command>"; empty: the
    // name search's): "=" and "= " trim to the same text, but one lists
    // files, the other the calculator's answer, and that is a new list.
    QString m_shownKeyword;
    bool m_refresh = false; // the current search is such a refresh
    bool m_quiet = false; // ...of complete results: it shows no progress
    bool m_scanComplete = false; // the content scan behind the rows on screen ran to the end
    bool m_replacePending = false; // the current search is for a new query and has shown nothing yet
    // Refresh: content matches kept from the last scan that the running one
    // has not found again yet. Removed if it finishes without them.
    QSet<QString> m_unconfirmed;
    QSet<QString> m_wslOpening;
    quint64 m_serverWaitId = 0; // which wait for a slow share is still wanted (bumped by any new action, by hiding)
    QString m_serverWaitFlash; // its "正在连接 …", which goes with it (dropServerWait)
    // Checks under way (checkServersThen), by the files asked about: the
    // newest asking's wait and what then.
    struct ServerCheck {
        quint64 wait = 0; // m_serverWaitId as it was asked
        bool clientStarting = false; // WebClient started first: waited for longer (checkServers)
        std::function<void(const ServerFound&)> then;
    };
    QHash<QString, ServerCheck> m_serverChecks;
    quint64 m_serverChecksStarted = 0;
    std::function<void(const QStringList& files)> m_fileCopier; // setFileCopier
    qint64 m_totalMatches = 0;
    qint64 m_elapsedUs = 0;
    bool m_contentRunning = false;
    bool m_withContent = false; // 全部: content matches follow the name matches
    int m_contentScanned = 0;
    int m_contentTotal = 0;
    int m_contentHits = 0; // 全部: content rows added below the name matches
    // Pages of rows asked for (kPage each): back to 1 for a new list.
    int m_pages = 1;
    bool m_loadingMore = false; // the current search is loadMore()'s: its new rows go at the end
    bool m_moreNames = false; // the last name search (or files written last) filled its page
    bool m_moreContent = false; // the last content scan stopped at its limit
    // What the search behind the rows passed over (leftOutHints): the content
    // scan's report (SearchEngine::contentLeftOut), and the removable drives
    // the index leaves out (SearchEngine::removableDrives). A hint taken up
    // (searchLeftOut) is the one running (m_hintRunning, its kind) and then
    // not offered again for this query (m_hintsTaken); the rows a one-off
    // brought (m_leftOutFound) stay through refreshes, which cannot find them
    // again (the limits, the settings, or the index leave them out). Back to
    // nothing with a new query.
    ContentLeftOut m_leftOut;
    RemovableDrives m_drives;
    QString m_hintRunning;
    QSet<QString> m_hintsTaken;
    QSet<QString> m_leftOutFound;
    int m_hintHits = 0; // rows the one running has brought
    // Only the chosen folders are indexed (setChosenFolders).
    bool m_foldersOnly = false;
    QStringList m_chosenFolders;
    bool m_folderNoteDismissed = false;
    QString m_hereFolder; // "search here" named it (noteSearchHere)
    bool m_hereSeen = false; // ... and the query has been that folder since
    // Typed in a folder's window (setTypingFolder, GitHub #110): the box's
    // start as the app put it ("D:\work\ "; empty: none), that folder
    // ("D:\work"), and whether only it is searched this time (from the
    // setting, m_typingOnlyDefault, each time; switchTypingFolder).
    QString m_typingPrefix;
    QString m_typingFolder;
    bool m_typingOnly = false;
    bool m_typingOnlyDefault = false;
    QString m_shownTyping; // typingKey() of the rows on screen, as m_shownText
    // The box is for the folder typed in: it still starts with it, and the
    // list is the names' (not 内容, which takes the folder as a path typed;
    // not the panel, which has a folder filter of its own).
    bool typedInFolder() const;
    // What the name search looks for: typed in a folder, the words after it
    // (none: the folder's own listing, as the path typed); else searchText().
    QString nameText() const;
    // The words after the folder typed in when there are some and the box is
    // for it (typedInFolder): "" otherwise.
    QString typingWords() const;
    // How the folder typed in shapes the list ("first:D:\work",
    // "only:D:\work"; empty: not at all), for telling a new list from a refresh.
    QString typingKey() const;
    mutable QString m_outsideQuery; // outsideFolder() was last asked for this query ...
    mutable QString m_outside; // ... and said this
    mutable bool m_outsideKnown = false;
    bool m_walkRunning = false; // ... it is a walk of a drive (SearchEngine::driveProgress ends it)
    int m_walkSeen = 0; // files and folders it has looked at
    bool m_approximate = false; // the rows were found by reading (approximateHeading)
    int m_currentRow = -1;
    QVariantMap m_keyHints; // see keyHints
    FileTypes m_fileTypes;
    QString m_folderKeyword = QStringLiteral("folder");
    struct PrefixScope { Scope scope; QStringList typeKeys; bool folders; };
    std::optional<PrefixScope> m_filePrefix;
    QString m_prefixWords;
    QString searchText() const { return m_filePrefix ? m_prefixWords : m_query.trimmed(); }
    void updateQueryPrefix();
    bool clearQueryPrefix(); // an explicit scope/type choice replaces the typed prefix
    int m_fileType = -1; // see fileType
    QList<int> m_extraFileTypes;
    bool m_gallery = false;
    int m_thumbnailSize = 160;
    QString m_queryError;
    bool m_foldersChosen = false; // 文件夹 was the strip's last choice (fileChoice)
    static QString fileTypeKey(const FileType& type); // as fileChoice names it
    int fileTypeIndex(const QString& key) const; // -1: none
    QStringList fileExtensions() const; // the kind chosen's, in 文件; none otherwise
    // 内容 in a folder chosen (a chip, a folder's row: SearchInFolder), as a
    // full path; empty: everywhere. Kept while the app runs, as the kind of
    // file is. A folder named among the words goes first (ContentFolderSpec).
    QString m_contentFolder;
    // One of the folders a name typed stands for, Tab along them ("abc/":
    // 所有 abc, D:\工作\abc, E:\备份\abc).
    SearchEngine::Request::ContentPick m_contentPick;
    ContentScope m_contentScope; // as the last content scan made it out (onContentScope)
    QString m_shownContentFolder; // what the rows are for, as m_shownText
    QString m_shownPick;
    QString m_shownPanelKey; // panelKey() of the rows

    // The search panel (see the properties).
    bool m_panel = false;
    PanelColumns m_columns;
    bool m_pinned = false;
    int m_dateChoice = 0;
    QString m_dateFrom;
    QString m_dateTo;
    int m_sizeChoice = 0;
    QString m_sizeMin;
    QString m_sizeMax;
    QString m_folderFilter;
    SortOrder m_panelSort;

    bool m_recordHistory = true;
    KindOrder m_kindOrder = KindOrder::FilesFirst;
    QStringList m_preferredTypes; // files of these types first among those that match about as well
    QStringList m_excludedTypes; // files of these types never listed (Settings::excludedTypes)
    QStringList m_excludedFileNames; // ... nor so named (Settings::excludedFileNames)
    Sort m_sort = BestMatch;
    StartList m_startList = RecentlyOpened;
    bool m_showModified = true;
    bool m_showFileSize = false;
    bool m_doubleClickToOpen = false;
    bool m_tabEntersFavoriteFolder = false;
    QString m_numberKeys = QStringLiteral("ctrl");
    QString m_numberHints = QStringLiteral("held");
    unsigned m_numberKeysLeft = 0;
    bool m_inTaskbarBox = false;
    bool m_systemMenuOpen = false; // the shell's menu is up (its loop runs inside runSystemMenu)
    bool m_systemCommand = false; // see systemCommandRunning
    QElapsedTimer m_systemMenuAsked; // since showSystemMenu was last called
    std::optional<std::pair<int, int>> m_held; // holdUntilShown(): action, number
    std::vector<FrequentApp> m_frequent;
    std::vector<AppUse> m_windowsUses; // Windows' record of what was started, read at most every minute
    QElapsedTimer m_windowsUsesAge;
    QStringList m_contentExtensions {QStringLiteral("txt")};
    ContentSizeLimits m_contentSizeLimits;
    bool m_contentInLowPriority = false; // 内容 also looks in system, program and tool folders
    WebShortcuts m_web;
    bool m_webSuggestions = true;
    WebSuggester m_suggester;
    SearchResults m_suggestRows; // the latest answer's, for the site typed; kept until the next answer
    QPointer<Favorites> m_favorites;
    QPointer<Switches> m_switches;
    QString m_stateFile; // state.ini
    bool m_previewWanted = false; // the preview was left open by the user: it comes back with the launcher
    bool m_previewResume = false; // shown now, and the preview not yet back
    void setPreviewWanted(bool on);
    bool m_switchPageTold = false; // "要打开设置页：Ctrl+Enter", said once for good (state.ini)
    // Toggles waiting for their state (Switches::Outcome::Waiting), by switch id.
    QHash<QString, SearchResult> m_switchWaiting;
    QString m_clickedSwitch; // the last click that switched, by path ...
    QElapsedTimer m_switchClicked; // ... and when
    bool m_contentDocuments = true; // and in Word, Excel, PowerPoint and PDF files
    bool m_contentEnabled = true;
    bool m_contentIndex = false;

    QString m_status;
    QString m_flash;
    QTimer m_contentDebounce;
    bool m_contentAtOnce = false; // search(): the text is the one shown, no pause in typing to wait for
    QTimer m_flashTimer;
    QTimer m_statusPoll;
    // Keep awake with an end time, while the window is up: the minutes left in
    // its row come round (scheduleAwakeTick). Not running while it is hidden.
    QTimer m_awakeTick;
    // A new query's rows a while in coming: the status says so, not the last
    // query's figures under its dimmed rows.
    QTimer m_slowSearch;
    bool m_searchSlow = false;

    // Plugins. The query ids are the launcher's own (keyword mode has no
    // engine request); the merge places their rows (plugin::Merge).
    QPointer<plugin::Host> m_plugins;
    std::unique_ptr<ModelSink> m_sink;
    std::unique_ptr<plugin::Merge> m_merge;
    std::uint64_t m_pluginQueryId = 0;
    std::optional<KeywordMode> m_keyword;
    QString m_shotKeyword; // setScreenshots
    ScreenshotHooks m_shotHooks;
    QSet<QString> m_pluginPending; // answering the current query
    QElapsedTimer m_pluginTimer; // since they were asked
    qint64 m_pluginElapsedUs = 0; // keyword mode: until the answer
    // followCurrentRow: when each row last changed by itself (m_rowClock's
    // ms + 1; 0 never), whatever number of changes came within a moment;
    // when the keyboard's row last went; the keyboard moving with its row.
    QElapsedTimer m_rowClock;
    QList<qint64> m_rowChangedAt;
    QElapsedTimer m_currentGone;
    bool m_following = false;
    QString m_toastPlugin; // openedFromToast: whose notification, and when
    QElapsedTimer m_toastOpened;
    QTimer m_pluginWait; // 150 ms: only then does the status say who is being waited for
    bool m_pluginSlow = false;
    QString m_pluginProgress; // winshun/progress: what the keyword's plugin says it is doing
    QString m_pluginError; // "X 没有响应": shown for a few seconds
    QTimer m_pluginErrorTimer;
    std::uint64_t m_pluginRequest = 0; // actions sent
    QSet<QString> m_openBlockedLogged; // plugins whose blocked `open` was already logged (once each)

    QPointer<FilePreview> m_preview;
    QPoint m_pointerAtKeys {-1, -1}; // where the pointer was when the keyboard last moved (keyboardMoved)
};

} // namespace ws
