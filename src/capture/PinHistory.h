#pragma once

#include <QByteArray>
#include <QImage>
#include <QPoint>
#include <QString>

#include <functional>
#include <optional>
#include <vector>

namespace ws::capture {

// Restoration order when pressing the global pin hotkey repeatedly (#446):
// - RecentClosed: most recently active first — max(createdMs, closedMs) descending,
//   so a freshly taken screenshot or a just-closed pin comes back first.
// - CreatedTime: strictly by capture/creation timestamp descending.
enum class PinHistoryOrder { RecentClosed, CreatedTime };

PinHistoryOrder pinHistoryOrderFromName(const QString& name);
QString pinHistoryOrderName(PinHistoryOrder order);

// One remembered pin or screenshot entry for repeated-hotkey recall.
// Large bitmaps are compressed to PNG off the GUI thread (setEncodedPng) so
// dozens of entries take a few megabytes instead of hundreds.
struct PinHistoryEntry {
    quint64 id = 0;
    quint64 generation = 1; // incremented whenever pixels change; drops stale background PNG encodings
    QImage image; // cleared once encoded to `png`
    QByteArray png;
    QByteArray project; // editable annotations (.wsedit), empty if none
    QPoint origin; // physical, virtual desktop (where it was captured or closed)
    QPoint anchor; // physical anchor for pinFitted()
    bool fitted = false;
    bool screenshot = true;
    QString sourcePath; // original writable image file path (#427), if any
    QString copiedText; // copied-text pin payload (#388), if any
    QString captureHistoryId; // on-disk CaptureHistory id, for deduplication
    QString clipboardOriginId; // WinShun capture origin id placed on clipboard
    quint32 clipboardSeq = 0; // clipboard sequence number when captured from clipboard
    qint64 createdMs = 0;
    qint64 closedMs = 0; // 0 until the pin is closed
};

// Bounded, pure-logic history stack for pin recall (#446).
// Keeps no window handles or threads of its own so every ordering, eviction,
// edit-update and deduplication rule is unit-tested directly (tst_pinhistory).
class PinHistory {
public:
    static constexpr int kDefaultLimit = 20;
    static constexpr int kMaxLimit = 100;

    void setLimit(int count);
    int limit() const { return m_limit; }
    bool enabled() const { return m_limit > 0; }

    void setOrder(PinHistoryOrder order) { m_order = order; }
    PinHistoryOrder order() const { return m_order; }

    int count() const { return static_cast<int>(m_entries.size()); }
    bool empty() const { return m_entries.empty(); }
    void clear() { m_entries.clear(); }

    // Records a new entry and returns its assigned id (0 when disabled or empty image/png).
    // Deduplicates by non-empty captureHistoryId (updating the existing entry instead).
    quint64 record(PinHistoryEntry entry);

    // Records that a pin closed: stamps `closedMs`, optionally refreshes its
    // edited/transformed image, project, origin and sourcePath.
    void markClosed(quint64 id, qint64 closedMs, const QImage& image = {}, const QByteArray& project = {},
        std::optional<QPoint> origin = std::nullopt, std::optional<QString> sourcePath = std::nullopt);

    // Updates an existing entry's pixels/project (e.g. after editing a pin in place).
    // Returns the new generation (0 if `id` is not found).
    quint64 updateImage(quint64 id, const QImage& image, const QByteArray& project);

    // Stores the background-encoded PNG and releases the uncompressed QImage when
    // `generation` still matches the entry's current generation.
    void setEncodedPng(quint64 id, quint64 generation, QByteArray png);

    // Current generation for `id`, or 0 if absent.
    quint64 generationOf(quint64 id) const;

    // Picks the next entry not currently open on screen according to `m_order`.
    // Returns nullopt when disabled, empty, or every remembered entry is already open.
    std::optional<PinHistoryEntry> nextToRestore(const std::function<bool(quint64)>& isOpen) const;

    // Lookup helpers for deduplication against on-disk CaptureHistory and clipboard state.
    bool containsCaptureHistoryId(const QString& captureHistoryId) const;
    std::optional<quint64> findByClipboardOrigin(const QString& originId) const;
    std::optional<quint64> findByClipboardSeq(quint32 seq) const;
    const PinHistoryEntry* find(quint64 id) const;

    // Fast PNG codec helpers (zlib level 1, matching CaptureHistory).
    static QByteArray encodePng(const QImage& image);
    static QImage decodePng(const QByteArray& png);
    static QImage resolveImage(const PinHistoryEntry& entry);

private:
    qint64 sortKey(const PinHistoryEntry& entry) const;
    void prune();
    PinHistoryEntry* findMutable(quint64 id);

    int m_limit = kDefaultLimit;
    PinHistoryOrder m_order = PinHistoryOrder::RecentClosed;
    quint64 m_nextId = 1;
    std::vector<PinHistoryEntry> m_entries;
};

} // namespace ws::capture
