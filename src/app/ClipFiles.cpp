#include "ClipFiles.h"

#include "PasteFile.h"

#include <QDateTime>
#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QUrl>

#include <windows.h>

#include <algorithm>

using namespace Qt::StringLiterals;

namespace ws::clipfiles {

namespace pf = pastefile;

namespace {

// "2026-10-29 151230": when it was copied, as 粘贴为文件 names by the time of the paste.
std::wstring stampOf(qint64 ms)
{
    const QDateTime when = QDateTime::fromMSecsSinceEpoch(ms);
    return pf::stamp(when.date().year(), when.date().month(), when.date().day(), when.time().hour(),
        when.time().minute(), when.time().second());
}

// The words as their file holds them (pastefile::Encoding), as the stub
// writes them (src/stub/PasteAsFile.cpp).
QByteArray encoded(const pf::TextFile& file)
{
    switch (file.encoding) {
    case pf::Encoding::Utf8:
        break;
    case pf::Encoding::Utf8Bom:
        return "\xEF\xBB\xBF" + QByteArray::fromStdString(pf::utf8(file.text));
    case pf::Encoding::Utf16Bom:
        return QByteArray("\xFF\xFE", 2)
            + QByteArray(reinterpret_cast<const char*>(file.text.data()),
                static_cast<qsizetype>(file.text.size() * sizeof(wchar_t)));
    case pf::Encoding::OemOrUtf8:
        if (file.text.size() < 0x7FFFFFFF) {
            const int length = static_cast<int>(file.text.size());
            BOOL lost = FALSE;
            const int n = ::WideCharToMultiByte(CP_OEMCP, WC_NO_BEST_FIT_CHARS, file.text.data(), length, nullptr, 0,
                nullptr, &lost);
            if (n > 0 && !lost) {
                QByteArray bytes(n, '\0');
                ::WideCharToMultiByte(CP_OEMCP, WC_NO_BEST_FIT_CHARS, file.text.data(), length, bytes.data(), n,
                    nullptr, nullptr);
                return bytes;
            }
        }
        break;
    }
    return QByteArray::fromStdString(pf::utf8(file.text));
}

Planned words(const QString& text, const pf::Words& names, qint64 created)
{
    const std::wstring w = text.toStdWString();
    const pf::Kind kind = pf::textKind(w, false);
    return {QString::fromStdWString(pf::baseName(kind, w, names, stampOf(created))),
        QString::fromStdWString(std::wstring(pf::extension(kind))), encoded(pf::textFile(kind, w)), {}};
}

// An Internet shortcut, which opens the page; named by its address.
Planned link(const QString& text, const pf::Words& names, qint64 created)
{
    QString address = text.trimmed();
    if (address.startsWith(u"www."_s, Qt::CaseInsensitive))
        address.prepend(u"https://"_s);
    const QUrl url = QUrl::fromUserInput(address);
    if (!url.isValid())
        return words(text, names, created);
    QString name = url.host() + url.path();
    if (name.startsWith(u"www."_s, Qt::CaseInsensitive))
        name.remove(0, 4);
    QString base = QString::fromStdWString(pf::cleanName(name.toStdWString()));
    if (base.isEmpty())
        base = QString::fromStdWString(names.text + L' ' + stampOf(created));
    // ASCII only (percent-encoded): Windows reads the [InternetShortcut] section in the ANSI code page.
    return {base, u".url"_s, "[InternetShortcut]\r\nURL=" + url.toEncoded() + "\r\n", {}};
}

bool isText(const Clip* clip)
{
    return clip->kind == ClipKind::Text || clip->kind == ClipKind::Link || clip->kind == ClipKind::Path;
}

// `from` and all in it, to `to` (not there yet). Links to folders are made
// folders, not followed.
bool copyFolder(const QString& from, const QString& to)
{
    if (!QDir().mkdir(to))
        return false;
    const QDir source(from);
    QDirIterator it(from, QDir::AllEntries | QDir::Hidden | QDir::System | QDir::NoDotAndDotDot,
        QDirIterator::Subdirectories);
    while (it.hasNext()) {
        const QFileInfo entry = it.nextFileInfo();
        const QString target = to + u'/' + source.relativeFilePath(entry.filePath());
        if (entry.isDir() ? !QDir().mkpath(target) : !QFile::copy(entry.filePath(), target))
            return false;
    }
    return true;
}

} // namespace

Plan plan(const ClipStore& store, const std::vector<const Clip*>& clips, bool english, bool merged,
    const QString& separator)
{
    const pf::Words names = english ? pf::englishWords() : pf::Words {};
    Plan result;
    if (merged) {
        QStringList texts;
        qint64 newest = 0;
        for (const Clip* clip : clips) {
            if (!clip || !isText(clip))
                continue;
            texts.append(store.payload(clip->id).text);
            newest = std::max(newest, clip->created);
        }
        if (!texts.isEmpty())
            result.files.push_back(words(texts.join(separator), names, newest));
        return result;
    }
    for (const Clip* clip : clips) {
        if (!clip)
            continue;
        switch (clip->kind) {
        case ClipKind::Text:
        case ClipKind::Path:
            result.files.push_back(words(store.payload(clip->id).text, names, clip->created));
            break;
        case ClipKind::Link:
            result.files.push_back(link(store.payload(clip->id).text, names, clip->created));
            break;
        case ClipKind::Image: {
            const QString original = store.payload(clip->id).imagePath; // waits for a just-captured image
            if (original.isEmpty() || !QFile::exists(original)) {
                ++result.missing;
                break;
            }
            result.files.push_back({QString::fromStdWString(names.picture + L' ' + stampOf(clip->created)),
                u'.' + clipImageExtension(clip->imageFormat), {}, original});
            break;
        }
        case ClipKind::Files:
            // Whether each is still there, and a folder, is looked at as it is
            // written (save): a network path gone away can hang.
            for (const QString& path : clip->files()) {
                const QFileInfo file(path); // names only, the disk not asked
                Planned copy {file.completeBaseName(), {}, {}, QDir::cleanPath(QDir::fromNativeSeparators(path)), true};
                if (!file.suffix().isEmpty())
                    copy.extension = u'.' + file.suffix();
                if (file.fileName().isEmpty()) // a drive's root: "D"
                    copy.base = QString::fromStdWString(pf::cleanName(path.toStdWString()));
                result.files.push_back(std::move(copy));
            }
            break;
        }
    }
    return result;
}

Saved save(const Plan& plan, const QString& folder)
{
    Saved saved;
    QString into = QDir::fromNativeSeparators(folder);
    while (into.size() > 3 && into.endsWith(u'/'))
        into.chop(1);
    const QString prefix = into.endsWith(u'/') ? into : into + u'/';
    if (!QDir().mkpath(into)) { // a drag's folder of its own: made now
        saved.failed = static_cast<int>(plan.files.size());
        return saved;
    }
    for (const Planned& file : plan.files) {
        const QFileInfo source(file.source);
        if (file.original && !source.exists()) {
            ++saved.missing;
            continue;
        }
        const bool folderCopy = file.original && source.isDir();
        // Never into itself: the copy would go on copying what it made.
        if (folderCopy
            && prefix.startsWith(file.source.endsWith(u'/') ? file.source : file.source + u'/', Qt::CaseInsensitive)) {
            ++saved.failed;
            continue;
        }
        // A folder's name is whole: " (2)" after "v1.2", not inside it.
        const QString extension = folderCopy ? QString() : file.extension;
        const QString base = QString::fromStdWString(pf::fitName((folderCopy ? file.base + file.extension : file.base)
                .toStdWString(), extension.toStdWString(), static_cast<std::size_t>(into.size())));
        QString made;
        for (int n = 1; n < 1000; ++n) {
            const QString path
                = prefix + base + (n > 1 ? u" ("_s + QString::number(n) + u')' : QString()) + extension;
            if (QFileInfo::exists(path))
                continue;
            bool ok = false;
            if (file.source.isEmpty()) {
                QFile out(path);
                const bool opened = out.open(QIODevice::WriteOnly | QIODevice::NewOnly);
                ok = opened && out.write(file.bytes) == file.bytes.size();
                if (opened) {
                    ok = out.flush() && ok;
                    out.close();
                    if (!ok)
                        out.remove(); // the half written just now
                }
            } else if (folderCopy) {
                ok = copyFolder(file.source, path);
                if (!ok && QFileInfo::exists(path))
                    QDir(path).removeRecursively(); // the half made just now, never the user's
            } else {
                ok = QFile::copy(file.source, path);
            }
            if (ok)
                made = QDir::toNativeSeparators(path);
            break;
        }
        if (made.isEmpty())
            ++saved.failed;
        else
            saved.paths.append(made);
    }
    return saved;
}

Export images(const ClipStore& store, const std::vector<const Clip*>& clips, const QString& folder, const QString& name)
{
    if (clips.empty()
        || std::ranges::any_of(clips, [](const Clip* clip) { return !clip || clip->kind != ClipKind::Image; }))
        return {{}, Error::NotImages};
    QStringList paths;
    for (const Clip* clip : clips) {
        const QString original = store.payload(clip->id).imagePath; // waits for a just-captured image
        // IDs also distinguish pictures copied in the same second when Explorer
        // flattens all the source folders into the destination.
        const QString directory = folder + u'/' + QString::number(clip->id);
        const QString path = directory + u'/' + name + u' '
            + QDateTime::fromMSecsSinceEpoch(clip->created).toString(u"yyyy-MM-dd HH.mm.ss"_s) + u' '
            + QString::number(clip->id) + u'.' + clipImageExtension(clip->imageFormat);
        if (!QFile::exists(path) && (!QDir().mkpath(directory) || !QFile::copy(original, path)))
            return {{}, Error::WriteFailed};
        paths.append(QDir::toNativeSeparators(path));
    }
    return {paths, Error::None};
}

QString fileNames(const QStringList& paths)
{
    QStringList names;
    for (const QString& path : paths) {
        // Quotes and newlines cannot name Windows files. Do not turn malformed
        // imported entries into extra selections in another program's dialog.
        if (path.isEmpty() || path.contains(u'"') || path.contains(u'\r') || path.contains(u'\n')
            || path.contains(QChar()))
            return {};
        names.append(u'"' + QDir::toNativeSeparators(path) + u'"');
    }
    return names.join(u' ');
}

} // namespace ws::clipfiles
