#pragma once

#include <QColor>
#include <QObject>
#include <QtQml/qqmlregistration.h>

namespace qf {

// System appearance for QML: light/dark mode, accent colour, icon font.
// Colours derived from these live in Theme.qml.
class SystemTheme : public QObject {
    Q_OBJECT
    QML_ELEMENT
    QML_SINGLETON
    Q_PROPERTY(bool dark READ dark NOTIFY changed FINAL)
    Q_PROPERTY(QColor accent READ accent NOTIFY changed FINAL)
    Q_PROPERTY(QString iconFont READ iconFont CONSTANT FINAL)
    // Settings > Accessibility > Visual effects > Animation effects.
    Q_PROPERTY(bool animations READ animations NOTIFY changed FINAL)

public:
    explicit SystemTheme(QObject* parent = nullptr);

    bool dark() const;
    QColor accent() const;
    QString iconFont() const { return m_iconFont; }
    bool animations() const;

signals:
    void changed();

protected:
    bool eventFilter(QObject* watched, QEvent* event) override;

private:
    QString m_iconFont;
};

} // namespace qf
