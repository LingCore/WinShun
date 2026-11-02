#include "DropPieHelper.h"

#include "DropPie.h"
#include "DropPieText.h"
#include "Favorites.h"
#include "Settings.h"
#include "SystemCatalog.h"
#include "SystemTheme.h"
#include "platform/Integrity.h"
#include "platform/KeyListener.h"
#include "platform/Shell.h"

#include <QDateTime>
#include <QDir>
#include <QDebug>
#include <QLocale>
#include <QFile>
#include <QFileInfo>
#include <QFileSystemWatcher>
#include <QGuiApplication>
#include <QJsonDocument>
#include <QJsonObject>
#include <QQmlComponent>
#include <QQmlEngine>
#include <QQuickWindow>
#include <QSaveFile>
#include <QSettings>
#include <QStyleHints>
#include <QTranslator>

#include <algorithm>
#include <chrono>
#include <memory>

using namespace Qt::StringLiterals;

namespace ws {
namespace {

// How runDropPieHelper ends: its exit code, for the launcher's log.
enum HelperExit : int { Refused = 2, NoParent = 3, NoWindow = 4, NoListener = 5, NoDropTarget = 6 };

QString codeText(DWORD code)
{
    return code < 0x10000 ? QString::number(code) : u"0x"_s + QString::number(code, 16).toUpper(); // an NTSTATUS in hex
}

QString exitReason(DWORD code)
{
    switch (code) {
    case Refused: return u"exit 2: no profile, or above the desktop shell"_s;
    case NoParent: return u"exit 3: cannot watch Win顺"_s;
    case NoWindow: return u"exit 4: pie window not made"_s;
    case NoListener: return u"exit 5: no input listener"_s;
    case NoDropTarget: return u"exit 6: no drop target"_s;
    default: return u"exit %1"_s.arg(codeText(code)); // ended from outside, or crashed
    }
}

// The other process may have the file open just then: Qt opens files without
// FILE_SHARE_DELETE, and replacing one open so is refused (error 5, GitHub
// #172). Reads take microseconds: a few short tries.
bool save(const QString& path, const QByteArray& bytes)
{
    for (int attempt = 1;; ++attempt) {
        QSaveFile file(path);
        if (file.open(QIODevice::WriteOnly) && file.write(bytes) == bytes.size() && file.commit())
            return true;
        if (attempt == 5) {
            qWarning().noquote() << "Drop pie helper:" << QFileInfo(path).fileName() << "not written:" << file.errorString();
            return false;
        }
        ::Sleep(20);
    }
}

QByteArray read(const QString& path)
{
    QFile file(path);
    return file.open(QIODevice::ReadOnly) ? file.read(1024 * 1024) : QByteArray();
}

// When the process of `window` started, in ms since the epoch; now when that cannot be told.
qint64 startedAt(HWND window)
{
    DWORD pid = 0;
    ::GetWindowThreadProcessId(window, &pid);
    const win32::UniqueHandle process(pid ? ::OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid) : nullptr);
    FILETIME created {}, exited {}, kernel {}, user {};
    if (!process.valid() || !::GetProcessTimes(process.get(), &created, &exited, &kernel, &user))
        return QDateTime::currentMSecsSinceEpoch();
    // 100 ns ticks since 1601.
    const qint64 ticks = qint64((quint64(created.dwHighDateTime) << 32) | created.dwLowDateTime);
    return ticks / 10'000 - 11'644'473'600'000;
}

} // namespace

DropPieHelper::DropPieHelper(Favorites* favorites, Notify notify, QObject* parent)
    : QObject(parent), m_favorites(favorites), m_notify(std::move(notify)),
      m_profile(QDir::tempPath() + u"/WinShun-drop-pie-XXXXXX"_s)
{
    connect(favorites, &Favorites::changed, this, [this] { snapshot(); });
    m_poll.setInterval(250);
    connect(&m_poll, &QTimer::timeout, this, &DropPieHelper::poll);
    m_retry.setSingleShot(true);
    connect(&m_retry, &QTimer::timeout, this, &DropPieHelper::start);
    // WINSHUN_TEST_PIE_NO_SHELL=<n> (live tests, a --profile run only): the
    // first n starts find no desktop shell, as at logon before Explorer.
    if (QCoreApplication::arguments().contains(u"--profile"_s))
        m_noShell = qEnvironmentVariableIntValue("WINSHUN_TEST_PIE_NO_SHELL");
}

DropPieHelper::~DropPieHelper()
{
    m_poll.stop();
    m_retry.stop();
    if (m_process.valid()) {
        save(m_profile.filePath(u"stop"_s), "stop");
        // A drop's confirmation or file operation may still be running.
        // Never kill it or remove its snapshot while it is finishing.
        if (::WaitForSingleObject(m_process.get(), 750) != WAIT_OBJECT_0) m_profile.setAutoRemove(false);
    }
}

bool DropPieHelper::snapshot()
{
    if (!m_profile.isValid() || !m_favorites) return false;
    FavoriteList folders;
    for (const Favorite& favorite : m_favorites->items())
        if (favorite.kind == Favorite::Kind::Folder || favorite.kind == Favorite::Kind::Place)
            folders.push_back(favorite);
    DropPieText::setFolderCount(int(std::count_if(m_favorites->items().cbegin(), m_favorites->items().cend(),
        [](const Favorite& favorite) { return favorite.kind == Favorite::Kind::Folder; })));
    return save(m_profile.filePath(u"favorites.json"_s), writeFavoritesFile(folders));
}

void DropPieHelper::update(const Settings& settings)
{
    if (!m_profile.isValid()) {
        if (m_notify && !m_reported) m_notify(tr("收藏圆盘未启动"), tr("无法创建圆盘的临时配置，请检查临时文件夹。"), {});
        m_reported = true;
        return;
    }
    {
        // Where the helper's Settings::load() reads it (Settings::useProfile).
        QSettings config(m_profile.filePath(u"WinShun/WinShun.ini"_s), QSettings::IniFormat);
        config.setValue(u"Appearance/Theme"_s, settings.theme);
        config.setValue(u"Appearance/Language"_s, settings.language);
        config.setValue(u"Appearance/UiSize"_s, settings.uiSize);
        config.setValue(u"Appearance/Corners"_s, settings.corners);
        config.setValue(u"Launcher/Renderer"_s, Settings::resolveRenderer(settings.renderer));
        config.sync();
    }
    snapshot(); // after it: the helper looks at both when its folder changes
    start();
}

void DropPieHelper::retry()
{
    m_reported = false;
    start();
}

void DropPieHelper::start()
{
    if (!m_profile.isValid() || (m_process.valid() && ::WaitForSingleObject(m_process.get(), 0) == WAIT_TIMEOUT)) return;
    m_retry.stop();
    const HWND shell = m_noShell > 0 ? (--m_noShell, nullptr) : ::GetShellWindow();
    if (!shell) {
        // An elevated autostart at logon can come before Explorer: no user's
        // token to take yet. Not a failure: looked for again shortly, and at
        // TaskbarCreated (retry).
        m_failure = u"waiting for the desktop shell"_s;
        m_retry.start(std::chrono::seconds(2));
        return;
    }
    m_shellSince = startedAt(shell);
    QFile::remove(m_profile.filePath(u"stop"_s));
    QFile::remove(m_profile.filePath(u"ready"_s));
    const QString args = u"--drop-pie-helper %1 --profile \"%2\""_s.arg(::GetCurrentProcessId()).arg(m_profile.path());
    shell::StartFailure failure;
    m_process = shell::startUnelevated(
        QDir::toNativeSeparators(QCoreApplication::applicationFilePath()).toStdWString(), args.toStdWString(), {}, &failure);
    if (!m_process.valid()) {
        failed(Failure::Start, u"not started: %1, error %2"_s.arg(QLatin1StringView(failure.step)).arg(failure.error),
            failure.error);
        return;
    }
    m_starting.start();
    m_ready = false;
    m_poll.start();
}

void DropPieHelper::poll()
{
    const QString noticePath = m_profile.filePath(u"notice.json"_s);
    const auto notice = QJsonDocument::fromJson(read(noticePath)).object();
    if (!notice.isEmpty()) {
        QFile::remove(noticePath);
        if (m_notify) m_notify(notice.value(u"title"_s).toString(), notice.value(u"text"_s).toString(), {});
    }
    if (!m_ready && QFile::exists(m_profile.filePath(u"ready"_s))) {
        m_ready = true;
        if (m_failures > 0) qWarning("Drop pie helper: running, after %d failed tries", m_failures);
    }
    if (::WaitForSingleObject(m_process.get(), 0) != WAIT_TIMEOUT) {
        DWORD code = 0;
        ::GetExitCodeProcess(m_process.get(), &code);
        m_poll.stop();
        m_process = {};
        if (m_ready && m_starting.elapsed() > 60'000) { // it worked: a new run of failures, if any
            m_failures = 0;
            m_reported = false;
        }
        failed(Failure::Exit, exitReason(code), code);
    } else if (!m_ready && m_starting.elapsed() > 20'000) {
        // Never got going: not in a drop (it is ready long before one), so ended.
        ::TerminateProcess(m_process.get(), 1);
        m_poll.stop();
        m_process = {};
        failed(Failure::NoAnswer, u"no answer in 20 s"_s, 0);
    }
}

void DropPieHelper::failed(Failure kind, const QString& detail, unsigned long code)
{
    ++m_failures;
    m_failure = detail;
    qWarning().noquote() << "Drop pie helper:" << detail << u"(%1 in a row)"_s.arg(m_failures);
    // 1, 2, 4 … 60 s: never given up, never a busy loop.
    m_retry.start(std::chrono::seconds(std::min(60, 1 << std::min(m_failures - 1, 6))));
    // Shown only when it keeps failing with Explorer up a while: at logon
    // it usually comes right by itself.
    if (m_reported || !m_notify || m_failures < 3 || QDateTime::currentMSecsSinceEpoch() - m_shellSince < 60'000)
        return;
    m_reported = true;
    const auto again = [this] { retry(); };
    if (!shell::runsAsShellUser())
        m_notify(tr("收藏圆盘未启动"), tr("Win顺 正以另一个账户的管理员身份运行，收藏圆盘无法使用。请用你自己的账户运行 Win顺。"), {});
    else if (kind == Failure::Start)
        m_notify(tr("收藏圆盘未启动"),
            tr("Windows 没有让圆盘以普通权限启动（错误 %1）。Win顺 会自动再试，也可以点此马上重试。").arg(code), again);
    else if (kind == Failure::Exit)
        m_notify(tr("收藏圆盘未启动"),
            tr("圆盘启动后很快就退出了（代码 %1）。Win顺 会自动再试，也可以点此马上重试。").arg(codeText(code)), again);
    else
        m_notify(tr("收藏圆盘未启动"), tr("圆盘启动后一直没有响应。Win顺 会自动再试，也可以点此马上重试。"), again);
}

QJsonObject DropPieHelper::state() const
{
    auto state = QJsonDocument::fromJson(read(m_profile.filePath(u"ready"_s))).object();
    const bool running = m_process.valid() && ::WaitForSingleObject(m_process.get(), 0) == WAIT_TIMEOUT;
    state.insert(u"ready"_s, running && !state.isEmpty());
    state.insert(u"running"_s, running);
    state.insert(u"failures"_s, m_failures);
    if (!m_failure.isEmpty()) state.insert(u"last_failure"_s, m_failure);
    return state;
}

int runDropPieHelper(const QStringList& arguments)
{
    const qsizetype at = arguments.indexOf(u"--drop-pie-helper"_s);
    // Started with the desktop shell's token: never above it, where drops
    // from the user's programs could not reach it. As high as it when the
    // shell is elevated too (UAC off, the built-in Administrator).
    if (at < 0 || at + 1 >= arguments.size() || Settings::profile().isEmpty() || integrity::lowerThanOurs(::GetShellWindow()))
        return Refused;
    bool valid = false;
    const DWORD pid = arguments[at + 1].toUInt(&valid);
    win32::UniqueHandle parent(valid ? ::OpenProcess(SYNCHRONIZE, FALSE, pid) : nullptr);
    if (!parent.valid()) return NoParent;
    const QString profile = Settings::profile();
    QGuiApplication::setQuitOnLastWindowClosed(false);
    QQuickWindow::setGraphicsApi(Settings::resolveRenderer(Settings::storedRenderer()) == u"software"
        ? QSGRendererInterface::Software : QSGRendererInterface::Direct3D11);
    QQuickWindow::setTextRenderType(QQuickWindow::NativeTextRendering);
    QTranslator translation;
    Settings settings;
    settings.load();
    const bool english = settings.language == u"en" || (settings.language == u"system" && QLocale().language() != QLocale::Chinese);
    if (english && translation.load(u":/i18n/winshun_en.qm"_s)) QCoreApplication::installTranslator(&translation);
    const auto appearance = [&] {
        settings.load();
        QGuiApplication::styleHints()->setColorScheme(settings.theme == u"dark" ? Qt::ColorScheme::Dark
            : settings.theme == u"light" ? Qt::ColorScheme::Light : Qt::ColorScheme::Unknown);
        SystemTheme::setUiSize(settings.uiSize);
        SystemTheme::setRoundedCorners(settings.corners != u"square");
    };
    appearance();
    Favorites favorites(profile + u"/favorites.json"_s);
    favorites.load();
    SystemCatalog places;
    places.refresh(true);
    piedrop::FileOperations operations;
    QQmlEngine engine;
    const auto notify = [profile](const QString& title, const QString& text) {
        QMetaObject::invokeMethod(qApp, [profile, title, text] {
            save(profile + u"/notice.json"_s, QJsonDocument(QJsonObject {{u"title"_s, title}, {u"text"_s, text}}).toJson(QJsonDocument::Compact));
        }, Qt::QueuedConnection);
    };
    DropPie pie(&favorites, &places, &operations, notify);
    QQmlComponent component(&engine, u"WinShun"_s, u"DropPieWindow"_s);
    std::unique_ptr<QObject> object(component.createWithInitialProperties({{u"dropPie"_s, QVariant::fromValue(&pie)}}));
    auto* window = qobject_cast<QQuickWindow*>(object.get());
    if (!window) { qWarning().noquote() << component.errorString(); return NoWindow; }
    pie.setWindow(window);
    if (!pie.dropReady()) return NoDropTarget;
    KeyListener::Callbacks callbacks;
    callbacks.dragMenu = [&pie](POINT pt, HWND capture, DWORD source) { pie.summoned(pt, capture, source); };
    callbacks.leftButtonUp = [&pie] { pie.buttonReleased(); };
    callbacks.log = [](const std::string& text) { qWarning().noquote() << QString::fromStdString(text); };
    KeyListener listener(std::move(callbacks));
    if (!listener.isActive()) return NoListener;
    // What the launcher reads to know it runs, with what. Written only when
    // it changes: writing it changes the folder watched below.
    QByteArray written;
    const auto ready = [&] {
        const QByteArray next = QJsonDocument(QJsonObject {{u"pid"_s, qint64(::GetCurrentProcessId())},
            {u"elevated"_s, integrity::processElevated()}, {u"folders"_s, pie.noFolders() ? 0 : pie.sectors().size()},
            {u"theme"_s, settings.theme}}).toJson(QJsonDocument::Compact);
        if (next != written && save(profile + u"/ready"_s, next)) written = next;
    };
    QFileSystemWatcher watcher({profile});
    QByteArray previous = read(profile + u"/favorites.json"_s);
    QObject::connect(&watcher, &QFileSystemWatcher::directoryChanged, &engine, [&] {
        const QByteArray next = read(profile + u"/favorites.json"_s);
        if (!next.isEmpty() && next != previous) { previous = next; favorites.load(); }
        appearance();
        ready();
    });
    QTimer lifetime;
    lifetime.setInterval(100);
    QObject::connect(&lifetime, &QTimer::timeout, &engine, [&] {
        if ((::WaitForSingleObject(parent.get(), 0) != WAIT_TIMEOUT || QFile::exists(profile + u"/stop"_s)) && !pie.inDrop())
            QCoreApplication::quit();
    });
    lifetime.start();
    QObject::connect(&pie, &DropPie::sectorsChanged, &engine, ready);
    ready();
    return QCoreApplication::exec();
}

} // namespace ws
