#include "PluginReadme.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QRegularExpression>
#include <QStringConverter>
#include <QTextBlock>
#include <QTextCursor>
#include <QTextDocument>
#include <QTextFragment>
#include <QUrl>

using namespace Qt::StringLiterals;

namespace ws::readme {
namespace {
QString path(const QString& directory)
{
    const QString root = QFileInfo(directory).canonicalFilePath();
    if (root.isEmpty()) return {};
    for (const QString& name : {u"README.md"_s, u"README.html"_s, u"README.htm"_s, u"README.txt"_s}) {
        const QFileInfo file(QDir(root).filePath(name));
        if (file.isFile() && file.canonicalFilePath().startsWith(root + u'/', Qt::CaseInsensitive))
            return file.canonicalFilePath();
    }
    return {};
}

// Plugin documentation is passive text. No local or remote resource is loaded,
// including HTML stylesheets and Markdown images, while parsing it.
class TextDocument final : public QTextDocument {
    QVariant loadResource(int, const QUrl&) override { return {}; }
};
}

bool exists(const QString& directory) { return !path(directory).isEmpty(); }

bool webLink(const QString& link)
{
    const QUrl url(link, QUrl::StrictMode);
    return url.isValid() && !url.host().isEmpty() && url.userInfo().isEmpty()
        && (url.scheme() == u"https" || url.scheme() == u"http");
}

Document load(const QString& directory)
{
    QFile file(path(directory));
    if (file.fileName().isEmpty() || !file.open(QIODevice::ReadOnly))
        return {{}, QCoreApplication::translate("PluginReadme", "无法读取插件说明，请确认插件文件完整")};
    constexpr qint64 limit = 1024 * 1024;
    if (file.size() > limit)
        return {{}, QCoreApplication::translate("PluginReadme", "插件说明超过 1 MB，暂时无法显示")};
    const QByteArray bytes = file.read(limit + 1);
    if (bytes.size() > limit || file.error() != QFileDevice::NoError)
        return {{}, QCoreApplication::translate("PluginReadme", "读取插件说明失败")};
    QStringDecoder decoder(QStringConverter::encodingForData(bytes).value_or(QStringConverter::Utf8));
    const QString source = decoder(bytes);
    TextDocument doc;
    const auto extension = QFileInfo(file).suffix();
    if (extension.compare(u"md", Qt::CaseInsensitive) == 0)
        doc.setMarkdown(source, QTextDocument::MarkdownFeatures(QTextDocument::MarkdownDialectGitHub) | QTextDocument::MarkdownNoHTML);
    else if (extension.compare(u"txt", Qt::CaseInsensitive) == 0)
        doc.setPlainText(source);
    else
        doc.setHtml(source);

    // Normalize the document for the current theme. Remove images before handing
    // the text to QML so its text engine cannot load resources either.
    QList<QTextFragment> fragments;
    for (auto block = doc.begin(); block.isValid(); block = block.next())
        for (auto it = block.begin(); !it.atEnd(); ++it)
            if (it.fragment().isValid()) fragments.append(it.fragment());
    for (auto it = fragments.crbegin(); it != fragments.crend(); ++it) {
        auto format = it->charFormat();
        QTextCursor cursor(&doc);
        cursor.setPosition(it->position());
        cursor.setPosition(it->position() + it->length(), QTextCursor::KeepAnchor);
        if (format.isImageFormat()) {
            const QString alt = format.property(QTextFormat::ImageAltText).toString();
            cursor.insertText(alt, QTextCharFormat());
        } else {
            if (format.isAnchor() && !webLink(format.anchorHref())) {
                format.setAnchor(false);
                format.setAnchorHref({});
            }
            format.clearProperty(QTextFormat::ForegroundBrush);
            format.clearProperty(QTextFormat::BackgroundBrush);
            format.clearProperty(QTextFormat::FontPointSize);
            format.clearProperty(QTextFormat::FontPixelSize);
            cursor.setCharFormat(format);
        }
    }
    // QTextDocument writes a fixed default body font. Let QML supply the user's
    // UI font and size instead; headings keep their relative HTML sizes.
    QString html = doc.toHtml();
    html.replace(QRegularExpression(u"<body[^>]*>"_s), u"<body>"_s);
    return {html, {}};
}
}
