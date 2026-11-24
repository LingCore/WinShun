#include "PadReader.h"

#include <QEvent>
#include <QMouseEvent>

#include <cmath>
#include <utility>

namespace ws::gestureseditor {

namespace {

constexpr int kStrokeLogical = 24; // as the engine's: how far a direction takes

std::optional<gestures::MouseButton> fromQtButton(Qt::MouseButton button)
{
    switch (button) {
    case Qt::LeftButton: return gestures::MouseButton::Left;
    case Qt::RightButton: return gestures::MouseButton::Right;
    case Qt::MiddleButton: return gestures::MouseButton::Middle;
    case Qt::BackButton: return gestures::MouseButton::X1;
    case Qt::ForwardButton: return gestures::MouseButton::X2;
    default: return std::nullopt;
    }
}

} // namespace

PadReader::PadReader(QObject* parent)
    : QObject(parent)
{
    m_recognizer.reset(0, 0, kStrokeLogical);
}

PadReader::~PadReader()
{
    if (m_window)
        m_window->removeEventFilter(this);
}

void PadReader::setTarget(QQuickItem* target)
{
    if (m_target == target)
        return;
    if (m_target)
        disconnect(m_target, &QQuickItem::windowChanged, this, &PadReader::attachWindow);
    cancelRecording();
    m_target = target;
    if (m_target) {
        connect(m_target, &QQuickItem::windowChanged, this, &PadReader::attachWindow);
        attachWindow(m_target->window());
    } else {
        attachWindow(nullptr);
    }
    emit targetChanged();
}

void PadReader::attachWindow(QQuickWindow* window)
{
    if (m_window == window)
        return;
    if (m_window)
        m_window->removeEventFilter(this);
    cancelRecording();
    m_window = window;
    if (m_window)
        m_window->installEventFilter(this);
}

void PadReader::setDpr(double dpr)
{
    const double next = dpr > 0 ? dpr : 1.0;
    if (qFuzzyCompare(m_dpr, next))
        return;
    m_dpr = next;
    emit dprChanged();
}

void PadReader::setChinese(bool chinese)
{
    if (m_chinese == chinese)
        return;
    m_chinese = chinese;
    emit chineseChanged();
    emit changed();
}

void PadReader::setActive(bool active)
{
    if (m_active == active)
        return;
    m_active = active;
    if (!m_active)
        cancelRecording();
    emit activeChanged();
}

void PadReader::setPinTrigger(bool pin)
{
    if (m_pinTrigger == pin)
        return;
    m_pinTrigger = pin;
    emit pinTriggerChanged();
    emit changed();
}

void PadReader::setEnabledButtons(const QStringList& buttons)
{
    if (m_enabledButtons == buttons)
        return;
    m_enabledButtons = buttons;
    emit enabledButtonsChanged();
    emit changed();
}

bool PadReader::targetInteractive() const
{
    if (!m_active || !m_target)
        return false;
    for (const QQuickItem* item = m_target; item; item = item->parentItem()) {
        if (!item->isVisible() || !item->isEnabled())
            return false;
    }
    return true;
}

gestures::MouseButton PadReader::effectiveTrigger() const
{
    if (!gestures::isTriggerButton(m_primaryButton))
        return gestures::MouseButton::Any;
    if (m_pinTrigger)
        return m_primaryButton;
    // If the user chords with the single currently enabled trigger button (e.g.
    // only Right is enabled, user holds Middle + clicks Right), pin the held
    // trigger button so the recorded rule is reachable instead of "+r".
    if (m_chordButton != gestures::MouseButton::Any && m_enabledButtons.size() == 1
        && m_enabledButtons.front() == QString::fromLatin1(gestures::buttonId(m_chordButton)))
        return m_primaryButton;
    return gestures::MouseButton::Any;
}

std::string PadReader::buildGesture() const
{
    if (tooLong())
        return {};
    const std::string stroke = m_chordButton != gestures::MouseButton::Any ? m_chordStroke : m_recognizer.gesture();
    return gestures::formatGestureTrigger(effectiveTrigger(), stroke, m_chordButton);
}

QString PadReader::gesture() const
{
    return QString::fromStdString(buildGesture());
}

QString PadReader::arrows() const
{
    if (tooLong())
        return QString::fromUtf8(gestures::arrows(m_recognizer.gesture().substr(0, gestures::kMaxDirections)));
    const std::string built = buildGesture();
    if (built.empty())
        return {};
    return QString::fromStdWString(gestures::gestureBadge(built, m_chinese));
}

void PadReader::begin(double x, double y, double dpr)
{
    m_dpr = dpr > 0 ? dpr : 1.0;
    m_primaryButton = gestures::MouseButton::Right;
    m_chordButton = gestures::MouseButton::Any;
    m_chordStroke.clear();
    m_leftPrimaryChord = false;
    m_recognizer.reset(static_cast<int>(std::lround(x * m_dpr)), static_cast<int>(std::lround(y * m_dpr)),
        static_cast<int>(std::lround(kStrokeLogical * m_dpr)));
    emit changed();
}

void PadReader::add(double x, double y)
{
    if (m_chordButton == gestures::MouseButton::Any
        && m_recognizer.add(static_cast<int>(std::lround(x * m_dpr)), static_cast<int>(std::lround(y * m_dpr))))
        emit changed();
}

void PadReader::clear()
{
    m_drawing = false;
    m_completed = false;
    m_leftPrimaryChord = false;
    m_primaryButton = gestures::MouseButton::Any;
    m_chordButton = gestures::MouseButton::Any;
    m_chordStroke.clear();
    m_points.clear();
    m_pendingGesture.clear();
    m_pendingTooLong = false;
    m_recognizer.reset(0, 0, static_cast<int>(std::lround(kStrokeLogical * m_dpr)));
    emit changed();
}

void PadReader::cancelRecording()
{
    if (!m_drawing)
        return;
    clear();
}

bool PadReader::eventFilter(QObject* watched, QEvent* event)
{
    if (watched != m_window || !m_target)
        return false;

    if (event->type() == QEvent::WindowDeactivate || event->type() == QEvent::FocusOut) {
        cancelRecording();
        return false;
    }

    if (event->type() != QEvent::MouseButtonPress && event->type() != QEvent::MouseMove
        && event->type() != QEvent::MouseButtonRelease)
        return false;

    const auto* me = static_cast<const QMouseEvent*>(event);
    const QPointF local = m_target->mapFromScene(me->scenePosition());

    if (event->type() == QEvent::MouseButtonPress) {
        if (!m_drawing) {
            if (!targetInteractive() || !m_target->contains(local))
                return false;
            const auto btn = fromQtButton(me->button());
            if (!btn)
                return false;
            m_drawing = true;
            m_completed = false;
            m_leftPrimaryChord = false;
            m_primaryButton = *btn;
            m_chordButton = gestures::MouseButton::Any;
            m_chordStroke.clear();
            m_pendingGesture.clear();
            m_pendingTooLong = false;
            m_points = {QVariant::fromValue(local)};
            m_recognizer.reset(static_cast<int>(std::lround(local.x() * m_dpr)),
                static_cast<int>(std::lround(local.y() * m_dpr)),
                static_cast<int>(std::lround(kStrokeLogical * m_dpr)));
            emit changed();
            return true;
        }

        // A second button pressed while the primary button is held on the pad!
        const auto btn = fromQtButton(me->button());
        if (btn && *btn != m_primaryButton && m_chordButton == gestures::MouseButton::Any && !m_completed) {
            if (!gestures::isTriggerButton(m_primaryButton)) {
                m_leftPrimaryChord = true;
            } else if (!m_recognizer.tooLong()) {
                m_chordButton = *btn;
                m_chordStroke = m_recognizer.gesture();
                emit changed();
            }
        }
        return true;
    }

    if (!m_drawing)
        return false;

    if (event->type() == QEvent::MouseMove) {
        if (m_chordButton == gestures::MouseButton::Any && !m_completed) {
            const QPointF last = m_points.isEmpty() ? QPointF() : m_points.last().toPointF();
            if (m_points.isEmpty() || std::abs(local.x() - last.x()) + std::abs(local.y() - last.y()) >= 2.0) {
                m_points.append(QVariant::fromValue(local));
                m_recognizer.add(static_cast<int>(std::lround(local.x() * m_dpr)),
                    static_cast<int>(std::lround(local.y() * m_dpr)));
                emit changed();
            }
        }
        return true;
    }

    // QEvent::MouseButtonRelease
    const auto btn = fromQtButton(me->button());
    if (!m_completed && (!btn || *btn == m_primaryButton || *btn == m_chordButton)) {
        m_pendingTooLong = tooLong();
        m_pendingGesture = m_pendingTooLong ? QString() : QString::fromStdString(buildGesture());
        m_completed = true;
    }
    if (me->buttons() == Qt::NoButton) {
        const QString finalGesture = std::exchange(m_pendingGesture, {});
        const bool finalTooLong = std::exchange(m_pendingTooLong, false);
        const bool leftChord = std::exchange(m_leftPrimaryChord, false);
        clear();
        emit finished(finalGesture, finalTooLong, leftChord);
    }
    return true;
}

} // namespace ws::gestureseditor
