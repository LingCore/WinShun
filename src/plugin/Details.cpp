#include "Details.h"

#include "Win32Util.h"

#include <QJsonObject>

#include <string>

#include <windows.h>

using namespace Qt::StringLiterals;

namespace ws::plugin {

namespace {

// CON, PRN, AUX, NUL, COM1–9, LPT1–9 (and the superscript digits Windows
// also takes), CONIN$, CONOUT$: a device under any folder and with any
// extension on Windows 10.
bool reservedName(QStringView component)
{
    QStringView base = component.left(component.indexOf(u'.') < 0 ? component.size() : component.indexOf(u'.'));
    while (base.endsWith(u' '))
        base.chop(1);
    for (const QStringView name : {u"CON", u"PRN", u"AUX", u"NUL", u"CONIN$", u"CONOUT$"}) {
        if (base.compare(name, Qt::CaseInsensitive) == 0)
            return true;
    }
    if (base.size() != 4)
        return false;
    const QStringView stem = base.left(3);
    if (stem.compare(u"COM", Qt::CaseInsensitive) != 0 && stem.compare(u"LPT", Qt::CaseInsensitive) != 0)
        return false;
    const char16_t digit = base[3].unicode();
    return (digit >= u'1' && digit <= u'9') || digit == u'\u00B9' || digit == u'\u00B2' || digit == u'\u00B3';
}

} // namespace

bool plainFilePath(QStringView text)
{
    if (text.size() < 4 || text.size() > 32767)
        return false;
    QString path = text.toString();
    path.replace(u'/', u'\\');
    qsizetype rest = 0; // where the steps after the root begin
    if (path[0].unicode() < 128 && path[0].isLetter() && path[1] == u':' && path[2] == u'\\') {
        rest = 3;
    } else if (path.startsWith(u"\\\\"_s)) {
        const qsizetype server = path.indexOf(u'\\', 2);
        if (server <= 2)
            return false;
        const QStringView host = QStringView(path).mid(2, server - 2);
        if (host == u"?" || host == u"." || host.contains(u':'))
            return false; // \\?\…, \\.\… (devices, raw volumes)
        const qsizetype share = path.indexOf(u'\\', server + 1);
        if (share <= server + 1)
            return false;
        rest = share + 1;
    } else {
        return false;
    }
    if (rest >= path.size() || path.endsWith(u'\\'))
        return false;
    for (qsizetype i = 0; i < path.size(); ++i) {
        const char16_t c = path[i].unicode();
        if (c < 32 || c == u'*' || c == u'?' || c == u'"' || c == u'<' || c == u'>' || c == u'|' || (c == u':' && i != 1))
            return false;
    }
    for (const QStringView step : QStringView(path).mid(rest).split(u'\\')) {
        // Windows drops a step's trailing dots and spaces: "..." is "..",
        // "a. " is "a"; such a step means what it does not say.
        if (step.isEmpty() || step.endsWith(u'.') || step.endsWith(u' ') || reservedName(step))
            return false;
    }
    return true;
}

QString rowFile(const PluginRow& row)
{
    if (row.openKind != PluginRow::OpenKind::Path || !row.openArgs.isEmpty() || !row.openCwd.isEmpty()
        || !plainFilePath(row.openTarget))
        return {};
    return QString(row.openTarget).replace(u'/', u'\\');
}

namespace {

// The path a handle's file or folder really has ("C:\…", without "\\?\").
QString finalPath(HANDLE handle)
{
    std::wstring buffer(512, L'\0');
    for (;;) {
        const DWORD n = ::GetFinalPathNameByHandleW(handle, buffer.data(), static_cast<DWORD>(buffer.size()),
            FILE_NAME_NORMALIZED | VOLUME_NAME_DOS);
        if (n == 0)
            return {};
        if (n < buffer.size()) {
            buffer.resize(n);
            break;
        }
        buffer.resize(n + 1);
    }
    QString path = QString::fromStdWString(buffer);
    if (path.startsWith(u"\\\\?\\UNC\\"_s))
        return {}; // on a share: not the plugin's own folder
    if (path.startsWith(u"\\\\?\\"_s))
        path.remove(0, 4);
    return path;
}

} // namespace

QString fileInFolder(const QString& path, const QString& folder)
{
    const QString native = QString(path).replace(u'/', u'\\');
    if (!plainFilePath(native) || native.startsWith(u"\\\\"_s) || folder.isEmpty())
        return {};
    // The folder: followed to where it leads (a data folder moved by a
    // junction is still the plugin's own).
    const win32::UniqueHandle dir(::CreateFileW(QString(folder).replace(u'/', u'\\').toStdWString().c_str(),
        FILE_READ_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
        FILE_FLAG_BACKUP_SEMANTICS, nullptr));
    if (!dir.valid())
        return {};
    QString root = finalPath(dir.get());
    if (root.isEmpty())
        return {};
    if (!root.endsWith(u'\\'))
        root += u'\\';
    // The file itself is not followed: a link in the folder to a file
    // elsewhere is refused, not read.
    const win32::UniqueHandle file(::CreateFileW(native.toStdWString().c_str(), FILE_READ_ATTRIBUTES,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
        FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr));
    BY_HANDLE_FILE_INFORMATION info {};
    if (!file.valid() || !::GetFileInformationByHandle(file.get(), &info)
        || (info.dwFileAttributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT | FILE_ATTRIBUTE_DEVICE))
        || info.nNumberOfLinks != 1 || ::GetFileType(file.get()) != FILE_TYPE_DISK)
        return {};
    // Where it really is (the folders on the way followed): in the folder.
    const QString real = finalPath(file.get());
    if (real.size() <= root.size() || !real.startsWith(root, Qt::CaseInsensitive))
        return {};
    return real;
}

DetailsAnswer parseDetails(const QJsonValue& result, const QString& dataDir)
{
    DetailsAnswer answer;
    const QJsonObject o = result.toObject();
    if (const QJsonValue file = o.value(u"file"_s); file.isString()) {
        const QString real = fileInFolder(file.toString(), dataDir);
        if (!real.isEmpty()) {
            answer.kind = DetailsAnswer::Kind::File;
            answer.file = real;
            return answer;
        }
    }
    if (const QJsonValue text = o.value(u"text"_s); text.isString()) {
        answer.kind = DetailsAnswer::Kind::Text;
        answer.text = text.toString();
        if (answer.text.size() > kMaxDetailsText) {
            qsizetype n = kMaxDetailsText;
            if (answer.text[n - 1].isHighSurrogate())
                --n; // never half a character
            answer.text.truncate(n);
            answer.cut = true;
        }
    }
    return answer;
}

} // namespace ws::plugin
