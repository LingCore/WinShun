#pragma once

#include <QList>
#include <QString>
#include <QStringList>

namespace ws {

// What the user said about the apps (#505; Settings' [Apps], AppCatalog).
struct AppName {
    QString path; // the app's launch path: shell:AppsFolder\<id>, or a program's file
    QString name; // what Win顺 calls it (cleanAppName)
    bool operator==(const AppName&) const = default;
};

struct UserApps {
    QStringList folders; // of portable programs, listed as apps (portablePrograms)
    QList<AppName> names;
    QStringList hidden; // launch paths of apps left out of the list
    bool operator==(const UserApps&) const = default;
};

} // namespace ws
