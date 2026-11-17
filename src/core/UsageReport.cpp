#include "UsageReport.h"

#include "Release.h"

#include <QCryptographicHash>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QUrl>

#include <windows.h>

using namespace Qt::StringLiterals;

namespace ws::usage {

namespace {

// scheme://host[:port] of the feed, or empty when no report may go there.
QString origin(const QString& feedUrl)
{
    const QUrl url(feedUrl);
    if (!url.isValid() || url.host().isEmpty() || !url.userInfo().isEmpty())
        return {};
    if (url.scheme() != u"https"_s && !(url.scheme() == u"http"_s && release::loopbackHttpUrl(feedUrl)))
        return {};
    return url.adjusted(QUrl::RemovePath | QUrl::RemoveQuery | QUrl::RemoveFragment).toString(QUrl::FullyEncoded);
}

} // namespace

QString installId(QStringView project, QStringView machine)
{
    if (machine.trimmed().isEmpty())
        return {};
    const QString text = u"gleaning.install.v1:"_s + project.toString() + u':' + machine.toString();
    return QString::fromLatin1(QCryptographicHash::hash(text.toUtf8(), QCryptographicHash::Sha256).toHex());
}

QString machineGuid()
{
    wchar_t value[64] = {};
    DWORD bytes = sizeof(value);
    if (::RegGetValueW(HKEY_LOCAL_MACHINE, L"SOFTWARE\\Microsoft\\Cryptography", L"MachineGuid",
            RRF_RT_REG_SZ | RRF_SUBKEY_WOW6464KEY, nullptr, value, &bytes)
        != ERROR_SUCCESS)
        return {};
    return QString::fromWCharArray(value).trimmed();
}

QByteArray reportBody(const QString& installId, const QString& version, const QString& osVersion)
{
    QJsonObject body {
        {u"installId"_s, installId},
        {u"version"_s, version},
        {u"platform"_s, u"windows"_s},
        {u"arch"_s, u"x64"_s},
    };
    if (!osVersion.isEmpty())
        body.insert(u"osVersion"_s, osVersion);
    return QJsonDocument(body).toJson(QJsonDocument::Compact);
}

QString reportUrl(const QString& feedUrl, QStringView project)
{
    const QString base = origin(feedUrl);
    return base.isEmpty() ? QString() : base + u"/api/v1/projects/"_s + project + u"/installs"_s;
}

QString projectsUrl(const QString& feedUrl)
{
    const QString base = origin(feedUrl);
    return base.isEmpty() ? QString() : base + u"/api/v1/projects"_s;
}

bool due(const QDate& last, const QDate& today)
{
    // A clock set back (last > today) sends again rather than never.
    return !last.isValid() || last != today;
}

QHash<QString, int> userCounts(const QByteArray& json)
{
    QHash<QString, int> counts;
    const QJsonArray projects = QJsonDocument::fromJson(json).object().value(u"projects"_s).toArray();
    for (const QJsonValue& value : projects) {
        const QJsonObject project = value.toObject();
        const QString slug = project.value(u"slug"_s).toString();
        const double users = project.value(u"stats"_s).toObject().value(u"users"_s).toDouble(0);
        if (!slug.isEmpty() && users >= 1 && users <= 2e9 && users == static_cast<int>(users))
            counts.insert(slug, static_cast<int>(users));
    }
    return counts;
}

} // namespace ws::usage
