#include "CaptureOcr.h"

#include "Geometry.h"
#include "QrCode.h"
#include "platform/PaddleOcr.h"

#include <QCoreApplication>
#include <QDebug>
#include <QElapsedTimer>
#include <QRegularExpression>

#include <windows.h>

#include <algorithm>
#include <chrono>

using namespace Qt::StringLiterals;
using namespace std::chrono_literals;

namespace ws::capture {

namespace {

// How long the reader stays loaded after the last picture.
constexpr auto kKeepLoaded = 60s;
// The search box takes one line; longer text is cut there.
constexpr int kMaxQuery = 256;

} // namespace

CaptureOcr::CaptureOcr(QObject* parent)
    : QObject(parent)
    , m_thread([this](std::stop_token stop) { work(stop); })
{
}

CaptureOcr::~CaptureOcr()
{
    m_thread.request_stop();
    m_wake.notify_all();
    if (m_thread.joinable())
        m_thread.join();
}

bool CaptureOcr::available()
{
    return PaddleOcr::installed(QCoreApplication::applicationDirPath());
}

void CaptureOcr::read(const QImage& image, double dpr)
{
    ++m_round;
    m_text.clear();
    m_message.clear();
    m_title.clear();
    m_link.clear();
    if (image.isNull()) {
        m_busy = false;
        m_message = tr("没有图片可以识别。");
        emit changed();
        return;
    }
    if (!available()) {
        m_busy = false;
        m_message = tr("没有安装文字识别模型（Win顺 文件夹里的 ocr 文件夹），无法识别文字。");
        emit changed();
        return;
    }
    m_busy = true;
    emit changed();
    {
        std::lock_guard lock(m_mutex);
        m_job = Job {m_round, image, ocrUpscale(image.size(), dpr)}; // an older one not started yet is dropped
    }
    m_wake.notify_all();
}

void CaptureOcr::showTexts(const QStringList& texts, const QString& title, const QString& none)
{
    ++m_round; // a reading still under way is for a picture no longer shown
    m_busy = false;
    m_title = title;
    m_text = texts.join(u'\n');
    m_message = texts.isEmpty() ? none : QString();
    m_link = texts.size() == 1 ? qrLink(texts.first()) : QString();
    emit changed();
}

void CaptureOcr::scan(const QImage& image, double dpr, std::function<void(QList<RedactLine>)> done)
{
    enqueueScan(image, image.rect(), dpr, false, [done = std::move(done)](RedactScan scan) { done(std::move(scan.lines)); });
}

void CaptureOcr::scan(const QImage& image, const QRect& region, double dpr, std::function<void(RedactScan)> done)
{
    enqueueScan(image, region, dpr, true, std::move(done));
}

void CaptureOcr::enqueueScan(const QImage& image, const QRect& region, double dpr, bool grayscale, std::function<void(RedactScan)> done)
{
    const int ticket = ++m_scanTicket;
    m_scanDone = std::move(done);
    if (image.isNull() || region.intersected(image.rect()).isEmpty() || !available()) {
        QMetaObject::invokeMethod(this, [this, ticket] { scanDone(ticket, {}); }, Qt::QueuedConnection);
        return;
    }
    {
        std::lock_guard lock(m_mutex);
        m_scan = ScanJob {ticket, image, region.intersected(image.rect()), dpr, grayscale}; // an older one not started yet is dropped
    }
    m_wake.notify_all();
}

void CaptureOcr::cancelScan()
{
    ++m_scanTicket;
    m_scanDone = nullptr;
    std::lock_guard lock(m_mutex);
    m_scan.reset();
}

void CaptureOcr::scanDone(int ticket, RedactScan scan)
{
    if (ticket != m_scanTicket || !m_scanDone)
        return; // asked again or given up since
    std::function<void(RedactScan)> done = std::move(m_scanDone);
    m_scanDone = nullptr;
    done(std::move(scan));
}

QString CaptureOcr::oneLine(const QString& text)
{
    return ocrOneLine(text, kMaxQuery); // lines as words: the search box is one line
}

void CaptureOcr::search(const QString& text)
{
    const QString query = oneLine(text);
    if (!query.isEmpty())
        emit searchRequested(query);
}

void CaptureOcr::webSearch(const QString& text)
{
    const QString query = oneLine(text);
    if (!query.isEmpty())
        emit webSearchRequested(query);
}

void CaptureOcr::done(int round, const QString& text, const QString& error)
{
    if (round != m_round)
        return; // a newer picture is being read
    m_busy = false;
    m_text = text;
    if (!error.isEmpty())
        m_message = tr("识别失败：%1").arg(error);
    else if (text.isEmpty())
        m_message = tr("图片里没有找到文字。");
    else
        m_message.clear();
    emit changed();
}

void CaptureOcr::work(const std::stop_token& stop)
{
    ::SetThreadPriority(::GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
    const QString folder = QCoreApplication::applicationDirPath();
    // Its own threads besides this one, as the clipboard's: up to 4, half the processors.
    const int threads = std::clamp(static_cast<int>(std::thread::hardware_concurrency()) / 2, 1, 4);
    std::unique_ptr<PaddleOcr> reader;
    for (;;) {
        Job job;
        std::optional<ScanJob> scan;
        {
            std::unique_lock lock(m_mutex);
            const bool woke = m_wake.wait_for(lock, stop, kKeepLoaded, [this] { return m_job.has_value() || m_scan.has_value(); });
            if (stop.stop_requested())
                break;
            if (!woke) {
                reader.reset(); // a minute without a picture: its memory goes
                continue;
            }
            if (m_job) {
                job = std::move(*m_job);
                m_job.reset();
            } else {
                scan = std::move(m_scan);
                m_scan.reset();
            }
        }
        QElapsedTimer clock;
        clock.start();
        QString error;
        qint64 loadMs = 0;
        if (!reader) {
            reader = PaddleOcr::load(folder, threads, &error);
            loadMs = clock.elapsed();
        }
        if (scan) {
            const QImage region = scan->region == scan->image.rect() ? scan->image : scan->image.copy(scan->region);
            const double scale = ocrUpscale(region.size(), scan->dpr);
            RedactScan result;
            if (scan->grayscale)
                result.gray = region.convertToFormat(QImage::Format_Grayscale8);
            if (reader) {
                QImage picture = region;
                if (scale > 1.0)
                    picture = picture.scaled(std::max(1, static_cast<int>(std::lround(picture.width() * scale))),
                        std::max(1, static_cast<int>(std::lround(picture.height() * scale))), Qt::IgnoreAspectRatio,
                        Qt::SmoothTransformation);
                QList<OcrPiece> pieces;
                if (reader->readPieces(picture, &pieces, &error)) {
                    for (const OcrPiece& p : std::as_const(pieces)) {
                        RedactLine line;
                        line.text = p.text;
                        for (const QPointF& corner : p.quad)
                            line.quad.append(corner / scale);
                        line.chars = p.chars;
                        result.lines.append(std::move(line));
                    }
                }
            }
            qInfo().noquote() << u"Screen capture: scan for redaction read in %1 ms (loading %2 ms), %3x%4 at %5x, %6 lines"_s.arg(clock.elapsed())
                                     .arg(loadMs)
                                     .arg(region.width())
                                     .arg(region.height())
                                     .arg(scale, 0, 'f', 2)
                                     .arg(result.lines.size());
            if (!error.isEmpty())
                qWarning().noquote() << "Screen capture: scan for redaction failed:" << error;
            QMetaObject::invokeMethod(this, [this, ticket = scan->ticket, result = std::move(result)]() mutable { scanDone(ticket, std::move(result)); },
                Qt::QueuedConnection);
            continue;
        }
        QStringList lines;
        if (reader) {
            QImage picture = job.image;
            if (job.scale > 1.0)
                picture = picture.scaled(std::max(1, static_cast<int>(std::lround(picture.width() * job.scale))),
                    std::max(1, static_cast<int>(std::lround(picture.height() * job.scale))), Qt::IgnoreAspectRatio,
                    Qt::SmoothTransformation);
            if (!reader->read(picture, &lines, &error))
                lines.clear();
        }
        qInfo().noquote() << u"Screen capture: text read in %1 ms (loading %2 ms), %3x%4 at %5x, %6 lines"_s.arg(clock.elapsed())
                                 .arg(loadMs)
                                 .arg(job.image.width())
                                 .arg(job.image.height())
                                 .arg(job.scale, 0, 'f', 2)
                                 .arg(lines.size());
        if (!error.isEmpty())
            qWarning().noquote() << "Screen capture: text recognition failed:" << error;
        QMetaObject::invokeMethod(this, [this, round = job.round, text = lines.join(u'\n'), error] { done(round, text, error); },
            Qt::QueuedConnection);
    }
}

} // namespace ws::capture
