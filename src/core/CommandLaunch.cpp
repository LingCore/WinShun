#include "CommandLaunch.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QStringList>

#include <windows.h>

#include <array>

namespace ws::commandlaunch {

namespace {

using namespace Qt::StringLiterals;

const wchar_t* wide(const QString& s)
{
    return reinterpret_cast<const wchar_t*>(s.utf16());
}

QString expanded(const QString& text)
{
    if (!text.contains(u'%'))
        return text;
    std::wstring out(32768, L'\0');
    const DWORD n = ::ExpandEnvironmentStringsW(wide(text), out.data(), static_cast<DWORD>(out.size()));
    if (n == 0 || n > out.size())
        return text;
    return QString::fromWCharArray(out.c_str());
}

QString quoted(const QString& path)
{
    return u'"' + path + u'"'; // a path cannot hold a quote, so none needs escaping
}

// The default value of App Paths\<name>, as ShellExecute finds "chrome" or "winword".
QString appPath(const QString& name)
{
    const QString key = u"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\App Paths\\"_s + name;
    for (const HKEY root : {HKEY_CURRENT_USER, HKEY_LOCAL_MACHINE}) {
        wchar_t value[MAX_PATH * 2] = {};
        DWORD size = sizeof value - sizeof(wchar_t);
        DWORD type = 0;
        if (::RegGetValueW(root, wide(key), nullptr, RRF_RT_REG_SZ | RRF_RT_REG_EXPAND_SZ, &type, value, &size) != ERROR_SUCCESS)
            continue;
        QString path = expanded(QString::fromWCharArray(value)).trimmed();
        if (path.startsWith(u'"') && path.endsWith(u'"') && path.size() > 1)
            path = path.mid(1, path.size() - 2);
        if (QFileInfo(path).isFile())
            return QDir::toNativeSeparators(path);
    }
    return {};
}

} // namespace

int peSubsystem(const QString& path)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly))
        return 0;
    const QByteArray dos = file.read(64);
    if (dos.size() < 64 || dos[0] != 'M' || dos[1] != 'Z')
        return 0;
    const auto at = [](const QByteArray& b, int offset) {
        return static_cast<quint32>(static_cast<quint8>(b[offset])) | static_cast<quint32>(static_cast<quint8>(b[offset + 1])) << 8
            | static_cast<quint32>(static_cast<quint8>(b[offset + 2])) << 16 | static_cast<quint32>(static_cast<quint8>(b[offset + 3])) << 24;
    };
    const quint32 header = at(dos, 0x3c); // e_lfanew
    if (header < 64 || header > (1u << 20) || !file.seek(header))
        return 0;
    // "PE\0\0", the 20-byte file header, then the optional header, whose
    // Subsystem word is 68 bytes in (the same in PE32 and PE32+).
    const QByteArray pe = file.read(4 + 20 + 70);
    if (pe.size() < 4 + 20 + 70 || pe[0] != 'P' || pe[1] != 'E' || pe[2] != 0 || pe[3] != 0)
        return 0;
    return static_cast<quint8>(pe[24 + 68]) | static_cast<quint8>(pe[24 + 69]) << 8;
}

Kind kindOf(const QString& path)
{
    const QString suffix = QFileInfo(path).suffix().toLower();
    if (suffix == u"bat" || suffix == u"cmd")
        return Kind::Batch;
    if (suffix == u"ps1")
        return Kind::PowerShell;
    if (suffix != u"exe" && suffix != u"com")
        return Kind::Other;
    switch (peSubsystem(path)) {
    case IMAGE_SUBSYSTEM_WINDOWS_CUI:
        return Kind::Console;
    case IMAGE_SUBSYSTEM_WINDOWS_GUI:
        return Kind::Gui;
    default:
        return Kind::Other; // unreadable (a packaged app's alias), DOS or another subsystem
    }
}

QString resolveProgram(const QString& program, const QString& directory)
{
    const QString name = expanded(program.trimmed());
    if (name.isEmpty())
        return {};
    const bool hasSeparator = name.contains(u'\\') || name.contains(u'/') || name.contains(u':');
    const bool hasExtension = !QFileInfo(name).suffix().isEmpty();
    if (hasSeparator) {
        const QFileInfo info(name);
        if (info.isFile())
            return QDir::toNativeSeparators(info.absoluteFilePath());
        if (!hasExtension) {
            for (const QString& extension : {u".exe"_s, u".com"_s, u".cmd"_s, u".bat"_s, u".ps1"_s}) {
                if (QFileInfo(name + extension).isFile())
                    return QDir::toNativeSeparators(QFileInfo(name + extension).absoluteFilePath());
            }
        }
        return {};
    }
    // The folder it runs in comes first, as for CreateProcess's lookup.
    if (!directory.isEmpty()) {
        const QDir dir(expanded(directory));
        for (const QString& candidate : hasExtension ? QStringList {name} : QStringList {name + u".exe"_s, name + u".com"_s, name + u".cmd"_s, name + u".bat"_s, name + u".ps1"_s}) {
            if (QFileInfo info(dir.filePath(candidate)); info.isFile())
                return QDir::toNativeSeparators(info.absoluteFilePath());
        }
    }
    const std::array<const wchar_t*, 5> extensions = {L".exe", L".com", L".cmd", L".bat", L".ps1"};
    wchar_t found[MAX_PATH] = {};
    if (hasExtension) {
        if (::SearchPathW(nullptr, wide(name), nullptr, MAX_PATH, found, nullptr))
            return QString::fromWCharArray(found);
    } else {
        for (const wchar_t* extension : extensions) {
            if (::SearchPathW(nullptr, wide(name), extension, MAX_PATH, found, nullptr))
                return QString::fromWCharArray(found);
        }
    }
    if (const QString registered = appPath(name); !registered.isEmpty())
        return registered;
    return hasExtension ? QString() : appPath(name + u".exe"_s);
}

Tools systemTools()
{
    wchar_t folder[MAX_PATH] = {};
    const UINT n = ::GetSystemDirectoryW(folder, MAX_PATH);
    if (n == 0 || n >= MAX_PATH)
        return {};
    const QString system = QString::fromWCharArray(folder, static_cast<qsizetype>(n));
    return {system + u"\\cmd.exe"_s, system + u"\\WindowsPowerShell\\v1.0\\powershell.exe"_s};
}

Plan silentPlan(const QString& program, const QString& arguments, Kind kind, const Tools& tools, bool visible)
{
    const QString rest = arguments.trimmed();
    const auto withRest = [&rest](const QString& head) { return rest.isEmpty() ? head : head + u' ' + rest; };
    const auto plan = [](const QString& application, const QString& args) {
        return Plan {application, args, args.isEmpty() ? quoted(application) : quoted(application) + u' ' + args, true};
    };
    switch (kind) {
    case Kind::Console:
        return plan(program, rest);
    case Kind::Batch:
        if (tools.cmd.isEmpty())
            return {};
        // /s: the first and last quote of what follows /c are removed and
        // nothing else touched, so the script's own quotes stay.
        return plan(tools.cmd, u"/d /s /c \""_s + withRest(quoted(program)) + u'"');
    case Kind::PowerShell:
        if (tools.powershell.isEmpty())
            return {};
        return plan(tools.powershell,
            withRest((visible ? u"-NoProfile -ExecutionPolicy Bypass -File "_s
                              : u"-NoProfile -NonInteractive -ExecutionPolicy Bypass -File "_s)
                + quoted(program)));
    case Kind::Gui:
    case Kind::Other:
        break;
    }
    return {};
}

bool safeForCmd(const QString& words)
{
    return !words.contains(u'"') && !words.contains(u'%') && !words.contains(u'^') && !words.contains(u'&')
        && !words.contains(u'|') && !words.contains(u'<') && !words.contains(u'>') && !words.contains(u'!')
        && !words.contains(u'\n') && !words.contains(u'\r');
}

} // namespace ws::commandlaunch
