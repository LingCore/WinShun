#pragma once

#include "Pinyin.h"

#include <QByteArray>
#include <QList>
#include <QString>
#include <QStringList>

#include <cstdint>
#include <functional>
#include <optional>
#include <span>
#include <vector>

namespace ws {

// What was copied. Link and Path are text that is a web address / one or
// more file paths: pasted as the text they are, but listed with links and
// files. Files are files themselves (copied in Explorer); Image is a picture.
enum class ClipKind : std::uint8_t { Text = 0, Link = 1, Path = 2, Files = 3, Image = 4 };

// The kind of copied text, from what it looks like.
ClipKind kindOfText(const QString& text);

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
    QByteArray hash; // Image: of its pixels; worked out from the text otherwise
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
// memory, newest first. Used on one thread (the GUI's).
class ClipStore {
public:
    static constexpr qint64 kPinned = 1; // the built-in group "固定"
    static constexpr qsizetype kTextInMemory = 64 * 1024; // characters of each text, for the list and searching

    struct Limits {
        int maxItems = 200; // entries in no group
        int maxDays = 30; // since last copied or pasted; 0 = no limit
    };

    // `folder` holds clipboard.db and images\.
    explicit ClipStore(QString folder);
    ~ClipStore();

    ClipStore(const ClipStore&) = delete;
    ClipStore& operator=(const ClipStore&) = delete;

    bool open(qint64 now); // loads the history; false if the database cannot be used
    bool isOpen() const { return m_open; }
    void setLimits(Limits limits, qint64 now); // drops what is over them

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

    ClipPayload payload(qint64 id) const;
    QString imagePath(qint64 id) const;

    // Small values kept with the history (the separator for joined pastes).
    QString value(const QString& key) const;
    void setValue(const QString& key, const QString& value);

    // Indexes into clips() of the entries to list, newest first.
    std::vector<int> find(const ClipFilter& filter) const;

private:
    void prune(qint64 now);
    void removeRows(const std::vector<qint64>& ids, bool deleteImages);
    void moveToFront(std::size_t index);

    QString m_folder;
    QString m_connection; // name of this store's database connection
    bool m_open = false;
    Limits m_limits;
    std::vector<Clip> m_clips;
    std::vector<ClipGroup> m_groups;
};

} // namespace ws
