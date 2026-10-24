#pragma once

#include "CommandLine.h"

#include <QObject>
#include <QString>
#include <QVariantMap>
#include <QtQml/qqmlregistration.h>

#include <functional>

namespace ws {

class SettingsEditor;

// Settings → AI 助手 → 命令行: whether the command line ws can be used from a
// terminal (ws.exe there, its folder on the PATH, no other ws in front) and
// the one button that sets it up. The PATH is read and written off the GUI
// thread (a PATH entry may be a disconnected drive); the answer comes back
// as `state`.
class CliSetup : public QObject {
    Q_OBJECT
    QML_ELEMENT
    QML_UNCREATABLE("Provided by the settings editor")
    // cli::toVariant(), and `checked`: false until the first look is done.
    Q_PROPERTY(QVariantMap state READ state NOTIFY stateChanged FINAL)
    Q_PROPERTY(QString message READ message NOTIFY messageChanged FINAL) // what came of the last button
    Q_PROPERTY(bool busy READ busy NOTIFY busyChanged FINAL) // the PATH is being written
    Q_PROPERTY(QString example READ example CONSTANT FINAL) // a command to try, in the UI language

public:
    explicit CliSetup(SettingsEditor* editor, QObject* parent = nullptr);

    QVariantMap state() const { return m_state; }
    QString message() const { return m_message; }
    bool busy() const { return m_busy; }
    QString example() const;

    // Looks again (the settings window comes to the front, or the page is shown).
    Q_INVOKABLE void refresh();
    // The connection switch on, and bin on the user's PATH, whichever is missing.
    Q_INVOKABLE void enable();
    // Takes bin off the user's PATH again: only the entry WinShun put there (cli::removeFromUserPath).
    Q_INVOKABLE void removeFromPath();
    Q_INVOKABLE void clearMessage() { setMessage({}); }

    // The look is made by `probe` instead, at once on the calling thread, and
    // the registry is not written (the preview runs on this PC's real one).
    void setProbe(std::function<cli::Status()> probe) { m_probe = std::move(probe); }

signals:
    void stateChanged();
    void messageChanged();
    void busyChanged();

private:
    void apply(int look, const cli::Status& status);
    void setMessage(const QString& message);
    void setBusy(bool busy);
    void changePath(bool add);
    void finishChange(bool add, unsigned long error);

    SettingsEditor* m_editor;
    std::function<cli::Status()> m_probe;
    QVariantMap m_state;
    QString m_message;
    bool m_busy = false;
    int m_looks = 0; // the newest look wins
};

} // namespace ws
