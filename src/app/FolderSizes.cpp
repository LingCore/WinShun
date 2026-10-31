#include "FolderSizes.h"

#include "FolderSize.h"
#include "Query.h"

using namespace Qt::StringLiterals;

namespace ws {

FolderSizes::FolderSizes(std::function<std::shared_ptr<FileIndex>()> index, QObject* parent)
    : QObject(parent), m_index(std::move(index))
{
}

void FolderSizes::reset()
{
    ++m_generation;
    m_values.clear();
}

QString FolderSizes::text(const QString& path)
{
    if (const auto value = m_values.constFind(path); value != m_values.cend()) return *value;
    const auto index = m_index ? m_index() : nullptr;
    if (!index) return {};
    m_values.insert(path, tr("计算中…"));
    if (!m_worker) m_worker = std::make_unique<ComWorker>();
    const unsigned generation = m_generation;
    m_worker->post([this, index, path, generation, stop = m_worker->stopToken()] {
        const auto cancelled = [&] { return stop.stop_requested() || generation != m_generation; };
        if (cancelled()) return;
        std::optional<std::uint64_t> bytes;
        {
            const auto lock = index->longReadLock();
            if (cancelled()) return;
            bytes = indexedFolderSize(*index, index->findPath(path.toStdWString()), cancelled);
        }
        if (cancelled()) return;
        QMetaObject::invokeMethod(this, [this, path, generation, bytes] {
            if (generation != m_generation) return;
            const QString text = bytes
                ? tr("≈ %1").arg(sizeText(*bytes)) : tr("未知");
            m_values.insert(path, text);
            emit changed(path);
        }, Qt::QueuedConnection);
    });
    return m_values.value(path);
}

} // namespace ws
