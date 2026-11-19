// The drawing pad's reading of a drag: the plugin's own recogniser
// (gestures::Recognizer), fed the pad's points in device pixels with the same
// stroke length (24 logical pixels) the engine uses, so that a gesture drawn
// here is read as it will be read on the desktop.
#pragma once

#include "Gesture.h"

#include <QObject>
#include <QString>
#include <QtQmlIntegration/qqmlintegration.h>

namespace ws::gestureseditor {

class PadReader : public QObject {
    Q_OBJECT
    QML_ELEMENT
    Q_PROPERTY(QString gesture READ gesture NOTIFY changed) // "DR", "cw"; empty while nothing is read
    Q_PROPERTY(QString arrows READ arrows NOTIFY changed) // "↓→"
    Q_PROPERTY(bool tooLong READ tooLong NOTIFY changed)

public:
    explicit PadReader(QObject* parent = nullptr);

    QString gesture() const;
    QString arrows() const;
    bool tooLong() const { return m_recognizer.tooLong(); }

    // A press at (x, y) logical pixels, on a screen of this pixel ratio.
    Q_INVOKABLE void begin(double x, double y, double dpr);
    Q_INVOKABLE void add(double x, double y);
    Q_INVOKABLE void clear();

signals:
    void changed();

private:
    gestures::Recognizer m_recognizer;
    double m_dpr = 1;
};

} // namespace ws::gestureseditor
