#include "PinKeyCard.h"

#include <QDebug>
#include <QFont>
#include <QFontMetrics>
#include <QGuiApplication>
#include <QPainter>
#include <QPen>

#include <algorithm>
#include <cmath>

using namespace Qt::StringLiterals;

namespace ws::capture {

namespace {

const wchar_t* const kClassName = L"WinShun.PinKeys";
constexpr UINT_PTR kFadeTimer = 1;
constexpr int kFadeMs = 150; // in, as the capture's card; away at once
constexpr int kFadeTickMs = 16;

// Logical px to device px.
int px(double logical, double dpr)
{
    return static_cast<int>(std::lround(logical * dpr));
}

// The capture's card (CaptureKeyHints.qml), in device px.
struct Metrics {
    explicit Metrics(double dpr)
        : padX(px(12, dpr))
        , padY(px(10, dpr))
        , rowGap(px(6, dpr))
        , columnGap(px(12, dpr))
        , capHeight(px(20, dpr))
        , capPad(px(6, dpr))
        , capGap(px(4, dpr))
        , footerGap(px(8, dpr))
        , footerSpacing(px(4, dpr) + px(2, dpr)) // the "?" keycap to its words
        , edge(std::max(1, px(1, dpr)))
        , textFont(QGuiApplication::font())
        , capFont(QGuiApplication::font())
    {
        textFont.setPixelSize(px(14, dpr)); // Theme.fontCaption
        capFont.setPixelSize(px(12, dpr));
    }
    int capWidth(const QFontMetrics& caps, const QString& label) const
    {
        return std::max(capHeight, caps.horizontalAdvance(label) + 2 * capPad);
    }
    int keysWidth(const QFontMetrics& caps, const QStringList& keys) const
    {
        int width = 0;
        for (qsizetype i = 0; i < keys.size(); ++i)
            width += capWidth(caps, keys[i]) + (i > 0 ? 2 * capGap + caps.horizontalAdvance(u"/"_s) : 0);
        return width;
    }

    int padX, padY, rowGap, columnGap, capHeight, capPad, capGap, footerGap, footerSpacing, edge;
    QFont textFont;
    QFont capFont;
};

QString hideText()
{
    return QCoreApplication::translate("CaptureKeyHints", "隐藏提示"); // the capture card's words
}

// The card's parts across: the keys' column and the words' column, and the footer.
struct Columns {
    int keys = 0;
    int text = 0;
    int footer = 0;
    int inner() const { return std::max(keys + text, footer); }
};

Columns columns(const std::vector<PinKeyRow>& rows, const Metrics& m, const QString& offText)
{
    const QFontMetrics caps(m.capFont);
    const QFontMetrics words(m.textFont);
    Columns c;
    for (const PinKeyRow& row : rows) {
        c.keys = std::max(c.keys, m.keysWidth(caps, row.keys));
        c.text = std::max(c.text, words.horizontalAdvance(row.text));
    }
    c.keys += m.columnGap;
    c.footer = m.capWidth(caps, u"?"_s) + m.footerSpacing + caps.horizontalAdvance(hideText()) + m.columnGap
        + caps.horizontalAdvance(offText);
    return c;
}

} // namespace

std::vector<PinKeyRow> pinKeyRows(const CaptureKeys& keys, bool ocr, bool otherSide, bool translationShown)
{
    // The menu's words (PinManager), so they read the same and are translated already.
    const auto menu = [](const char* text) { return QCoreApplication::translate("ws::capture::PinManager", text); };
    std::vector<PinKeyRow> rows;
    const auto add = [&](std::initializer_list<KeyAction> actions, const QString& text) {
        QStringList caps;
        for (const KeyAction action : actions) {
            if (keys.has(action))
                caps.append(keys.text(action));
        }
        if (!caps.isEmpty())
            rows.push_back({caps, text});
    };
    add({KeyAction::PinEdit}, menu(QT_TRANSLATE_NOOP("ws::capture::PinManager", "编辑")));
    add({KeyAction::PinCopy}, menu(QT_TRANSLATE_NOOP("ws::capture::PinManager", "复制")));
    add({KeyAction::PinSave}, menu(QT_TRANSLATE_NOOP("ws::capture::PinManager", "保存到“屏幕截图”")));
    if (ocr) {
        add({KeyAction::PinOcr}, menu(QT_TRANSLATE_NOOP("ws::capture::PinManager", "识别文字")));
        add({KeyAction::PinOcrCopy}, menu(QT_TRANSLATE_NOOP("ws::capture::PinManager", "复制全部文字")));
        add({KeyAction::PinTextPick}, menu(QT_TRANSLATE_NOOP("ws::capture::PinManager", "选择文字")));
    }
    if (otherSide) {
        rows.push_back({{u"Tab"_s}, translationShown ? menu(QT_TRANSLATE_NOOP("ws::capture::PinManager", "显示原文"))
                                                     : menu(QT_TRANSLATE_NOOP("ws::capture::PinManager", "显示译文"))});
    }
    add({KeyAction::PinRotateRight, KeyAction::PinFlipHorizontal, KeyAction::PinFlipVertical},
        menu(QT_TRANSLATE_NOOP("ws::capture::PinManager", "旋转与翻转")));
    const QString wheel = QCoreApplication::translate("CaptureKeyHints", "滚轮"); // the capture card's word
    const QString ctrlWheel = PinKeyCard::tr("Ctrl+滚轮");
    rows.push_back({{wheel, ctrlWheel}, PinKeyCard::tr("缩放 / 透明度")});
    add({KeyAction::PinCollapse}, menu(QT_TRANSLATE_NOOP("ws::capture::PinManager", "收成小块")));
    add({KeyAction::PinClose}, menu(QT_TRANSLATE_NOOP("ws::capture::PinManager", "关闭")));
    return rows;
}

QRect pinKeyCardPlace(QSize card, const QRect& work, const QRect& avoid, int margin, int gap)
{
    const int y = work.top() + work.height() - margin - card.height();
    const QRect left(work.left() + margin, y, card.width(), card.height());
    const QRect right(work.left() + work.width() - margin - card.width(), y, card.width(), card.height());
    const auto blocked = [&](const QRect& corner) { return corner.adjusted(-gap, -gap, gap, gap).intersects(avoid); };
    if (!blocked(left))
        return left;
    if (!blocked(right))
        return right;
    return {};
}

PinKeyCard::Look PinKeyCard::Look::fallback(bool dark)
{
    // Fluent's (themes/fluent/Look.qml): what a host without SystemTheme gets.
    Look look;
    look.background = dark ? QColor(0x2C, 0x2C, 0x2C) : QColor(0xF9, 0xF9, 0xF9);
    look.edge = dark ? QColor(255, 255, 255, 23) : QColor(0, 0, 0, 26);
    look.text = dark ? QColor(0xF3, 0xF3, 0xF3) : QColor(0x1B, 0x1B, 0x1B);
    look.subtext = dark ? QColor(0xB8, 0xB8, 0xB8) : QColor(0x66, 0x66, 0x66);
    look.faint = QColor(0x8A, 0x8A, 0x8A);
    look.divider = dark ? QColor(255, 255, 255, 21) : QColor(0, 0, 0, 15);
    look.keyEdge = dark ? QColor(255, 255, 255, 43) : QColor(0, 0, 0, 36);
    look.keyText = look.text;
    look.shadow = dark ? QColor(0, 0, 0, 110) : QColor(0, 0, 0, 46);
    return look;
}

int PinKeyCard::shadowMargin(double dpr)
{
    return px(8, dpr);
}

QSize PinKeyCard::cardSize(const std::vector<PinKeyRow>& rows, double dpr)
{
    const Metrics m(dpr);
    const Columns c = columns(rows, m, tr("截图设置里可关闭"));
    const int count = static_cast<int>(rows.size());
    const int grid = count * m.capHeight + std::max(0, count - 1) * m.rowGap;
    return QSize(c.inner() + 2 * m.padX, grid + 2 * m.footerGap + m.edge + m.capHeight + 2 * m.padY);
}

QImage PinKeyCard::picture(const std::vector<PinKeyRow>& rows, double dpr, const Look& look)
{
    const int s = shadowMargin(dpr);
    const QSize card = cardSize(rows, dpr);
    QImage canvas(card.width() + 2 * s, card.height() + 2 * s, QImage::Format_ARGB32_Premultiplied);
    paint(canvas, rows, dpr, look);
    return canvas;
}

void PinKeyCard::paint(QImage& canvas, const std::vector<PinKeyRow>& rows, double dpr, const Look& look)
{
    const Metrics m(dpr);
    const QString offText = tr("截图设置里可关闭");
    const Columns c = columns(rows, m, offText);
    const int s = shadowMargin(dpr);
    const QRect card(s, s, canvas.width() - 2 * s, canvas.height() - 2 * s);
    const qreal radius = std::max<qreal>(0, look.radius) * dpr;
    const qreal keyRadius = std::max<qreal>(0, look.keyRadius) * dpr;
    const qreal half = m.edge / 2.0;
    canvas.fill(Qt::transparent);
    QPainter painter(&canvas);
    painter.setRenderHint(QPainter::Antialiasing, true);
    painter.setRenderHint(QPainter::TextAntialiasing, true);

    // A soft shadow close under it, as the menus' (fading out quadratically).
    painter.setCompositionMode(QPainter::CompositionMode_Source);
    painter.setPen(Qt::NoPen);
    const int drop = px(2, dpr);
    for (int i = s; i >= 1; --i) {
        const double f = static_cast<double>(s - i + 1) / (s + 1);
        QColor shade = look.shadow;
        shade.setAlphaF(look.shadow.alphaF() * f * f);
        painter.setBrush(shade);
        painter.drawRoundedRect(QRectF(card).adjusted(-i, -i + drop, i, i + drop), radius + i, radius + i);
    }
    painter.setCompositionMode(QPainter::CompositionMode_Source);
    painter.setBrush(look.background);
    painter.drawRoundedRect(QRectF(card), radius, radius);
    painter.setCompositionMode(QPainter::CompositionMode_SourceOver);
    painter.setBrush(Qt::NoBrush);
    painter.setPen(QPen(look.edge, m.edge));
    painter.drawRoundedRect(QRectF(card).adjusted(half, half, -half, -half), std::max<qreal>(0, radius - half),
        std::max<qreal>(0, radius - half));

    const QFontMetrics caps(m.capFont);
    const int slash = caps.horizontalAdvance(u"/"_s);
    const auto drawCap = [&](int x, int y, const QString& label) {
        const int w = m.capWidth(caps, label);
        painter.setPen(QPen(look.keyEdge, m.edge));
        painter.setBrush(Qt::NoBrush);
        painter.drawRoundedRect(QRectF(x, y, w, m.capHeight).adjusted(half, half, -half, -half),
            std::max<qreal>(0, keyRadius - half), std::max<qreal>(0, keyRadius - half));
        painter.setFont(m.capFont);
        painter.setPen(look.keyText);
        painter.drawText(QRect(x, y, w, m.capHeight), Qt::AlignCenter, label);
        return w;
    };

    const int left = card.left() + m.padX;
    int y = card.top() + m.padY;
    for (const PinKeyRow& row : rows) {
        int x = left;
        for (qsizetype i = 0; i < row.keys.size(); ++i) {
            if (i > 0) {
                x += m.capGap;
                painter.setFont(m.capFont);
                painter.setPen(look.faint);
                painter.drawText(QRect(x, y, slash, m.capHeight), Qt::AlignCenter, u"/"_s);
                x += slash + m.capGap;
            }
            x += drawCap(x, y, row.keys[i]);
        }
        painter.setFont(m.textFont);
        painter.setPen(look.text);
        painter.drawText(QRect(left + c.keys, y, c.text + m.padX, m.capHeight), Qt::AlignLeft | Qt::AlignVCenter, row.text);
        y += m.capHeight + m.rowGap;
    }
    y += m.footerGap - (rows.empty() ? 0 : m.rowGap);
    painter.fillRect(QRect(left, y, c.inner(), m.edge), look.divider); // the line above the footer (the style's separator)
    y += m.edge + m.footerGap;
    // ? 隐藏提示, and on the right that the settings turn it off (nothing here takes a click).
    const int x = left + drawCap(left, y, u"?"_s) + m.footerSpacing;
    painter.setFont(m.capFont);
    painter.setPen(look.subtext);
    painter.drawText(QRect(x, y, caps.horizontalAdvance(hideText()) + m.capPad, m.capHeight), Qt::AlignLeft | Qt::AlignVCenter,
        hideText());
    painter.setPen(look.faint);
    const int offWidth = caps.horizontalAdvance(offText);
    painter.drawText(QRect(left + c.inner() - offWidth, y, offWidth + m.capPad, m.capHeight), Qt::AlignLeft | Qt::AlignVCenter,
        offText);
}

PinKeyCard::~PinKeyCard()
{
    if (m_hwnd) {
        ::SetWindowLongPtrW(m_hwnd, GWLP_USERDATA, 0);
        ::DestroyWindow(m_hwnd);
    }
    if (m_dc) {
        ::SelectObject(m_dc, m_oldBitmap);
        ::DeleteObject(m_bitmap);
        ::DeleteDC(m_dc);
    }
}

bool PinKeyCard::create()
{
    if (m_hwnd)
        return true;
    static const bool registered = [] {
        WNDCLASSEXW wc {sizeof wc};
        wc.lpfnWndProc = &PinKeyCard::proc;
        wc.hInstance = ::GetModuleHandleW(nullptr);
        wc.hCursor = ::LoadCursorW(nullptr, IDC_ARROW);
        wc.lpszClassName = kClassName;
        return ::RegisterClassExW(&wc) != 0;
    }();
    Q_UNUSED(registered);
    // Per-monitor aware: it is drawn at its monitor's scale, in physical px.
    const DPI_AWARENESS_CONTEXT previous = ::SetThreadDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    m_hwnd = ::CreateWindowExW(WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_TOOLWINDOW | WS_EX_TOPMOST | WS_EX_NOACTIVATE, kClassName,
        L"Win顺 贴图按键", WS_POPUP, 0, 0, 1, 1, nullptr, nullptr, ::GetModuleHandleW(nullptr), nullptr);
    if (previous)
        ::SetThreadDpiAwarenessContext(previous);
    if (!m_hwnd) {
        qWarning() << "Screen capture: no pin key card, error" << ::GetLastError();
        return false;
    }
    ::SetWindowLongPtrW(m_hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(this));
    return true;
}

bool PinKeyCard::show(const std::vector<PinKeyRow>& rows, const QRect& work, const QRect& avoid, double dpr, const Look& look)
{
    const QSize card = cardSize(rows, dpr);
    const QRect place = rows.empty() ? QRect() : pinKeyCardPlace(card, work, avoid, px(16, dpr), px(12, dpr));
    if (place.isEmpty() || !create()) {
        hide();
        return false;
    }
    const int s = shadowMargin(dpr);
    m_rect = place;
    const QRect window = place.adjusted(-s, -s, s, s);
    if (window.size() != m_windowRect.size() || !m_dc) {
        if (m_dc) {
            ::SelectObject(m_dc, m_oldBitmap);
            ::DeleteObject(m_bitmap);
        } else {
            m_dc = ::CreateCompatibleDC(nullptr);
        }
        BITMAPINFO info {};
        info.bmiHeader.biSize = sizeof info.bmiHeader;
        info.bmiHeader.biWidth = window.width();
        info.bmiHeader.biHeight = -window.height(); // top-down, as QImage
        info.bmiHeader.biPlanes = 1;
        info.bmiHeader.biBitCount = 32;
        info.bmiHeader.biCompression = BI_RGB;
        void* bits = nullptr;
        m_bitmap = ::CreateDIBSection(nullptr, &info, DIB_RGB_COLORS, &bits, nullptr, 0);
        if (!m_bitmap) {
            ::DeleteDC(m_dc);
            m_dc = nullptr;
            m_bits = nullptr;
            m_windowRect = {};
            hide();
            return false;
        }
        m_oldBitmap = ::SelectObject(m_dc, m_bitmap);
        m_bits = static_cast<uchar*>(bits);
    }
    {
        // The DIB as a QImage (BGRA in memory = ARGB32 on little endian), no copy.
        QImage canvas(m_bits, window.width(), window.height(), window.width() * 4, QImage::Format_ARGB32_Premultiplied);
        paint(canvas, rows, dpr, look);
    }
    m_windowRect = window;
    const bool appearing = !visible();
    if (appearing)
        m_alpha = 0;
    present(static_cast<BYTE>(m_alpha));
    if (appearing) {
        ::ShowWindow(m_hwnd, SW_SHOWNOACTIVATE);
        // Over the pins (topmost too): the one shown last is on top.
        ::SetWindowPos(m_hwnd, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
        ::SetTimer(m_hwnd, kFadeTimer, kFadeTickMs, nullptr);
    }
    return true;
}

void PinKeyCard::present(BYTE alpha)
{
    if (!m_hwnd || !m_dc)
        return;
    POINT to {m_windowRect.x(), m_windowRect.y()};
    SIZE size {m_windowRect.width(), m_windowRect.height()};
    POINT from {0, 0};
    BLENDFUNCTION blend {AC_SRC_OVER, 0, alpha, AC_SRC_ALPHA};
    if (!::UpdateLayeredWindow(m_hwnd, nullptr, &to, &size, m_dc, &from, 0, &blend, ULW_ALPHA))
        qWarning() << "Screen capture: pin key card UpdateLayeredWindow failed, error" << ::GetLastError();
}

void PinKeyCard::hide()
{
    if (!m_hwnd)
        return;
    ::KillTimer(m_hwnd, kFadeTimer);
    ::ShowWindow(m_hwnd, SW_HIDE);
}

bool PinKeyCard::visible() const
{
    return m_hwnd && ::IsWindowVisible(m_hwnd);
}

LRESULT CALLBACK PinKeyCard::proc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam)
{
    auto* self = reinterpret_cast<PinKeyCard*>(::GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    if (message == WM_MOUSEACTIVATE)
        return MA_NOACTIVATE;
    if (self && message == WM_TIMER && wParam == kFadeTimer) {
        self->m_alpha = std::min(255, self->m_alpha + 255 * kFadeTickMs / kFadeMs + 1);
        self->present(static_cast<BYTE>(self->m_alpha));
        if (self->m_alpha >= 255)
            ::KillTimer(hwnd, kFadeTimer);
        return 0;
    }
    return ::DefWindowProcW(hwnd, message, wParam, lParam);
}

} // namespace ws::capture
