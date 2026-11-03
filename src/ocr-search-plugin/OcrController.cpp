#include "OcrController.h"

#include "platform/OcrFiles.h"
#include "platform/Shell.h"
#include "Query.h"
#include "OcrWorker.h"

#include <QClipboard>
#include <QDesktopServices>
#include <QDateTime>
#include <QElapsedTimer>
#include <QDir>
#include <QFileInfo>
#include <QGuiApplication>
#include <QJsonArray>
#include <QMutex>
#include <QMutexLocker>
#include <QQuickImageProvider>
#include <QUrl>

#include <shellapi.h>

using namespace Qt::StringLiterals;

namespace ws::ocrsearch {

class PreviewImages : public QQuickImageProvider {
public:
    PreviewImages()
        : QQuickImageProvider(QQuickImageProvider::Image)
    {
    }
    void set(QImage image)
    {
        const QMutexLocker lock(&mutex);
        current = std::move(image);
    }
    QImage requestImage(const QString&, QSize* size, const QSize& requested) override
    {
        const QMutexLocker lock(&mutex);
        if (size)
            *size = current.size();
        return requested.isValid() && (requested.width() < current.width() || requested.height() < current.height())
            ? current.scaled(requested, Qt::KeepAspectRatio, Qt::SmoothTransformation)
            : current;
    }
    QMutex mutex;
    QImage current;
};

QJsonObject hitJson(const Hit& hit)
{
    return {{u"path"_s, hit.path}, {u"location"_s, hit.location}, {u"snippet"_s, hit.snippet}, {u"asset"_s, hit.asset},
        {u"size"_s, hit.size}, {u"modified"_s, hit.modified}, {u"source"_s, hit.source}};
}
Hit hitFromJson(const QJsonObject& o)
{
    return {o.value(u"path"_s).toString(), o.value(u"location"_s).toString(), o.value(u"snippet"_s).toString(),
        o.value(u"asset"_s).toInteger(), o.value(u"size"_s).toInteger(), o.value(u"modified"_s).toInteger(),
        o.value(u"source"_s).toObject()};
}

Controller::Controller(QString appDir, QString dataDir, QObject* parent)
    : QObject(parent)
    , m_appDir(std::move(appDir))
    , m_dataDir(std::move(dataDir))
    , m_settings(QDir(m_dataDir).filePath(u"ocr-search.ini"_s), QSettings::IniFormat)
{
    m_options.folders = m_settings.value(u"folders"_s).toStringList();
    m_options.excluded = m_settings.value(u"excluded"_s).toStringList();
    m_options.capacityMB = std::clamp(m_settings.value(u"capacityMB"_s, 512).toInt(), 16, 4096);
    m_options.workerProgram = QCoreApplication::applicationFilePath();
    m_enabled = m_settings.value(u"autoUpdate"_s, false).toBool();
    m_queries.setMaxThreadCount(2);
    m_searches.setMaxThreadCount(1);
    m_previews.setMaxThreadCount(1);
    m_timer.setInterval(5 * 60 * 1000);
    connect(&m_timer, &QTimer::timeout, this, [this] {
        emit changed();
        if (m_enabled && !m_busy)
            start();
    });
    m_timer.start();
    // The first walk waits a minute after the plugin starts (with Windows),
    // unless the user searches or opens the window first: wake().
    m_firstScan.setSingleShot(true);
    m_firstScan.setInterval(60 * 1000);
    connect(&m_firstScan, &QTimer::timeout, this, [this] {
        if (m_enabled)
            start();
    });
    // Recognition parts missing: looked for again less and less often, and
    // every second again while the user is installing them.
    m_components.setSingleShot(true);
    connect(&m_components, &QTimer::timeout, this, [this] {
        if (setupNeeded()) {
            m_components.start(std::min(m_components.interval() * 2, 60 * 1000));
            return;
        }
        emit changed();
        if (m_enabled && !m_busy)
            start();
    });
    connect(this, &Controller::setupRequested, this, [this] {
        if (setupNeeded())
            m_components.start(1000);
    });
    if (setupNeeded())
        m_components.start(1000);
    m_debounce.setSingleShot(true);
    connect(&m_debounce, &QTimer::timeout, this, [this] {
        auto [params, reply] = std::exchange(m_pendingQuery, {});
        if (reply)
            runQuery(params, std::move(reply));
    });
    open();
}

Controller::~Controller()
{
    m_scan.request_stop();
    m_searchStop.request_stop();
    m_previewStop.request_stop();
    m_externalStop.request_stop();
    m_queries.clear();
    m_searches.clear();
    m_previews.clear();
    m_queries.waitForDone();
    m_searches.waitForDone();
    m_previews.waitForDone();
    if (m_scan.joinable())
        m_scan.join();
}

bool Controller::setupNeeded() const
{
    return !PaddleOcr::installed(m_appDir);
}
QQuickImageProvider* Controller::imageProvider()
{
    m_images = new PreviewImages;
    return m_images;
}

void Controller::save()
{
    m_settings.setValue(u"folders"_s, m_options.folders);
    m_settings.setValue(u"excluded"_s, m_options.excluded);
    m_settings.setValue(u"capacityMB"_s, m_options.capacityMB);
    m_settings.setValue(u"autoUpdate"_s, m_enabled);
    m_settings.sync();
    emit changed();
}
void Controller::refresh()
{
    try {
        Store store(m_dataDir, 0);
        m_statistics = store.statistics().toVariantMap();
        m_errors = store.errorText();
    } catch (const std::exception& e) {
        m_errors = QString::fromUtf8(e.what());
    }
    emit changed();
}

// Opening the writable index may migrate it or rebuild its terms: never on
// the thread that answers the host (it waits for `initialize`) or draws.
void Controller::open()
{
    m_busy = true;
    m_opening = true;
    m_status = tr("正在打开索引…");
    m_scan = std::jthread([this, capacity = m_options.capacityMB](std::stop_token) {
        QString error;
        try {
            Store store(m_dataDir, capacity);
        } catch (const std::exception& e) {
            error = QString::fromUtf8(e.what());
        }
        QMetaObject::invokeMethod(
            this,
            [this, error] {
                m_opening = false;
                m_ready = error.isEmpty();
                finished(!error.isEmpty()           ? error
                        : m_options.folders.isEmpty() ? tr("添加要搜索的文件夹")
                        : m_enabled                   ? tr("稍后自动更新索引")
                                                      : tr("已暂停"));
                if (m_ready && m_enabled && !m_busy) {
                    if (m_woken)
                        start();
                    else
                        m_firstScan.start();
                }
            },
            Qt::QueuedConnection);
    });
}

// Index upkeep the user asked for (removing a folder, clearing): in one
// transaction each, on the background thread; the window stays responsive.
void Controller::maintain(std::function<void(Store&)> work, const QString& done)
{
    m_busy = true;
    m_status = tr("正在整理索引…");
    emit changed();
    if (m_scan.joinable())
        m_scan.join();
    m_scan = std::jthread([this, work = std::move(work), done, capacity = m_options.capacityMB](std::stop_token) {
        QString status = done;
        try {
            Store store(m_dataDir, capacity);
            work(store);
            store.compact();
        } catch (const std::exception& e) {
            status = QString::fromUtf8(e.what());
        }
        QMetaObject::invokeMethod(this, [this, status] { finished(status); }, Qt::QueuedConnection);
    });
}

// A background job is over: its outcome shows, then what was asked meanwhile.
void Controller::finished(const QString& status)
{
    m_busy = false;
    m_status = status;
    refresh();
    search(m_words);
    emit refreshRequested();
    if (m_pendingStart) {
        const bool retry = *std::exchange(m_pendingStart, std::nullopt);
        start(retry);
    }
}

// The user is here (searching, or the window opened): the first walk need
// not wait any longer.
void Controller::wake()
{
    m_woken = true;
    if (m_firstScan.isActive()) {
        m_firstScan.stop();
        start();
    }
}

void Controller::setCapacityMB(int value)
{
    if (m_busy)
        return;
    m_options.capacityMB = std::clamp(value, 16, 4096);
    save();
}
void Controller::addPath(const QString& path, bool exclusion)
{
    if (m_busy || !QDir(path).exists())
        return;
    auto& paths = exclusion ? m_options.excluded : m_options.folders;
    if (std::any_of(paths.begin(), paths.end(), [&](const QString& p) { return inside(path, p); }))
        return;
    paths.removeIf([&](const QString& p) { return inside(p, path); });
    paths.append(QDir::toNativeSeparators(QFileInfo(path).absoluteFilePath()));
    m_status = tr("目录已添加，点击开始更新");
    save();
    if (exclusion)
        maintain([path](Store& store) { store.prune(path, {}); }, m_status);
}
void Controller::addFolder(bool exclusion)
{
    if (m_busy)
        return;
    const QString path = shell::pickFolder(nullptr, exclusion ? tr("选择不识别的文件夹") : tr("选择要搜索的文件夹"));
    if (!path.isEmpty())
        addPath(path, exclusion);
}
void Controller::removeFolder(int index, bool exclusion)
{
    if (m_busy)
        return;
    auto& paths = exclusion ? m_options.excluded : m_options.folders;
    if (index < 0 || index >= paths.size())
        return;
    paths.removeAt(index);
    save();
    maintain([folders = m_options.folders](Store& store) { store.retainFolders(folders); }, m_status);
}
void Controller::start(bool retry)
{
    if (m_options.folders.isEmpty())
        return;
    if (m_busy) {
        // Asked while the index opens or is tidied: it runs right after.
        if (!m_indexing)
            m_pendingStart = retry || m_pendingStart.value_or(false);
        return;
    }
    if (setupNeeded()) {
        m_status = tr("请先安装增强识别组件");
        emit changed();
        return;
    }
    m_firstScan.stop();
    m_busy = true;
    m_indexing = true;
    m_enabled = true;
    m_status = tr("正在更新索引");
    m_progress.clear();
    save();
    if (m_scan.joinable())
        m_scan.join();
    ScanOptions options = m_options;
    options.retry = retry;
    m_scan = std::jthread([this, options](std::stop_token token) {
        QString message;
        QElapsedTimer progressClock;
        progressClock.start();
        try {
            scan(
                m_appDir, m_dataDir, options, [token] { return token.stop_requested(); },
                [this, token, &progressClock](const ScanProgress& state) {
                    if (token.stop_requested())
                        return;
                    if (state.checked != 1 && state.message.isEmpty() && progressClock.elapsed() < 100)
                        return;
                    progressClock.restart();
                    QMetaObject::invokeMethod(
                        this,
                        [this, state] {
                            m_progress
                                = tr("已检查 %1 个文件 · 新识别 %2 页／张").arg(state.checked).arg(state.recognized)
                                + u"\n"_s + state.current;
                            if (!state.message.isEmpty())
                                m_status = state.message;
                            emit changed();
                        },
                        Qt::QueuedConnection);
                });
        } catch (const std::exception& e) {
            message = QString::fromUtf8(e.what());
        }
        QMetaObject::invokeMethod(
            this,
            [this, message, token] {
                m_indexing = false;
                finished(token.stop_requested() ? tr("已暂停，下次从未完成的文件继续")
                        : message.isEmpty()     ? tr("索引已更新，每 5 分钟检查文件变化")
                                                : message);
                const int unfinished = m_statistics.value(u"failed"_s).toInt()
                    + m_statistics.value(u"partial"_s).toInt() + m_statistics.value(u"unsupported"_s).toInt()
                    + m_statistics.value(u"offline"_s).toInt();
                if (!token.stop_requested() && message.isEmpty() && unfinished) {
                    m_status = tr("索引已更新，%1 个文件未完成，可查看原因").arg(unfinished);
                    emit changed();
                }
            },
            Qt::QueuedConnection);
    });
}
void Controller::pause()
{
    m_enabled = false;
    m_firstScan.stop();
    m_pendingStart.reset();
    m_scan.request_stop();
    m_status = m_indexing ? tr("正在暂停，保留已完成的结果…") : tr("已暂停");
    save();
}
void Controller::setAutoUpdate(bool enabled)
{
    if (!enabled)
        pause();
    else
        start();
}
void Controller::clearIndex()
{
    if (m_busy)
        return;
    m_enabled = false;
    m_firstScan.stop();
    save();
    maintain([](Store& store) { store.retainFolders({}); }, tr("索引已清空，原文件保持原样"));
}

void Controller::search(const QString& words)
{
    m_words = words;
    m_searchStop.request_stop();
    m_searchStop = {};
    m_searches.clear();
    const auto token = m_searchStop.get_token();
    m_searching = !words.trimmed().isEmpty();
    if (m_searching)
        wake();
    if (!m_searching) {
        m_previewStop.request_stop();
        m_previews.clear();
        ++m_previewRevision;
        m_preview.clear();
        m_hits.clear();
        m_results.clear();
        emit resultsChanged();
        emit previewChanged();
        return;
    }
    emit resultsChanged();
    m_searches.start([this, words, token] {
        QList<Hit> hits;
        QString error;
        try {
            Store store(m_dataDir, 0);
            hits = store.search(words, 200, {}, [token] { return token.stop_requested(); });
        } catch (const std::exception& e) {
            error = QString::fromUtf8(e.what());
        }
        QMetaObject::invokeMethod(
            this,
            [this, hits = std::move(hits), words, error, token] {
                if (token.stop_requested())
                    return;
                m_searching = false;
                m_hits = hits;
                m_results.clear();
                for (const Hit& hit : hits) {
                    QVariantMap item = hitJson(hit).toVariantMap();
                    item.insert(u"name"_s, QFileInfo(hit.path).fileName());
                    m_results.append(item);
                }
                if (!error.isEmpty()) {
                    m_status = error;
                    emit changed();
                }
                emit resultsChanged();
                if (!hits.isEmpty())
                    showHit(hits.first(), words);
                else {
                    m_previewStop.request_stop();
                    m_previews.clear();
                    ++m_previewRevision;
                    m_preview.clear();
                    emit previewChanged();
                }
            },
            Qt::QueuedConnection);
    });
}
void Controller::select(int index)
{
    if (index >= 0 && index < m_hits.size())
        showHit(m_hits[index], m_words);
}

void Controller::showHit(const Hit& hit, const QString& words)
{
    m_previewStop.request_stop();
    m_previewStop = {};
    m_previews.clear();
    const auto token = m_previewStop.get_token();
    const quint64 revision = ++m_previewRevision;
    m_preview = {{u"path"_s, hit.path}, {u"location"_s, hit.location}, {u"loading"_s, true}};
    emit previewChanged();
    m_previews.start([this, hit, words, revision, token] {
        QImage image;
        QVariantList boxes;
        QString text, error;
        try {
            const QFileInfo info(hit.path);
            if (!info.exists() || info.size() != hit.size
                || info.lastModified().toMSecsSinceEpoch()
                    != hit.source.value(u"fileModifiedMs"_s).toInteger(hit.modified * 1000))
                throw std::runtime_error(tr("文件已变化，请更新索引后再预览").toUtf8().constData());
            Worker worker(m_options.workerProgram, m_appDir, m_dataDir, {}, 30000);
            image = worker.preview(hit.path, hit.source, [token] { return token.stop_requested(); });
            Store store(m_dataDir, 0);
            QSize recognised;
            const auto pieces = store.pieces(hit.asset, &recognised);
            // Boxes are in the recognised image's pixels; the preview may be smaller.
            if (recognised.isEmpty())
                recognised = image.size();
            const QStringList terms = searchTerms(splitContentQuery(words).words);
            QStringList lines;
            for (const auto& piece : pieces) {
                lines.append(piece.text);
                const QString normalizedText = normalized(piece.text);
                if (std::any_of(terms.begin(), terms.end(),
                        [&](const QString& term) { return containsTerm(normalizedText, term); })) {
                    const QRectF rect = piece.quad.boundingRect();
                    boxes.append(QVariantMap {{u"x"_s, rect.x() / recognised.width()},
                        {u"y"_s, rect.y() / recognised.height()}, {u"width"_s, rect.width() / recognised.width()},
                        {u"height"_s, rect.height() / recognised.height()}});
                }
            }
            text = lines.join(u'\n');
        } catch (const std::exception& e) {
            error = QString::fromUtf8(e.what());
        }
        QMetaObject::invokeMethod(
            this,
            [this, image = std::move(image), boxes, text, error, revision, token] {
                if (token.stop_requested() || revision != m_previewRevision)
                    return;
                if (m_images)
                    m_images->set(image);
                m_preview.insert(
                    u"url"_s, image.isNull() ? QString() : u"image://ocrpreview/current?%1"_s.arg(revision));
                m_preview.insert(u"width"_s, image.width());
                m_preview.insert(u"height"_s, image.height());
                m_preview.insert(u"boxes"_s, boxes);
                m_preview.insert(u"text"_s, text);
                m_preview.insert(u"error"_s, error);
                m_preview.insert(u"loading"_s, false);
                emit previewChanged();
            },
            Qt::QueuedConnection);
    });
}
void Controller::releasePreview()
{
    m_previewStop.request_stop();
    m_previews.clear();
    ++m_previewRevision;
    if (m_images)
        m_images->set({});
    m_preview.clear();
    emit previewChanged();
}
void Controller::openFile(const QString& path)
{
    if (QFileInfo::exists(path))
        QDesktopServices::openUrl(QUrl::fromLocalFile(path));
}
void Controller::showFolder(const QString& path)
{
    const QString args = u"/select,\"%1\""_s.arg(QDir::toNativeSeparators(path));
    ::ShellExecuteW(nullptr, L"open", L"explorer.exe", reinterpret_cast<LPCWSTR>(args.utf16()), nullptr, SW_SHOWNORMAL);
}
void Controller::copyText()
{
    QGuiApplication::clipboard()->setText(m_preview.value(u"text"_s).toString());
}

void Controller::cancelQuery()
{
    m_debounce.stop();
    m_pendingQuery = {};
    m_externalStop.request_stop();
    m_queries.clear();
}
void Controller::externalQuery(const QJsonObject& params, std::function<void(const QJsonObject&)> reply)
{
    cancelQuery();
    const QString words = params.value(u"text"_s).toString().trimmed();
    const bool content = params.value(u"trigger"_s).toString() == u"content";
    if (words.isEmpty()) {
        reply({{u"items"_s,
            QJsonArray {QJsonObject {{u"id"_s, u"manage"_s}, {u"title"_s, tr("管理图片文字搜索")},
                {u"subtitle"_s, m_status}, {u"icon"_s, u"glyph:U+E721"_s}}}}});
        return;
    }
    wake();
    if (m_opening) {
        // Not ready yet: asked again shortly.
        reply({{u"items"_s,
                  content ? QJsonArray {}
                          : QJsonArray {QJsonObject {{u"id"_s, u"manage"_s}, {u"title"_s, m_status},
                                {u"icon"_s, u"glyph:U+E721"_s}}}},
            {u"rerun"_s, 2}});
        return;
    }
    // Content queries come with every key typed in the launcher: only the
    // last of a quick burst is looked up.
    if (content) {
        m_pendingQuery = {params, std::move(reply)};
        m_debounce.start(150);
        return;
    }
    runQuery(params, std::move(reply));
}
void Controller::runQuery(const QJsonObject& params, std::function<void(const QJsonObject&)> reply)
{
    m_externalStop = {};
    const auto token = m_externalStop.get_token();
    const QString words = params.value(u"text"_s).toString().trimmed();
    const bool content = params.value(u"trigger"_s).toString() == u"content";
    const bool configured = !m_options.folders.isEmpty();
    m_queries.start([this, params, words, content, configured, reply = std::move(reply), token] {
        QJsonArray items;
        QString error;
        try {
            Store store(m_dataDir, 0);
            const auto hits = store.search(words, std::clamp(params.value(u"limit"_s).toInt(100), 1, 200),
                params.value(u"filters"_s).toObject(), [token] { return token.stop_requested(); });
            for (const Hit& hit : hits) {
                const QJsonObject data {{u"hit"_s, hitJson(hit)}, {u"words"_s, words}};
                items.append(QJsonObject {{u"id"_s, pathKey(hit.path)}, {u"title"_s, QFileInfo(hit.path).fileName()},
                    {u"subtitle"_s, u"OCR · "_s + hit.location + u" · "_s + hit.snippet},
                    {u"icon"_s, u"file:"_s + hit.path}, {u"open"_s, QJsonObject {{u"path"_s, hit.path}}},
                    {u"copy"_s, hit.snippet}, {u"preview"_s, QJsonObject {{u"text"_s, hit.snippet}}},
                    {u"size"_s, hit.size}, {u"modified"_s, hit.modified}, {u"data"_s, data}});
            }
            if (items.isEmpty() && !content)
                items.append(QJsonObject {{u"id"_s, u"manage"_s}, {u"title"_s, tr("没有找到，查看索引状态")},
                    {u"subtitle"_s,
                        !configured ? tr("先添加要搜索的文件夹") : tr("可检查目录范围、识别进度和失败项")}});
        } catch (const std::exception& e) {
            error = QString::fromUtf8(e.what());
        }
        QMetaObject::invokeMethod(
            this,
            [this, reply, items, error, content, token] {
                if (token.stop_requested())
                    return;
                QJsonArray answer = items;
                if (!error.isEmpty() && !content)
                    answer.append(QJsonObject {{u"id"_s, u"manage"_s}, {u"title"_s, error}});
                QJsonObject result {{u"items"_s, answer}};
                // Only while the index changes: an idle list is not asked again.
                if (m_busy)
                    result.insert(u"rerun"_s, 5);
                reply(result);
            },
            Qt::QueuedConnection);
    });
}

} // namespace ws::ocrsearch
