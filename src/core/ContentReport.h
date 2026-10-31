#pragma once

#include <QByteArray>
#include <QString>
#include <QStringList>
#include <vector>

namespace ws::contentreport {

enum class Format { Text, Csv, Html, Excel };
struct Table {
    QString title;
    QStringList headings;
    std::vector<QStringList> rows;
};
struct Report {
    QString title;
    QStringList notes; // query, scope, completion and failure information
    std::vector<Table> tables;
};
// Every value is untrusted document text. HTML/XML escape it; Excel cells
// are explicit strings; CSV protects formula-like fields on opening in Excel.
QByteArray render(const Report& report, Format format);
bool save(const Report& report, Format format, const QString& path);
QString text(const Report& report);

} // namespace ws::contentreport
