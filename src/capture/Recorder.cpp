#include "Recorder.h"

#include "Geometry.h"
#include "GifExport.h"
#include "KeyStream.h"
#include "RecordingPointer.h"
#include "platform/Shell.h"
#include "platform/WindowEffects.h"

#include <QDateTime>
#include <QDir>
#include <QFileInfo>
#include <QFile>
#include <QGuiApplication>
#include <QLocale>
#include <QQmlComponent>
#include <QQmlEngine>
#include <QQuickWindow>
#include <QScreen>
#include <QStandardPaths>

#include <algorithm>
#include <array>
#include <utility>

using namespace Qt::StringLiterals;

namespace ws::capture {

class Recorder::Border {
public:
    Border(const QRect& region, double dpr)
    {
        static const bool registered = [] {
            WNDCLASSW wc {};
            wc.hInstance = ::GetModuleHandleW(nullptr);
            wc.lpszClassName = L"WinShunRecordingEdge";
            wc.lpfnWndProc = [](HWND window, UINT message, WPARAM wp, LPARAM lp) -> LRESULT {
                if (message == WM_PAINT) {
                    PAINTSTRUCT paint {};
                    const HDC dc = ::BeginPaint(window, &paint);
                    ::SetDCBrushColor(dc, RGB(220, 55, 55));
                    ::FillRect(dc, &paint.rcPaint, static_cast<HBRUSH>(::GetStockObject(DC_BRUSH)));
                    ::EndPaint(window, &paint);
                    return 0;
                }
                return ::DefWindowProcW(window, message, wp, lp);
            };
            return ::RegisterClassW(&wc) != 0;
        }();
        if (!registered) return;
        const int edge = std::max(1, qRound(dpr));
        const QRect outer = region.adjusted(-edge, -edge, edge, edge);
        const std::array<QRect, 4> strips {{
            QRect(outer.left(), outer.top(), outer.width(), edge),
            QRect(outer.left(), region.bottom() + 1, outer.width(), edge),
            QRect(outer.left(), region.top(), edge, region.height()),
            QRect(region.right() + 1, region.top(), edge, region.height())}};
        for (size_t i = 0; i < strips.size(); ++i) {
            const QRect& r = strips[i];
            m_windows[i] = ::CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_TOPMOST | WS_EX_NOACTIVATE | WS_EX_TRANSPARENT,
                L"WinShunRecordingEdge", L"", WS_POPUP, r.x(), r.y(), r.width(), r.height(),
                nullptr, nullptr, ::GetModuleHandleW(nullptr), nullptr);
            if (m_windows[i]) {
                ::SetWindowDisplayAffinity(m_windows[i], WDA_EXCLUDEFROMCAPTURE);
                ::ShowWindow(m_windows[i], SW_SHOWNOACTIVATE);
            }
        }
    }
    ~Border() { for (HWND window : m_windows) if (window) ::DestroyWindow(window); }
private:
    std::array<HWND, 4> m_windows {};
};

Recorder::Recorder(QQmlEngine* engine, Hooks hooks, QObject* parent)
    : QObject(parent), m_engine(engine), m_hooks(std::move(hooks))
{
    m_timer.setInterval(1000);
    connect(&m_timer, &QTimer::timeout, this, [this] {
        if (--m_countdown <= 0) {
            m_timer.stop();
            begin();
        } else {
            emit changed();
        }
    });
}

Recorder::~Recorder()
{
    m_timer.stop();
    if (m_control) m_control->stop = true; // keep the recording when the feature is switched off
    if (m_worker.joinable()) m_worker.join();
    releaseClicks();
    releaseKeys();
    m_border.reset();
    delete m_panel.data();
}

bool Recorder::active() const
{
    return m_control || m_stage == Counting || (m_stage == Ready && m_panel && m_panel->isVisible());
}

QString Recorder::status() const
{
    switch (m_stage) {
    case Ready: return tr("准备录屏");
    case Counting: return tr("%1 秒后开始").arg(m_countdown);
    case Starting: return tr("正在准备录制…");
    case Recording: return tr("正在录制");
    case Paused: return tr("已暂停");
    case Saving:
        if (m_exporting)
            return m_progress.percent < 0 ? tr("正在生成 GIF…") : tr("正在生成 GIF… %1%").arg(m_progress.percent);
        return tr("正在完成保存…");
    case Done: return tr("录屏已保存");
    case Failed: return tr("录屏未完成");
    }
    return {};
}

QString Recorder::timeText() const
{
    const qint64 seconds = m_progress.elapsedMs / 1000;
    return u"%1:%2"_s.arg(seconds / 60, 2, 10, QChar(u'0')).arg(seconds % 60, 2, 10, QChar(u'0'));
}

QString Recorder::sizeText() const
{
    if (m_control && m_progress.bytes < 1024) return tr("编码中");
    return QLocale().formattedDataSize(m_progress.bytes);
}

QString Recorder::folder() const
{
    return m_preferences.folder.isEmpty()
        ? QDir::toNativeSeparators(QDir(QStandardPaths::writableLocation(QStandardPaths::MoviesLocation)).filePath(u"WinShun"_s))
        : m_preferences.folder;
}

QString Recorder::fileName() const { return QFileInfo(m_path).fileName(); }
QString Recorder::dimensions() const
{
    if (format() == Gif) {
        const QSize size = recording::gifSize(m_region.size(), quality() == SmallFile ? 960 : quality() == HighQuality ? 1920 : 1280);
        return tr("%1 × %2 · %3 帧/秒 · GIF").arg(size.width()).arg(size.height()).arg(quality() == SmallFile ? 10 : quality() == HighQuality ? 20 : 15);
    }
    return tr("%1 × %2 · %3 帧/秒 · MP4").arg(m_region.width()).arg(m_region.height()).arg(quality() == SmallFile ? 15 : 30);
}

void Recorder::remember()
{
    if (m_hooks.remember) m_hooks.remember(m_preferences);
    emit changed();
}

void Recorder::setSystemAudio(bool on)
{
    if (m_stage != Ready || m_preferences.systemAudio == on) return;
    m_preferences.systemAudio = on;
    remember();
}

void Recorder::setMicrophone(bool on)
{
    if (m_stage != Ready || m_preferences.microphone == on) return;
    m_preferences.microphone = on;
    remember();
}

void Recorder::setQuality(Quality value)
{
    if (m_stage != Ready || value < SmallFile || value > HighQuality || m_preferences.quality == value) return;
    m_preferences.quality = value;
    remember();
}

void Recorder::setFormat(Format value)
{
    if (m_stage != Ready || value < Mp4 || value > Gif || m_preferences.format == value) return;
    m_preferences.format = value;
    remember();
}

void Recorder::setCursor(bool on)
{
    if (m_stage != Ready || m_preferences.cursor == on) return;
    m_preferences.cursor = on;
    remember();
}

void Recorder::setCountdown(bool on)
{
    if (m_stage != Ready || m_preferences.countdown == on) return;
    m_preferences.countdown = on;
    remember();
}

void Recorder::setClickHighlights(bool on)
{
    if (m_stage != Ready || m_preferences.clickHighlights == on) return;
    m_preferences.clickHighlights = on;
    remember();
}

void Recorder::setKeys(KeyDisplay value)
{
    if (m_stage != Ready || value < KeysOff || value > KeysAll || m_preferences.keys == value) return;
    m_preferences.keys = value;
    remember();
}

void Recorder::setMouseButtons(bool on)
{
    if (m_stage != Ready || m_preferences.mouseButtons == on) return;
    m_preferences.mouseButtons = on;
    remember();
}

// The hooks out at once: nothing pressed after the stop is looked at.
void Recorder::releaseKeys()
{
    m_keyHook.reset();
}

void Recorder::releaseClicks()
{
    if (std::exchange(m_clicksActive, false) && m_hooks.clicks) m_hooks.clicks({});
}

void Recorder::createPanel()
{
    if (m_panel) return;
    QQmlComponent component(m_engine, u"WinShun"_s, u"RecordingPanel"_s);
    QObject* object = component.createWithInitialProperties({{u"recorder"_s, QVariant::fromValue(this)}});
    m_panel = qobject_cast<QQuickWindow*>(object);
    if (!m_panel) {
        qWarning().noquote() << "Recording panel:" << component.errorString();
        delete object;
        return;
    }
    m_panel->create();
    win::styleFramelessWindow(m_panel, false);
    ::SetWindowDisplayAffinity(reinterpret_cast<HWND>(m_panel->winId()), WDA_EXCLUDEFROMCAPTURE);
}

void Recorder::prepare(const QRect& region, HMONITOR monitor, double dpr)
{
    if (active()) return;
    if (m_worker.joinable()) m_worker.join();
    m_region = region;
    m_monitor = monitor;
    m_dpr = dpr;
    m_preferences = m_hooks.preferences ? m_hooks.preferences() : Preferences {};
    if (m_preferences.quality < SmallFile || m_preferences.quality > HighQuality)
        m_preferences.quality = Standard;
    if (m_preferences.format < Mp4 || m_preferences.format > Gif)
        m_preferences.format = Mp4;
    if (m_preferences.keys < KeysOff || m_preferences.keys > KeysAll)
        m_preferences.keys = KeysOff;
    m_exporting = false;
    m_progress = {};
    m_path.clear();
    m_error.clear();
    m_stage = Ready;
    createPanel();
    if (!m_panel) return;
    QScreen* target = nullptr;
    for (QScreen* screen : QGuiApplication::screens()) {
        const auto native = screen->nativeInterface<QNativeInterface::QWindowsScreen>();
        if (native && native->handle() == monitor) { target = screen; break; }
    }
    if (target) m_panel->setScreen(target);
    MONITORINFO info {sizeof info};
    if (::GetMonitorInfoW(monitor, &info)) {
        const QRect work(info.rcWork.left, info.rcWork.top, info.rcWork.right - info.rcWork.left, info.rcWork.bottom - info.rcWork.top);
        const QSize extent(qRound(m_panel->width() * dpr), qRound(m_panel->height() * dpr));
        const QPoint at = placeBeside(region, extent, work, qRound(12 * dpr));
        ::SetWindowPos(reinterpret_cast<HWND>(m_panel->winId()), HWND_TOPMOST, at.x(), at.y(), extent.width(), extent.height(), SWP_NOACTIVATE);
    }
    m_border = std::make_unique<Border>(region, dpr);
    emit changed();
    m_panel->show();
    m_panel->requestActivate();
}

void Recorder::start()
{
    if (m_stage != Ready) return;
    if (!QDir::isAbsolutePath(folder())) {
        m_error = tr("请选择完整的录屏保存文件夹路径。");
        emit changed();
        return;
    }
    if (!QDir().mkpath(folder())) {
        m_error = tr("无法写入 %1，请选择其他保存文件夹。").arg(folder());
        emit changed();
        return;
    }
    m_error.clear();
    if (m_hooks.starting) m_hooks.starting();
    if (!countdown()) { begin(); return; }
    m_countdown = 3;
    m_stage = Counting;
    m_timer.start();
    emit changed();
}

void Recorder::skipCountdown()
{
    if (m_stage != Counting) return;
    m_timer.stop();
    begin();
}

void Recorder::begin()
{
    recording::Options options;
    options.region = m_region;
    const bool gif = format() == Gif;
    options.fps = gif ? (quality() == SmallFile ? 10 : quality() == HighQuality ? 20 : 15) : (quality() == SmallFile ? 15 : 30);
    options.quality = gif ? 92 : quality() == SmallFile ? 70 : quality() == HighQuality ? 88 : 75;
    options.cursor = cursor();
    options.systemAudio = !gif && systemAudio();
    options.microphone = !gif && microphone();
    options.clickHighlights = clickHighlights();
    options.pointerScale = m_dpr;
    if (options.clickHighlights) {
        options.clickInput = std::make_shared<recording::ClickStream>(m_region, m_panel ? quintptr(m_panel->winId()) : 0);
        if (!m_hooks.clicks || !m_hooks.clicks(options.clickInput)) {
            if (m_hooks.clicks) m_hooks.clicks({});
            recording::Result failure;
            failure.error = tr("无法启用点击高亮，请关闭该选项后重试。");
            completed(failure);
            return;
        }
        m_clicksActive = true;
    }
    const int gifLongest = quality() == SmallFile ? 960 : quality() == HighQuality ? 1920 : 1280;
    const auto keyMode = static_cast<keycaps::Mode>(keys());
    if (keyMode != keycaps::Mode::Off || mouseButtons()) {
        // GitHub #98: the keys and buttons pressed, drawn into the frames.
        options.keys.mode = keyMode;
        options.keys.mouse = mouseButtons();
        options.keys.words = {tr("空格"), tr("左键"), tr("右键"), tr("中键"), tr("后退键"), tr("前进键")};
        if (m_hooks.ownKeys) options.keys.own = m_hooks.ownKeys();
        options.keys.doubleTap = qint64(std::clamp<UINT>(::GetDoubleClickTime(), 400, 900)) * 10'000;
        options.keyFont = QGuiApplication::font();
        // A GIF is scaled down: its keycaps are drawn larger to stay legible.
        if (gif)
            options.outputScale = std::min(1.0, double(recording::gifSize(m_region.size(), gifLongest).width()) / m_region.width());
        options.keyInput = std::make_shared<recording::KeyStream>();
        m_keyHook = std::make_unique<recording::KeyHook>(recording::KeyHook::Setup {
            keyMode, m_region, m_panel ? quintptr(m_panel->winId()) : 0, options.keyInput});
        if (m_keyHook->failed()) {
            releaseKeys();
            recording::Result failure;
            failure.error = keyMode != keycaps::Mode::Off ? tr("无法显示按键，请选“不显示按键”后重试。")
                                                          : tr("无法显示鼠标按键，请关闭该选项后重试。");
            completed(failure);
            return;
        }
    }
    const QString base = u"WinShun_%1"_s.arg(QDateTime::currentDateTime().toString(u"yyyyMMdd_HHmmss_zzz"_s));
    QString name = base + u".mp4"_s;
    for (int i = 2; QFileInfo::exists(QDir(folder()).filePath(name))
            || (gif && QFileInfo::exists(QDir(folder()).filePath(QFileInfo(name).completeBaseName() + u".gif"_s))); ++i)
        name = base + u"_%1.mp4"_s.arg(i);
    options.path = QDir(folder()).filePath(name);
    const recording::GifOptions gifOptions {QDir(folder()).filePath(QFileInfo(name).completeBaseName() + u".gif"_s),
        options.fps, gifLongest};
    m_control = std::make_unique<recording::Control>();
    m_stage = Starting;
    emit changed();
    m_worker = std::jthread([this, options, gif, gifOptions, control = m_control.get()] {
        const auto update = [this](const recording::Progress& progress) {
            QMetaObject::invokeMethod(this, [this, progress] {
                if (!m_control) return;
                const qint64 capturedMs = m_progress.elapsedMs;
                m_progress = progress;
                if (m_exporting) m_progress.elapsedMs = capturedMs;
                if (m_stage == Starting) m_stage = Recording;
                emit changed();
            }, Qt::QueuedConnection);
        };
        auto result = recording::record(options, *control, update);
        if (gif && result.error.isEmpty() && !result.path.isEmpty()) {
            QMetaObject::invokeMethod(this, [this, elapsedMs = result.elapsedMs] {
                m_exporting = true;
                m_stage = Saving;
                m_progress.elapsedMs = elapsedMs;
                m_border.reset();
                releaseClicks();
                releaseKeys();
                emit changed();
            }, Qt::QueuedConnection);
            auto exported = recording::exportGif(result.path, gifOptions, *control, update);
            if (!exported.path.isEmpty() && exported.error.isEmpty()) {
                QFile::remove(result.path); // this worker created the intermediate video
                result = std::move(exported);
            } else {
                result.error = tr("%1\n已保留原始视频：%2").arg(exported.error, QDir::toNativeSeparators(result.path));
            }
        }
        QMetaObject::invokeMethod(this, [this, result] { completed(result); }, Qt::QueuedConnection);
    });
}

void Recorder::pause()
{
    if (!m_control || (m_stage != Recording && m_stage != Paused)) return;
    m_stage = m_stage == Recording ? Paused : Recording;
    m_control->paused = m_stage == Paused;
    if (m_keyHook) m_keyHook->setPaused(m_stage == Paused); // out while paused: nothing typed then is looked at
    emit changed();
}

void Recorder::finish()
{
    if (m_control) {
        m_control->stop = true;
        releaseKeys();
        m_stage = Saving;
        emit changed();
    } else {
        close();
    }
}

void Recorder::completed(const recording::Result& result)
{
    if (m_worker.joinable()) m_worker.join();
    releaseClicks(); // detach before a changed handler can begin the next take
    releaseKeys();
    m_control.reset();
    m_progress = result;
    m_path = result.path;
    m_error = result.error;
    m_stage = result.error.isEmpty() && !result.path.isEmpty() ? Done : Failed;
    if (!result.frames && result.error.isEmpty()) { close(); return; }
    m_border.reset();
    emit changed();
    if (m_panel) m_panel->requestActivate();
    if (m_hooks.finished) m_hooks.finished();
}

void Recorder::close()
{
    if (m_control) { finish(); return; }
    m_timer.stop();
    m_border.reset();
    m_stage = Ready;
    if (m_panel) { m_panel->hide(); m_panel->releaseResources(); }
    emit changed();
}

void Recorder::chooseFolder()
{
    if (m_stage != Ready) return;
    const QString picked = shell::pickFolder(m_panel ? reinterpret_cast<HWND>(m_panel->winId()) : nullptr, tr("选择录屏保存文件夹"));
    if (!picked.isEmpty()) { m_preferences.folder = picked; remember(); }
}

void Recorder::play() { if (!m_path.isEmpty()) shell::open(m_path); }
void Recorder::copy() { if (!m_path.isEmpty()) shell::copyFiles({m_path}); }
void Recorder::reveal() { if (!m_path.isEmpty()) shell::reveal(m_path); }
void Recorder::again() { if (m_stage == Done || m_stage == Failed) prepare(m_region, m_monitor, m_dpr); }

} // namespace ws::capture
