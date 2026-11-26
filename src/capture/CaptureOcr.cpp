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
#include <utility>

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

QString CaptureOcr::programFolder()
{
    return s_programFolder.isEmpty() ? QCoreApplication::applicationDirPath() : s_programFolder;
}

bool CaptureOcr::available()
{
    return PaddleOcr::installed(programFolder());
}

void CaptureOcr::read(const QImage& image, double dpr, bool thenTranslate)
{
    clearTranslation();
    m_thenTranslate = thenTranslate;
    m_pendingImage = {};
    ++m_round;
    m_text.clear();
    m_message.clear();
    m_title.clear();
    m_link.clear();
    m_table.clear();
    m_tableHtml.clear();
    if (image.isNull()) {
        m_busy = false;
        m_message = tr("没有图片可以识别。");
        emit changed();
        return;
    }
    if (!available()) {
        m_busy = false;
        m_pendingImage = image;
        m_pendingDpr = dpr;
        m_message = tr("增强文字识别尚未安装。安装后可以离线使用，并继续识别这张图片。");
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

void CaptureOcr::retry()
{
    if (!m_pendingImage.isNull() && available())
        read(std::exchange(m_pendingImage, {}), m_pendingDpr, m_thenTranslate);
}

void CaptureOcr::showTexts(const QStringList& texts, const QString& title, const QString& none)
{
    clearTranslation();
    m_thenTranslate = false;
    m_pendingImage = {};
    ++m_round; // a reading still under way is for a picture no longer shown
    m_busy = false;
    m_title = title;
    m_text = texts.join(u'\n');
    m_table.clear();
    m_tableHtml.clear();
    m_message = texts.isEmpty() ? none : QString();
    m_link = texts.size() == 1 ? qrLink(texts.first()) : QString();
    emit changed();
}

void CaptureOcr::showText(const QString& text)
{
    clearTranslation();
    m_thenTranslate = false;
    m_pendingImage = {};
    ++m_round;
    m_busy = false;
    m_title.clear();
    m_link.clear();
    m_table.clear();
    m_tableHtml.clear();
    m_text = text;
    m_message.clear();
    emit changed();
}

void CaptureOcr::scan(const QImage& image, double dpr, std::function<void(QList<RedactLine>)> done,
    std::shared_ptr<const std::atomic<bool>> cancelled)
{
    // Reap callbacks of already-cancelled jobs on the GUI thread.
    std::erase_if(m_lineDone, [](const auto& pair) {
        return pair.second.cancelled && pair.second.cancelled->load(std::memory_order_acquire);
    });
    if (cancelled && cancelled->load(std::memory_order_acquire))
        return;
    const int ticket = ++m_lineTicket;
    if (image.isNull() || !available()) {
        if (done) {
            m_lineDone.emplace(ticket, LineCallback {std::move(done), std::move(cancelled)});
            QMetaObject::invokeMethod(this, [this, ticket] { lineScanDone(ticket, {}); }, Qt::QueuedConnection);
        }
        return;
    }
    if (done)
        m_lineDone.emplace(ticket, LineCallback {std::move(done), cancelled});
    {
        std::lock_guard lock(m_mutex);
        std::erase_if(m_lineQueue, [](const LineJob& j) { return j.cancelled && j.cancelled->load(std::memory_order_acquire); });
        m_lineQueue.push_back(LineJob {ticket, image, dpr, std::move(cancelled)});
    }
    m_wake.notify_all();
}

void CaptureOcr::lineScanDone(int ticket, QList<RedactLine> lines)
{
    const auto it = m_lineDone.find(ticket);
    if (it == m_lineDone.end())
        return;
    LineCallback entry = std::move(it->second);
    m_lineDone.erase(it);
    if (entry.cancelled && entry.cancelled->load(std::memory_order_acquire))
        return;
    if (entry.done)
        entry.done(std::move(lines));
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

QString CaptureOcr::mergeLines(const QString& text)
{
    const QStringList rawLines = text.split(u'\n');
    QString out;
    QString prevLine;
    bool pendingBlank = false;
    for (const QString& raw : rawLines) {
        const QString line = raw.simplified();
        if (line.isEmpty()) {
            if (!out.isEmpty())
                pendingBlank = true;
            prevLine.clear();
            continue;
        }
        if (out.isEmpty()) {
            out = line;
        } else if (pendingBlank) {
            out += u"\n\n"_s + line;
            pendingBlank = false;
        } else if (!prevLine.isEmpty() && ocrGoesOn(prevLine, line)) {
            ocrGlue(out, line);
        } else {
            out += u'\n' + line;
        }
        prevLine = line;
    }
    return out;
}

void CaptureOcr::search(const QString& text)
{
    const QString query = oneLine(text);
    if (!query.isEmpty())
        emit searchRequested(query);
}

void CaptureOcr::translate(const QString& text)
{
    const QString words = ocrParagraphs(text, kMaxTranslate);
    if (words.isEmpty())
        return;
    m_translating = true;
    m_translation.clear();
    m_translationNote.clear();
    m_translationNeeds.clear();
    emit translationChanged();
    emit translateRequested(words, ++m_translateTicket);
}

void CaptureOcr::setTranslation(int ticket, Translated outcome, const QString& text)
{
    if (ticket != m_translateTicket || !m_translating)
        return; // asked again, or another picture, since
    m_translating = false;
    switch (outcome) {
    case Translated::Done:
        m_translation = text.trimmed();
        break;
    case Translated::Failed:
        m_translationNote = text.trimmed().isEmpty() ? tr("翻译失败。") : tr("翻译失败：%1").arg(text.trimmed());
        break;
    case Translated::NoService: // `text`: the translate plugin's words on what to set (a key it refused …)
        m_translationNote = text.trimmed().isEmpty() ? tr("还没有设置翻译服务，设置好后再点“翻译”。")
                                                     : tr("%1。设置好后再点“翻译”。").arg(text.trimmed());
        m_translationNeeds = u"service"_s;
        break;
    case Translated::NoPlugin:
        m_translationNote = tr("需要插件库里的“翻译”插件，装好后再点“翻译”。");
        m_translationNeeds = u"plugin"_s;
        break;
    case Translated::PluginOff: // installed, switched off (GitHub #340)
        m_translationNote = tr("翻译插件关着，打开后再点“翻译”。");
        m_translationNeeds = u"on"_s;
        break;
    }
    emit translationChanged();
}

void CaptureOcr::clearTranslation()
{
    ++m_translateTicket; // an answer still on its way is for words no longer shown
    if (!translationShown())
        return;
    m_translating = false;
    m_translation.clear();
    m_translationNote.clear();
    m_translationNeeds.clear();
    emit translationChanged();
}

void CaptureOcr::webSearch(const QString& text)
{
    const QString query = oneLine(text);
    if (!query.isEmpty())
        emit webSearchRequested(query);
}

void CaptureOcr::done(int round, const QString& text, const OcrTable& table, const QString& error)
{
    if (round != m_round)
        return; // a newer picture is being read
    m_busy = false;
    m_text = text;
    m_table = error.isEmpty() ? table.tabbed() : QString();
    m_tableHtml = m_table.isEmpty() ? QString() : table.html();
    if (!error.isEmpty())
        m_message = tr("识别失败：%1").arg(error);
    else if (text.isEmpty())
        m_message = tr("图片里没有找到文字。");
    else
        m_message.clear();
    emit changed();
    if (std::exchange(m_thenTranslate, false) && m_message.isEmpty())
        translate(m_text);
}

void CaptureOcr::work(const std::stop_token& stop)
{
    ::SetThreadPriority(::GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
    const QString folder = programFolder();
    // Its own threads besides this one, as the clipboard's: up to 4, half the processors.
    const int threads = std::clamp(static_cast<int>(std::thread::hardware_concurrency()) / 2, 1, 4);
    std::unique_ptr<PaddleOcr> reader;
    for (;;) {
        Job job;
        std::optional<ScanJob> scan;
        std::optional<LineJob> lineJob;
        {
            std::unique_lock lock(m_mutex);
            const bool woke = m_wake.wait_for(lock, stop, kKeepLoaded,
                [this] { return m_job.has_value() || m_scan.has_value() || !m_lineQueue.empty(); });
            if (stop.stop_requested())
                break;
            if (!woke) {
                reader.reset(); // a minute without a picture: its memory goes
                continue;
            }
            if (m_job) {
                job = std::move(*m_job);
                m_job.reset();
            } else if (m_scan) {
                scan = std::move(m_scan);
                m_scan.reset();
            } else {
                lineJob = std::move(m_lineQueue.front());
                m_lineQueue.pop_front();
            }
        }
        if (lineJob && lineJob->cancelled && lineJob->cancelled->load(std::memory_order_acquire)) {
            QMetaObject::invokeMethod(this, [this, ticket = lineJob->ticket] { lineScanDone(ticket, {}); }, Qt::QueuedConnection);
            continue;
        }
        QElapsedTimer clock;
        clock.start();
        QString error;
        qint64 loadMs = 0;
        if (!reader) {
            reader = PaddleOcr::load(folder, threads, &error);
            loadMs = clock.elapsed();
        }
        if (lineJob) {
            if (lineJob->cancelled && lineJob->cancelled->load(std::memory_order_acquire)) {
                QMetaObject::invokeMethod(this, [this, ticket = lineJob->ticket] { lineScanDone(ticket, {}); }, Qt::QueuedConnection);
                continue;
            }
            const double scale = ocrUpscale(lineJob->image.size(), lineJob->dpr);
            QList<RedactLine> lines;
            if (reader) {
                QImage picture = lineJob->image;
                if (scale > 1.0)
                    picture = picture.scaled(std::max(1, static_cast<int>(std::lround(picture.width() * scale))),
                        std::max(1, static_cast<int>(std::lround(picture.height() * scale))), Qt::IgnoreAspectRatio,
                        Qt::SmoothTransformation);
                QList<OcrPiece> pieces;
                if (reader->readPieces(picture, &pieces, &error)) {
                    lines.reserve(pieces.size());
                    for (const OcrPiece& p : std::as_const(pieces)) {
                        RedactLine line;
                        line.text = p.text;
                        for (const QPointF& corner : p.quad)
                            line.quad.append(corner / scale);
                        line.chars = p.chars;
                        lines.append(std::move(line));
                    }
                }
            }
            qInfo().noquote() << u"Screen capture: pin text scan read in %1 ms (loading %2 ms), %3x%4 at %5x, %6 lines"_s.arg(clock.elapsed())
                                     .arg(loadMs)
                                     .arg(lineJob->image.width())
                                     .arg(lineJob->image.height())
                                     .arg(scale, 0, 'f', 2)
                                     .arg(lines.size());
            if (!error.isEmpty())
                qWarning().noquote() << "Screen capture: pin text scan failed:" << error;
            QMetaObject::invokeMethod(this,
                [this, ticket = lineJob->ticket, lines = std::move(lines)]() mutable { lineScanDone(ticket, std::move(lines)); },
                Qt::QueuedConnection);
            continue;
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
        OcrTable table;
        if (reader && !reader->readTable(job.image, job.scale, &lines, &table, &error))
            lines.clear();
        qInfo().noquote() << u"Screen capture: text read in %1 ms (loading %2 ms), %3x%4 at %5x, %6 lines"_s.arg(clock.elapsed())
                                 .arg(loadMs)
                                 .arg(job.image.width())
                                 .arg(job.image.height())
                                 .arg(job.scale, 0, 'f', 2)
                                 .arg(lines.size())
                          + (table.isEmpty() ? QString() : u", a table of %1 rows, %2 columns, %3 merged cells"_s.arg(table.rows.size()).arg(table.rows.first().size()).arg(table.spans.size()));
        if (!error.isEmpty())
            qWarning().noquote() << "Screen capture: text recognition failed:" << error;
        QMetaObject::invokeMethod(this,
            [this, round = job.round, text = lines.join(u'\n'), table = std::move(table), error] { done(round, text, table, error); },
            Qt::QueuedConnection);
    }
}

} // namespace ws::capture
