#pragma once

#include <QImage>
#include <QJsonObject>
#include <QObject>
#include <QString>
#include <QTimer>
#include <QtQml/qqmlregistration.h>

#include <atomic>
#include <condition_variable>
#include <functional>
#include <mutex>
#include <optional>
#include <thread>

class QQuickWindow;

namespace ws::capture {

// 公式识别 (Shift+F on a capture or a pin): the formula in the picture read
// as LaTeX by FormulaReader on a thread of its own below normal priority,
// for the formula window (FormulaPanel.qml), up at once saying so meanwhile.
// The reading is tidied (latex::tidyModel) into what a person writes; it can
// be edited there, its drawing following, and copied as LaTeX, Markdown,
// Word's formula (MathML) or Typst, or as a picture.
//
// The model (~120 MB) is Win顺's component (ocr::formulaModel), fetched by
// Win顺, which may go on the network (the formula_model tool, setModelCall):
// the window offers it when it is not there, shows its progress, and reads
// the picture once it is. The reader stays loaded a minute after the last
// picture, as the words' reader does.
class CaptureFormula : public QObject {
    Q_OBJECT
    QML_ELEMENT
    QML_UNCREATABLE("Provided by the application")

    Q_PROPERTY(bool busy READ busy NOTIFY changed FINAL)
    Q_PROPERTY(QString latex READ latex NOTIFY changed FINAL) // what was read, tidied
    Q_PROPERTY(int round READ round NOTIFY changed FINAL) // counts up with each picture: the field takes the new text
    Q_PROPERTY(bool unsure READ unsure NOTIFY changed FINAL) // to be checked against the picture
    Q_PROPERTY(QString message READ message NOTIFY changed FINAL) // nothing read: why
    Q_PROPERTY(QImage picture READ picture NOTIFY changed FINAL)
    // "ready", "missing" (to be downloaded), "downloading", "update" (Win顺 too old to fetch it)
    Q_PROPERTY(QString model READ model NOTIFY modelChanged FINAL)
    Q_PROPERTY(double progress READ progress NOTIFY modelChanged FINAL)
    Q_PROPERTY(QString modelSize READ modelSize NOTIFY modelChanged FINAL)
    Q_PROPERTY(QString modelStatus READ modelStatus NOTIFY modelChanged FINAL) // the last download's failure
    // What 复制 copies: "latex", "markdown", "word" or "typst".
    Q_PROPERTY(QString format READ format WRITE setFormat NOTIFY formatChanged FINAL)

public:
    // Win顺's formula_model tool: `action` (status, install, cancel), answered
    // with the model's state, or an error ("Unknown tool…" from a Win顺
    // without it).
    using ModelCall = std::function<void(const QString& action, std::function<void(const QJsonObject& state, const QString& error)> answer)>;

    explicit CaptureFormula(const QString& programFolder, QObject* parent = nullptr);
    ~CaptureFormula() override;

    void setModelCall(ModelCall call) { m_modelCall = std::move(call); }
    // The model is here, ONNX Runtime with it (nothing loaded by this).
    bool modelReady() const;

    // Reads `image`; replaces what is shown, a reading still under way for
    // an older picture is dropped. No model: kept until there is one.
    void read(const QImage& image);
    void refresh(); // the components changed: a picture waiting for the model is read now
    // The window closed: a reading under way stops, a picture waiting for the model is let go of.
    void discard();

    bool busy() const { return m_busy; }
    QString latex() const { return m_latex; }
    int round() const { return m_round; }
    bool unsure() const { return m_unsure; }
    QString message() const { return m_message; }
    QImage picture() const { return m_picture; }
    QString model() const { return m_model; }
    double progress() const { return m_progress; }
    QString modelSize() const { return m_modelSize; }
    QString modelStatus() const { return m_modelStatus; }
    QString format() const { return m_format; }
    void setFormat(const QString& format);

    Q_INVOKABLE void downloadModel();
    Q_INVOKABLE void cancelDownload();
    // `latex` (the field's) as `format` puts it on the clipboard; "word": the MathML.
    Q_INVOKABLE static QString converted(const QString& latex, const QString& format);
    // To the clipboard in the chosen format.
    Q_INVOKABLE void copy(const QString& latex);
    // The drawn formula: "copy" (a picture), "png" or "svg" (to a file the
    // user picks, the dialog owned by `owner`). False when it cannot be drawn.
    Q_INVOKABLE bool exportPicture(const QString& latex, const QString& how, QQuickWindow* owner);
    Q_INVOKABLE void close() { emit closeRequested(); }

signals:
    void changed();
    void modelChanged();
    void formatChanged();
    void copyTextRequested(const QString& text); // through Win顺, into its history
    void copyMathMlRequested(const QString& mathMl); // as text and as the format Word takes as an equation
    void copyImageRequested(const QImage& image);
    // What became of a copy or an export, for the window's header: "已复制", "已保存", or why not.
    void noticed(const QString& text, bool failed);
    void closeRequested();

private:
    struct Job {
        int round = 0;
        QImage image;
    };
    void work(const std::stop_token& stop);
    void start(const QImage& image);
    void done(int round, const QString& latex, bool unsure, const QString& error);
    void askModel(const QString& action);
    void setModelState(const QJsonObject& state, const QString& error);

    QString m_programFolder;
    ModelCall m_modelCall;
    bool m_busy = false;
    QString m_latex;
    int m_round = 0;
    bool m_unsure = false;
    QString m_message;
    QImage m_picture;
    QImage m_pending; // waiting for the model
    QString m_model = QStringLiteral("ready");
    double m_progress = 0;
    QString m_modelSize;
    QString m_modelStatus;
    QString m_format = QStringLiteral("latex");
    QTimer m_poll; // asks how the download is going while it goes

    std::mutex m_mutex;
    std::condition_variable_any m_wake;
    std::optional<Job> m_job;
    std::atomic_bool m_newer {false}; // a newer picture waits: the reading under way stops
    std::jthread m_thread; // last: started once the rest is there
};

} // namespace ws::capture
