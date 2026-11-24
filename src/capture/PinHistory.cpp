#include "PinHistory.h"

#include <QBuffer>
#include <QDateTime>
#include <QImageReader>
#include <QImageWriter>

#include <algorithm>

using namespace Qt::StringLiterals;

namespace ws::capture {

namespace {

// Matches CaptureHistory: fast zlib level 1 PNG compression.
constexpr int kPngQuality = 80;

} // namespace

PinHistoryOrder pinHistoryOrderFromName(const QString& name)
{
    return name.trimmed().compare(u"created"_s, Qt::CaseInsensitive) == 0 ? PinHistoryOrder::CreatedTime
                                                                          : PinHistoryOrder::RecentClosed;
}

QString pinHistoryOrderName(PinHistoryOrder order)
{
    return order == PinHistoryOrder::CreatedTime ? u"created"_s : u"closed"_s;
}

void PinHistory::setLimit(int count)
{
    m_limit = std::clamp(count, 0, kMaxLimit);
    prune();
}

quint64 PinHistory::record(PinHistoryEntry entry)
{
    if (!enabled() || (entry.image.isNull() && entry.png.isEmpty()))
        return 0;
    if (entry.createdMs <= 0)
        entry.createdMs = QDateTime::currentMSecsSinceEpoch();
    if (!entry.captureHistoryId.isEmpty()) {
        for (PinHistoryEntry& existing : m_entries) {
            if (existing.captureHistoryId == entry.captureHistoryId) {
                existing.image = std::move(entry.image);
                existing.png = std::move(entry.png);
                existing.project = std::move(entry.project);
                existing.origin = entry.origin;
                existing.anchor = entry.anchor;
                existing.fitted = entry.fitted;
                existing.screenshot = entry.screenshot;
                existing.sourcePath = std::move(entry.sourcePath);
                existing.copiedText = std::move(entry.copiedText);
                existing.clipboardOriginId = std::move(entry.clipboardOriginId);
                if (entry.clipboardSeq != 0)
                    existing.clipboardSeq = entry.clipboardSeq;
                existing.createdMs = entry.createdMs;
                existing.closedMs = entry.closedMs;
                ++existing.generation;
                prune();
                return existing.id;
            }
        }
    }
    entry.id = m_nextId++;
    entry.generation = 1;
    m_entries.push_back(std::move(entry));
    const quint64 id = m_entries.back().id;
    prune();
    return id;
}

void PinHistory::markClosed(quint64 id, qint64 closedMs, const QImage& image, const QByteArray& project,
    std::optional<QPoint> origin, std::optional<QString> sourcePath)
{
    PinHistoryEntry* entry = findMutable(id);
    if (!entry)
        return;
    entry->closedMs = closedMs > 0 ? closedMs : QDateTime::currentMSecsSinceEpoch();
    if (!image.isNull()) {
        entry->image = image;
        entry->png.clear();
        ++entry->generation;
    }
    if (!project.isEmpty() || !image.isNull())
        entry->project = project;
    if (origin) {
        entry->origin = *origin;
        // A pin moved or reopened after closing returns to where it was closed.
        entry->anchor = *origin;
    }
    if (sourcePath)
        entry->sourcePath = *sourcePath;
}

quint64 PinHistory::updateImage(quint64 id, const QImage& image, const QByteArray& project)
{
    PinHistoryEntry* entry = findMutable(id);
    if (!entry || image.isNull())
        return 0;
    entry->image = image;
    entry->png.clear();
    entry->project = project;
    return ++entry->generation;
}

void PinHistory::setEncodedPng(quint64 id, quint64 generation, QByteArray png)
{
    if (png.isEmpty())
        return;
    PinHistoryEntry* entry = findMutable(id);
    if (!entry || entry->generation != generation)
        return;
    entry->png = std::move(png);
    entry->image = {};
}

quint64 PinHistory::generationOf(quint64 id) const
{
    const PinHistoryEntry* entry = find(id);
    return entry ? entry->generation : 0;
}

std::optional<PinHistoryEntry> PinHistory::nextToRestore(const std::function<bool(quint64)>& isOpen) const
{
    if (!enabled() || m_entries.empty())
        return std::nullopt;
    const PinHistoryEntry* best = nullptr;
    for (const PinHistoryEntry& entry : m_entries) {
        if (isOpen && isOpen(entry.id))
            continue;
        if (!best) {
            best = &entry;
            continue;
        }
        const qint64 key = sortKey(entry);
        const qint64 bestKey = sortKey(*best);
        if (key > bestKey || (key == bestKey && entry.id > best->id))
            best = &entry;
    }
    return best ? std::optional<PinHistoryEntry>(*best) : std::nullopt;
}

bool PinHistory::containsCaptureHistoryId(const QString& captureHistoryId) const
{
    if (captureHistoryId.isEmpty())
        return false;
    return std::ranges::any_of(m_entries, [&](const PinHistoryEntry& e) { return e.captureHistoryId == captureHistoryId; });
}

std::optional<quint64> PinHistory::findByClipboardOrigin(const QString& originId) const
{
    if (originId.isEmpty())
        return std::nullopt;
    for (auto it = m_entries.rbegin(); it != m_entries.rend(); ++it) {
        if (it->clipboardOriginId == originId)
            return it->id;
    }
    return std::nullopt;
}

std::optional<quint64> PinHistory::findByClipboardSeq(quint32 seq) const
{
    if (seq == 0)
        return std::nullopt;
    for (auto it = m_entries.rbegin(); it != m_entries.rend(); ++it) {
        if (it->clipboardSeq == seq)
            return it->id;
    }
    return std::nullopt;
}

const PinHistoryEntry* PinHistory::find(quint64 id) const
{
    if (id == 0)
        return nullptr;
    const auto it = std::ranges::find(m_entries, id, &PinHistoryEntry::id);
    return it == m_entries.end() ? nullptr : &*it;
}

PinHistoryEntry* PinHistory::findMutable(quint64 id)
{
    if (id == 0)
        return nullptr;
    const auto it = std::ranges::find(m_entries, id, &PinHistoryEntry::id);
    return it == m_entries.end() ? nullptr : &*it;
}

qint64 PinHistory::sortKey(const PinHistoryEntry& entry) const
{
    return m_order == PinHistoryOrder::CreatedTime ? entry.createdMs : std::max(entry.createdMs, entry.closedMs);
}

void PinHistory::prune()
{
    if (m_limit <= 0) {
        m_entries.clear();
        return;
    }
    if (std::ssize(m_entries) <= m_limit)
        return;
    std::ranges::sort(m_entries, [this](const PinHistoryEntry& a, const PinHistoryEntry& b) {
        const qint64 ka = sortKey(a);
        const qint64 kb = sortKey(b);
        return ka != kb ? ka > kb : a.id > b.id;
    });
    m_entries.resize(static_cast<std::size_t>(m_limit));
}

QByteArray PinHistory::encodePng(const QImage& image)
{
    if (image.isNull())
        return {};
    QByteArray bytes;
    QBuffer buffer(&bytes);
    if (!buffer.open(QIODevice::WriteOnly))
        return {};
    QImageWriter writer(&buffer, "png");
    writer.setQuality(kPngQuality);
    const QImage opaque = image.hasAlphaChannel() ? image.convertToFormat(QImage::Format_ARGB32_Premultiplied) : image;
    return writer.write(opaque) ? bytes : QByteArray();
}

QImage PinHistory::decodePng(const QByteArray& png)
{
    if (png.isEmpty())
        return {};
    QBuffer buffer;
    buffer.setData(png);
    if (!buffer.open(QIODevice::ReadOnly))
        return {};
    QImageReader reader(&buffer, "png");
    reader.setAllocationLimit(0);
    return reader.read();
}

QImage PinHistory::resolveImage(const PinHistoryEntry& entry)
{
    return !entry.image.isNull() ? entry.image : decodePng(entry.png);
}

} // namespace ws::capture
