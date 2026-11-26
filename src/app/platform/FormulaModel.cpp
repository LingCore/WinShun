#include "FormulaModel.h"

#include "Download.h"
#include "OcrFiles.h"
#include "OcrPackage.h"

#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QLockFile>
#include <QPointer>
#include <QSaveFile>
#include <QTemporaryDir>
#include <QUrl>

#include <algorithm>
#include <numeric>

using namespace Qt::StringLiterals;

namespace ws {

namespace {
// lupdate files a tr() in ws::formula under "ws::formula": the same here, or English never shows.
QString tr(const char* text) { return QCoreApplication::translate("ws::formula", text); }

bool isWeb(const QString& source)
{
    const QString scheme = QUrl(source).scheme();
    return scheme == u"https" || scheme == u"http";
}

// The model's folder gone: its own files one by one, then the folder, so
// nothing else is ever deleted with it (a link put in it by hand). False
// when it is still there.
bool removeFolder(const QString& folder, const formula::Spec& spec)
{
    QFile::remove(folder + u"/.ready"_s); // first: no reader takes the model from here on
    for (const formula::File& file : spec.files)
        QFile::remove(QDir(folder).filePath(file.name));
    QDir().rmdir(folder);
    return !QFileInfo::exists(folder);
}

// A file of a folder to `sink`, as a download would give it.
bool readLocal(const QString& file, qint64 maxBytes, const http::Sink& sink)
{
    QFile in(file);
    if (!in.open(QIODevice::ReadOnly) || in.size() > maxBytes)
        return false;
    QByteArray piece(256 * 1024, Qt::Uninitialized);
    for (;;) {
        const qint64 read = in.read(piece.data(), piece.size());
        if (read < 0)
            return false;
        if (read == 0)
            return true;
        if (!sink(piece.constData(), read))
            return false;
    }
}
} // namespace

namespace formula {

qint64 Spec::size() const
{
    return std::accumulate(files.begin(), files.end(), qint64(0), [](qint64 sum, const File& file) { return sum + file.size; });
}

// breezedeus/pix2text-mfr-1.5 (MIT) at its commit of 2025-06-30; Hugging
// Face's mirror in China first, Hugging Face itself after.
Spec spec()
{
    const QString path = u"breezedeus/pix2text-mfr-1.5/resolve/1cef9f0bdcd6a4c63df7de1311fb0894593340cc/"_s;
    return {QString(ocr::kFormulaVersion),
        {u"https://hf-mirror.com/"_s + path, u"https://huggingface.co/"_s + path},
        {{QString(ocr::kFormulaFiles[0]), "080a3f660f08bc9ebcacdd96e34be6b6400f8c7e62d7cd0dd8251badc37f610b", 87510770},
         {QString(ocr::kFormulaFiles[1]), "917deb98e91a0453c5f234f58a0f32f9fb037de8527c7eb4ed394daf9e692f2a", 32026253},
         {QString(ocr::kFormulaFiles[2]), "4ffbeb2143e6a38324bb6111b7a8109530d38a076a8439aa5777535f0a32758a", 113168}}};
}

QString install(const Spec& spec, const QStringList& sources, const Fetcher& fetch, const Progress& progress,
    const std::atomic_bool& cancelled)
{
    const QString cancelledText = tr("已取消，稍后可以重新下载。");
    if (spec.files.isEmpty() || sources.isEmpty())
        return tr("公式识别模型无效。");
    const QString root = ocr::componentsRoot();
    if (!QDir().mkpath(root))
        return tr("无法创建公式识别模型的安装目录。");
    QLockFile lock(root + u"/formula-install.lock"_s);
    if (!lock.tryLock(0))
        return tr("另一个窗口正在安装公式识别模型，完成后会自动可用。");
    QTemporaryDir staged(root + u"/formula-XXXXXX"_s);
    if (!staged.isValid())
        return tr("无法创建公式识别模型的安装目录。");
    const bool local = !isWeb(sources.front());
    qint64 done = 0;
    for (const File& file : spec.files) {
        bool fetched = false;
        bool wrong = false; // a source had it, not as it is to be
        for (qsizetype i = 0; i < sources.size() && !fetched; ++i) {
            if (cancelled.load())
                return cancelledText;
            const QString from = sources[i] + file.name;
            QFile out(staged.filePath(file.name));
            if (!out.open(QIODevice::WriteOnly | QIODevice::Truncate))
                return tr("无法保存公式识别模型，请检查磁盘空间。");
            QCryptographicHash hash(QCryptographicHash::Sha256);
            qint64 received = 0;
            bool full = false;
            QElapsedTimer timer;
            timer.start();
            const bool another = i + 1 < sources.size();
            const http::Sink sink = [&](const char* data, qsizetype size) {
                if (cancelled.load() || (another && download::tooSlow(received, timer.elapsed())))
                    return false;
                if (out.write(data, size) != size) {
                    full = true;
                    return false;
                }
                hash.addData(QByteArrayView(data, size));
                received += size;
                if (progress)
                    progress(done + received);
                return true;
            };
            bool whole = false;
            if (isWeb(from)) {
                const http::Response response = fetch(from, file.size, sink);
                whole = response.status == 200 && response.error == 0;
            } else {
                whole = readLocal(from, file.size, sink);
            }
            out.close();
            if (full)
                return tr("无法保存公式识别模型，请检查磁盘空间。");
            if (whole && (received != file.size || hash.result().toHex() != file.sha256))
                wrong = true;
            else
                fetched = whole;
        }
        if (cancelled.load())
            return cancelledText;
        if (!fetched) {
            if (local)
                return tr("所选文件夹里没有完整的公式识别模型：需要 %1，且与官方发布的文件一致。")
                    .arg(QStringList {QString(ocr::kFormulaFiles[0]), QString(ocr::kFormulaFiles[1]), QString(ocr::kFormulaFiles[2])}.join(u"、"));
            return wrong ? tr("下载的模型文件未通过校验，请稍后重试。")
                         : tr("下载没有完成，请检查网络后重试，或从本地安装模型文件。");
        }
        done += file.size;
    }
    QSaveFile receipt(staged.filePath(u".ready"_s));
    const QByteArray version = spec.version.toUtf8();
    if (!receipt.open(QIODevice::WriteOnly) || receipt.write(version) != version.size() || !receipt.commit())
        return tr("无法保存公式识别模型，请检查磁盘空间。");
    if (cancelled.load())
        return cancelledText;
    // What is left of an earlier copy (no receipt, or one being read) out of the way first.
    const QString destination = QDir(root).filePath(spec.version);
    if (QFileInfo::exists(destination) && !removeFolder(destination, spec))
        return tr("公式识别模型正在使用，请关闭公式识别窗口后重试。");
    if (!QDir().rename(staged.path(), destination))
        return tr("无法完成公式识别模型的安装。");
    staged.setAutoRemove(false);
    return {};
}

QString remove(const Spec& spec)
{
    // Whatever of it a reader still holds open goes with the next removal
    // or installation; without its receipt it is not taken meanwhile.
    const QString folder = QDir(ocr::componentsRoot()).filePath(spec.version);
    if (QFileInfo::exists(folder) && !removeFolder(folder, spec))
        return tr("公式识别模型正在使用，请关闭公式识别窗口后重试。");
    return {};
}

} // namespace formula

FormulaModel::FormulaModel(OcrPackage* runtime, formula::Fetcher fetcher, formula::Spec spec)
    : QObject(runtime)
    , m_runtime(runtime)
    , m_fetcher(std::move(fetcher))
    , m_spec(std::move(spec))
{
    QDir().mkpath(ocr::componentsRoot());
    m_watcher.addPath(ocr::componentsRoot());
    connect(&m_watcher, &QFileSystemWatcher::directoryChanged, this, &FormulaModel::refresh);
    connect(m_runtime, &OcrPackage::changed, this, &FormulaModel::runtimeChanged);
    refresh();
}

FormulaModel::~FormulaModel()
{
    if (m_cancel)
        m_cancel->store(true);
}

QString FormulaModel::downloadSize() const
{
    return tr("约 %1 MB").arg((m_spec.size() + 999999) / 1000000);
}

QJsonObject FormulaModel::state() const
{
    return {{u"installed"_s, m_installed && m_runtime->available()}, {u"busy"_s, m_busy}, {u"progress"_s, m_progress},
        {u"status"_s, m_status}, {u"size"_s, downloadSize()}};
}

void FormulaModel::refresh()
{
    // The components folder alone is watched: the model's folder comes and
    // goes whole, and a watch on it would hold it open against that.
    const bool installed = !ocr::formulaModel().isEmpty();
    if (m_installed != installed) {
        m_installed = installed;
        if (!m_busy)
            m_status.clear();
        emit changed();
    }
}

void FormulaModel::download()
{
    if (m_busy || (m_installed && m_runtime->available()))
        return;
    if (m_runtime->available()) {
        fetch(0);
        return;
    }
    // ONNX Runtime first, which the OCR package brings.
    m_awaitingRuntime = true;
    m_busy = true;
    m_progress = 0;
    m_status = tr("正在下载增强文字识别（公式识别要用它运行）…");
    emit changed();
    if (!m_runtime->busy())
        m_runtime->download();
}

void FormulaModel::runtimeChanged()
{
    if (!m_awaitingRuntime)
        return;
    if (m_runtime->available()) {
        m_awaitingRuntime = false;
        m_busy = false;
        if (m_installed) {
            m_status.clear();
            m_progress = 1;
            emit changed();
        } else {
            fetch(runtimeShare());
        }
    } else if (!m_runtime->busy()) {
        m_awaitingRuntime = false;
        m_busy = false;
        m_progress = 0;
        m_status = m_runtime->status();
        emit changed();
    } else {
        m_progress = m_runtime->progress() * runtimeShare();
        emit changed();
    }
}

void FormulaModel::cancel()
{
    if (!m_busy)
        return;
    if (m_awaitingRuntime) {
        m_runtime->cancel();
    } else if (m_cancel) {
        m_cancel->store(true);
        m_status = tr("正在取消…");
        emit changed();
    }
}

void FormulaModel::installLocal()
{
    emit localRequested();
}

void FormulaModel::installFolder(const QString& folder)
{
    if (!m_runtime->available()) {
        m_status = tr("请先安装增强文字识别，公式识别要用它运行。");
        emit changed();
        return;
    }
    start({QDir(folder).absolutePath() + u'/'}, 0);
}

double FormulaModel::runtimeShare() const
{
    const double runtime = static_cast<double>(ocr::package().downloadBytes);
    return runtime / (runtime + static_cast<double>(m_spec.size()));
}

void FormulaModel::fetch(double done)
{
    const QString given = qEnvironmentVariable("WINSHUN_FORMULA_MODEL_SOURCE"); // a folder or address, for tests
    start(given.isEmpty() ? m_spec.sources : QStringList {given}, done);
}

void FormulaModel::remove()
{
    if (m_busy || !m_installed)
        return;
    const QString error = formula::remove(m_spec);
    refresh();
    m_status = error;
    m_progress = 0;
    emit changed();
}

void FormulaModel::start(const QStringList& sources, double done)
{
    if (m_busy || m_installed)
        return;
    m_busy = true;
    m_progress = done;
    m_status = tr("正在下载公式识别模型…");
    m_cancel = std::make_shared<std::atomic_bool>(false);
    emit changed();
    m_worker = std::jthread([self = QPointer(this), sources, spec = m_spec, cancel = m_cancel, fetcher = m_fetcher, done] {
        const double size = static_cast<double>(std::max<qint64>(1, spec.size()));
        // A step of 0.5% at a time: ~200 updates for the whole model.
        const formula::Progress progress = [self, size, done, shown = -1](qint64 received) mutable {
            const int step = static_cast<int>((done + (1 - done) * static_cast<double>(received) / size) * 200);
            if (step == shown || !self)
                return;
            shown = step;
            QMetaObject::invokeMethod(self, [self, step] {
                if (self && self->m_busy) {
                    self->m_progress = std::min(0.99, step / 200.0);
                    emit self->changed();
                }
            }, Qt::QueuedConnection);
        };
        const formula::Fetcher web = fetcher ? fetcher : [](const QString& url, qint64 maxBytes, const http::Sink& sink) {
            return http::download(url, {}, 15000, maxBytes, sink);
        };
        const QString error = formula::install(spec, sources, web, progress, *cancel);
        if (self)
            QMetaObject::invokeMethod(self, [self, error] {
                if (!self)
                    return;
                self->m_busy = false;
                self->refresh();
                self->m_status = error;
                self->m_progress = error.isEmpty() ? 1 : 0;
                emit self->changed();
            }, Qt::QueuedConnection);
    });
}

} // namespace ws
