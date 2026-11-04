#include "UninstallCleanup.h"

#include "AutostartRun.h"
#include "CommandLine.h"
#include "ScoopInstall.h"
#include "ServiceVolumeAccess.h"
#include "Settings.h"
#include "UpdatePackage.h"
#include "platform/FileManagers.h"
#include "platform/MessageWindow.h"
#include "platform/SettingBroadcast.h"
#include "platform/Shell.h"
#include "platform/ShellMenu.h"
#include "platform/WinV.h"

#include <PathText.h>

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QLocale>
#include <QSaveFile>

#include <windows.h>
#include <shellapi.h>
#include <tlhelp32.h>

#include <algorithm>
#include <chrono>
#include <string>
#include <thread>
#include <vector>

using namespace Qt::StringLiterals;

namespace ws::uninstall {

namespace {

bool g_chinese = true;

// What was done, one line each, to standard output as UTF-8. Started from a
// terminal the program has no stdout of its own: then the parent's console.
void say(const QString& zh, const QString& en)
{
    static HANDLE out = [] {
        HANDLE handle = ::GetStdHandle(STD_OUTPUT_HANDLE);
        if (!handle || handle == INVALID_HANDLE_VALUE) {
            if (::AttachConsole(ATTACH_PARENT_PROCESS))
                handle = ::GetStdHandle(STD_OUTPUT_HANDLE);
        }
        return handle;
    }();
    if (!out || out == INVALID_HANDLE_VALUE)
        return;
    const QByteArray line = (g_chinese ? zh : en).toUtf8() + "\r\n";
    DWORD written = 0;
    ::WriteFile(out, line.constData(), static_cast<DWORD>(line.size()), &written, nullptr);
}

struct Running {
    DWORD pid = 0;
    QString name;
    bool main = false; // WinShun.exe itself
};

// The programs of this copy that run (WinShun*.exe from its folder: the main
// program, the plugin host, the relay an AI tool started...). A process whose
// image cannot be read (another account's, or elevated when this is not) is
// not known to be ours.
std::vector<Running> runningFromHere(const QString& appDir)
{
    std::vector<Running> found;
    const HANDLE snapshot = ::CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snapshot == INVALID_HANDLE_VALUE)
        return found;
    PROCESSENTRY32W entry {sizeof(entry)};
    for (BOOL more = ::Process32FirstW(snapshot, &entry); more; more = ::Process32NextW(snapshot, &entry)) {
        const QString name = QString::fromWCharArray(entry.szExeFile);
        if (!name.startsWith(u"WinShun"_s, Qt::CaseInsensitive) || entry.th32ProcessID == ::GetCurrentProcessId())
            continue;
        const HANDLE process = ::OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, entry.th32ProcessID);
        if (!process)
            continue;
        wchar_t image[MAX_PATH * 2] = {};
        DWORD size = static_cast<DWORD>(std::size(image));
        const bool read = ::QueryFullProcessImageNameW(process, 0, image, &size);
        ::CloseHandle(process);
        if (read && scoop::sameCopy(QFileInfo(QString::fromWCharArray(image, static_cast<qsizetype>(size))).absolutePath(), appDir))
            found.push_back({entry.th32ProcessID, name, name.compare(u"WinShun.exe"_s, Qt::CaseInsensitive) == 0});
    }
    ::CloseHandle(snapshot);
    return found;
}

// Ends this copy: WinShun.exe the usual way (it saves), then waits for the
// rest. False when something of it is still there.
bool endThisCopy(const QString& appDir)
{
    using namespace std::chrono;
    std::vector<Running> running = runningFromHere(appDir);
    if (running.empty())
        return true;
    // Only the copy that runs from here is told: the instance mutex is the
    // same for every copy, so a command goes to whichever one runs.
    if (std::any_of(running.begin(), running.end(), [](const Running& r) { return r.main; })) {
        say(u"正在关闭运行中的 Win顺…"_s, u"Closing the running WinShun..."_s);
        MessageWindow::sendToRunningInstance(u"quit"_s);
    }
    const auto deadline = steady_clock::now() + seconds(30);
    while (steady_clock::now() < deadline) {
        std::this_thread::sleep_for(milliseconds(250));
        running = runningFromHere(appDir);
        if (running.empty())
            return true;
    }
    for (const Running& r : running)
        say(u"还在运行：%1（%2）"_s.arg(r.name).arg(r.pid), u"Still running: %1 (%2)"_s.arg(r.name).arg(r.pid));
    return false;
}

// Directory Opus's double click, when it runs this copy's WinShunSearch.exe:
// back to what it was before (the Win顺 setting keeps that), as switching it
// off in the settings does, but as a plain edit of the file: Opus is not
// closed and started again here, which would hold up whoever waits for this.
// Opus reads the file when it starts.
bool opusMenuOff(const Settings& settings, bool* touched)
{
    const std::wstring file = filemanager::opusPrefsFile();
    if (file.empty())
        return true;
    QFile prefsFile(QString::fromStdWString(file));
    if (!prefsFile.open(QIODevice::ReadOnly))
        return true;
    const QByteArray prefs = prefsFile.readAll();
    prefsFile.close();
    const pathtext::OpusBlankClick click = pathtext::opusBlankClick(prefs);
    if (!click.enabled || !click.command.contains(u"WinShunSearch.exe\" --folder-menu"_s, Qt::CaseInsensitive))
        return true;
    const std::wstring program = autostart::runCommandProgram(click.command.toStdWString());
    if (!scoop::sameCopy(QString::fromStdWString(program), QCoreApplication::applicationDirPath() + u"/WinShunSearch.exe"_s))
        return true; // another copy's (or the user's own): it stays
    const QByteArray changed = pathtext::withOpusBlankClick(prefs, settings.folderMenuOpusBefore.toUtf8());
    if (changed.isEmpty())
        return false;
    QSaveFile save(QString::fromStdWString(file));
    if (!save.open(QIODevice::WriteOnly) || save.write(changed) != changed.size() || !save.commit())
        return false;
    *touched = true;
    return true;
}

// The index service, when this copy installed it; the elevated
// `WinShunService.exe --uninstall` waited for (UAC asks).
bool removeService(const QString& appDir, bool* touched)
{
    const std::wstring from = ntfs::ServiceVolumeAccess::installedFrom();
    if (from.empty() || !scoop::sameCopy(QString::fromStdWString(from), appDir + u"/WinShun.exe"_s))
        return true;
    say(u"索引服务是这份 Win顺 装的，正在卸载（会问是否允许管理员权限）…"_s,
        u"The index service was installed by this copy; removing it (Windows asks for administrator rights)..."_s);
    const std::wstring exe = QDir::toNativeSeparators(appDir + u"/WinShunService.exe"_s).toStdWString();
    SHELLEXECUTEINFOW info {sizeof(info)};
    info.fMask = SEE_MASK_NOCLOSEPROCESS | SEE_MASK_NOASYNC;
    info.lpVerb = L"runas";
    info.lpFile = exe.c_str();
    info.lpParameters = L"--uninstall";
    info.nShow = SW_HIDE;
    if (!::ShellExecuteExW(&info) || !info.hProcess)
        return false;
    ::WaitForSingleObject(info.hProcess, 60000);
    DWORD code = 1;
    ::GetExitCodeProcess(info.hProcess, &code);
    ::CloseHandle(info.hProcess);
    *touched = true;
    return code == 0;
}

} // namespace

int cleanup()
{
    g_chinese = QLocale::system().language() == QLocale::Chinese;
    const QString appDir = QDir::fromNativeSeparators(QCoreApplication::applicationDirPath());
    bool failed = false;

    if (!endThisCopy(appDir)) {
        say(u"这份 Win顺 还有程序在运行，什么也没改。请先关掉它们（比如 AI 工具启动的 WinShunMcp.exe）。"_s,
            u"Programs of this WinShun are still running; nothing was changed. Close them first (for example WinShunMcp.exe started by an AI tool)."_s);
        return 3;
    }

    Settings settings;
    settings.load(); // this copy's own (its data folder)

    // Before the autostart goes: whether another copy takes over the keys
    // and the logon start (the installed one, or one that starts at logon).
    const bool otherCopy = update::setupInstalled() || autostart::otherCopyStarts();

    if (autostart::removeIfThisCopy())
        say(u"已取消开机自动启动。"_s, u"Autostart removed."_s);

    shellmenu::apply(false);
    shellmenu::applyPasteFile(false);
    say(u"已清除资源管理器右键菜单里属于这份的项。"_s, u"Explorer menu entries of this copy removed."_s);

    const DWORD pathError = cli::removeFromUserPath(cli::binFolder());
    if (pathError == 0) {
        settingbroadcast::send(settingbroadcast::Area::Environment);
        say(u"已把 ws 命令行的文件夹从 PATH 里拿掉。"_s, u"The ws folder was taken off the PATH."_s);
    } else if (pathError != ERROR_NOT_FOUND) {
        failed = true;
        say(u"没能改 PATH（错误 %1）。"_s.arg(pathError), u"Could not change the PATH (error %1)."_s.arg(pathError));
    }

    bool touched = false;
    if (!opusMenuOff(settings, &touched)) {
        failed = true;
        say(u"没能改 Directory Opus 的设置文件。"_s, u"Could not change Directory Opus's settings file."_s);
    } else if (touched) {
        say(u"已还原 Directory Opus 的双击空白处命令。"_s, u"Directory Opus's double click was restored."_s);
    }

    // Win+V and Win+S, to Windows' own, when this copy took them and nobody
    // else uses them (the letters in Explorer's DisabledHotkeys are one
    // setting for every copy). Explorer reads that when it next starts.
    const struct {
        wchar_t key;
        bool on;
        const char* name;
    } keys[] = {{L'V', settings.clipboardWinV, "Win+V"}, {L'S', settings.taskbarWinS, "Win+S"}};
    for (const auto& k : keys) {
        if (!k.on || !winv::recordedAsOurs(k.key))
            continue;
        if (otherCopy) {
            say(u"%1 留给另一份 Win顺 用，没动。"_s.arg(QLatin1StringView(k.name)),
                u"%1 is left to another WinShun."_s.arg(QLatin1StringView(k.name)));
        } else if (winv::giveBack(k.key)) {
            say(u"%1 已还给 Windows（下次登录或重启资源管理器后生效）。"_s.arg(QLatin1StringView(k.name)),
                u"%1 was given back to Windows (after the next sign-in or an Explorer restart)."_s.arg(QLatin1StringView(k.name)));
        } else {
            failed = true;
        }
    }

    touched = false;
    if (!removeService(appDir, &touched)) {
        failed = true;
        say(u"没能卸载索引服务（可能没有允许管理员权限）。"_s, u"The index service was not removed (administrator rights refused?)."_s);
    } else if (touched) {
        say(u"索引服务已卸载。"_s, u"The index service was removed."_s);
    }

    say(failed ? u"清理没有全部完成。"_s : u"清理完成，设置和数据没动。"_s,
        failed ? u"Cleanup did not finish everything."_s : u"Cleanup done; settings and data were left alone."_s);
    return failed ? 1 : 0;
}

} // namespace ws::uninstall
