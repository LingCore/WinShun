#pragma once

#include "Pinyin.h"

#include <QByteArray>
#include <QHash>
#include <QList>
#include <QString>
#include <QStringList>

#include <QSet>

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <vector>

class QObject;

namespace ws {

// What was copied. Link and Path are text that is a web address / one or
// more file paths: pasted as the text they are, but listed with links and
// files. Files are files themselves (copied in Explorer); Image is a picture.
enum class ClipKind : std::uint8_t { Text = 0, Link = 1, Path = 2, Files = 3, Image = 4 };

enum class ClipImageFormat : std::uint8_t { Png = 0, Gif, Jpeg };
QString clipImageExtension(ClipImageFormat format);

// The kind of copied text, from what it looks like.
ClipKind kindOfText(const QString& text);

// What tells copied text (or files) from any other: the same again is the
// same entry. Any thread.
QByteArray clipHash(ClipKind kind, const QString& text);

// A clipboard format as the program that copied it gave it, kept for what
// text and a picture cannot paste back as it was: an equation, an embedded
// object (GitHub #58). Registered formats by their name ("Embed Source");
// of the standard ones, "CF_UNICODETEXT" and "CF_ENHMETAFILE" (the
// metafile's bits).
struct ClipFormat {
    QString name;
    QByteArray data;
};

// Several, in their order on the clipboard, as one value for the database
// (empty for none) ...
QByteArray packClipFormats(const QList<ClipFormat>& formats);
// ... and back; none if `packed` is not one.
QList<ClipFormat> unpackClipFormats(const QByteArray& packed);

// One entry of the clipboard history, as kept in memory: what the list, the
// preview and searching need. The formats that paste it as it was (HTML,
// RTF, the picture) stay on disk until then (ClipStore::payload).
struct Clip {
    qint64 id = 0;
    ClipKind kind = ClipKind::Text;
    // Text, Link, Path: the text, or its first kTextInMemory characters;
    // Files: one path per line; Image: empty, but for an equation's or an
    // object's own text (Word's linear form of an equation).
    QString text;
    qint64 textLength = 0; // of the whole text, in UTF-16 units
    QByteArray hash; // of the content: copying the same again moves the entry to the top
    qint64 created = 0; // ms since the epoch, first copied
    qint64 used = 0; // last copied or pasted: the list is newest first by this
    // Last pasted from the history, 0 = never. Expiry counts the later of
    // this and `used`; with keepsPlaceOnPaste a paste sets only this one.
    qint64 pasted = 0;
    QString source; // the program it was copied from ("微信") ...
    QString sourcePath; // ... and its file
    qint64 group = 0; // 0: none (it expires); else a ClipGroup, kept for good
    // Its place among its group's entries, smallest first (the user's order:
    // pasting or copying it again does not move it there). Unused out of groups.
    qint64 rank = 0;
    int width = 0; // Image, in pixels
    int height = 0;
    qint64 bytes = 0; // Image: of its original encoded file
    ClipImageFormat imageFormat = ClipImageFormat::Png;
    bool animated = false;
    bool html = false; // it has formatted versions to paste
    bool rtf = false;
    // Image: the words in it (Windows' text recognition), for searching;
    // `read` once it was looked at, words or none (GitHub #9).
    QString ocr;
    bool read = false;
    // Image: taken with Win顺's own screen capture (src/capture), not copied
    // from a program: the screenshot history (jt, the 截图 tag, MCP's
    // screenshot_history). Kept when the same pixels are copied again.
    bool screenshot = false;
    // Image: it has a project (ClipStore::project), the annotated picture's
    // editable form, in a file of its own beside the PNG.
    bool project = false;
    // Image: it is an equation or an embedded object, its picture what the
    // list shows; the program's own formats paste it (ClipCapture::formats).
    bool formats = false;

    QStringList files() const; // Files, Path: the paths
    bool truncated() const { return textLength > text.size(); }
    // What the search finds it by: its text; a picture's words read from it,
    // or the text an equation or an object came with (which the reading
    // does not replace: it goes to `ocr`).
    const QString& searchText() const { return kind == ClipKind::Image && (!formats || text.isEmpty()) ? ocr : text; }
};

// Entries the user keeps. The built-in one, "固定", has no name of its own.
struct ClipGroup {
    qint64 id = 0;
    QString name;
};

// Something just copied, as read from the clipboard.
struct ClipCapture {
    ClipKind kind = ClipKind::Text;
    QString text; // Files: one path per line; Image: an equation's or an object's (with `formats`)
    QByteArray html; // "HTML Format" as it was on the clipboard (UTF-8, with its header)
    QByteArray rtf;
    QByteArray imageData; // Image: original encoded bytes, including all animation frames
    ClipImageFormat imageFormat = ClipImageFormat::Png;
    bool animated = false;
    QByteArray hash; // Image: pixels for stills, original bytes for animations
    int width = 0;
    int height = 0;
    QString source;
    QString sourcePath;
    bool screenshot = false; // Image: Win顺's screen capture took it (Clip::screenshot)
    // Image: the project to keep with it (opaque bytes; empty: none). The
    // same pixels copied again with one replace the old entry's; without,
    // the old one stays.
    QByteArray project;
    // Image: an equation or an embedded object's own formats, put on the
    // clipboard before the picture when it is pasted (packClipFormats();
    // empty: none). Replaced and kept like `project`.
    QByteArray formats;
    // On the clipboard already when the history started: copied at some
    // earlier time, so an entry it matches stays where it is.
    bool initial = false;
};

// An entry brought in from elsewhere (ClipStore::importClips): the history of
// another program, or lines of a text file. Times are as the source had them.
struct ClipImportItem {
    ClipKind kind = ClipKind::Text;
    QString text; // Files: one path per line
    QByteArray html;
    QByteArray rtf;
    QByteArray imageData; // Image: original encoded bytes
    ClipImageFormat imageFormat = ClipImageFormat::Png;
    bool animated = false;
    QByteArray hash; // as for ClipCapture; worked out if empty
    int width = 0;
    int height = 0;
    qint64 created = 0; // ms since the epoch
    qint64 used = 0;
    QString source; // the program it was copied from, if known
    // The group it goes in, by name (made if there is none of that name);
    // empty: the import's own group. Their order in a batch is their order in it.
    QString group;
};

struct ClipImportResult {
    int added = 0;
    int duplicates = 0; // the history had it already, or an earlier one of the batch did
    int failed = 0; // could not be saved
    bool ownGroupUsed = false; // some went into the import's own group, not one they named
};

// The rest of an entry, read when it is pasted.
struct ClipPayload {
    QString text; // whole
    QByteArray html;
    QByteArray rtf;
    QString imagePath; // Image: its original encoded file
    QByteArray formats; // Image: packClipFormats(), if it has them
};

// A removed entry with everything needed to put it back (undo).
struct ClipRecord {
    Clip clip;
    QString text; // whole
    QByteArray html;
    QByteArray rtf;
    QByteArray formats;
};

// A removed group with its entries, to put back (undo).
struct ClipGroupRecord {
    ClipGroup group; // id 0: nothing was removed
    int index = 0; // its place among the groups
    int position = 0; // as saved
    std::vector<ClipRecord> clips;
};

// Which entries to list.
struct ClipFilter {
    enum class Category { All, Text, Links, Images, Files, Group };
    Category category = Category::All;
    qint64 group = 0; // Category::Group
    // Words that must all occur; "@name" is the program it came from. With
    // any, hidden entries are found too (ClipStore::setHidden).
    QString query;
    // The entries in "固定" first, in their own order, then the rest newest
    // first (not for a group: its entries are in their own order anyway).
    bool pinnedFirst = false;
    // The screenshot history (jt): only pictures Win顺's capture took, the
    // newest taken first (by `created`, not by when last pasted); the words
    // are looked for in what was read in them (ocr). Overrides the rest.
    bool screenshots = false;
};

// Finds the words of a query in entries: as written (ignoring case), and
// Chinese by pinyin ("hy" for 会议).
class ClipMatcher {
public:
    explicit ClipMatcher(const QString& query);

    bool empty() const { return m_terms.isEmpty() && m_sourceTerms.isEmpty(); }
    bool matches(const Clip& clip) const;
    // Where the words occur in `text` (start, length), at most one each.
    QList<std::pair<qsizetype, qsizetype>> spans(QStringView text) const;

private:
    struct Term {
        QString text;
        std::optional<pinyin::Matcher> pinyin; // when the term can be pinyin ("hy" for 会议)
    };
    static bool find(const Term& term, QStringView text, qsizetype* start, qsizetype* length);

    QList<Term> m_terms;
    QList<Term> m_sourceTerms; // "@微信"
};

// What pasting several entries at once puts on the clipboard: if they are
// all files, all of those files; otherwise their text, in the given order,
// joined by `separator` (files as their paths). Pictures cannot be joined
// with anything and are left out.
struct ClipBundle {
    QString text;
    QStringList files; // only files were picked
    int skippedImages = 0;
};
ClipBundle bundle(std::span<const Clip* const> clips, const std::function<QString(const Clip&)>& wholeText,
    QStringView separator);

// The clipboard history: entries and groups in an SQLite database, pictures
// as image files next to it. Everything but the formats for pasting is kept in
// memory, newest first. Used on one thread (the GUI's, or whichever made it).
//
// The disk is written on a thread of its own, in the order things happen:
// what is copied, pasted, moved or dropped is in memory (and listed) at once,
// on disk a moment later, so a large copy does not hold the GUI up. What has
// to read the disk (pasting the formats, removing for undo, the groups) waits
// for what was queued before it.
class ClipStore {
public:
    static constexpr qint64 kPinned = 1; // the built-in group "固定"
    static constexpr qsizetype kTextInMemory = 64 * 1024; // characters of each text, for the list and searching

    struct Limits {
        int maxItems = 300; // entries in no group; 0 = no limit
        int maxDays = 30; // since last copied or pasted; 0 = no limit
    };

    // What became of what was written in the background, told on the store's
    // thread.
    struct Listener {
        std::function<void()> lost; // entries that could not be saved went from clips() again
        std::function<void(qint64 id)> pictureSaved; // its file is there now (imagePending())
    };

    // `folder` holds clipboard.db, images\ and projects\.
    explicit ClipStore(QString folder);
    ~ClipStore(); // writes what is still queued first

    ClipStore(const ClipStore&) = delete;
    ClipStore& operator=(const ClipStore&) = delete;

    bool open(qint64 now); // loads the history; false if the database cannot be used
    void close(); // flushes writes, releases loaded history and the database thread; files stay on disk
    bool isOpen() const { return m_open; }
    void setLimits(Limits limits, qint64 now); // drops what is over them
    void setListener(Listener listener) { m_listener = std::move(listener); }
    // Waits until everything queued is on disk and the listener was told.
    void flush();

    const std::vector<Clip>& clips() const { return m_clips; } // newest first
    const Clip* clip(qint64 id) const;
    const std::vector<ClipGroup>& groups() const { return m_groups; } // "固定" first
    const ClipGroup* group(qint64 id) const;
    int groupSize(qint64 id) const;

    // A new entry, or the same content copied again: that entry moves to the
    // top (and keeps its group), unless the capture is `initial`. Returns its
    // id, 0 if it could not be saved.
    qint64 add(const ClipCapture& capture, qint64 now);
    // Pasted from the history: `pasted` is set, and the entry goes to the top
    // (unless keepsPlaceOnPaste).
    void touch(qint64 id, qint64 now);
    // On, a paste from the history leaves the entry where it is: neither its
    // place nor `used` changes (the list is sorted by it on every start, so
    // changing it alone would move the entry then); `pasted` keeps it from
    // expiring. Copying the content again is a new copy and still moves it
    // up (add()). GitHub #122.
    void setKeepsPlaceOnPaste(bool on) { m_keepsPlace = on; }
    bool keepsPlaceOnPaste() const { return m_keepsPlace; }
    // The pictures (and projects) of removed entries stay until
    // discardImages(), so that restore() can put them back.
    std::vector<ClipRecord> remove(std::span<const qint64> ids);
    void restore(std::vector<ClipRecord> records);
    void discardImages(std::span<const qint64> ids); // of entries no longer in the history
    void setGroup(std::span<const qint64> ids, qint64 group); // 0: out of any group; into one: at its top
    // Moves an entry of a group to where `target`, of the same group, is
    // (before it moving up, after it moving down); the rest keep their order.
    void moveInGroup(qint64 id, qint64 target);
    std::vector<qint64> groupOrder(qint64 group) const; // its entries' ids, in their order
    qint64 addGroup(const QString& name); // 0 if it could not be saved
    // `base`, or "base 2", "base 3" ... when a group has that name already.
    QString uniqueGroupName(const QString& base) const;
    // Entries from elsewhere, all at once: one write, the list updated once.
    // Never added by add(), which would move the one it matches to the top
    // and count toward the limits: these go into groups (named by each item;
    // `ownGroup` for those with none, made when the first needs it), which
    // the limits leave alone, after the entries a group has. An entry the
    // history has already is skipped. Pictures are written beside the
    // others like copied ones.
    // `written` is called on the writer's thread once the batch is on disk
    // (at once if there is nothing to write): a reader that waits for it
    // never gets more than a batch ahead of the disk.
    ClipImportResult importClips(std::vector<ClipImportItem> items, const QString& ownGroup,
        std::function<void()> written = {});
    void renameGroup(qint64 id, const QString& name);
    // A group and its entries; not the built-in one. Their pictures stay
    // until discardImages(), as for remove().
    ClipGroupRecord removeGroup(qint64 id);
    void restoreGroup(ClipGroupRecord record);
    int clearHistory(); // the entries in no group; returns how many

    ClipPayload payload(qint64 id) const; // with the picture's file there
    QString imagePath(qint64 id) const;
    // A picture's project: where its file goes, and its bytes (waits for what
    // was queued before). Empty if the entry has none, or has lost the file:
    // then it is marked as having none, and the picture alone is what is left.
    QString projectPath(qint64 id) const;
    QByteArray project(qint64 id) const;
    QString folder() const { return m_folder; } // the history's files are in it
    // Just copied, its picture still being written: no file at imagePath() yet.
    bool imagePending(qint64 id) const { return m_picturesPending.contains(id); }
    // Pictures whose words were not read yet, newest first (their files
    // written); with `all`, then those read already too.
    std::vector<qint64> unreadPictures(bool all = false) const;
    void setPictureText(qint64 id, const QString& text); // read: the words in it, maybe none

    // Small values kept with the history (the separator for joined pastes).
    QString value(const QString& key) const;
    void setValue(const QString& key, const QString& value);

    // Entries find() leaves out unless words are searched for: records of
    // copied files that are gone (MissingFiles). By id, with `used` as it
    // was when judged: one copied or pasted again since, or put in a group,
    // is listed again. In memory only; they still count toward the limits.
    void setHidden(QHash<qint64, qint64> used) { m_hidden = std::move(used); }
    bool hidden(const Clip& clip) const;
    int hiddenCount() const; // of the entries in the history now
    // Whether gone files are judged (hidden or cleaned out): the entries in
    // no group are then marked gone as judged, not by their first file.
    void setJudged(bool judged) { m_judged = judged; }
    bool judged() const { return m_judged; }

    // Indexes into clips() of the entries to list: newest first; a group's
    // in their order (and "固定"'s first with pinnedFirst); screenshots by
    // when they were taken.
    std::vector<int> find(const ClipFilter& filter) const;
    // The first nine of All, independent of a window's search or category.
    // Stable entry id (not a model row), or 0 when that slot is empty.
    qint64 quickPasteEntry(int number, bool pinnedFirst) const;

private:
    class Writer;

    void prune(qint64 now);
    void dropRows(const std::vector<qint64>& ids); // and their pictures and projects
    void moveToFront(std::size_t index);
    void saved(qint64 id, bool ok); // from the writer
    QString imagePath(qint64 id, ClipImageFormat format) const;
    void removeImageFiles(qint64 id) const; // also after an entry left memory
    bool writeProject(qint64 id, const QByteArray& bytes) const; // on the writer's thread

    QString m_folder;
    QString m_connection; // name of this store's database connection
    bool m_open = false;
    Limits m_limits;
    bool m_keepsPlace = false;
    std::vector<Clip> m_clips;
    std::vector<ClipGroup> m_groups;
    qint64 m_nextId = 1; // ids are never used twice (a removed entry's picture stays for undo)
    QSet<qint64> m_picturesPending;
    QHash<qint64, qint64> m_hidden; // id -> when it was last used, as judged (setHidden)
    bool m_judged = false;
    Listener m_listener;
    std::unique_ptr<QObject> m_context; // what the writer's news is queued to, on the store's thread
    std::unique_ptr<Writer> m_writer; // last: stopped first
};

} // namespace ws
