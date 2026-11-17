#pragma once

#include <QByteArray>
#include <QDate>
#include <QHash>
#include <QString>
#include <QStringView>

// How many people use the author's works: once a day each copy says to the
// 拾穗 service (lcstp.com, the update service's site) that this computer
// still runs it, and the 拾穗计划 page shows the counts the service keeps.
// The protocol is the service's own (gleaning-web, "installs"), the same as
// MacShun's: POST /api/v1/projects/<project>/installs with a JSON body of
// installId, version, platform, arch and osVersion; any 2xx is taken.
namespace ws::usage {

inline constexpr char16_t kProject[] = u"winshun";

// This computer, as the service counts it: SHA-256 of
// "gleaning.install.v1:<project>:<MachineGuid>", 64 lowercase hex digits.
// The MachineGuid never leaves the computer, and with the project in the
// hash one computer's IDs for two works cannot be matched. Empty for an
// empty machine identifier (nothing is sent then).
QString installId(QStringView project, QStringView machine);

// HKLM\SOFTWARE\Microsoft\Cryptography\MachineGuid, from the 64-bit view;
// empty when it cannot be read.
QString machineGuid();

// The request's body, at most 1 KiB (the service refuses more).
// `osVersion` is "10.0.26200": up to four numbers.
QByteArray reportBody(const QString& installId, const QString& version, const QString& osVersion);

// The service's addresses, at the update feed's origin (scheme, host, port):
// "https://lcstp.com/api/v1/projects/winshun/installs" and
// "https://lcstp.com/api/v1/projects". Empty unless the feed is https, or
// http to this computer (a test's fake service).
QString reportUrl(const QString& feedUrl, QStringView project = kProject);
QString projectsUrl(const QString& feedUrl);

// Whether today's report is still to be sent: once per calendar day, local
// time. `last` is the day of the last one (invalid: never).
bool due(const QDate& last, const QDate& today);

// The users of each work from the service's GET /api/v1/projects:
// {"projects":[{"slug":"winshun","stats":{"users":30,...}},...]}. Works with
// no count, or a count that is not a whole number above zero, are left out.
QHash<QString, int> userCounts(const QByteArray& json);

} // namespace ws::usage
