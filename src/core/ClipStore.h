#pragma once

#include "Pinyin.h"

#include <QByteArray>
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

// The kind of copied text, from what it looks like.
ClipKind kindOfText(const QString& text);

// What tells copied text (or files) from any other: the same again is the
// same entry. Any thread.
QByteArray clipHash(ClipKind kind, const QString& text);

// One entry of the clipboard history, as kept in memory: what the list, the
// preview and searching need. The formats that paste it as it was (HTML,
// RTF, the picture) stay on disk until then (ClipStore::payload).
struct Clip {
    qint64 id = 0;
    ClipKind kind = ClipKind::Text;
    // Text, Link, Path: the text, or its first kTextInMemory characters;
    // Files: one path per line; Image: empty.
    QString text;
    qint64 textLength = 0; // of the whole text, in UTF-16 units
    QByteArray hash; // of the content: copying the same again moves the entry to the top
    qint64 created = 0; // ms since the epoch, first copied
    qint64 used = 0; // last copied or pasted: the list is newest first by this
    QString source; // the program it was copied from ("微信") ...
    QString sourcePath; // ... and its file
    qint64 group = 0; // 0: none (it expires); else a ClipGroup, kept for good
    int width = 0; // Image, in pixels
    int height = 0;
    qint64 bytes = 0; // Image: of its PNG file
    bool html = false; // it has formatted versions to paste
    bool rtf = false;

    QStringList files() const; // Files, Path: the paths
    bool truncated() const { return textLength > text.size(); }
};

// Entries the user keeps. The built-in one, "固定", has no name of its own.
struct ClipGroup {
    qint64 id = 0;
    QString name;
};

// Something just copied, as read from the clipboard.
struct ClipCapture {
    ClipKind kind = ClipKind::Text;
    QString text; // Files: one path per line
    QByteArray html; // "HTML Format" as it was on the clipboard (UTF-8, with its header)
    QByteArray rtf;
    QByteArray png; // Image
    QByteArray hash; // Image: of its pixels; else clipHash(), worked out by add() if empty
    int width = 0;
    int height = 0;
    QString source;
    QString sourcePath;
    // On the clipboard already when the history started: copied at some
    // earlier time, so an entry it matches stays where it is.
    bool initial = false;
};

// The rest of an entry, read when it is pasted.
struct ClipPayload {
    QString text; // whole
    QByteArray html;
    QByteArray rtf;
    QString imagePath; // Image: its PNG file
};

// A removed entry with everything needed to put it back (undo).
struct ClipRecord {
    Clip clip;
    QString text; // whole
    QByteArray html;
    QByteArray rtf;
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
    QString query; // words that must all occur; "@name" is the program it came from
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
// as PNG files next to it. Everything but the formats for pasting is kept in
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
        int maxItems = 200; // entries in no group
        int maxDays = 30; // since last copied or pasted; 0 = no limit
    };

    // What became of what was written in the background, told on the store's
    // thread.
    struct Listener {
        std::function<void()> lost; // entries that could not be saved went from clips() again
        std::function<void(qint64 id)> pictureSaved; // its file is there now (imagePending())
    };

    // `folder` holds clipboard.db and images\.
    explicit ClipStore(QString folder);
    ~ClipStore(); // writes what is still queued first

    ClipStore(const ClipStore&) = delete;
    ClipStore& operator=(const ClipStore&) = delete;

    bool open(qint64 now); // loads the history; false if the database cannot be used
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
    void touch(qint64 id, qint64 now); // pasted from the history: to the top
    // The pictures of removed entries stay until discardImages(), so that
    // restore() can put them back.
    std::vector<ClipRecord> remove(std::span<const qint64> ids);
    void restore(std::vector<ClipRecord> records);
    void discardImages(std::span<const qint64> ids); // of entries no longer in the history
    void setGroup(std::span<const qint64> ids, qint64 group); // 0: out of any group
    qint64 addGroup(const QString& name); // 0 if it could not be saved
    void renameGroup(qint64 id, const QString& name);
    // A group and its entries; not the built-in one. Their pictures stay
    // until discardImages(), as for remove().
    ClipGroupRecord removeGroup(qint64 id);
    void restoreGroup(ClipGroupRecord record);
    int clearHistory(); // the entries in no group; returns how many

    ClipPayload payload(qint64 id) const; // with the picture's file there
    QString imagePath(qint64 id) const;
    // Just copied, its picture still being written: no file at imagePath() yet.
    bool imagePending(qint64 id) const { return m_picturesPending.contains(id); }

    // Small values kept with the history (the separator for joined pastes).
    QString value(const QString& key) const;
    void setValue(const QString& key, const QString& value);

    // Indexes into clips() of the entries to list, newest first.
    std::vector<int> find(const ClipFilter& filter) const;

private:
    class Writer;

    void prune(qint64 now);
    void dropRows(const std::vector<qint64>& ids); // and their pictures
    void moveToFront(std::size_t index);
    void saved(qint64 id, bool ok); // from the writer

    QString m_folder;
    QString m_connection; // name of this store's database connection
    bool m_open = false;
    Limits m_limits;
    std::vector<Clip> m_clips;
    std::vector<ClipGroup> m_groups;
    qint64 m_nextId = 1; // ids are never used twice (a removed entry's picture stays for undo)
    QSet<qint64> m_picturesPending;
    Listener m_listener;
    std::unique_ptr<QObject> m_context; // what the writer's news is queued to, on the store's thread
    std::unique_ptr<Writer> m_writer; // last: stopped first
};

} // namespace ws
