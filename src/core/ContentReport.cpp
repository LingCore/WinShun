#include "ContentReport.h"

#include <QXmlStreamWriter>
#include <QSaveFile>
#include <miniz.h>
#include <algorithm>

using namespace Qt::StringLiterals;

namespace ws::contentreport {
namespace {

QString csvCell(QString value)
{
    const QString check = value.trimmed();
    QStringView leading(check);
    while (!leading.isEmpty() && (leading.front().unicode() < 0x20 || leading.front().category() == QChar::Other_Format))
        leading = leading.mid(1);
    if (!leading.isEmpty() && (leading.front() == u'=' || leading.front() == u'+' || leading.front() == u'-'
            || leading.front() == u'@' || value.front() == u'\t' || value.front() == u'\r'))
        value.prepend(u'\'');
    value.replace(u"\""_s, u"\"\""_s);
    return u'"' + value + u'"';
}

QString xmlText(QString value)
{
    value = QString::fromUtf8(value.toUtf8()); // normalize unpaired UTF-16 before writing XML
    // XML 1.0 cannot represent these controls, even as numeric entities.
    value.removeIf([](QChar c) { return c.unicode() < 0x20 && c != u'\t' && c != u'\n' && c != u'\r'; });
    return value;
}

QByteArray sheet(const Table& table)
{
    QByteArray bytes;
    QXmlStreamWriter xml(&bytes);
    xml.writeStartDocument();
    xml.writeStartElement(u"worksheet"_s);
    xml.writeDefaultNamespace(u"http://schemas.openxmlformats.org/spreadsheetml/2006/main"_s);
    xml.writeStartElement(u"sheetData"_s);
    const auto writeRow = [&](const QStringList& row) {
        xml.writeStartElement(u"row"_s);
        for (const QString& cell : row) {
            xml.writeStartElement(u"c"_s);
            xml.writeAttribute(u"t"_s, u"inlineStr"_s);
            xml.writeStartElement(u"is"_s);
            xml.writeStartElement(u"t"_s);
            xml.writeAttribute(u"xml:space"_s, u"preserve"_s);
            xml.writeCharacters(xmlText(cell.left(32767)));
            xml.writeEndElement();
            xml.writeEndElement();
            xml.writeEndElement();
        }
        xml.writeEndElement();
    };
    writeRow(table.headings);
    for (const auto& row : table.rows)
        writeRow(row);
    xml.writeEndElement();
    xml.writeEndElement();
    xml.writeEndDocument();
    return bytes;
}

QByteArray xlsx(const Report& report)
{
    std::vector<Table> tables {{u"Search"_s, {report.title}, {}}};
    for (const QString& note : report.notes)
        tables.front().rows.push_back({note});
    tables.insert(tables.end(), report.tables.begin(), report.tables.end());
    mz_zip_archive zip {};
    if (!mz_zip_writer_init_heap(&zip, 0, 0))
        return {};
    bool ok = true;
    const auto put = [&](const QByteArray& name, const QByteArray& bytes) {
        ok = ok && mz_zip_writer_add_mem(&zip, name.constData(), bytes.constData(),
            static_cast<size_t>(bytes.size()), MZ_BEST_SPEED);
    };
    QByteArray types = "<?xml version=\"1.0\" encoding=\"UTF-8\"?><Types xmlns=\"http://schemas.openxmlformats.org/package/2006/content-types\"><Default Extension=\"rels\" ContentType=\"application/vnd.openxmlformats-package.relationships+xml\"/><Default Extension=\"xml\" ContentType=\"application/xml\"/><Override PartName=\"/xl/workbook.xml\" ContentType=\"application/vnd.openxmlformats-officedocument.spreadsheetml.sheet.main+xml\"/>";
    QByteArray workbook = "<?xml version=\"1.0\" encoding=\"UTF-8\"?><workbook xmlns=\"http://schemas.openxmlformats.org/spreadsheetml/2006/main\" xmlns:r=\"http://schemas.openxmlformats.org/officeDocument/2006/relationships\"><sheets>";
    QByteArray relationships = "<?xml version=\"1.0\" encoding=\"UTF-8\"?><Relationships xmlns=\"http://schemas.openxmlformats.org/package/2006/relationships\">";
    for (std::size_t i = 0; i < tables.size(); ++i) {
        const QByteArray n = QByteArray::number(static_cast<qulonglong>(i + 1));
        const QByteArray name = (i == 0 ? u"Search"_s : u"%1 %2"_s.arg(i).arg(tables[i].title.left(24)))
                                   .toHtmlEscaped().toUtf8();
        workbook += "<sheet name=\"" + name + "\" sheetId=\"" + n + "\" r:id=\"rId" + n + "\"/>";
        relationships += "<Relationship Id=\"rId" + n + "\" Type=\"http://schemas.openxmlformats.org/officeDocument/2006/relationships/worksheet\" Target=\"worksheets/sheet" + n + ".xml\"/>";
        types += "<Override PartName=\"/xl/worksheets/sheet" + n + ".xml\" ContentType=\"application/vnd.openxmlformats-officedocument.spreadsheetml.worksheet+xml\"/>";
        put("xl/worksheets/sheet" + n + ".xml", sheet(tables[i]));
    }
    put("[Content_Types].xml", types + "</Types>");
    put("xl/workbook.xml", workbook + "</sheets></workbook>");
    put("xl/_rels/workbook.xml.rels", relationships + "</Relationships>");
    put("_rels/.rels", "<?xml version=\"1.0\" encoding=\"UTF-8\"?><Relationships xmlns=\"http://schemas.openxmlformats.org/package/2006/relationships\"><Relationship Id=\"rId1\" Type=\"http://schemas.openxmlformats.org/officeDocument/2006/relationships/officeDocument\" Target=\"xl/workbook.xml\"/></Relationships>");
    size_t size = 0;
    void* data = nullptr;
    ok = ok && mz_zip_writer_finalize_heap_archive(&zip, &data, &size);
    QByteArray result;
    if (ok)
        result = QByteArray(static_cast<const char*>(data), static_cast<qsizetype>(size));
    mz_free(data);
    mz_zip_writer_end(&zip);
    return result;
}

} // namespace

QString text(const Report& report)
{
    QString out = report.title + u'\n' + report.notes.join(u'\n') + u'\n';
    for (const auto& table : report.tables) {
        out += u'\n' + table.title + u'\n' + table.headings.join(u'\t') + u'\n';
        for (const auto& row : table.rows)
            out += row.join(u'\t') + u'\n';
    }
    return out;
}

QByteArray render(const Report& report, Format format)
{
    if (format == Format::Excel)
        return xlsx(report);
    if (format == Format::Text)
        return text(report).toUtf8();
    if (format == Format::Csv) {
        QString csv;
        const auto row = [&](const QStringList& cells) {
            QStringList escaped;
            for (const QString& cell : cells)
                escaped.append(csvCell(cell));
            csv += escaped.join(u',') + u"\r\n"_s;
        };
        row({report.title});
        for (const auto& note : report.notes)
            row({note});
        for (const auto& table : report.tables) {
            row({table.title});
            row(table.headings);
            for (const auto& cells : table.rows)
                row(cells);
        }
        return QByteArray("\xEF\xBB\xBF", 3) + csv.toUtf8();
    }
    QString html = u"<!doctype html><html><head><meta charset=\"utf-8\"><title>%1</title><style>body{font:14px system-ui,sans-serif;margin:32px;color:#202020}table{border-collapse:collapse;width:100%;margin-bottom:24px}th,td{border-bottom:1px solid #ddd;padding:8px;text-align:left;vertical-align:top;white-space:pre-wrap;overflow-wrap:anywhere}thead{display:table-header-group}@media print{body{margin:0}tr{break-inside:avoid}}</style></head><body><h1>%1</h1>"_s.arg(report.title.toHtmlEscaped());
    for (const QString& note : report.notes)
        html += u"<p>%1</p>"_s.arg(note.toHtmlEscaped());
    for (const auto& table : report.tables) {
        html += u"<h2>%1</h2><table><thead><tr>"_s.arg(table.title.toHtmlEscaped());
        for (const QString& heading : table.headings)
            html += u"<th>%1</th>"_s.arg(heading.toHtmlEscaped());
        html += u"</tr></thead><tbody>"_s;
        for (const auto& row : table.rows) {
            html += u"<tr>"_s;
            for (const QString& cell : row)
                html += u"<td>%1</td>"_s.arg(cell.toHtmlEscaped());
            html += u"</tr>"_s;
        }
        html += u"</tbody></table>"_s;
    }
    return (html + u"</body></html>"_s).toUtf8();
}

bool save(const Report& report, Format format, const QString& path)
{
    const QByteArray bytes = render(report, format);
    QSaveFile file(path);
    return !bytes.isEmpty() && file.open(QIODevice::WriteOnly) && file.write(bytes) == bytes.size() && file.commit();
}

} // namespace ws::contentreport
