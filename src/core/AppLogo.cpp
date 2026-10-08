#include "AppLogo.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QUrl>
#include <QXmlStreamReader>

#include <windows.h>
// After windows.h; ole2.h first, as WIN32_LEAN_AND_MEAN keeps it out of windows.h.
#include <ole2.h>

#include <shlobj.h>
#include <shlwapi.h>
#include <shobjidl.h>
#include <wrl/client.h>

#include <algorithm>
#include <iterator>
#include <tuple>
#include <vector>

using namespace Qt::StringLiterals;

namespace ws {

namespace {

// System.AppUserModel.* properties, as in AppCatalog.cpp.
constexpr GUID kAppUserModel {0x9F4C2855, 0x9F79, 0x4B39, {0xA8, 0xD0, 0xE1, 0xD4, 0x2D, 0xE1, 0xD5, 0xF3}};
constexpr PROPERTYKEY kPackageInstallPath {kAppUserModel, 15};
constexpr PROPERTYKEY kPackageFullName {kAppUserModel, 21};

// Assets far larger than asked for are not worth decoding.
constexpr int kMaxTargetSizeFactor = 8;

// One asset file and its qualifiers: "Square44x44Logo.targetsize-48_altform-unplated.png".
struct Candidate {
    QString name;
    QHash<QString, QString> qualifiers; // lower-case names
    int targetSize = 0; // 0: none
    int scale = 0;
    QString theme;
    QString altform;
    QString contrast;

    bool highContrast() const { return !contrast.isEmpty() && contrast.compare(u"standard", Qt::CaseInsensitive) != 0; }
    bool unplated() const
    {
        return altform.compare(u"unplated", Qt::CaseInsensitive) == 0
            || altform.compare(u"lightunplated", Qt::CaseInsensitive) == 0;
    }
};

// Qualifiers that pick among sizes and looks; the others (language,
// configuration, ...) come from the context Windows resolved.
bool isSelectionQualifier(const QString& name)
{
    return name == u"scale" || name == u"targetsize" || name == u"contrast" || name == u"theme" || name == u"altform";
}

QString qualifierName(QString name)
{
    name = name.toLower();
    if (name == u"alternateform")
        return u"altform"_s;
    if (name == u"lang")
        return u"language"_s;
    if (name == u"config")
        return u"configuration"_s;
    if (name == u"layoutdir")
        return u"layoutdirection"_s;
    return name;
}

// `file` is a name or a path; the qualifiers are in the name.
std::optional<Candidate> parseCandidate(QStringView logicalBase, QStringView suffix, const QString& file)
{
    const QStringView name = QStringView(file).mid(std::max(file.lastIndexOf(u'/'), file.lastIndexOf(u'\\')) + 1);
    const qsizetype dot = name.lastIndexOf(u'.');
    if (dot < 0 || name.mid(dot).compare(suffix, Qt::CaseInsensitive) != 0)
        return std::nullopt;
    const QStringView base = name.left(dot);
    Candidate c;
    c.name = file;
    if (base.compare(logicalBase, Qt::CaseInsensitive) != 0) {
        if (base.size() <= logicalBase.size() + 1 || !base.startsWith(logicalBase, Qt::CaseInsensitive)
            || base[logicalBase.size()] != u'.')
            return std::nullopt;
        for (const QStringView q : base.mid(logicalBase.size() + 1).split(u'_', Qt::SkipEmptyParts)) {
            const qsizetype dash = q.indexOf(u'-');
            if (dash <= 0 || dash == q.size() - 1)
                return std::nullopt;
            const QString key = qualifierName(q.left(dash).toString());
            if (!c.qualifiers.contains(key))
                c.qualifiers.insert(key, q.mid(dash + 1).toString());
        }
    }
    c.targetSize = std::max(0, c.qualifiers.value(u"targetsize"_s).toInt());
    c.scale = std::max(0, c.qualifiers.value(u"scale"_s).toInt());
    c.theme = c.qualifiers.value(u"theme"_s);
    c.altform = c.qualifiers.value(u"altform"_s);
    c.contrast = c.qualifiers.value(u"contrast"_s);
    return c;
}

// A file for the same language and configuration as the resolved one.
bool compatible(const Candidate& c, const Candidate* resolved)
{
    if (!resolved)
        return true;
    for (auto it = c.qualifiers.cbegin(); it != c.qualifiers.cend(); ++it) {
        if (isSelectionQualifier(it.key()))
            continue;
        const auto r = resolved->qualifiers.constFind(it.key());
        if (r != resolved->qualifiers.cend() && r->compare(*it, Qt::CaseInsensitive) != 0)
            return false;
    }
    return true;
}

int contextScore(const Candidate& c, const Candidate* resolved)
{
    int score = 0;
    if (resolved) {
        for (auto it = resolved->qualifiers.cbegin(); it != resolved->qualifiers.cend(); ++it) {
            if (!isSelectionQualifier(it.key()))
                score += c.qualifiers.contains(it.key()) ? 10 : -1;
        }
    }
    for (auto it = c.qualifiers.cbegin(); it != c.qualifiers.cend(); ++it) {
        if (!isSelectionQualifier(it.key()) && !(resolved && resolved->qualifiers.contains(it.key())))
            --score;
    }
    return score;
}

int themeScore(const Candidate& c, bool dark)
{
    if (c.theme.isEmpty())
        return 1;
    return c.theme.compare(dark ? u"dark" : u"light", Qt::CaseInsensitive) == 0 ? 2 : 0;
}

// Light theme: the dark-on-light logo first; dark theme: the light-on-dark one.
int altformScore(const Candidate& c, bool dark)
{
    if (c.altform.isEmpty())
        return 1; // plated
    const bool unplated = c.altform.compare(u"unplated", Qt::CaseInsensitive) == 0;
    const bool lightUnplated = c.altform.compare(u"lightunplated", Qt::CaseInsensitive) == 0;
    if (dark)
        return unplated ? 3 : lightUnplated ? 0 : 2;
    return lightUnplated ? 3 : unplated ? 2 : 0;
}

using Preference = std::tuple<int, int, int>;

QString stringProperty(IShellItem2* item, const PROPERTYKEY& key)
{
    PWSTR s = nullptr;
    if (FAILED(item->GetString(key, &s)) || !s)
        return {};
    QString out = QString::fromWCharArray(s);
    ::CoTaskMemFree(s);
    return out;
}

struct VisualElements {
    QString logo; // package-relative, e.g. "Assets\Square44x44Logo.png"
    QString background;
};

// The app's <VisualElements> in AppxManifest.xml (the first app's if `appId` is not found).
VisualElements readManifest(const QString& file, const QString& appId)
{
    QFile f(file);
    if (!f.open(QIODevice::ReadOnly))
        return {};
    QXmlStreamReader xml(&f);
    VisualElements first;
    bool haveFirst = false;
    bool inApp = false;
    bool isTheApp = false;
    while (!xml.atEnd()) {
        xml.readNext();
        if (xml.isEndElement() && xml.name() == u"Application") {
            inApp = isTheApp = false;
            continue;
        }
        if (!xml.isStartElement())
            continue;
        if (xml.name() == u"Application") {
            inApp = true;
            isTheApp = !appId.isEmpty() && xml.attributes().value(u"Id") == appId;
        } else if (inApp && xml.name() == u"VisualElements") {
            const QXmlStreamAttributes attributes = xml.attributes();
            VisualElements v;
            for (const auto key : {u"Square44x44Logo"_s, u"Square30x30Logo"_s, u"SmallLogo"_s}) { // Windows 10, 8.1, 8
                if (attributes.hasAttribute(key)) {
                    v.logo = attributes.value(key).toString();
                    break;
                }
            }
            v.background = attributes.value(u"BackgroundColor").toString();
            if (isTheApp)
                return v;
            if (!haveFirst) {
                first = v;
                haveFirst = true;
            }
        }
    }
    return first;
}

// The file Windows' resource system (resources.pri) picks for a package file
// in the current language and scale. Assets may live in qualifier folders
// ("Images\en-US\") that only it knows about.
QString resolveResource(const QString& fullName, const QString& logical)
{
    const qsizetype underscore = fullName.indexOf(u'_');
    if (underscore <= 0)
        return {};
    QStringList segments;
    for (const QString& segment : logical.split(u'/', Qt::SkipEmptyParts))
        segments.append(QString::fromLatin1(QUrl::toPercentEncoding(segment)));
    const QString source = u"@{"_s + fullName + u"?ms-resource://"_s + fullName.left(underscore) + u"/Files/"_s
        + segments.join(u'/') + u'}';
    wchar_t out[2048] = {};
    if (FAILED(::SHLoadIndirectString(reinterpret_cast<LPCWSTR>(source.utf16()), out, static_cast<UINT>(std::size(out)),
            nullptr)))
        return {};
    const QString path = QDir::fromNativeSeparators(QString::fromWCharArray(out));
    return QFileInfo(path).isFile() ? path : QString();
}

} // namespace

std::optional<LogoFile> chooseLogoFile(
    const QString& logical, const QStringList& files, int pixels, bool dark, const QString& resolved)
{
    const qsizetype dot = logical.lastIndexOf(u'.');
    if (dot <= 0 || pixels <= 0)
        return std::nullopt;
    const QStringView base = QStringView(logical).left(dot);
    const QStringView suffix = QStringView(logical).mid(dot);

    const std::optional<Candidate> context = resolved.isEmpty() ? std::nullopt : parseCandidate(base, suffix, resolved);
    const Candidate* resolvedCandidate = context ? &*context : nullptr;
    std::vector<Candidate> candidates;
    for (const QString& file : files) {
        std::optional<Candidate> c = parseCandidate(base, suffix, file);
        if (c && !c->highContrast() && compatible(*c, resolvedCandidate))
            candidates.push_back(std::move(*c));
    }

    const auto preference = [&](const Candidate& c) {
        return Preference {contextScore(c, resolvedCandidate), themeScore(c, dark), altformScore(c, dark)};
    };
    const auto byName = [](const Candidate& a, const Candidate& b) {
        return a.name.compare(b.name, Qt::CaseInsensitive) < 0;
    };
    // Drawn for a size: the right look first, then the smallest at least as big as
    // needed (or the biggest there is).
    const auto betterTarget = [&](const Candidate& a, const Candidate& b) {
        if (const Preference pa = preference(a), pb = preference(b); pa != pb)
            return pa > pb;
        const bool aFits = a.targetSize >= pixels;
        const bool bFits = b.targetSize >= pixels;
        if (aFits != bFits)
            return aFits;
        if (a.targetSize != b.targetSize)
            return aFits ? a.targetSize < b.targetSize : a.targetSize > b.targetSize;
        return byName(a, b);
    };
    // Drawn for a display scale: the right look first, then the biggest.
    const auto betterScale = [&](const Candidate& a, const Candidate& b) {
        if (const Preference pa = preference(a), pb = preference(b); pa != pb)
            return pa > pb;
        if (a.scale != b.scale)
            return a.scale > b.scale;
        return byName(a, b);
    };

    const Candidate* target = nullptr;
    const Candidate* scaled = nullptr;
    for (const Candidate& c : candidates) {
        if (c.targetSize > 0) {
            if (c.targetSize <= pixels * kMaxTargetSizeFactor && (!target || betterTarget(c, *target)))
                target = &c;
        } else if (!scaled || betterScale(c, *scaled)) {
            scaled = &c;
        }
    }
    const Candidate* chosen = target && target->targetSize >= pixels ? target : scaled ? scaled : target;
    if (!chosen)
        return std::nullopt;
    return LogoFile {chosen->name, !chosen->unplated()};
}

std::optional<AppLogo> findAppLogo(const QString& appId, int pixels, bool dark)
{
    Microsoft::WRL::ComPtr<IShellItem2> item;
    const std::wstring shellPath = (u"shell:AppsFolder\\"_s + appId).toStdWString();
    if (FAILED(::SHCreateItemFromParsingName(shellPath.c_str(), nullptr, IID_PPV_ARGS(&item))))
        return std::nullopt;
    const QString fullName = stringProperty(item.Get(), kPackageFullName);
    const QString installPath = QDir::fromNativeSeparators(stringProperty(item.Get(), kPackageInstallPath));
    if (fullName.isEmpty() || installPath.isEmpty())
        return std::nullopt; // a desktop program

    const qsizetype bang = appId.indexOf(u'!');
    const VisualElements visuals
        = readManifest(installPath + u"/AppxManifest.xml"_s, bang >= 0 ? appId.mid(bang + 1) : QString());
    QString logical = QDir::fromNativeSeparators(visuals.logo);
    while (logical.startsWith(u'/'))
        logical.remove(0, 1);
    if (logical.isEmpty())
        return std::nullopt;
    const QString resolved = resolveResource(fullName, logical);

    // Where the asset files may be: next to the one resolved, or where the
    // manifest says (possibly under Assets\ or Images\).
    const QFileInfo logicalInfo(logical);
    QStringList folders;
    const auto addFolder = [&](const QString& folder) {
        const QString clean = QDir::cleanPath(folder);
        if (!folders.contains(clean, Qt::CaseInsensitive))
            folders.append(clean);
    };
    if (!resolved.isEmpty())
        addFolder(QFileInfo(resolved).absolutePath());
    for (const QString& root : {installPath, installPath + u"/Assets"_s, installPath + u"/Images"_s})
        addFolder(root + u'/' + logicalInfo.path());

    // One choice among them all: the resolved file may be in a resource
    // package of its own ("..._split.scale-150_..."), which holds only the
    // assets for that scale, while the main package has every target size.
    const QString filter = logicalInfo.completeBaseName() + u"*."_s + logicalInfo.suffix();
    QStringList files;
    for (const QString& folder : folders) {
        const QDir dir(folder);
        for (const QString& file : dir.entryList({filter}, QDir::Files))
            files.append(dir.filePath(file));
    }
    if (const auto choice = chooseLogoFile(logicalInfo.fileName(), files, pixels, dark, resolved))
        return AppLogo {choice->name, choice->plated, visuals.background};
    if (!resolved.isEmpty())
        return AppLogo {resolved, !resolved.contains(u"unplated"_s, Qt::CaseInsensitive), visuals.background};
    return std::nullopt;
}

} // namespace ws
