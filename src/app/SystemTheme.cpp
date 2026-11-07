#include "SystemTheme.h"

#include "Phosphor.h"
#include "UiSize.h"
#include "platform/WindowEffects.h"

#include <QEvent>
#include <QFont>
#include <QGuiApplication>
#include <QOperatingSystemVersion>
#include <QPalette>
#include <QStyleHints>
#include <QTimer>

#include <windows.h>

#include <algorithm>
#include <cmath>

using namespace Qt::StringLiterals;

namespace ws {

SystemTheme::SystemTheme(QObject* parent)
    : QObject(parent)
    , m_iconFont(iconFontName())
{
    connect(QGuiApplication::styleHints(), &QStyleHints::colorSchemeChanged, this, &SystemTheme::changed);
    qApp->installEventFilter(this); // accent colour changes arrive as a palette change
    qApp->installNativeEventFilter(this); // transparency effects, high contrast: WM_SETTINGCHANGE
    s_instance = this;
}

QString SystemTheme::iconFontName()
{
    // Windows 11 ships "Segoe Fluent Icons"; Windows 10 has "Segoe MDL2 Assets".
    // Decided by OS version: enumerating all installed fonts is expensive.
    return QOperatingSystemVersion::current() >= QOperatingSystemVersion::Windows11 ? u"Segoe Fluent Icons"_s
                                                                                    : u"Segoe MDL2 Assets"_s;
}

qreal SystemTheme::iconScale()
{
    return phosphor::kScale;
}

QVariant SystemTheme::phosphorIcon(const QString& glyph) const
{
    const std::optional<phosphor::Icon> icon = phosphor::forSegoe(glyph);
    if (!icon)
        return {};
    return QVariantMap {{u"text"_s, icon->text}, {u"family"_s, icon->family}};
}

namespace {

bool supports(const QRawFont& font, const QString& family, const QString& glyph)
{
    if (!font.isValid() || font.familyName().compare(family, Qt::CaseInsensitive) != 0)
        return true; // another font stands in: nothing to tell
    const QList<uint> characters = glyph.toUcs4();
    return std::ranges::all_of(characters, [&](uint c) { return font.supportsCharacter(c); });
}

} // namespace

bool SystemTheme::fontHas(const QString& family, const QString& glyph)
{
    return supports(QRawFont::fromFont(QFont(family)), family, glyph);
}

bool SystemTheme::hasGlyph(const QString& glyph) const
{
    if (glyph.isEmpty())
        return true;
    if (const auto known = m_glyphs.constFind(glyph); known != m_glyphs.cend())
        return *known;
    if (!m_iconRaw.isValid())
        m_iconRaw = QRawFont::fromFont(QFont(m_iconFont));
    const bool has = supports(m_iconRaw, m_iconFont, glyph);
    m_glyphs.insert(glyph, has);
    if (!has) // in the diagnostics: which icons an old font lacks
        qWarning("WinShun: the icon font (%s) has no U+%04X; drawn another way, or not at all", qPrintable(m_iconFont),
            glyph.toUcs4().value(0));
    return has;
}

bool SystemTheme::backdropSystem() const
{
    return win::backdropSupported();
}

bool SystemTheme::roundedCorners() const
{
    return win::roundedCorners();
}

bool SystemTheme::nativeWindowBorder() const
{
    return win::nativeWindowCorners();
}

void SystemTheme::setRoundedCorners(bool on)
{
    if (win::setRoundedCorners(on) && s_instance)
        emit s_instance->changed();
}

void SystemTheme::setBackdropOn(bool on)
{
    if (s_backdropOn == on)
        return;
    s_backdropOn = on;
    if (s_instance)
        emit s_instance->changed();
}

int SystemTheme::headerHeight()
{
    return uisize::metrics(s_uiSize).header;
}

void SystemTheme::setGlassAppearance(int transparency, int clarity)
{
    transparency = std::clamp(transparency, 0, 100);
    clarity = std::clamp(clarity, 0, 100);
    if (s_glassTransparency == transparency && s_glassClarity == clarity) return;
    s_glassTransparency = transparency;
    s_glassClarity = clarity;
    if (s_instance) emit s_instance->changed();
}

QMetaObject::Connection SystemTheme::onAppearanceChanged(QObject* context, std::function<void()> slot)
{
    return s_instance ? QObject::connect(s_instance, &SystemTheme::changed, context, std::move(slot)) : QMetaObject::Connection{};
}

void SystemTheme::setUiStyle(const QString& style)
{
    const QString known = style == u"brand" || style == u"tactile" || style == u"liquid" ? style : QStringLiteral("fluent");
    if (s_uiStyle == known)
        return;
    s_uiStyle = known;
    win::setBackdropMaterial(known == u"liquid" ? win::BackdropMaterial::Acrylic : win::BackdropMaterial::Mica);
    if (s_instance)
        emit s_instance->changed();
}

namespace {

// nativeLook's changes, told once the event loop turns: one style change
// rewrites it once per token. A single-shot timer is both the coalescing
// and the signal; it goes with the application.
QTimer* nativeLookHub()
{
    static QPointer<QTimer> hub;
    if (!hub && qApp) {
        hub = new QTimer(qApp);
        hub->setSingleShot(true);
        hub->setInterval(0);
    }
    return hub;
}

} // namespace

void SystemTheme::setNativeLook(const QVariantMap& look)
{
    if (look == s_nativeLook)
        return;
    s_nativeLook = look;
    if (QTimer* hub = nativeLookHub())
        hub->start();
}

QColor SystemTheme::nativeColor(const char* name, const QColor& fallback)
{
    const QVariant value = s_nativeLook.value(QString::fromLatin1(name));
    const QColor color = value.metaType() == QMetaType::fromType<QColor>() ? value.value<QColor>() : QColor(value.toString());
    return color.isValid() ? color : fallback;
}

qreal SystemTheme::nativeReal(const char* name, qreal fallback)
{
    bool ok = false;
    const qreal value = s_nativeLook.value(QString::fromLatin1(name)).toReal(&ok);
    return ok && std::isfinite(value) ? value : fallback;
}

QMetaObject::Connection SystemTheme::onNativeLookChanged(QObject* context, std::function<void()> slot)
{
    QTimer* hub = nativeLookHub();
    return hub ? QObject::connect(hub, &QTimer::timeout, context, std::move(slot)) : QMetaObject::Connection();
}

QJsonObject SystemTheme::nativeLookJson()
{
    QJsonObject json;
    for (auto it = s_nativeLook.cbegin(); it != s_nativeLook.cend(); ++it) {
        const QVariant& value = it.value();
        if (value.metaType() == QMetaType::fromType<QColor>())
            json.insert(it.key(), value.value<QColor>().name(QColor::HexArgb));
        else if (value.metaType() == QMetaType::fromType<bool>())
            json.insert(it.key(), value.toBool());
        else if (value.metaType() == QMetaType::fromType<QString>())
            json.insert(it.key(), value.toString());
        else if (value.canConvert<double>())
            json.insert(it.key(), value.toDouble());
    }
    return json;
}

void SystemTheme::setUiSize(const QString& size)
{
    if (s_uiSize == size)
        return;
    s_uiSize = size;
    if (s_instance)
        emit s_instance->uiSizeChanged();
}

bool SystemTheme::dark() const
{
    return QGuiApplication::styleHints()->colorScheme() == Qt::ColorScheme::Dark;
}

QColor SystemTheme::accent() const
{
    return accentColor();
}

namespace {

// WCAG relative luminance.
double luminance(const QColor& c)
{
    const auto channel = [](double v) { return v <= 0.03928 ? v / 12.92 : std::pow((v + 0.055) / 1.055, 2.4); };
    return 0.2126 * channel(c.redF()) + 0.7152 * channel(c.greenF()) + 0.0722 * channel(c.blueF());
}

} // namespace

QColor SystemTheme::accentFill() const
{
    // Under white words or marks (a highlighted button, a switch that is
    // on, the current sub-tab): the accent, a little lighter on dark, then
    // darker until white on it reads (WCAG AA, 4.5:1), whatever the accent
    // (a yellow one too). Theme.accent, much lighter on dark, is for text
    // and lines, where black on it would be the only legible word colour.
    QColor fill = accentColor();
    if (dark())
        fill = fill.lighter(115);
    for (int i = 0; i < 30 && 1.05 / (luminance(fill) + 0.05) < 4.5; ++i)
        fill = fill.darker(108);
    return fill;
}

QColor SystemTheme::accentColor()
{
    const QColor accent = QGuiApplication::palette().color(QPalette::Accent);
    // A grey accent (a popular Windows choice) would make highlighted matches
    // indistinguishable from plain text; use the standard Windows blue then.
    if (!accent.isValid() || accent.hsvSaturationF() < 0.25)
        return QColor(0x00, 0x67, 0xC0);
    return accent;
}

bool SystemTheme::animations() const
{
    BOOL on = TRUE;
    return !::SystemParametersInfoW(SPI_GETCLIENTAREAANIMATION, 0, &on, 0) || on;
}

int SystemTheme::hoverTime()
{
    UINT ms = 400; // Windows' default
    if (!::SystemParametersInfoW(SPI_GETMOUSEHOVERTIME, 0, &ms, 0))
        ms = 400;
    return static_cast<int>(std::clamp<UINT>(ms, 100, 3000));
}

bool SystemTheme::materials() const
{
    return win::materialsEnabled();
}

bool SystemTheme::eventFilter(QObject* watched, QEvent* event)
{
    if (watched == qApp && event->type() == QEvent::ApplicationPaletteChange)
        emit changed();
    return QObject::eventFilter(watched, event);
}

bool SystemTheme::nativeEventFilter(const QByteArray& eventType, void* message, qintptr*)
{
    // Sent to every top-level window when the colour settings change.
    const auto* msg = static_cast<const MSG*>(message);
    if (eventType != "windows_generic_MSG" || !msg || msg->message != WM_SETTINGCHANGE)
        return false;
    if (msg->wParam == SPI_SETHIGHCONTRAST
        || (msg->lParam && ::lstrcmpiW(reinterpret_cast<LPCWSTR>(msg->lParam), L"ImmersiveColorSet") == 0))
        emit changed();
    return false;
}

} // namespace ws
