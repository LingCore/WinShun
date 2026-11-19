#pragma once

#include <QImage>
#include <QMutex>
#include <QQuickImageResponse>
#include <QQuickTextureFactory>
#include <QString>

#include <atomic>
#include <memory>
#include <utility>

namespace ws {

// An image response a job on a thread pool finishes (ThumbnailProvider,
// ClipThumbnailProvider). Qt may delete a response before its job is done:
// the engine that asked is gone (a window's, a test's), or it no longer wants
// the picture. So a job holds a Handle, never the response: through it the
// job reaches the response only while it lives, under the lock its
// destructor takes too.
class PooledImageResponse : public QQuickImageResponse {
    struct Link {
        QMutex mutex;
        PooledImageResponse* response = nullptr;
        std::atomic<bool> cancelled {false};
    };

public:
    class Handle {
    public:
        bool cancelled() const { return m_link->cancelled.load(std::memory_order_relaxed); }
        // Once, even when cancelled: Qt cleans up after finished(). Nothing
        // when the response is gone.
        void finish(QImage image, QString error = {}) const
        {
            QMutexLocker lock(&m_link->mutex);
            if (m_link->response)
                m_link->response->finish(std::move(image), std::move(error));
        }

    private:
        friend class PooledImageResponse;
        explicit Handle(std::shared_ptr<Link> link)
            : m_link(std::move(link))
        {
        }
        std::shared_ptr<Link> m_link;
    };

    PooledImageResponse() { m_link->response = this; }
    ~PooledImageResponse() override
    {
        QMutexLocker lock(&m_link->mutex); // a job finishing it now is let through first
        m_link->response = nullptr;
    }

    Handle handle() const { return Handle(m_link); }

    // On the thread that asked: a picture kept from before, or none at all.
    void finish(QImage image, QString error = {})
    {
        m_image = std::move(image);
        m_error = std::move(error);
        emit finished();
    }

    QQuickTextureFactory* textureFactory() const override
    {
        return m_image.isNull() ? nullptr : QQuickTextureFactory::textureFactoryForImage(m_image);
    }
    QString errorString() const override { return m_error; }
    void cancel() override { m_link->cancelled.store(true, std::memory_order_relaxed); }

private:
    const std::shared_ptr<Link> m_link = std::make_shared<Link>();
    QImage m_image;
    QString m_error;
};

} // namespace ws
