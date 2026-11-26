// The drawing pad's reading of a drag or multi-button combo: uses the
// plugin's own recogniser (gestures::Recognizer) in device pixels with the
// same stroke length (24 logical pixels) as Engine, and installs a window
// event filter so multi-button chords (e.g. hold Right + click Left, or
// hold Right + drag Down + click Left) are captured directly on the pad —
// Qt Quick's MouseArea cannot report a second button press while one is held.
#pragma once

#include "Gesture.h"

#include <QObject>
#include <QPointF>
#include <QPointer>
#include <QQuickItem>
#include <QQuickWindow>
#include <QString>
#include <QStringList>
#include <QVariantList>
#include <QtQmlIntegration/qqmlintegration.h>

namespace ws::gestureseditor {

class PadReader : public QObject {
    Q_OBJECT
    QML_ELEMENT
    Q_PROPERTY(QQuickItem* target READ target WRITE setTarget NOTIFY targetChanged)
    Q_PROPERTY(double dpr READ dpr WRITE setDpr NOTIFY dprChanged)
    Q_PROPERTY(bool chinese READ chinese WRITE setChinese NOTIFY chineseChanged)
    Q_PROPERTY(bool active READ active WRITE setActive NOTIFY activeChanged)
    // When true (multi-button mode), recorded gestures are scoped to the trigger
    // button physically held on the pad ("x1:D", "r:+l"); when false, trigger-agnostic.
    Q_PROPERTY(bool pinTrigger READ pinTrigger WRITE setPinTrigger NOTIFY pinTriggerChanged)
    Q_PROPERTY(QStringList enabledButtons READ enabledButtons WRITE setEnabledButtons NOTIFY enabledButtonsChanged)
    Q_PROPERTY(bool drawing READ drawing NOTIFY changed)
    Q_PROPERTY(QVariantList points READ points NOTIFY changed)
    Q_PROPERTY(QString gesture READ gesture NOTIFY changed) // "DR", "cw", "D+l", "+l", "x1:U", "shift:DR"
    Q_PROPERTY(QString arrows READ arrows NOTIFY changed) // localized live badge: "↓→", "↓+左", "侧1 ↑", "Shift ↓→"
    Q_PROPERTY(bool tooLong READ tooLong NOTIFY changed)
    Q_PROPERTY(bool hasChord READ hasChord NOTIFY changed)

public:
    explicit PadReader(QObject* parent = nullptr);
    ~PadReader() override;

    QQuickItem* target() const { return m_target; }
    void setTarget(QQuickItem* target);
    double dpr() const { return m_dpr; }
    void setDpr(double dpr);
    bool chinese() const { return m_chinese; }
    void setChinese(bool chinese);
    bool active() const { return m_active; }
    void setActive(bool active);
    bool pinTrigger() const { return m_pinTrigger; }
    void setPinTrigger(bool pin);
    QStringList enabledButtons() const { return m_enabledButtons; }
    void setEnabledButtons(const QStringList& buttons);

    bool drawing() const { return m_drawing; }
    QVariantList points() const { return m_points; }
    QString gesture() const;
    QString arrows() const;
    bool tooLong() const { return m_chordButton == gestures::MouseButton::Any && m_recognizer.tooLong(); }
    bool hasChord() const { return m_chordButton != gestures::MouseButton::Any; }

    Q_INVOKABLE void begin(double x, double y, double dpr);
    Q_INVOKABLE void add(double x, double y);
    Q_INVOKABLE void clear();

signals:
    void targetChanged();
    void dprChanged();
    void chineseChanged();
    void activeChanged();
    void pinTriggerChanged();
    void enabledButtonsChanged();
    void changed();
    // Emitted once all pressed buttons are released after a recording ends.
    // `leftPrimaryChord` is true if the user tried to start a multi-button chord
    // with the left button (which is only supported as the second button);
    // `winHeld` if Win was held as it began (no gesture then: Win is Windows').
    void finished(const QString& gesture, bool tooLong, bool leftPrimaryChord, bool winHeld);

protected:
    bool eventFilter(QObject* watched, QEvent* event) override;

private:
    void attachWindow(QQuickWindow* window);
    bool targetInteractive() const;
    gestures::MouseButton effectiveTrigger() const;
    std::string buildGesture() const;
    void cancelRecording();

    QPointer<QQuickItem> m_target;
    QPointer<QQuickWindow> m_window;
    gestures::Recognizer m_recognizer;
    double m_dpr = 1.0;
    bool m_chinese = true;
    bool m_active = true;
    bool m_pinTrigger = false;
    QStringList m_enabledButtons {QStringLiteral("right")};

    bool m_drawing = false;
    bool m_completed = false;
    bool m_leftPrimaryChord = false;
    gestures::Modifiers m_modifiers = 0; // Ctrl, Alt, Shift held as the primary button went down
    bool m_winHeld = false;
    gestures::MouseButton m_primaryButton = gestures::MouseButton::Any;
    gestures::MouseButton m_chordButton = gestures::MouseButton::Any;
    std::string m_chordStroke;
    QVariantList m_points;
    QString m_pendingGesture;
    bool m_pendingTooLong = false;
};

} // namespace ws::gestureseditor
