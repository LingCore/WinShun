#pragma once

#include "DoubleTapDetector.h"
#include "Settings.h"

#include <QObject>
#include <QPointer>
#include <QtQml/qqmlregistration.h>

#include <memory>

class QWindow;

namespace ws {

class PluginSettings;
class ShortcutCapture;
Q_MOC_INCLUDE("PluginSettings.h")
namespace plugin { class Host; }

// A draft until apply(): nothing here starts an index, a clipboard watcher
// or a plugin. The normal application starts only after a successful save.
class Onboarding : public QObject {
    Q_OBJECT
    QML_ELEMENT
    QML_UNCREATABLE("Provided by the application")
    Q_PROPERTY(bool contentIndex MEMBER m_contentIndex NOTIFY choicesChanged FINAL)
    Q_PROPERTY(int activation MEMBER m_activation NOTIFY choicesChanged FINAL)
    Q_PROPERTY(QString launchHotkey MEMBER m_launchHotkey NOTIFY choicesChanged FINAL)
    Q_PROPERTY(bool clipboard MEMBER m_clipboard NOTIFY choicesChanged FINAL)
    Q_PROPERTY(bool clipboardWinV MEMBER m_clipboardWinV NOTIFY choicesChanged FINAL)
    Q_PROPERTY(bool capture MEMBER m_captureEnabled NOTIFY choicesChanged FINAL)
    Q_PROPERTY(QString captureHotkey MEMBER m_captureHotkey NOTIFY choicesChanged FINAL)
    Q_PROPERTY(bool recording READ recording WRITE setRecording NOTIFY recordingChanged FINAL)
    Q_PROPERTY(bool testing READ testing NOTIFY stateChanged FINAL)
    Q_PROPERTY(QString testMessage READ testMessage NOTIFY stateChanged FINAL)
    // The keys just tried were recognised: until another try, choice or page.
    Q_PROPERTY(bool testPassed READ testPassed NOTIFY stateChanged FINAL)
    Q_PROPERTY(bool committed READ committed NOTIFY stateChanged FINAL)
    Q_PROPERTY(int page READ page NOTIFY stateChanged FINAL)
    Q_PROPERTY(QString message READ message NOTIFY stateChanged FINAL)
    Q_PROPERTY(bool busy READ busy NOTIFY stateChanged FINAL)
    Q_PROPERTY(bool canRetry READ canRetry NOTIFY stateChanged FINAL)
    Q_PROPERTY(bool captureReady READ captureReady NOTIFY stateChanged FINAL)
    Q_PROPERTY(double progress READ progress NOTIFY stateChanged FINAL)
    Q_PROPERTY(ws::PluginSettings* plugins READ plugins NOTIFY pluginsChanged FINAL)

public:
    explicit Onboarding(const Settings& settings, QObject* parent = nullptr);
    ~Onboarding() override;

    const Settings& settings() const { return m_settings; }
    void setWindow(QWindow* window);
    void attachPlugins(PluginSettings* plugins, plugin::Host* host);
    bool recording() const { return m_recording; }
    void setRecording(bool on);
    bool testing() const { return m_testing; }
    QString testMessage() const { return m_testMessage; }
    bool testPassed() const { return m_testPassed; }
    bool committed() const { return m_committed; }
    int page() const { return m_page; }
    QString message() const { return m_message; }
    bool busy() const { return m_busy; }
    bool canRetry() const { return m_canRetry; }
    bool captureReady() const { return m_captureReady; }
    double progress() const { return m_progress; }
    PluginSettings* plugins() const { return m_plugins; }

    Q_INVOKABLE void next();
    Q_INVOKABLE void previous();
    Q_INVOKABLE void apply(bool later = false);
    Q_INVOKABLE void beginTest(bool capture = false);
    Q_INVOKABLE void stopTest();
    Q_INVOKABLE void retryCapture();
    Q_INVOKABLE void finish();

signals:
    void choicesChanged();
    void recordingChanged();
    void stateChanged();
    void pluginsChanged();
    void accepted();
    void finished();
    void captureActivated();

protected:
    bool eventFilter(QObject* watched, QEvent* event) override;

private:
    QString validateOpening() const;
    QString validate();
    void updateKeyCapture();
    void updateCaptureState();
    void captureProblem(const QString& message);

    Settings m_settings;
    QPointer<QWindow> m_window;
    PluginSettings* m_plugins = nullptr; // owned by the application's temporary settings editor
    plugin::Host* m_host = nullptr;
    std::unique_ptr<ShortcutCapture> m_keyCapture;
    DoubleTapDetector m_doubleTap;
    bool m_contentIndex = false;
    int m_activation = 0; // double Ctrl, Alt, Shift, custom shortcut
    QString m_launchHotkey;
    bool m_clipboard = false;
    bool m_clipboardWinV = false;
    bool m_captureEnabled = false;
    QString m_captureHotkey;
    bool m_recording = false;
    bool m_testing = false;
    bool m_testCapture = false;
    QString m_testMessage;
    bool m_testPassed = false;
    bool m_committed = false;
    int m_page = 0;
    QString m_message;
    bool m_busy = false;
    bool m_canRetry = false;
    bool m_captureReady = false;
    double m_progress = -1;
    bool m_installRequested = false;
    bool m_enableRequested = false;
    QString m_captureFolder;
};

} // namespace ws
