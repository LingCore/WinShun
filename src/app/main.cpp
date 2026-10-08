#include "App.h"
#include "Migration.h"
#include "Settings.h"
#include "platform/MessageWindow.h"
#include "platform/Shell.h"

#include <QCommandLineParser>
#include <QDateTime>
#include <QDebug>
#include <QFile>
#include <QFont>
#include <QFontDatabase>
#include <QGuiApplication>
#include <QProcess>

#include <windows.h>

#include <mutex>

using namespace Qt::StringLiterals;

namespace {

QtMessageHandler g_defaultHandler = nullptr;

// Warnings and errors also go to %LOCALAPPDATA%\WinShun\WinShun.log: a
// tray app has no console, and the user's machine is where things go wrong.
// Over 1 MB, the log becomes WinShun.old.log and a new one starts.
void logToFile(QtMsgType type, const QMessageLogContext& context, const QString& message)
{
    if (type != QtDebugMsg && type != QtInfoMsg) {
        static std::mutex mutex; // messages come from every thread
        const std::lock_guard lock(mutex);
        const QString path = ws::Settings::dataDir() + u"\\WinShun.log"_s;
        if (QFile::exists(path) && QFile(path).size() > (1 << 20)) {
            const QString old = ws::Settings::dataDir() + u"\\WinShun.old.log"_s;
            QFile::remove(old);
            QFile::rename(path, old);
        }
        QFile file(path);
        if (file.open(QIODevice::Append | QIODevice::Text)) {
            const char* level = type == QtWarningMsg ? "warning" : type == QtCriticalMsg ? "error" : "fatal";
            file.write(QDateTime::currentDateTime().toString(Qt::ISODateWithMs).toUtf8() + ' ' + level + ": "
                + message.toUtf8() + '\n');
        }
    }
    if (g_defaultHandler)
        g_defaultHandler(type, context, message); // the debugger's output, as before
}

} // namespace

int main(int argc, char* argv[])
{
    // Never show "insert a disk" dialogs while probing drives.
    ::SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOOPENFILEERRORBOX);
    ws::migrateFromQuickFind(); // before anything reads the settings

    // Font engine. The UI font (Alibaba PuHuiTi, below) has no hinting, so GDI
    // and DirectWrite smear its horizontal strokes across two pixel rows;
    // FreeType with vertical hinting snaps them to the grid. But the software
    // renderer clips each text run to the glyphs' estimated bounds, which
    // FreeType's output overshoots (the dot of 毫 goes missing), so it gets
    // GDI. Both avoid the ~35 MB the DirectWrite database (Qt's default since
    // 6.8) needs once font fallback kicks in, e.g. for an emoji.
    const bool setPlatform = !qEnvironmentVariableIsSet("QT_QPA_PLATFORM");
    if (setPlatform)
        qputenv("QT_QPA_PLATFORM", ws::Settings::resolveRenderer(ws::Settings::storedRenderer()) == u"software"
                                       ? "windows:fontengine=gdi" : "windows:fontengine=freetype");

    QCoreApplication::setOrganizationName(u"WinShun"_s);
    QCoreApplication::setApplicationName(u"WinShun"_s);
    QCoreApplication::setApplicationVersion(QStringLiteral(WINSHUN_VERSION));
    QGuiApplication app(argc, argv);
    if (setPlatform)
        qunsetenv("QT_QPA_PLATFORM"); // programs we launch must not inherit it
    QGuiApplication::setApplicationDisplayName(u"Win顺"_s); // in English "WinShun" (App::applyAppearance)
    QGuiApplication::setQuitOnLastWindowClosed(false);

    QCommandLineParser parser;
    parser.setApplicationDescription(u"Win顺 · WinShun — 文件、文件夹与文本内容搜索"_s);
    parser.addHelpOption();
    parser.addVersionOption();
    const QCommandLineOption background(u"background"_s, u"Start in the tray without showing the window."_s);
    const QCommandLineOption toggle(u"toggle"_s, u"Show or hide the window of the running instance."_s);
    const QCommandLineOption query(u"query"_s, u"Open the window with this search text."_s, u"text"_s);
    const QCommandLineOption quit(u"quit"_s, u"Exit the running instance (saves its index)."_s);
    const QCommandLineOption settings(u"settings"_s, u"Open the settings window."_s);
    const QCommandLineOption takeAutostart(u"take-autostart"_s,
        u"If WinShun starts at login, start this copy instead, then exit (run by the installer)."_s);
    parser.addOptions({background, toggle, query, quit, settings, takeAutostart});
    parser.process(app);

    // The installer, once it has installed this copy: an autostart set up by
    // a portable copy now starts it. Through the Task Scheduler API like the
    // switch in the app; schtasks /Change would ask for the user's password.
    if (parser.isSet(takeAutostart)) {
        if (ws::autostart::isSetUp())
            ws::autostart::setEnabled(true);
        return 0;
    }

    // Single instance: hand the request to the copy that is already running.
    const HANDLE instanceMutex = ::CreateMutexW(nullptr, FALSE, L"Local\\WinShun.Instance");
    if (instanceMutex && ::GetLastError() == ERROR_ALREADY_EXISTS) {
        QString command = u"show"_s;
        if (parser.isSet(quit))
            command = u"quit"_s;
        else if (parser.isSet(settings))
            command = u"settings"_s;
        else if (parser.isSet(query))
            command = u"query:"_s + parser.value(query);
        else if (parser.isSet(toggle))
            command = u"toggle"_s;
        else if (parser.isSet(background))
            return 0;
        ws::MessageWindow::sendToRunningInstance(command);
        ::CloseHandle(instanceMutex);
        return 0;
    }
    if (parser.isSet(quit))
        return 0; // nothing running
    g_defaultHandler = qInstallMessageHandler(logToFile); // the running copy only

    // UI font: Alibaba PuHuiTi 3.0 (Regular + Bold) from fonts\ next to the
    // exe. It covers Latin and CJK, so no fallback is needed for normal text.
    // Without the files, the system UI font (Microsoft YaHei UI) is kept;
    // never "Segoe UI Variable", whose CJK fallback costs ~35 MB of memory.
    QString uiFamily;
    const QString fontDir = QCoreApplication::applicationDirPath() + u"/fonts/"_s;
    for (const QString& file : {u"AlibabaPuHuiTi-3-55-Regular.ttf"_s, u"AlibabaPuHuiTi-3-85-Bold.ttf"_s}) {
        const QStringList families = QFontDatabase::applicationFontFamilies(QFontDatabase::addApplicationFont(fontDir + file));
        if (uiFamily.isEmpty() && !families.isEmpty())
            uiFamily = families.first(); // both files share one family, Bold is its bold style
    }
    if (!uiFamily.isEmpty()) {
        QFont font(uiFamily);
        // Vertical only: full hinting also moves glyphs sideways ("you ng").
        font.setHintingPreference(QFont::PreferVerticalHinting);
        QGuiApplication::setFont(font);
    }

    int code = 1;
    std::optional<QStringList> restart;
    {
        ws::App application;
        ws::App::StartOptions options;
        options.background = parser.isSet(background);
        options.query = parser.value(query);
        options.settings = parser.isSet(settings);
        if (application.start(options))
            code = QGuiApplication::exec();
        restart = application.restartArguments();
    } // the index is saved here, while we still own the instance mutex
    if (instanceMutex)
        ::CloseHandle(instanceMutex);
    // Only now, or the new copy would find the mutex taken and just hand us a
    // command. It inherits our elevation, so no UAC prompt.
    if (restart && !QProcess::startDetached(QCoreApplication::applicationFilePath(), *restart))
        qWarning() << "Restart: could not start" << QCoreApplication::applicationFilePath();
    return code;
}
