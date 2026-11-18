#pragma once

#include <QObject>
#include <QPointer>
#include <QQuickTextDocument>
#include <QtQml/qqmlregistration.h>

namespace ws {

// A TextEdit's lines spaced as a Text's proportional lineHeight spaces them,
// which TextEdit has no property for: a selectable text that reads like the
// Text it stands in for (the clipboard's preview, GitHub #375). Set again
// whenever the text changes, as a new text starts with plain blocks.
class TextLineSpacing : public QObject {
    Q_OBJECT
    QML_ELEMENT
    Q_PROPERTY(QQuickTextDocument* textDocument READ textDocument WRITE setTextDocument NOTIFY textDocumentChanged FINAL)
    Q_PROPERTY(qreal factor READ factor WRITE setFactor NOTIFY factorChanged FINAL) // 1.15: as Text's lineHeight 1.15

public:
    using QObject::QObject;

    QQuickTextDocument* textDocument() const { return m_quick; }
    void setTextDocument(QQuickTextDocument* document);
    qreal factor() const { return m_factor; }
    void setFactor(qreal factor);

signals:
    void textDocumentChanged();
    void factorChanged();

private:
    void apply();

    QPointer<QQuickTextDocument> m_quick;
    QMetaObject::Connection m_changed;
    qreal m_factor = 1.0;
    bool m_applying = false;
};

} // namespace ws
