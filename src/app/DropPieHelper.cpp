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

#include <QDir>
#include <QDebug>
#include <QLocale>
#include <QFile>
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
#include <memory>

using namespace Qt::StringLiterals;

namespace ws {
namespace {

bool save(const QString& path, const QByteArray& bytes)
{
    QSaveFile file(path);
    return file.open(QIODevice::WriteOnly) && file.write(bytes) == bytes.size() && file.commit();
}

QByteArray read(const QString& path)
{
    QFile file(path);
    return file.open(QIODevice::ReadOnly) ? file.read(1024 * 1024) : QByteArray();
}

} // namespace

DropPieHelper::DropPieHelper(Favorites* favorites, Notify notify, QObject* parent)
    : QObject(parent), m_favorites(favorites), m_notify(std::move(notify)),
      m_profile(QDir::tempPath() + u"/WinShun-drop-pie-XXXXXX"_s)
{
    connect(favorites, &Favorites::changed, this, [this] { snapshot(); });
    m_poll.setInterval(250);
    connect(&m_poll, &QTimer::timeout, this, &DropPieHelper::poll);
}

DropPieHelper::~DropPieHelper()
{
    m_poll.stop();
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
    if (!snapshot()) {
        if (m_notify) m_notify(tr("收藏圆盘未启动"), tr("无法创建圆盘的临时配置，请检查临时文件夹。"));
        return;
    }
    {
        QSettings config(m_profile.filePath(u"WinShun.ini"_s), QSettings::IniFormat);
        config.setValue(u"Appearance/Theme"_s, settings.theme);
        config.setValue(u"Appearance/Language"_s, settings.language);
        config.setValue(u"Appearance/UiSize"_s, settings.uiSize);
        config.setValue(u"Appearance/Corners"_s, settings.corners);
        config.setValue(u"Launcher/Renderer"_s, Settings::resolveRenderer(settings.renderer));
        config.sync();
    }
    if (m_process.valid() && ::WaitForSingleObject(m_process.get(), 0) == WAIT_TIMEOUT) return;
    QFile::remove(m_profile.filePath(u"stop"_s));
    QFile::remove(m_profile.filePath(u"ready"_s));
    const QString args = u"--drop-pie-helper %1 --profile \"%2\""_s.arg(::GetCurrentProcessId()).arg(m_profile.path());
    m_process = shell::startUnelevated(QDir::toNativeSeparators(QCoreApplication::applicationFilePath()).toStdWString(), args.toStdWString());
    m_starting.start();
    m_reported = false;
    m_poll.start();
    poll();
}

void DropPieHelper::poll()
{
    const QString noticePath = m_profile.filePath(u"notice.json"_s);
    const auto notice = QJsonDocument::fromJson(read(noticePath)).object();
    if (!notice.isEmpty()) {
        QFile::remove(noticePath);
        if (m_notify) m_notify(notice.value(u"title"_s).toString(), notice.value(u"text"_s).toString());
    }
    const bool exited = !m_process.valid() || ::WaitForSingleObject(m_process.get(), 0) != WAIT_TIMEOUT;
    const bool timeout = m_starting.elapsed() > 5000 && !QFile::exists(m_profile.filePath(u"ready"_s));
    if (!m_reported && (exited || timeout)) {
        m_reported = true;
        qWarning("Drop pie helper: %s", exited ? "did not start or exited" : "startup timed out");
        if (m_notify) m_notify(tr("收藏圆盘未启动"), tr("无法启动普通权限的圆盘进程。请确认资源管理器正在运行，再关闭并重新开启收藏圆盘。"));
    }
    if (exited) m_poll.stop();
}

QJsonObject DropPieHelper::state() const
{
    auto state = QJsonDocument::fromJson(read(m_profile.filePath(u"ready"_s))).object();
    const bool running = m_process.valid() && ::WaitForSingleObject(m_process.get(), 0) == WAIT_TIMEOUT;
    state.insert(u"ready"_s, running && !state.isEmpty());
    state.insert(u"running"_s, running);
    return state;
}

int runDropPieHelper(const QStringList& arguments)
{
    const qsizetype at = arguments.indexOf(u"--drop-pie-helper"_s);
    if (at < 0 || at + 1 >= arguments.size() || Settings::profile().isEmpty() || integrity::processElevated()) return 2;
    bool valid = false;
    const DWORD pid = arguments[at + 1].toUInt(&valid);
    win32::UniqueHandle parent(valid ? ::OpenProcess(SYNCHRONIZE, FALSE, pid) : nullptr);
    if (!parent.valid()) return 3;
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
    if (!window) { qWarning().noquote() << component.errorString(); return 4; }
    pie.setWindow(window);
    if (!pie.dropReady()) return 6;
    KeyListener::Callbacks callbacks;
    callbacks.dragMenu = [&pie](POINT pt, HWND capture, DWORD source) { pie.summoned(pt, capture, source); };
    callbacks.leftButtonUp = [&pie] { pie.buttonReleased(); };
    callbacks.log = [](const std::string& text) { qWarning().noquote() << QString::fromStdString(text); };
    KeyListener listener(std::move(callbacks));
    if (!listener.isActive()) return 5;
    QFileSystemWatcher watcher({profile});
    QByteArray previous = read(profile + u"/favorites.json"_s);
    QObject::connect(&watcher, &QFileSystemWatcher::directoryChanged, &engine, [&] {
        const QByteArray next = read(profile + u"/favorites.json"_s);
        if (!next.isEmpty() && next != previous) { previous = next; favorites.load(); }
        appearance();
    });
    QTimer lifetime;
    lifetime.setInterval(100);
    QObject::connect(&lifetime, &QTimer::timeout, &engine, [&] {
        if ((::WaitForSingleObject(parent.get(), 0) != WAIT_TIMEOUT || QFile::exists(profile + u"/stop"_s)) && !pie.inDrop())
            QCoreApplication::quit();
    });
    lifetime.start();
    const auto ready = [&] {
        save(profile + u"/ready"_s, QJsonDocument(QJsonObject {{u"pid"_s, qint64(::GetCurrentProcessId())},
            {u"elevated"_s, integrity::processElevated()}, {u"folders"_s, pie.noFolders() ? 0 : pie.sectors().size()}}).toJson(QJsonDocument::Compact));
    };
    QObject::connect(&pie, &DropPie::sectorsChanged, &engine, ready);
    ready();
    return QCoreApplication::exec();
}

} // namespace ws
