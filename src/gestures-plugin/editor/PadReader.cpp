#include "PadReader.h"

#include <cmath>

namespace ws::gestureseditor {

namespace {
constexpr int kStrokeLogical = 24; // as the engine's: how far a direction takes
}

PadReader::PadReader(QObject* parent)
    : QObject(parent)
{
    m_recognizer.reset(0, 0, kStrokeLogical);
}

QString PadReader::gesture() const
{
    if (m_recognizer.tooLong())
        return {};
    return QString::fromStdString(m_recognizer.gesture());
}

QString PadReader::arrows() const
{
    return QString::fromUtf8(gestures::arrows(m_recognizer.gesture()));
}

void PadReader::begin(double x, double y, double dpr)
{
    m_dpr = dpr > 0 ? dpr : 1;
    m_recognizer.reset(static_cast<int>(std::lround(x * m_dpr)), static_cast<int>(std::lround(y * m_dpr)),
        static_cast<int>(std::lround(kStrokeLogical * m_dpr)));
    emit changed();
}

void PadReader::add(double x, double y)
{
    if (m_recognizer.add(static_cast<int>(std::lround(x * m_dpr)), static_cast<int>(std::lround(y * m_dpr))))
        emit changed();
}

void PadReader::clear()
{
    m_recognizer.reset(0, 0, static_cast<int>(std::lround(kStrokeLogical * m_dpr)));
    emit changed();
}

} // namespace ws::gestureseditor
