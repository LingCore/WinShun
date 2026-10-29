#include "OpenPolicy.h"

#include "Win32Util.h"

#include <QDir>
#include <QUrl>

#include <windows.h>

#include <shlwapi.h> // AssocIsDangerous

using namespace Qt::StringLiterals;

namespace ws::plugin {

const QStringList& dangerousOpenExtensions()
{
    // On top of %PATHEXT% and AssocIsDangerous: the shell tricks that run code
    // or change the system although they are not in PATHEXT -- shortcuts and
    // shell links (.lnk/.url/.appref-ms), the files that open a settings page
    // or run a shell command (.settingcontent-ms/.library-ms/.search-ms/.scf),
    // scripting and installer types (.hta/.reg/.msi/.jar/.py...), screen savers
    // and the PIF.
    static const QStringList kList = {
        u".exe"_s, u".com"_s, u".scr"_s, u".pif"_s, u".bat"_s, u".cmd"_s,
        u".msi"_s, u".msp"_s, u".mst"_s, u".cpl"_s, u".msc"_s, u".reg"_s,
        u".hta"_s, u".vbs"_s, u".vbe"_s, u".js"_s, u".jse"_s, u".ws"_s,
        u".wsf"_s, u".wsc"_s, u".wsh"_s, u".ps1"_s, u".psc1"_s, u".psm1"_s,
        u".lnk"_s, u".url"_s, u".scf"_s, u".appref-ms"_s, u".settingcontent-ms"_s,
        u".library-ms"_s, u".search-ms"_s, u".gadget"_s, u".application"_s,
        u".jar"_s, u".py"_s, u".pyw"_s, u".pyz"_s, u".chm"_s, u".sh"_s,
    };
    return kList;
}

namespace {

// The extension of the last component, lower case with its dot, after
// Windows' own trimming of trailing dots and spaces. Empty when there is none.
QString extensionOf(QStringView path)
{
    qsizetype slash = -1;
    for (qsizetype i = path.size() - 1; i >= 0; --i) {
        if (path[i] == u'\\' || path[i] == u'/') {
            slash = i;
            break;
        }
    }
    QStringView name = path.mid(slash + 1);
    // Windows opens "x.exe." and "x.exe " as "x.exe".
    while (!name.isEmpty() && (name.back() == u'.' || name.back() == u' '))
        name.chop(1);
    const qsizetype dot = name.lastIndexOf(u'.');
    if (dot <= 0) // no dot, or a leading dot (".gitignore"): no extension to judge
        return {};
    return name.mid(dot).toString().toLower();
}

bool isAbsoluteLocal(QStringView p)
{
    if (p.size() >= 2 && p[0] == u'\\' && p[1] == u'\\') // UNC, or the \\?\ prefix
        return true;
    return p.size() >= 3 && p[1] == u':' && (p[2] == u'\\' || p[2] == u'/')
        && ((p[0] >= u'A' && p[0] <= u'Z') || (p[0] >= u'a' && p[0] <= u'z'));
}

// A colon that is not the drive letter's: an alternate data stream
// ("notes.txt:payload.exe"), which can carry and run an executable.
bool hasDataStream(QStringView p)
{
    const qsizetype start = (p.size() >= 2 && p[1] == u':') ? 2 : 0; // skip "C:"
    return p.indexOf(u':', start) >= 0;
}

} // namespace

OpenRuling ruleOpen(const Open& open, bool hasProcesses, const OpenProbe& probe)
{
    OpenRuling r;
    r.target = open.target;
    if (open.kind == Open::Kind::None || hasProcesses)
        return r; // allowed

    if (open.kind == Open::Kind::Url) {
        const QString scheme = QUrl(open.target, QUrl::TolerantMode).scheme().toLower();
        if (scheme != u"http" && scheme != u"https" && scheme != u"mailto")
            r.gate = OpenGate::Scheme;
        return r;
    }

    // A path.
    if (!open.args.isEmpty() || !open.cwd.isEmpty()) {
        r.gate = OpenGate::Args;
        return r;
    }
    QString t = open.target;
    if (t.startsWith(u"file:"_s, Qt::CaseInsensitive)) {
        const QString local = QUrl(t, QUrl::TolerantMode).toLocalFile();
        if (!local.isEmpty())
            t = local;
    }
    if (probe.expandEnv)
        t = probe.expandEnv(t);
    t = QDir::toNativeSeparators(t);
    r.target = t;
    if (hasDataStream(t) || !isAbsoluteLocal(t)) {
        r.gate = OpenGate::Program; // cannot be vouched for
        return r;
    }
    if (probe.isDirectory && probe.isDirectory(t))
        return r; // a folder is always fine
    const QString ext = extensionOf(t);
    const bool dangerous = ext.isEmpty() // a bare or extension-less name: ShellExecute may find a program
        || probe.pathExt.contains(ext)
        || dangerousOpenExtensions().contains(ext)
        || (probe.dangerousAssoc && probe.dangerousAssoc(ext));
    if (dangerous)
        r.gate = OpenGate::Program;
    return r;
}

OpenProbe systemProbe()
{
    OpenProbe probe;
    probe.expandEnv
        = [](const QString& s) { return QString::fromStdWString(win32::expandEnvironment(s.toStdWString())); };
    probe.isDirectory = [](const QString& s) {
        const DWORD a = ::GetFileAttributesW(win32::longPath(QDir::toNativeSeparators(s).toStdWString()).c_str());
        return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY) != 0;
    };
    probe.dangerousAssoc = [](QStringView ext) {
        const std::wstring w = ext.toString().toStdWString();
        return ::AssocIsDangerous(w.c_str()) != FALSE;
    };
    // %PATHEXT% (".COM;.EXE;...") -> {".com", ".exe", ...}; a default when unset.
    const QString raw = QString::fromStdWString(win32::expandEnvironment(L"%PATHEXT%"));
    const QStringList parts = raw.isEmpty() || raw == u"%PATHEXT%"
        ? QStringList {u".com"_s, u".exe"_s, u".bat"_s, u".cmd"_s}
        : raw.toLower().split(u';', Qt::SkipEmptyParts);
    for (QString e : parts) {
        e = e.trimmed();
        if (!e.isEmpty())
            probe.pathExt.push_back(e.startsWith(u'.') ? e : u'.' + e);
    }
    return probe;
}

} // namespace ws::plugin
