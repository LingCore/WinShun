#pragma once

#include <QAbstractNativeEventFilter>
#include <QColor>
#include <QHash>
#include <QObject>
#include <QRawFont>
#include <QVariant>
#include <QtQml/qqmlregistration.h>

namespace ws {

// System appearance for QML: light/dark mode, accent colour, icon font.
// Colours derived from these live in Theme.qml.
class SystemTheme : public QObject, public QAbstractNativeEventFilter {
    Q_OBJECT
    QML_ELEMENT
    QML_SINGLETON
    Q_PROPERTY(bool dark READ dark NOTIFY changed FINAL)
    Q_PROPERTY(QColor accent READ accent NOTIFY changed FINAL)
    // A fill that carries white words or marks (see accentFill()).
    Q_PROPERTY(QColor accentFill READ accentFill NOTIFY changed FINAL)
    Q_PROPERTY(QString iconFont READ iconFont CONSTANT FINAL)
    // How much larger a Phosphor icon is drawn than its box (phosphor::kScale).
    Q_PROPERTY(qreal iconScale READ iconScale CONSTANT FINAL)
    // Settings > Accessibility > Visual effects > Animation effects.
    Q_PROPERTY(bool animations READ animations NOTIFY changed FINAL)
    // Whether our windows can stand on Mica (see WindowEffects): Windows 11
    // 22H2 or later and the GPU renderer. Decided once at startup.
    Q_PROPERTY(bool backdropAvailable READ backdropAvailable CONSTANT FINAL)
    // Windows has Mica (11 22H2 or later), whatever the renderer.
    Q_PROPERTY(bool backdropSystem READ backdropSystem CONSTANT FINAL)
    // They do (the 透明效果 setting): their surfaces are translucent then.
    Q_PROPERTY(bool backdrop READ backdrop NOTIFY changed FINAL)
    // Transparency effects on and no high contrast: otherwise Windows draws
    // no Mica.
    Q_PROPERTY(bool materials READ materials NOTIFY changed FINAL)
    // Rounded on Windows 11 when the appearance setting allows it.
    Q_PROPERTY(bool roundedCorners READ roundedCorners NOTIFY changed FINAL)
    // How large the launcher and the clipboard are drawn (界面大小: compact,
    // standard, roomy; see uisize::), and its header's height, as C++ places by.
    Q_PROPERTY(QString uiSize READ uiSize NOTIFY uiSizeChanged FINAL)
    Q_PROPERTY(int headerHeight READ headerHeight NOTIFY uiSizeChanged FINAL)
    // How long the mouse rests before a tip shows (SPI_GETMOUSEHOVERTIME, ms).
    Q_PROPERTY(int hoverTime READ hoverTime NOTIFY changed FINAL)

public:
    explicit SystemTheme(QObject* parent = nullptr);

    bool dark() const;
    QColor accent() const;
    QColor accentFill() const;
    static QColor accentColor(); // accent(), for C++
    QString iconFont() const { return m_iconFont; }
    static QString iconFontName(); // iconFont(), for C++ (FileIconProvider draws glyph icons)
    static qreal iconScale();
    // The Phosphor icon drawn for a Segoe code point, as {text, family}, or
    // undefined: the icon font draws it then (see phosphor::forSegoe).
    Q_INVOKABLE QVariant phosphorIcon(const QString& glyph) const;
    // The icon font has the glyph (every character of it). An older Windows
    // 10 has an older "Segoe MDL2 Assets", without the icons added since: the
    // text would then take the character from whatever font has it, a Chinese
    // character from a font of a company's own characters, say. Remembered;
    // the GUI thread only.
    Q_INVOKABLE bool hasGlyph(const QString& glyph) const;
    // hasGlyph's check, in any installed font; true when the font is not
    // there to ask (the text is drawn as it ever was).
    static bool fontHas(const QString& family, const QString& glyph);
    bool animations() const;
    static int hoverTime();
    bool materials() const;
    bool backdropSystem() const;
    bool roundedCorners() const;
    static void setRoundedCorners(bool on);
    static bool backdropAvailable() { return s_backdropAvailable; }
    static void setBackdropAvailable(bool on) { s_backdropAvailable = on; } // before the first window
    static bool backdrop() { return s_backdropAvailable && s_backdropOn; }
    static void setBackdropOn(bool on);
    static QString uiSize() { return s_uiSize; }
    static int headerHeight();
    static void setUiSize(const QString& size);

signals:
    void changed();
    void uiSizeChanged(); // not changed(): the colours stay

protected:
    bool eventFilter(QObject* watched, QEvent* event) override;
    bool nativeEventFilter(const QByteArray& eventType, void* message, qintptr* result) override;

private:
    static inline bool s_backdropAvailable = false;
    static inline bool s_backdropOn = true;
    static inline QString s_uiSize = QStringLiteral("standard");
    static inline SystemTheme* s_instance = nullptr; // the QML singleton
    QString m_iconFont;
    mutable QRawFont m_iconRaw; // made the first time hasGlyph asks
    mutable QHash<QString, bool> m_glyphs; // hasGlyph's answers
};

} // namespace ws
