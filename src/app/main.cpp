#include "App.h"
#include "Settings.h"
#include "platform/MessageWindow.h"
#include "platform/Shell.h"

#include <QCommandLineParser>
#include <QDateTime>
#include <QFile>
#include <QFont>
#include <QFontDatabase>
#include <QGuiApplication>

#include <windows.h>

#include <mutex>

using namespace Qt::StringLiterals;

namespace {

QtMessageHandler g_defaultHandler = nullptr;

// Warnings and errors also go to %LOCALAPPDATA%\QuickFind\QuickFind.log: a
// tray app has no console, and the user's machine is where things go wrong.
// Over 1 MB, the log becomes QuickFind.old.log and a new one starts.
void logToFile(QtMsgType type, const QMessageLogContext& context, const QString& message)
{
    if (type != QtDebugMsg && type != QtInfoMsg) {
        static std::mutex mutex; // messages come from every thread
        const std::lock_guard lock(mutex);
        const QString path = qf::Settings::dataDir() + u"\\QuickFind.log"_s;
        if (QFile::exists(path) && QFile(path).size() > (1 << 20)) {
            const QString old = qf::Settings::dataDir() + u"\\QuickFind.old.log"_s;
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

    // Font engine. The UI font (Alibaba PuHuiTi, below) has no hinting, so GDI
    // and DirectWrite smear its horizontal strokes across two pixel rows;
    // FreeType with vertical hinting snaps them to the grid. But the software
    // renderer clips each text run to the glyphs' estimated bounds, which
    // FreeType's output overshoots (the dot of 毫 goes missing), so it gets
    // GDI. Both avoid the ~35 MB the DirectWrite database (Qt's default since
    // 6.8) needs once font fallback kicks in, e.g. for an emoji.
    const bool setPlatform = !qEnvironmentVariableIsSet("QT_QPA_PLATFORM");
    if (setPlatform)
        qputenv("QT_QPA_PLATFORM", qf::Settings::resolveRenderer(qf::Settings::storedRenderer()) == u"software"
                                       ? "windows:fontengine=gdi" : "windows:fontengine=freetype");

    QCoreApplication::setOrganizationName(u"QuickFind"_s);
    QCoreApplication::setApplicationName(u"QuickFind"_s);
    QCoreApplication::setApplicationVersion(QStringLiteral(QUICKFIND_VERSION));
    QGuiApplication app(argc, argv);
    if (setPlatform)
        qunsetenv("QT_QPA_PLATFORM"); // programs we launch must not inherit it
    QGuiApplication::setApplicationDisplayName(u"快搜"_s);
    QGuiApplication::setQuitOnLastWindowClosed(false);

    QCommandLineParser parser;
    parser.setApplicationDescription(u"快搜 QuickFind — 文件、文件夹与文本内容搜索"_s);
    parser.addHelpOption();
    parser.addVersionOption();
    const QCommandLineOption background(u"background"_s, u"Start in the tray without showing the window."_s);
    const QCommandLineOption toggle(u"toggle"_s, u"Show or hide the window of the running instance."_s);
    const QCommandLineOption query(u"query"_s, u"Open the window with this search text."_s, u"text"_s);
    const QCommandLineOption quit(u"quit"_s, u"Exit the running instance (saves its index)."_s);
    const QCommandLineOption settings(u"settings"_s, u"Open the settings window."_s);
    parser.addOptions({background, toggle, query, quit, settings});
    parser.process(app);

    // Single instance: hand the request to the copy that is already running.
    const HANDLE instanceMutex = ::CreateMutexW(nullptr, FALSE, L"Local\\QuickFind.Instance");
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
        qf::MessageWindow::sendToRunningInstance(command);
        ::CloseHandle(instanceMutex);
        return 0;
    }
    if (parser.isSet(quit))
        return 0; // nothing running
    g_defaultHandler = qInstallMessageHandler(logToFile); // the running copy only

    qf::autostart::migrate();

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
    {
        qf::App application;
        qf::App::StartOptions options;
        options.background = parser.isSet(background);
        options.query = parser.value(query);
        options.settings = parser.isSet(settings);
        if (application.start(options))
            code = QGuiApplication::exec();
    } // the index is saved here, while we still own the instance mutex
    if (instanceMutex)
        ::CloseHandle(instanceMutex);
    return code;
}
