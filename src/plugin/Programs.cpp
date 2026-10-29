#include "Programs.h"

#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QMutex>
#include <QRegularExpression>
#include <QStandardPaths>
#include <QtEndian>

#include <windows.h>

#include <algorithm>
#include <chrono>
#include <iterator>
#include <map>
#include <memory>
#include <string>
#include <type_traits>

using namespace Qt::StringLiterals;

namespace ws::plugin::programs {

namespace {

constexpr int kMaxHops = 4; // venv → trampoline → interpreter is two; more is a loop
constexpr qsizetype kMaxTrampolinePath = 32 * 1024; // uv's MAX_PATH_LENGTH
constexpr qint64 kMaxVenvConfig = 64 * 1024; // a pyvenv.cfg is a few lines
constexpr QStringView kPythonHint = u"需要 Python 3：https://www.python.org/downloads/";
constexpr std::chrono::seconds kLookAgain {5}; // after a miss: it may have been installed since

struct Found {
    QString exe;
    QList<std::pair<QString, QString>> env; // a venv's __PYVENV_LAUNCHER__
    QString hint; // when exe is empty: what to install, or why the one found cannot run
    std::chrono::steady_clock::time_point at; // when it was looked up
    QString venvConfig; // a venv's pyvenv.cfg, and when it was written: a venv made again
    QDateTime venvConfigTime; // in its place may have another home
};

QMutex g_mutex;
std::map<Program, QString> g_overrides;
std::map<Program, Found> g_found; // looked up once; again when the file has gone, or a while after a miss

QString native(const QString& path)
{
    return QDir::toNativeSeparators(path);
}

bool inWindowsApps(const QString& path)
{
    // The Store's python.exe under WindowsApps is a stub that opens the
    // Store (or, with Python from the Store, an app-execution alias that
    // starts python as a child): not what a job allowing one process wants.
    return native(path).contains(u"\\WindowsApps\\"_s, Qt::CaseInsensitive);
}

// `name` in PATH's folders but the Store's: a python.exe after the Store's
// stub is found (uv's bin folder often comes after it).
QString onPath(const QString& name)
{
    QStringList folders;
    for (QString folder : qEnvironmentVariable("PATH").split(u';', Qt::SkipEmptyParts)) {
        folder.remove(u'"');
        if (!folder.trimmed().isEmpty() && !inWindowsApps(folder + u'\\'))
            folders.push_back(folder);
    }
    const QString found = folders.isEmpty() ? QString() : QStandardPaths::findExecutable(name, folders);
    return found.isEmpty() ? QString() : native(found);
}

// A registry key opened only to read. (QSettings opens the keys it reads
// with RegCreateKeyEx: it would add the ones that are missing, under other
// programs' keys, and in HKLM when run elevated.)
class RegistryKey {
public:
    RegistryKey(HKEY parent, const QString& path, REGSAM view)
    {
        if (!parent || ::RegOpenKeyExW(parent, reinterpret_cast<LPCWSTR>(path.utf16()), 0, KEY_READ | view, &m_key) != ERROR_SUCCESS)
            m_key = nullptr;
    }
    ~RegistryKey()
    {
        if (m_key)
            ::RegCloseKey(m_key);
    }
    RegistryKey(const RegistryKey&) = delete;
    RegistryKey& operator=(const RegistryKey&) = delete;

    HKEY get() const { return m_key; }

    QStringList subkeys() const
    {
        QStringList names;
        wchar_t name[512]; // documented: 255 characters at most (Windows 11 takes 256)
        for (DWORD i = 0; m_key; ++i) {
            DWORD length = DWORD(std::size(name));
            const LSTATUS status = ::RegEnumKeyExW(m_key, i, name, &length, nullptr, nullptr, nullptr, nullptr);
            if (status == ERROR_SUCCESS)
                names.push_back(QString::fromWCharArray(name, length));
            else if (status != ERROR_MORE_DATA) // a name longer still: skipped
                break; // ERROR_NO_MORE_ITEMS
        }
        return names;
    }

    // A string value; empty when there is none. An empty name is the
    // key's default value.
    QString text(const wchar_t* name) const
    {
        std::wstring value(MAX_PATH, L'\0');
        for (int attempt = 0; m_key && attempt < 3; ++attempt) {
            DWORD bytes = DWORD(value.size() * sizeof(wchar_t));
            const LSTATUS status = ::RegGetValueW(m_key, nullptr, name, RRF_RT_REG_SZ, nullptr, value.data(), &bytes);
            if (status == ERROR_SUCCESS)
                return QString::fromWCharArray(value.c_str());
            if (status != ERROR_MORE_DATA)
                break;
            value.assign(bytes / sizeof(wchar_t) + 1, L'\0');
        }
        return {};
    }

private:
    HKEY m_key = nullptr;
};

// The first run of digits and dots: "3.12.14", "3.13t", "3.11-32", "CPython3.12.14".
QVersionNumber versionIn(const QString& text)
{
    static const QRegularExpression digits(u"\\d+(?:\\.\\d+)*"_s);
    const QRegularExpressionMatch match = digits.match(text);
    return match.hasMatch() ? QVersionNumber::fromString(match.captured()) : QVersionNumber();
}

// The pyvenv.cfg that makes `exe` a venv's python.exe: one folder up (the
// venv, above Scripts), else beside it -- where getpath.py looks.
QString venvConfigOf(const QFileInfo& exe)
{
    const QString folder = exe.absolutePath();
    for (const QString& cfg : {QDir::cleanPath(folder + u"/../pyvenv.cfg"_s), folder + u"/pyvenv.cfg"_s}) {
        if (QFileInfo(cfg).isFile())
            return cfg;
    }
    return {};
}

bool isPythonCore(const QString& company)
{
    return company.compare(u"PythonCore"_s, Qt::CaseInsensitive) == 0;
}

// Every registration Win顺 may use: the user's, then the machine's (64-bit,
// then 32-bit installs for all users).
QList<RegisteredPython> registered()
{
    QList<RegisteredPython> all = registeredPythons(u"HKEY_CURRENT_USER\\Software\\Python"_s);
    all += registeredPythons(u"HKEY_LOCAL_MACHINE\\Software\\Python"_s);
    all += registeredPythons(u"HKEY_LOCAL_MACHINE\\Software\\Python"_s, RegistryView::Bits32);
    // Tests: WINSHUN_TEST_PYTHON_COMPANIES=Astral (a comma list; "none":
    // none) reads only those companies, so that the machine's own Python
    // does not hide the one under test (docs/pitfalls.md, "uv 装的 Python").
    if (const QString test = qEnvironmentVariable("WINSHUN_TEST_PYTHON_COMPANIES"); !test.isEmpty()) {
        const QStringList only = test.split(u',', Qt::SkipEmptyParts);
        all.removeIf([&](const RegisteredPython& r) { return !only.contains(r.company, Qt::CaseInsensitive); });
    }
    return all;
}

// The settings' choice when there is one, else the registry's
// registrations in rank, else python.exe on PATH (never a .bat a PATHEXT
// search would give): the first that resolves to an interpreter.
Found lookUpPython(const QString& chosen)
{
    Found found;
    QString why; // the first launcher found that led nowhere
    const auto take = [&](const QString& path) {
        if (path.isEmpty())
            return false;
        QString reason;
        const std::optional<Python> python = resolvePython(path, &reason);
        if (!python) {
            if (why.isEmpty() && QFileInfo::exists(path)) // a stale registration is no news
                why = reason;
            return false;
        }
        found.exe = python->exe;
        if (!python->venvLauncher.isEmpty()) {
            found.env = {{u"__PYVENV_LAUNCHER__"_s, python->venvLauncher}};
            found.venvConfig = venvConfigOf(QFileInfo(python->venvLauncher));
            found.venvConfigTime = QFileInfo(found.venvConfig).lastModified();
        }
        return true;
    };
    if (!chosen.isEmpty()) {
        if (!take(chosen))
            found.hint = why.isEmpty() ? u"找不到 %1"_s.arg(native(chosen)) : why;
        return found;
    }
    for (const RegisteredPython& r : ranked(registered())) {
        if (take(r.exe))
            return found;
    }
    if (take(onPath(u"python.exe"_s)))
        return found;
    found.hint = why.isEmpty() ? kPythonHint.toString() : u"需要 Python 3：%1"_s.arg(why);
    return found;
}

Found lookUp(Program program, const QString& chosen)
{
    if (program == Program::Python)
        return lookUpPython(chosen);
    Found found;
    if (!chosen.isEmpty()) {
        found.exe = chosen;
        return found;
    }
    switch (program) {
    // .exe names: a .cmd or .bat runs in cmd.exe, which starts the program
    // as a child.
    case Program::Pwsh: {
        found.exe = onPath(u"pwsh.exe"_s);
        if (found.exe.isEmpty()) {
            const QString fixed = qEnvironmentVariable("ProgramFiles") + u"\\PowerShell\\7\\pwsh.exe"_s;
            if (QFileInfo::exists(fixed))
                found.exe = fixed;
        }
        found.hint = u"需要 PowerShell 7：https://aka.ms/powershell"_s;
        break;
    }
    case Program::PowerShell: {
        const QString fixed = qEnvironmentVariable("SystemRoot") + u"\\System32\\WindowsPowerShell\\v1.0\\powershell.exe"_s;
        found.exe = QFileInfo::exists(fixed) ? fixed : onPath(u"powershell.exe"_s);
        found.hint = u"找不到 Windows PowerShell"_s;
        break;
    }
    case Program::Node: {
        found.exe = onPath(u"node.exe"_s);
        if (found.exe.isEmpty()) {
            const QString fixed = qEnvironmentVariable("ProgramFiles") + u"\\nodejs\\node.exe"_s;
            if (QFileInfo::exists(fixed))
                found.exe = fixed;
        }
        found.hint = u"需要 Node.js：https://nodejs.org/"_s;
        break;
    }
    case Program::Python:
    case Program::Exe:
        break;
    }
    return found;
}

bool stale(const Found& found)
{
    if (found.exe.isEmpty())
        return std::chrono::steady_clock::now() - found.at >= kLookAgain;
    return !QFileInfo::exists(found.exe)
        || (!found.venvConfig.isEmpty() && QFileInfo(found.venvConfig).lastModified() != found.venvConfigTime);
}

// What was found for `program`, looked up the first time, again when the
// file has gone since (uninstalled, or a folder `uv python upgrade`
// replaced) or its venv was made again, and a few seconds after a miss
// (the card said what to install; no restart wanted). g_mutex held.
const Found& foundFor(Program program)
{
    auto it = g_found.find(program);
    if (it != g_found.end() && stale(it->second)) {
        g_found.erase(it);
        it = g_found.end();
    }
    if (it == g_found.end()) {
        const auto chosen = g_overrides.find(program);
        it = g_found.emplace(program, lookUp(program, chosen == g_overrides.end() ? QString() : chosen->second)).first;
        it->second.at = std::chrono::steady_clock::now(); // a slow lookup is not stale on arrival
    }
    return it->second;
}

} // namespace

QString find(Program program, QString* hint)
{
    const QMutexLocker lock(&g_mutex);
    const Found& found = foundFor(program);
    if (found.exe.isEmpty() && hint)
        *hint = found.hint;
    return found.exe;
}

void setOverride(Program program, const QString& path)
{
    const QMutexLocker lock(&g_mutex);
    g_overrides[program] = path;
    g_found.erase(program);
}

std::optional<Launch> launchFor(const Runtime& runtime, const QString& pluginDir, QString* error)
{
    Launch launch;
    const QDir dir(pluginDir);
    launch.cwd = QDir::toNativeSeparators(runtime.cwd.isEmpty() ? dir.absolutePath() : dir.absoluteFilePath(runtime.cwd));
    const QString script = runtime.script.isEmpty() ? QString() : QDir::toNativeSeparators(dir.absoluteFilePath(runtime.script));
    if (runtime.program == Program::Exe) {
        launch.exe = QDir::toNativeSeparators(dir.absoluteFilePath(runtime.programPath));
        if (!QFileInfo::exists(launch.exe)) {
            if (error)
                *error = u"插件里没有 %1"_s.arg(runtime.programPath);
            return std::nullopt;
        }
        if (!script.isEmpty())
            launch.args.push_back(script);
    } else {
        Found found;
        {
            const QMutexLocker lock(&g_mutex);
            found = foundFor(runtime.program);
        }
        launch.exe = found.exe;
        if (launch.exe.isEmpty()) {
            if (error)
                *error = found.hint;
            return std::nullopt;
        }
        if (!QFileInfo::exists(script)) {
            if (error)
                *error = u"插件里没有 %1"_s.arg(runtime.script);
            return std::nullopt;
        }
        switch (runtime.program) {
        case Program::Python:
            // -u: lines reach the pipe as they are printed (a pipe is block
            // buffered otherwise, and a resident plugin's answers would
            // sit in the buffer). -X utf8: UTF-8 stdio whatever the code page.
            launch.args = {u"-u"_s, u"-X"_s, u"utf8"_s, script};
            launch.env = {{u"PYTHONIOENCODING"_s, u"utf-8"_s}, {u"PYTHONUTF8"_s, u"1"_s}, {u"PYTHONDONTWRITEBYTECODE"_s, u"1"_s},
                {u"PYTHONUNBUFFERED"_s, u"1"_s}};
            launch.env += found.env;
            break;
        case Program::Pwsh:
        case Program::PowerShell:
            launch.args = {u"-NoProfile"_s, u"-NonInteractive"_s, u"-ExecutionPolicy"_s, u"Bypass"_s, u"-File"_s, script};
            break;
        case Program::Node:
            launch.args = {script};
            break;
        case Program::Exe:
            break;
        }
    }
    launch.args += runtime.args;
    return launch;
}

// ---- finding Python ---------------------------------------------------------

std::optional<Python> resolvePython(const QString& path, QString* why)
{
    const auto fail = [&](const QString& text) -> std::optional<Python> {
        if (why)
            *why = text;
        return std::nullopt;
    };
    Python python;
    QString exe = native(path);
    for (int hop = 0; hop < kMaxHops; ++hop) {
        const QFileInfo info(exe);
        if (info.isDir())
            return fail(u"%1 是文件夹，不是 python.exe"_s.arg(exe));
        if (!info.isFile())
            return fail(u"%1 不存在"_s.arg(exe));
        if (info.suffix().compare(u"exe"_s, Qt::CaseInsensitive) != 0)
            return fail(u"%1 不是 exe，是转一手的脚本"_s.arg(exe));
        if (inWindowsApps(exe))
            return fail(u"%1 是 Microsoft Store 的转接程序"_s.arg(exe));
        if (info.fileName().compare(u"py.exe"_s, Qt::CaseInsensitive) == 0 || info.fileName().compare(u"pyw.exe"_s, Qt::CaseInsensitive) == 0)
            return fail(u"%1 是 Python 启动器（py），不是 Python 本身"_s.arg(exe));

        // A venv's python.exe (CPython's venv launcher, or uv's trampoline)
        // starts home\python.exe as a child: start that one here, telling it
        // which venv it is, as the launcher would. The first venv counts.
        if (const QString cfg = venvConfigOf(info); !cfg.isEmpty()) {
            QFile file(cfg);
            if (!file.open(QIODevice::ReadOnly))
                return fail(u"%1 属于一个虚拟环境，但读不了它的 %2"_s.arg(exe, native(cfg)));
            QString home = venvHome(file.read(kMaxVenvConfig));
            if (home.isEmpty())
                return fail(u"%1 属于一个虚拟环境，但它的 %2 里没写 home"_s.arg(exe, native(cfg)));
            if (QDir::isRelativePath(home))
                home = QFileInfo(cfg).absoluteDir().filePath(home);
            const QString base = native(QDir(home).filePath(u"python.exe"_s));
            if (!QFileInfo(base).isFile())
                return fail(u"%1 属于一个虚拟环境，它的 home 里没有 python.exe（%2）"_s.arg(exe, base));
            if (python.venvLauncher.isEmpty())
                python.venvLauncher = exe;
            exe = base;
            continue;
        }

        // uv's python.exe in its bin folder: a proxy that starts the
        // managed interpreter as a child.
        if (const std::optional<UvTrampoline> trampoline = uvTrampoline(exe)) {
            if (trampoline->kind == UvTrampoline::Kind::Script)
                return fail(u"%1 是 uv 给脚本做的启动器，不是 Python"_s.arg(exe));
            if (trampoline->kind != UvTrampoline::Kind::Python)
                return fail(u"%1 是 uv 的启动器，但这一种 Win顺 不认得"_s.arg(exe));
            QString target = trampoline->target;
            if (QDir::isRelativePath(target))
                target = QDir::cleanPath(info.absoluteDir().filePath(target));
            target = native(target);
            if (!QFileInfo(target).isFile())
                return fail(u"%1 是 uv 的启动器，它指向的 %2 不在了（uv python install 重装一下）"_s.arg(exe, target));
            exe = target;
            continue;
        }

        python.exe = exe;
        return python;
    }
    return fail(u"%1 一层套一层的启动器太多"_s.arg(native(path)));
}

QString venvHome(QByteArrayView cfg)
{
    // key = value lines, the key in any case, as getpath.py reads them.
    QString text = QString::fromUtf8(cfg);
    if (text.startsWith(QChar(0xFEFF)))
        text.remove(0, 1);
    for (const QStringView line : QStringView(text).tokenize(u'\n')) {
        const qsizetype equals = line.indexOf(u'=');
        if (equals > 0 && line.left(equals).trimmed().compare(u"home", Qt::CaseInsensitive) == 0)
            return line.mid(equals + 1).trimmed().toString();
    }
    return {};
}

std::optional<UvTrampoline> uvTrampoline(const QString& exe)
{
    // Mapped as data (and as an image, for the resources): no code of it
    // runs, its imports are not loaded.
    const std::wstring path = native(exe).toStdWString();
    const std::unique_ptr<std::remove_pointer_t<HMODULE>, decltype(&::FreeLibrary)> module(
        ::LoadLibraryExW(path.c_str(), nullptr, LOAD_LIBRARY_AS_DATAFILE | LOAD_LIBRARY_AS_IMAGE_RESOURCE), &::FreeLibrary);
    if (module) {
        const auto resource = [&](const wchar_t* name) -> QByteArray {
            const HRSRC found = ::FindResourceW(module.get(), name, MAKEINTRESOURCEW(10)); // RT_RCDATA
            const DWORD size = found ? ::SizeofResource(module.get(), found) : 0;
            const HGLOBAL loaded = size ? ::LoadResource(module.get(), found) : nullptr;
            const void* data = loaded ? ::LockResource(loaded) : nullptr;
            return data ? QByteArray(static_cast<const char*>(data), static_cast<qsizetype>(size)) : QByteArray();
        };
        const QByteArray kind = resource(L"UV_TRAMPOLINE_KIND");
        const QByteArray target = resource(L"UV_PYTHON_PATH");
        if (!kind.isEmpty() && !target.isEmpty()) {
            if (target.size() > kMaxTrampolinePath) // no path is that long
                return UvTrampoline {};
            const auto byte = static_cast<UvTrampoline::Kind>(static_cast<quint8>(kind.front())); // any quint8 is a Kind
            const bool known = byte == UvTrampoline::Kind::Python || byte == UvTrampoline::Kind::Script;
            return UvTrampoline {known ? byte : UvTrampoline::Kind::Unknown, QString::fromUtf8(target)};
        }
    }
    // uv up to 0.9.0 appended the path to the file instead.
    QFile file(exe);
    if (!file.open(QIODevice::ReadOnly))
        return std::nullopt;
    const qint64 want = std::min<qint64>(file.size(), kMaxTrampolinePath + 8);
    if (want < 8 || !file.seek(file.size() - want))
        return std::nullopt;
    return uvTrampolineTail(file.read(want));
}

std::optional<UvTrampoline> uvTrampolineTail(QByteArrayView tail)
{
    if (tail.size() < 8)
        return std::nullopt;
    const QByteArrayView magic = tail.last(4);
    UvTrampoline::Kind kind = UvTrampoline::Kind::Unknown;
    if (magic == QByteArrayView("UVPY"))
        kind = UvTrampoline::Kind::Python;
    else if (magic == QByteArrayView("UVSC"))
        kind = UvTrampoline::Kind::Script;
    else
        return std::nullopt;
    const auto length = static_cast<qsizetype>(qFromLittleEndian<quint32>(tail.data() + tail.size() - 8));
    if (length == 0 || length > kMaxTrampolinePath || length > tail.size() - 8)
        return std::nullopt;
    return UvTrampoline {kind, QString::fromUtf8(tail.sliced(tail.size() - 8 - length, length))};
}

QList<RegisteredPython> registeredPythons(const QString& root, RegistryView view)
{
    QList<RegisteredPython> found;
    HKEY hive = nullptr;
    QString path;
    for (const auto& [name, key] : {std::pair {u"HKEY_CURRENT_USER\\"_s, HKEY_CURRENT_USER}, std::pair {u"HKEY_LOCAL_MACHINE\\"_s, HKEY_LOCAL_MACHINE}}) {
        if (root.startsWith(name, Qt::CaseInsensitive)) {
            hive = key;
            path = root.mid(name.size());
        }
    }
    if (!hive)
        return found;
    const REGSAM sam = view == RegistryView::Bits32 ? KEY_WOW64_32KEY : 0;
    const RegistryKey python(hive, path, sam);
    for (const QString& company : python.subkeys()) {
        if (company.compare(u"PyLauncher"_s, Qt::CaseInsensitive) == 0)
            continue;
        const RegistryKey companyKey(python.get(), company, sam);
        for (const QString& tag : companyKey.subkeys()) {
            const RegistryKey entry(companyKey.get(), tag, sam);
            const RegistryKey install(entry.get(), u"InstallPath"_s, sam);
            RegisteredPython r {company, tag, {}, install.text(L"ExecutablePath")};
            // Without ExecutablePath an entry runs nothing (PEP 514), but
            // python.org's older installers wrote only the folder.
            if (r.exe.isEmpty() && isPythonCore(company)) {
                if (const QString folder = install.text(L""); !folder.isEmpty())
                    r.exe = QDir(folder).filePath(u"python.exe"_s);
            }
            if (r.exe.isEmpty())
                continue;
            r.exe = native(r.exe);
            // SysVersion is the language's (3.12); Version, the
            // distribution's, says more when it is the same (3.12.14), but
            // Anaconda's is its own (2022.10).
            for (const QString& text : {entry.text(L"SysVersion"), entry.text(L"Version"), tag}) {
                const QVersionNumber version = versionIn(text);
                if (r.version.isNull() || (r.version.isPrefixOf(version) && version.segmentCount() > r.version.segmentCount()))
                    r.version = version;
            }
            found.push_back(std::move(r));
        }
    }
    return found;
}

QList<RegisteredPython> ranked(QList<RegisteredPython> entries)
{
    entries.removeIf([](const RegisteredPython& r) { return !r.version.isNull() && r.version.majorVersion() < 3; });
    // Stable: on a tie the user's registration stays before the machine's.
    std::stable_sort(entries.begin(), entries.end(), [](const RegisteredPython& a, const RegisteredPython& b) {
        if (isPythonCore(a.company) != isPythonCore(b.company))
            return isPythonCore(a.company);
        return a.version > b.version;
    });
    return entries;
}

} // namespace ws::plugin::programs
