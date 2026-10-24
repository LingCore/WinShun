#include "App.h"
#include "Diagnostics.h"
#include "Backup.h"
#include "Migration.h"
#include "Preview.h"
#include "Settings.h"
#include "SettingsCommand.h"
#include "SystemCatalog.h"
#include "platform/MessageWindow.h"
#include "platform/Shell.h"

#include <QClipboard>
#include <QCommandLineParser>
#include <QDateTime>
#include <QDebug>
#include <QDir>
#include <QFile>
#include <QFont>
#include <QFontDatabase>
#include <QGuiApplication>
#include <QProcess>

#include <windows.h>
#include <shellapi.h>

#include <mutex>
#include <string_view>

using namespace Qt::StringLiterals;

namespace {

QtMessageHandler g_defaultHandler = nullptr;

// Warnings and errors also go to %LOCALAPPDATA%\WinShun\WinShun.log: a
// tray app has no console, and the user's machine is where things go wrong.
// Over 1 MB, the log becomes WinShun.old.log and a new one starts. With
// WINSHUN_LOG_INFO set (a test, a user asked to), the info lines too (what
// the folder menu did, the dialogs followed…).
void logToFile(QtMsgType type, const QMessageLogContext& context, const QString& message)
{
    static const bool info = qEnvironmentVariableIsSet("WINSHUN_LOG_INFO");
    if (type != QtDebugMsg && (type != QtInfoMsg || info)) {
        ws::diagnostics::record(u"app-message"_s,
            {{u"level"_s, type == QtWarningMsg ? u"warning"_s : type == QtCriticalMsg ? u"error"_s
                    : type == QtFatalMsg ? u"fatal"_s : u"info"_s},
                {u"message"_s, message}});
        static std::mutex mutex; // messages come from every thread
        const std::lock_guard lock(mutex);
        const QString path = ws::Settings::dataDir() + u"\\WinShun.log"_s;
        const QByteArray text = message.left(4096).toUtf8();
        bool canAppend = true;
        if (QFile::exists(path) && QFile(path).size() + text.size() + 64 > (1 << 20)) {
            const QString old = ws::Settings::dataDir() + u"\\WinShun.old.log"_s;
            QFile::remove(old);
            canAppend = QFile::rename(path, old);
        }
        QFile file(path);
        if (canAppend && file.open(QIODevice::Append | QIODevice::Text)) {
            const char* level = type == QtInfoMsg ? "info"
                : type == QtWarningMsg         ? "warning"
                : type == QtCriticalMsg        ? "error"
                                               : "fatal";
            file.write(QDateTime::currentDateTime().toString(Qt::ISODateWithMs).toUtf8() + ' ' + level + ": "
                + text + '\n');
        }
    }
    if (g_defaultHandler)
        g_defaultHandler(type, context, message); // the debugger's output, as before
}

// UI font: Alibaba PuHuiTi 3.0 (Regular + Bold) from fonts\ next to the
// exe. It covers Latin and CJK, so no fallback is needed for normal text.
// Without the files, the system UI font (Microsoft YaHei UI) is kept;
// never "Segoe UI Variable", whose CJK fallback costs ~35 MB of memory.
//
// Given by file name, FreeType (the engine of the GPU renderer) opens the
// file through the C runtime, which cannot take a path outside the ANSI
// code page (F:\电脑便捷工具\…, D:\软件\WinShun): the font silently stayed
// unloaded and the window came up in Microsoft YaHei UI. Such a path is
// read here and handed over as data (the ~8 MB of each file stay in
// memory then; by name the face is read from disk as needed).
void applyUiFont()
{
    QString uiFamily;
    const QString fontDir = QCoreApplication::applicationDirPath() + u"/fonts/"_s;
    bool ascii = true;
    for (const QChar c : fontDir)
        ascii = ascii && c.unicode() < 128;
    for (const QString& file : {u"AlibabaPuHuiTi-3-55-Regular.ttf"_s, u"AlibabaPuHuiTi-3-85-Bold.ttf"_s}) {
        int id = -1;
        if (ascii) {
            id = QFontDatabase::addApplicationFont(fontDir + file);
        } else {
            QFile data(fontDir + file);
            if (data.open(QIODevice::ReadOnly))
                id = QFontDatabase::addApplicationFontFromData(data.readAll());
        }
        const QStringList families = QFontDatabase::applicationFontFamilies(id);
        if (uiFamily.isEmpty() && !families.isEmpty())
            uiFamily = families.first(); // both files share one family, Bold is its bold style
    }
    if (uiFamily.isEmpty()) {
        qWarning("UI font: nothing loaded from %s, using the system font", qUtf8Printable(QDir::toNativeSeparators(fontDir)));
        return;
    }
    QFont font(uiFamily);
    // Vertical only: full hinting also moves glyphs sideways ("you ng").
    font.setHintingPreference(QFont::PreferVerticalHinting);
    QGuiApplication::setFont(font);
}

// The arguments as one command line (CreateProcess, ShellExecute): quoted
// where they have spaces (a --profile folder), quotes escaped.
std::wstring commandLine(const QStringList& arguments)
{
    QString line;
    for (const QString& argument : arguments) {
        if (!line.isEmpty())
            line += u' ';
        if (argument.isEmpty() || argument.contains(u' ') || argument.contains(u'"') || argument.contains(u'\t'))
            line += u'"' + QString(argument).replace(u'"', u"\\\""_s) + u'"';
        else
            line += argument;
    }
    return line.toStdWString();
}

// Starts the new copy once this one has let go (see main): as this one ran,
// as administrator (UAC asks; declined, as this one ran, so that Win顺 is
// back at all), or with the user's normal rights (through the desktop
// shell's token while this one is elevated; without a shell, as this one
// ran, and the log says so).
void startAgain(const QStringList& arguments, ws::App::Elevation elevation)
{
    const QString exe = QCoreApplication::applicationFilePath();
    const std::wstring program = QDir::toNativeSeparators(exe).toStdWString();
    const std::wstring folder = QDir::toNativeSeparators(QCoreApplication::applicationDirPath()).toStdWString();
    if (elevation == ws::App::Elevation::Elevated && ws::shell::startElevated(program, commandLine(arguments), folder))
        return;
    if (elevation == ws::App::Elevation::Unelevated) {
        if (ws::shell::startUnelevated(program, commandLine(arguments), folder).valid())
            return;
        qWarning() << "Restart: no shell to start Win顺 with normal rights; started as before";
    }
    if (!QProcess::startDetached(exe, arguments))
        qWarning() << "Restart: could not start" << exe;
}

} // namespace

int main(int argc, char* argv[])
{
    // Never show "insert a disk" dialogs while probing drives.
    ::SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOOPENFILEERRORBOX);
    // --profile <folder> (see Settings::useProfile), before anything reads
    // the settings, so before Qt has the arguments: wide, as argv is ANSI.
    // --preview-plugins (Preview.h) wants its environment set here too.
    int wideCount = 0;
    QStringList wideArgs;
    if (LPWSTR* wide = ::CommandLineToArgvW(::GetCommandLineW(), &wideCount)) {
        for (int i = 1; i < wideCount; ++i)
            wideArgs.push_back(QString::fromWCharArray(wide[i]));
        for (int i = 1; i + 1 < wideCount; ++i) {
            if (std::wstring_view(wide[i]) == L"--profile")
                ws::Settings::useProfile(QString::fromWCharArray(wide[i + 1]));
        }
        ::LocalFree(wide);
    }
    // A portable copy: everything in the "data" folder beside the program.
    bool portable = false;
    if (ws::Settings::profile().isEmpty()) {
        const QString folder = ws::Settings::portableFolder();
        portable = !folder.isEmpty();
        if (portable)
            ws::Settings::useProfile(folder);
    }
    const bool preview = ws::preview::wanted(wideArgs);
    if (preview)
        ws::preview::prepare(wideArgs);
    if (ws::Settings::profile().isEmpty())
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
    if (preview) {
        // A picture of a settings page, and nothing else: before the single
        // instance below, which would hand the arguments to the running copy.
        ws::preview::note(u"making the application"_s);
        QCoreApplication::setOrganizationName(u"WinShun"_s);
        QCoreApplication::setApplicationName(u"WinShun"_s);
        QCoreApplication::setApplicationVersion(QStringLiteral(WINSHUN_VERSION)); // the version the window shows
        QGuiApplication previewApp(argc, argv);
        QGuiApplication::setApplicationDisplayName(u"Win顺"_s);
        ws::preview::note(u"loading the font"_s);
        applyUiFont();
        return ws::preview::run(wideArgs);
    }

    // Qt keeps a thread waiting for every vertical blank of the screen (240
    // times a second at 240 Hz) as long as a D3D11 window exists, hidden or
    // not, only to time QWindow::requestUpdate(). Without it a short timer
    // does that, and only when an update is asked for; Qt Quick's render
    // threads keep their pace by presenting.
    qputenv("QT_D3D_NO_VBLANK_THREAD", "1");

    QCoreApplication::setOrganizationName(u"WinShun"_s);
    QCoreApplication::setApplicationName(u"WinShun"_s);
    QCoreApplication::setApplicationVersion(QStringLiteral(WINSHUN_VERSION));
    QGuiApplication app(argc, argv);
    if (setPlatform)
        qunsetenv("QT_QPA_PLATFORM"); // programs we launch must not inherit it
    // Started from inside VS Code (its terminal, or a tool run by one of its
    // extensions), Win顺 inherits ELECTRON_RUN_AS_NODE=1, and every Electron
    // app it then opens (VS Code itself, Cherry Studio...) runs as a bare
    // Node.js and quits at once, without a window.
    qunsetenv("ELECTRON_RUN_AS_NODE");
    QGuiApplication::setApplicationDisplayName(u"Win顺"_s); // in English "WinShun" (App::applyAppearance)
    QGuiApplication::setQuitOnLastWindowClosed(false);
    // On every clipboard change each Qt Quick text field reads the clipboard's
    // whole text, only to update its canPaste (unused here): on the GUI
    // thread, with 50 ms sleeps while another program holds the clipboard
    // ("Retrying to obtain clipboard."). Pasting reads it when it happens, and
    // the clipboard history has its own listener (ClipboardWatcher).
    QGuiApplication::clipboard()->blockSignals(true);

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
    // With --take-autostart: the installer removed this copy's task (it may
    // have been another user's: the administrator who answered UAC for a
    // standard user's install), so one is written whether or not one is left.
    QCommandLineOption hadAutostart(u"had-autostart"_s, u"With --take-autostart: there was an autostart task."_s);
    hadAutostart.setFlags(QCommandLineOption::HiddenFromHelp);
    // With --settings, after a restart from them: where they were (App::restart).
    QCommandLineOption settingsAt(u"settings-at"_s, u"With --settings: open them there."_s, u"place"_s);
    settingsAt.setFlags(QCommandLineOption::HiddenFromHelp);
    QCommandLineOption profile(u"profile"_s, u"Keep the settings and data in this folder."_s, u"folder"_s);
    profile.setFlags(QCommandLineOption::HiddenFromHelp); // read above
    // For tests: the running copy writes what its launcher shows into this file (App::dumpLauncher).
    QCommandLineOption dump(u"dump"_s, u"Write the launcher's rows and state to this file."_s, u"file"_s);
    dump.setFlags(QCommandLineOption::HiddenFromHelp);
    parser.addOptions({background, toggle, query, quit, settings, settingsAt, takeAutostart, hadAutostart, profile, dump});
    parser.process(app);

    // The installer, once it has installed this copy: an autostart set up by
    // a portable copy (or the version before) now starts it. Through the
    // Task Scheduler API like the switch in the app; schtasks /Change would
    // ask for the user's password. Run as the user who started Setup, with
    // normal rights (GitHub #21): the task is theirs and runs Win顺 so (a
    // process without administrator rights may not register a task that has
    // them: writeTask). The 管理员直读 mode's elevated task is written again
    // by the copy that runs as administrator, when it next starts.
    if (parser.isSet(takeAutostart)) {
        if (ws::autostart::isSetUp() || parser.isSet(hadAutostart)) {
            ws::Settings stored;
            stored.load();
            ws::autostart::setElevated(stored.indexAccess == u"direct" && ws::userIsAdministrator());
            ws::autostart::setEnabled(true);
        }
        return 0;
    }

    // Single instance: hand the request to the copy that is already running.
    // An elevated copy's opens to one that is not (measured 2026-10-14); should
    // it ever be closed to it, access denied means one runs as well. The
    // command gets through to an elevated copy too (MessageWindow lets
    // WM_COPYDATA in).
    const HANDLE instanceMutex = ::CreateMutexW(nullptr, FALSE, L"Local\\WinShun.Instance");
    const DWORD mutexError = ::GetLastError();
    if ((instanceMutex && mutexError == ERROR_ALREADY_EXISTS) || (!instanceMutex && mutexError == ERROR_ACCESS_DENIED)) {
        QString command = u"show"_s;
        if (parser.isSet(quit))
            command = u"quit"_s;
        else if (parser.isSet(settings))
            command = ws::settingscommand::make(parser.value(settingsAt)); // --settings-at too: a running copy opens them there
        else if (parser.isSet(query))
            command = u"query:"_s + parser.value(query);
        else if (parser.isSet(toggle))
            command = u"toggle"_s;
        else if (parser.isSet(dump))
            command = u"dump:"_s + parser.value(dump);
        else if (parser.isSet(background))
            return 0;
        ws::MessageWindow::sendToRunningInstance(command);
        if (instanceMutex)
            ::CloseHandle(instanceMutex);
        return 0;
    }
    if (parser.isSet(quit))
        return 0; // nothing running
    if (portable)
        ws::copyIntoPortable(); // its first start: what the usual places hold
    ws::Backup::applyRestored(); // a backup chosen to restore, before the settings are read
    ws::diagnostics::initialize(ws::Settings::dataDir());
    ws::diagnostics::record(u"app-start"_s, ws::diagnostics::environment());
    g_defaultHandler = qInstallMessageHandler(logToFile); // the running copy only

    applyUiFont(); // the UI font (see above)

    int code = 1;
    std::optional<QStringList> restart;
    ws::App::Elevation elevation = ws::App::Elevation::Same;
    {
        ws::App application;
        ws::App::StartOptions options;
        options.background = parser.isSet(background);
        options.query = parser.value(query);
        options.settings = parser.isSet(settings);
        options.settingsAt = parser.value(settingsAt);
        if (application.start(options))
            code = QGuiApplication::exec();
        restart = application.restartArguments();
        elevation = application.restartElevation();
        if (restart && !ws::Settings::profile().isEmpty())
            *restart << u"--profile"_s << ws::Settings::profile();
    } // the index is saved here, while we still own the instance mutex
    if (instanceMutex)
        ::CloseHandle(instanceMutex);
    // Only now, or the new copy would find the mutex taken and just hand us a
    // command. It runs with whatever rights this one had (no UAC prompt),
    // unless the 索引方式 setting asked for others (App::Elevation).
    if (restart)
        startAgain(*restart, elevation);
    return code;
}
