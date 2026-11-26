#include "CaptureFormula.h"

#include "FormulaPicture.h"
#include "Latex.h"
#include "platform/FormulaReader.h"
#include "platform/OcrFiles.h"
#include "platform/Shell.h"

#include <QDateTime>
#include <QDebug>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QPointer>
#include <QQuickWindow>
#include <QSaveFile>

#include <windows.h>

#include <algorithm>
#include <atomic>
#include <chrono>

using namespace Qt::StringLiterals;
using namespace std::chrono_literals;

namespace ws::capture {

namespace {

// How long the reader stays loaded after the last picture.
constexpr auto kKeepLoaded = 60s;
// The formula as a picture: drawn this large, then this many times over
// (a 2560-pixel-wide picture for a long formula, sharp in a document).
constexpr qreal kPictureSize = 30;
constexpr qreal kPictureScale = 3;

const QStringList kFormats {u"latex"_s, u"markdown"_s, u"word"_s, u"typst"_s};

} // namespace

CaptureFormula::CaptureFormula(const QString& programFolder, QObject* parent)
    : QObject(parent)
    , m_programFolder(programFolder)
    , m_thread([this](std::stop_token stop) { work(stop); })
{
    m_poll.setInterval(400);
    connect(&m_poll, &QTimer::timeout, this, [this] { askModel(u"status"_s); });
}

CaptureFormula::~CaptureFormula()
{
    m_thread.request_stop();
    m_newer.store(true); // a reading under way stops at its next token
    m_wake.notify_all();
    if (m_thread.joinable())
        m_thread.join();
}

bool CaptureFormula::modelReady() const
{
    return !ocr::formulaModel().isEmpty() && !ocr::folder(m_programFolder).isEmpty();
}

void CaptureFormula::read(const QImage& image)
{
    ++m_round; // a reading still under way is for a picture no longer shown
    m_picture = image;
    m_pending = {};
    m_latex.clear();
    m_unsure = false;
    m_message.clear();
    if (image.isNull()) {
        m_busy = false;
        m_message = tr("没有图片可以识别。");
        emit changed();
        return;
    }
    if (!modelReady()) {
        m_busy = false;
        m_pending = image;
        if (m_model == u"ready") {
            m_model = u"missing"_s;
            emit modelChanged();
        }
        askModel(u"status"_s); // downloading already, how large, or a Win顺 without the tool
        emit changed();
        return;
    }
    start(image);
}

void CaptureFormula::start(const QImage& image)
{
    if (m_model != u"ready") {
        m_model = u"ready"_s;
        m_poll.stop();
        emit modelChanged();
    }
    m_busy = true;
    emit changed();
    {
        // Both under the lock the reader takes a job with (and clears the
        // flag): set after, it could stop this very job just taken.
        std::lock_guard lock(m_mutex);
        m_newer.store(true); // one under way stops
        m_job = Job {m_round, image}; // an older one not started yet is dropped
    }
    m_wake.notify_all();
}

void CaptureFormula::refresh()
{
    if (!m_pending.isNull() && modelReady())
        start(std::exchange(m_pending, {}));
}

void CaptureFormula::discard()
{
    ++m_round; // what is still being read is for nobody
    m_pending = {};
    m_poll.stop();
    {
        std::lock_guard lock(m_mutex);
        m_job.reset();
        m_newer.store(true); // the next job taken clears it
    }
    if (m_busy) {
        m_busy = false;
        emit changed();
    }
}

void CaptureFormula::work(const std::stop_token& stop)
{
    ::SetThreadPriority(::GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
    // Its own threads besides this one, as the words' reader: up to 4, half the processors.
    const int threads = std::clamp(static_cast<int>(std::thread::hardware_concurrency()) / 2, 1, 4);
    std::unique_ptr<FormulaReader> reader;
    for (;;) {
        Job job;
        {
            std::unique_lock lock(m_mutex);
            const bool woke = m_wake.wait_for(lock, stop, kKeepLoaded, [this] { return m_job.has_value(); });
            if (stop.stop_requested())
                break;
            if (!woke) {
                reader.reset(); // a minute without a picture: its memory goes
                continue;
            }
            job = std::move(*m_job);
            m_job.reset();
            m_newer.store(false);
        }
        QElapsedTimer clock;
        clock.start();
        QString error;
        qint64 loadMs = 0;
        if (!reader) {
            reader = FormulaReader::load(ocr::folder(m_programFolder), ocr::formulaModel(), threads, &error);
            loadMs = clock.elapsed();
            if (!reader) {
                qWarning().noquote() << "Screen capture: the formula model did not load:" << error;
                QMetaObject::invokeMethod(this, [this, round = job.round] {
                    done(round, {}, false, tr("公式识别模型无法加载，请在 Win顺 设置 › 剪贴板 › 公式识别模型中移除后重新下载。"));
                }, Qt::QueuedConnection);
                continue;
            }
        }
        const std::optional<FormulaReader::Reading> reading = reader->read(job.image, &error, &m_newer);
        if (!reading && error == u"cancelled")
            continue; // a newer picture's turn
        if (!reading)
            qWarning().noquote() << "Screen capture: the formula was not read:" << error;
        else
            qInfo().noquote() << u"Screen capture: formula read in %1 ms (loading %2 ms), %3x%4, score %5%6"_s.arg(clock.elapsed())
                                     .arg(loadMs).arg(job.image.width()).arg(job.image.height())
                                     .arg(reading->score, 0, 'f', 3).arg(reading->cut ? u", cut short"_s : QString());
        const QString latex = reading ? latex::tidyModel(reading->latex) : QString();
        const bool unsure = reading && (reading->cut || reading->score < FormulaReader::kSure);
        const QString failure = reading ? QString() : tr("识别没有完成，请再试一次。");
        QMetaObject::invokeMethod(this, [this, round = job.round, latex, unsure, failure] { done(round, latex, unsure, failure); },
            Qt::QueuedConnection);
    }
}

void CaptureFormula::done(int round, const QString& latex, bool unsure, const QString& error)
{
    if (round != m_round)
        return;
    m_busy = false;
    m_latex = latex;
    m_unsure = unsure;
    m_message = !error.isEmpty() ? error : latex.isEmpty() ? tr("图中没有找到公式。") : QString();
    emit changed();
}

// ---- the model, fetched by Win顺 ------------------------------------------------------

void CaptureFormula::downloadModel()
{
    m_modelStatus.clear();
    askModel(u"install"_s);
}

void CaptureFormula::cancelDownload()
{
    askModel(u"cancel"_s);
}

void CaptureFormula::askModel(const QString& action)
{
    if (!m_modelCall) {
        setModelState({}, u"Unknown tool"_s);
        return;
    }
    QPointer self(this);
    m_modelCall(action, [self](const QJsonObject& state, const QString& error) {
        if (self)
            self->setModelState(state, error);
    });
}

void CaptureFormula::setModelState(const QJsonObject& state, const QString& error)
{
    QString model = u"missing"_s;
    if (error.startsWith(u"Unknown tool")) {
        model = u"update"_s; // a Win顺 before 公式识别
    } else if (!error.isEmpty()) {
        m_modelStatus = error;
    } else {
        m_modelSize = state.value(u"size"_s).toString();
        m_progress = state.value(u"progress"_s).toDouble();
        if (state.value(u"busy"_s).toBool())
            model = u"downloading"_s;
        else if (state.value(u"installed"_s).toBool() && modelReady())
            model = u"ready"_s;
        else
            m_modelStatus = state.value(u"status"_s).toString(); // why the last one failed, else empty
    }
    if (model == u"downloading" && !m_poll.isActive())
        m_poll.start();
    else if (model != u"downloading")
        m_poll.stop();
    m_model = model;
    emit modelChanged();
    if (model == u"ready")
        refresh();
}

// ---- what is done with it ------------------------------------------------------------

void CaptureFormula::setFormat(const QString& format)
{
    if (format == m_format || !kFormats.contains(format))
        return;
    m_format = format;
    emit formatChanged();
}

QString CaptureFormula::converted(const QString& latex, const QString& format)
{
    const QString text = latex.trimmed();
    if (text.isEmpty())
        return {};
    if (format == u"markdown")
        return u"$$\n"_s + text + u"\n$$"_s;
    if (format == u"word")
        return latex::toMathMl(latex::tidy(latex::parse(text)));
    if (format == u"typst")
        return u"$ "_s + latex::toTypst(latex::tidy(latex::parse(text))) + u" $"_s;
    return text;
}

void CaptureFormula::copy(const QString& latex)
{
    const QString text = converted(latex, m_format);
    if (text.isEmpty())
        return;
    if (m_format == u"word")
        emit copyMathMlRequested(text);
    else
        emit copyTextRequested(text);
    emit noticed(tr("已复制"), false);
}

bool CaptureFormula::exportPicture(const QString& latex, const QString& how, QQuickWindow* owner)
{
    // Black on white, as a document has it; the SVG on nothing, to be placed on anything.
    const FormulaPicture formula(latex.trimmed(), kPictureSize, Qt::black);
    if (!formula.isValid()) {
        emit noticed(tr("公式有误，无法生成图片"), true);
        return false;
    }
    if (how == u"copy") {
        emit copyImageRequested(formula.image(kPictureScale, kPictureSize / 2, Qt::white));
        emit noticed(tr("已复制图片"), false);
        return true;
    }
    const bool svg = how == u"svg";
    const QString name = u"公式_%1.%2"_s.arg(QDateTime::currentDateTime().toString(u"yyyyMMdd_HHmmss"_s), svg ? u"svg"_s : u"png"_s);
    const QString path = shell::pickSaveFile(owner ? reinterpret_cast<HWND>(owner->winId()) : nullptr, tr("保存公式"), name,
        svg ? tr("SVG 矢量图") : tr("PNG 图片"), svg ? u"svg"_s : u"png"_s);
    if (path.isEmpty())
        return true; // not wanted after all
    bool saved = false;
    if (svg) {
        QSaveFile file(path);
        const QByteArray bytes = formula.svg(kPictureSize / 2);
        saved = file.open(QIODevice::WriteOnly) && file.write(bytes) == bytes.size() && file.commit();
    } else {
        saved = formula.image(kPictureScale, kPictureSize / 2, Qt::white).save(path, "PNG");
    }
    if (!saved) {
        qWarning().noquote() << "Screen capture: the formula picture was not saved to" << path;
        emit noticed(tr("无法保存到 %1").arg(QDir::toNativeSeparators(path)), true);
        return false;
    }
    emit noticed(tr("已保存"), false);
    return true;
}

} // namespace ws::capture
