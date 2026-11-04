#pragma once

#include "DiagnosticLog.h"

#include <QJsonObject>
#include <QString>

namespace ws::diagnostics {
struct Exported {
    QString path;
    QString error;
};
QJsonObject environment(); // no account names, paths, queries or settings secrets
// A process's rights as UIPI weighs them, for the journal: "integrity"
// (low, medium, medium-plus, high, system; unknown when it cannot be asked),
// "elevated", "ui_access" (left out when they cannot be asked). `pid` 0:
// Win顺's own. No names.
QJsonObject rightsOf(unsigned long pid);
Exported exportArchive(Log& log, const QString& dataFolder, const QString& serviceFolder, QJsonObject context);
} // namespace ws::diagnostics
