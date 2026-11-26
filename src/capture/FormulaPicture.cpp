#include "FormulaPicture.h"

#include "graphic/font_style.h"
#include "graphic/graphic.h"
#include "microtex.h"
#include "utils/exceptions.h"

#include <QBuffer>
#include <QFile>
#include <QFont>
#include <QFontMetricsF>
#include <QPainter>
#include <QPainterPath>
#include <QRegularExpression>
#include <QSvgGenerator>
#include <QtMath>

#include <mutex>

using namespace Qt::StringLiterals;

namespace ws::capture {

namespace {

namespace tex = microtex;

std::mutex& texLock()
{
    static std::mutex lock;
    return lock;
}

QColor toQColor(tex::color c)
{
    return QColor(static_cast<int>(tex::color_r(c)), static_cast<int>(tex::color_g(c)),
        static_cast<int>(tex::color_b(c)), static_cast<int>(tex::color_a(c)));
}

// The glyphs are outlines from the .clm2 (GLYPH_RENDER_TYPE 1): no font is
// ever drawn with, so a font is only something to compare.
class NoFont final : public tex::Font {
public:
    bool operator==(const tex::Font& other) const override { return this == &other; }
};

class Painter2D;

// Words in \text{} and letters the math font lacks (汉字): Windows' fonts,
// a serif one as TeX's text is, with a Chinese one after it.
class Words final : public tex::TextLayout {
public:
    Words(const std::string& text, tex::FontStyle style, float size)
        : m_text(QString::fromStdString(text))
    {
        if (tex::isMono(style))
            m_font.setFamilies({u"Consolas"_s, u"SimSun"_s});
        else if (tex::isSansSerif(style))
            m_font.setFamilies({u"Segoe UI"_s, u"Microsoft YaHei UI"_s});
        else
            m_font.setFamilies({u"Times New Roman"_s, u"SimSun"_s});
        m_font.setPixelSize(std::max(1, qRound(size)));
        m_font.setBold(tex::isBold(style));
        m_font.setItalic(tex::isItalic(style));
    }

    // Line metrics, not the ink's: words line up on the baseline as text does.
    void getBounds(tex::Rect& bounds) override
    {
        const QFontMetricsF metrics(m_font);
        bounds.x = 0;
        bounds.y = static_cast<float>(-metrics.ascent());
        bounds.w = static_cast<float>(metrics.horizontalAdvance(m_text));
        bounds.h = static_cast<float>(metrics.ascent() + metrics.descent());
    }

    void draw(tex::Graphics2D& g2, float x, float y) override;

private:
    QString m_text;
    QFont m_font;
};

class Factory final : public tex::PlatformFactory {
public:
    tex::sptr<tex::Font> createFont(const std::string&) override { return std::make_shared<NoFont>(); }
    tex::sptr<tex::TextLayout> createTextLayout(const std::string& text, tex::FontStyle style, float size) override
    {
        return std::make_shared<Words>(text, style, size);
    }
};

// MicroTeX's drawing on a QPainter, in the painter's own coordinates.
class Painter2D final : public tex::Graphics2D {
public:
    explicit Painter2D(QPainter& painter)
        : m_painter(painter)
        , m_base(painter.transform())
    {
        m_pen.setCapStyle(Qt::RoundCap);
        m_pen.setJoinStyle(Qt::RoundJoin);
        setColor(tex::black);
    }

    QPainter& painter() { return m_painter; }

    void setColor(tex::color c) override
    {
        m_color = c;
        m_pen.setColor(toQColor(c));
        m_painter.setPen(m_pen);
    }
    tex::color getColor() const override { return m_color; }

    void setStroke(const tex::Stroke& s) override
    {
        m_stroke = s;
        m_pen.setWidthF(s.lineWidth);
        m_pen.setCapStyle(s.cap == tex::CAP_ROUND ? Qt::RoundCap
                : s.cap == tex::CAP_SQUARE        ? Qt::SquareCap
                                                  : Qt::FlatCap);
        m_pen.setJoinStyle(s.join == tex::JOIN_ROUND ? Qt::RoundJoin
                : s.join == tex::JOIN_BEVEL          ? Qt::BevelJoin
                                                     : Qt::MiterJoin);
        if (s.miterLimit > 0)
            m_pen.setMiterLimit(s.miterLimit);
        m_painter.setPen(m_pen);
    }
    const tex::Stroke& getStroke() const override { return m_stroke; }

    void setStrokeWidth(float width) override
    {
        m_stroke.lineWidth = width;
        m_pen.setWidthF(width);
        m_painter.setPen(m_pen);
    }

    // Qt's dashes are in pen widths, MicroTeX's in pixels.
    void setDash(const std::vector<float>& dash) override
    {
        m_dash = dash;
        if (dash.empty()) {
            m_pen.setStyle(Qt::SolidLine);
        } else {
            const qreal width = std::max<qreal>(m_pen.widthF(), 1e-3);
            QList<qreal> pattern;
            pattern.reserve(static_cast<qsizetype>(dash.size()));
            for (const float d : dash)
                pattern.push_back(d / width);
            m_pen.setDashPattern(pattern);
        }
        m_painter.setPen(m_pen);
    }
    std::vector<float> getDash() override { return m_dash; }

    tex::sptr<tex::Font> getFont() const override { return m_font; }
    void setFont(const tex::sptr<tex::Font>& font) override { m_font = font; }
    float getFontSize() const override { return m_fontSize; }
    void setFontSize(float size) override { m_fontSize = size; }

    void translate(float dx, float dy) override { m_painter.translate(dx, dy); }
    void scale(float sx, float sy) override
    {
        m_sx *= sx;
        m_sy *= sy;
        m_painter.scale(sx, sy);
    }
    void rotate(float angle) override { m_painter.rotate(qRadiansToDegrees(angle)); }
    void rotate(float angle, float px, float py) override
    {
        m_painter.translate(px, py);
        m_painter.rotate(qRadiansToDegrees(angle));
        m_painter.translate(-px, -py);
    }
    void reset() override
    {
        m_painter.setTransform(m_base);
        m_sx = m_sy = 1;
    }
    float sx() const override { return m_sx; }
    float sy() const override { return m_sy; }

    void drawGlyph(tex::u16, float, float) override { } // outlines only

    bool beginPath(tex::i32) override
    {
        m_path = QPainterPath();
        return false; // nothing kept: draw it
    }
    void moveTo(float x, float y) override { m_path.moveTo(x, y); }
    void lineTo(float x, float y) override { m_path.lineTo(x, y); }
    void cubicTo(float x1, float y1, float x2, float y2, float x3, float y3) override
    {
        m_path.cubicTo(x1, y1, x2, y2, x3, y3);
    }
    void quadTo(float x1, float y1, float x2, float y2) override { m_path.quadTo(x1, y1, x2, y2); }
    void closePath() override { m_path.closeSubpath(); }
    void fillPath(tex::i32) override { m_painter.fillPath(m_path, toQColor(m_color)); }

    void drawLine(float x1, float y1, float x2, float y2) override
    {
        m_painter.drawLine(QPointF(x1, y1), QPointF(x2, y2));
    }
    void drawRect(float x, float y, float w, float h) override
    {
        m_painter.setBrush(Qt::NoBrush);
        m_painter.drawRect(QRectF(x, y, w, h));
    }
    void fillRect(float x, float y, float w, float h) override
    {
        m_painter.fillRect(QRectF(x, y, w, h), toQColor(m_color));
    }
    void drawRoundRect(float x, float y, float w, float h, float rx, float ry) override
    {
        m_painter.setBrush(Qt::NoBrush);
        m_painter.drawRoundedRect(QRectF(x, y, w, h), rx, ry);
    }
    void fillRoundRect(float x, float y, float w, float h, float rx, float ry) override
    {
        m_painter.setPen(Qt::NoPen);
        m_painter.setBrush(toQColor(m_color));
        m_painter.drawRoundedRect(QRectF(x, y, w, h), rx, ry);
        m_painter.setBrush(Qt::NoBrush);
        m_painter.setPen(m_pen);
    }

private:
    QPainter& m_painter;
    QTransform m_base;
    QPen m_pen;
    QPainterPath m_path;
    tex::color m_color = tex::black;
    tex::Stroke m_stroke;
    std::vector<float> m_dash;
    tex::sptr<tex::Font> m_font;
    float m_fontSize = 1;
    float m_sx = 1;
    float m_sy = 1;
};

void Words::draw(tex::Graphics2D& g2, float x, float y)
{
    QPainter& painter = static_cast<Painter2D&>(g2).painter();
    painter.save();
    painter.setFont(m_font);
    painter.setPen(toQColor(g2.getColor()));
    painter.drawText(QPointF(x, y), m_text);
    painter.restore();
}

// MicroTeX with Latin Modern Math, once; the reason when it could not start.
// Under texLock().
const QString& start()
{
    static const QString problem = []() -> QString {
        static QByteArray font; // MicroTeX reads it as long as it runs
        QFile file(u":/microtex/latinmodern-math.clm2"_s);
        if (!file.open(QIODevice::ReadOnly))
            return u"The math font is not in the program"_s;
        font = file.readAll();
        try {
            const tex::FontSrcData source(static_cast<size_t>(font.size()),
                reinterpret_cast<const tex::u8*>(font.constData()));
            tex::MicroTeX::init(source);
            tex::PlatformFactory::registerFactory("winshun", std::make_unique<Factory>());
            tex::PlatformFactory::activate("winshun");
            // amsmath's \operatorname* (a name with limits under it, argmax),
            // which MicroTeX lacks: by its older name, which formulas are
            // given in (FormulaPicture's constructor).
            delete tex::MicroTeX::parse("\\newcommand{\\operatornamewithlimits}[1]{\\mathop{\\mathrm{#1}}}", 0, 20,
                5, tex::black);
        } catch (const std::exception& e) {
            return QString::fromUtf8(e.what());
        }
        return {};
    }();
    return problem;
}

} // namespace

FormulaPicture::FormulaPicture(const QString& latex, qreal size, const QColor& color)
{
    if (latex.trimmed().isEmpty())
        return;
    const std::lock_guard lock(texLock());
    if (m_error = start(); !m_error.isEmpty())
        return;
    static const QRegularExpression starred(uR"(\\operatorname\s*\*)"_s);
    const QString source = QString(latex).replace(starred, u"\\operatornamewithlimits"_s);
    try {
        // As a formula on its own line: a sum's limits under and over it.
        m_render.reset(tex::MicroTeX::parse(source.toStdString(), 0, static_cast<float>(size),
            static_cast<float>(size / 3), tex::argb(color.alpha(), color.red(), color.green(), color.blue()), false,
            {true, tex::TexStyle::display}));
    } catch (const std::exception& e) {
        m_error = QString::fromUtf8(e.what()).trimmed();
        return;
    }
    if (!m_render) {
        m_error = u"Nothing to draw"_s;
        return;
    }
    // Whole pixels, rounded down by MicroTeX: one more so no ink is cut.
    m_size = QSizeF(m_render->getWidth() + 1, m_render->getHeight() + 1);
}

FormulaPicture::~FormulaPicture()
{
    if (m_render) {
        const std::lock_guard lock(texLock());
        m_render.reset();
    }
}

FormulaPicture::FormulaPicture(FormulaPicture&&) noexcept = default;
FormulaPicture& FormulaPicture::operator=(FormulaPicture&& other) noexcept
{
    if (this != &other) {
        if (m_render) {
            const std::lock_guard lock(texLock());
            m_render.reset();
        }
        m_render = std::move(other.m_render);
        m_error = std::move(other.m_error);
        m_size = other.m_size;
    }
    return *this;
}

void FormulaPicture::draw(QPainter& painter, QPointF topLeft) const
{
    if (!m_render)
        return;
    const std::lock_guard lock(texLock());
    painter.save();
    painter.setRenderHints(QPainter::Antialiasing | QPainter::TextAntialiasing);
    painter.translate(topLeft);
    Painter2D g2(painter);
    m_render->draw(g2, 0, 0);
    painter.restore();
}

QImage FormulaPicture::image(qreal scale, qreal margin, const QColor& background) const
{
    if (!m_render)
        return {};
    const QSize pixels(qCeil((m_size.width() + 2 * margin) * scale), qCeil((m_size.height() + 2 * margin) * scale));
    QImage picture(pixels, QImage::Format_ARGB32_Premultiplied);
    picture.setDevicePixelRatio(scale);
    picture.fill(background);
    QPainter painter(&picture);
    draw(painter, QPointF(margin, margin));
    painter.end();
    return picture;
}

QByteArray FormulaPicture::svg(qreal margin) const
{
    if (!m_render)
        return {};
    QBuffer buffer;
    buffer.open(QIODevice::WriteOnly);
    const QSizeF size(m_size.width() + 2 * margin, m_size.height() + 2 * margin);
    QSvgGenerator generator;
    generator.setOutputDevice(&buffer);
    generator.setSize(size.toSize());
    generator.setViewBox(QRectF(QPointF(0, 0), size));
    generator.setResolution(96);
    QPainter painter(&generator);
    draw(painter, QPointF(margin, margin));
    painter.end();
    return buffer.data();
}

} // namespace ws::capture
