#pragma once

#include "FileIndex.h"
#include "platform/ComWorker.h"

#include <QHash>
#include <QObject>

#include <atomic>
#include <functional>
#include <memory>

namespace ws {

class FolderSizes : public QObject {
    Q_OBJECT
public:
    explicit FolderSizes(std::function<std::shared_ptr<FileIndex>()> index, QObject* parent = nullptr);
    QString text(const QString& path);
    void reset();
signals:
    void changed(const QString& path);
private:
    const std::function<std::shared_ptr<FileIndex>()> m_index;
    QHash<QString, QString> m_values;
    std::atomic<unsigned> m_generation {0};
    std::unique_ptr<ComWorker> m_worker; // last: work finishes before its state is destroyed
};

} // namespace ws
