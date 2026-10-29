#pragma once

#include <QElapsedTimer>
#include <QImage>
#include <QMutex>
#include <QObject>
#include <QPointer>
#include <QQuickImageProvider>
#include <QQuickWindow>
#include <QRect>
#include <QSize>
#include <QStringList>
#include <QStringListModel>
#include <QTimer>
#include <QUuid>
#include <QVector>
#include <QtQml/qqmlregistration.h>

#include <atomic>
#include <functional>
#include <memory>
#include <optional>

class QQuickItem;
class QWindow;

namespace ws {

class ComWorker;
class PreviewHost;
namespace extpreview {
enum class Engine;
}

// A picture file's first frame (PNG, JPEG, BMP, GIF, WebP, … whatever
// Windows Imaging Component reads), upright as the camera says, at most
// `largest` pixels on its longer side (0: as stored); null if it cannot be
// read. Any thread with COM initialised. For 贴到屏幕 (dist has no Qt image
// plugins: QImage alone reads PNG and BMP only).
QImage decodePictureFile(const QString& path, int largest, QSize* original = nullptr);

// A look at a result's file without opening it (Alt+P on a row, 预览 in its
// menu; the forum's #157, #162, #225): QuickLook (QL-Win) or Seer when one of
// them runs in the background and the settings leave it to them (预览方式:
// 自动), else Win顺's own window (PreviewWindow.qml): a picture (Windows
// Imaging Component decodes it, any format Windows reads), the start of a
// text file (decoded as the content search decodes), a document through
// the preview handler Explorer's preview pane uses (PreviewHost), else a
// large thumbnail and what is known of the file; a folder, its icon and
// what it holds.
//
// The launcher stays open and keeps the keyboard (the window never takes
// the focus): Alt+P again or the × closes the preview (Esc closes the
// launcher, and the preview comes back with it), the arrows show the next
// row's file in it (follow). Reading the file is never done on the GUI
// thread; a newer request makes an older one's result go unseen.
class FilePreview : public QObject {
    Q_OBJECT
    QML_ELEMENT
    QML_UNCREATABLE("Provided by the application")
    Q_PROPERTY(Kind kind READ kind NOTIFY contentChanged FINAL)
    Q_PROPERTY(QString name READ name NOTIFY contentChanged FINAL)
    Q_PROPERTY(QString path READ path NOTIFY contentChanged FINAL)
    Q_PROPERTY(QString iconUrl READ iconUrl NOTIFY contentChanged FINAL)
    // A line under the name: the type, the size, when written, and per kind
    // the picture's size, the text's encoding ("UTF-8 · 120 行"), …
    Q_PROPERTY(QString summary READ summary NOTIFY contentChanged FINAL)
    // Picture: the decoded picture (pixels); Facts: a thumbnail or the icon.
    Q_PROPERTY(QString imageUrl READ imageUrl NOTIFY contentChanged FINAL)
    Q_PROPERTY(QSize imageSize READ imageSize NOTIFY contentChanged FINAL)
    // Text: a line a row, long ones in pieces (lineNumberAt: 0 for a piece
    // that goes on from the row before).
    Q_PROPERTY(QStringListModel* lines READ lines CONSTANT FINAL)
    Q_PROPERTY(int lineCount READ lineCount NOTIFY contentChanged FINAL)
    // Facts: label, value, label, value …
    Q_PROPERTY(QStringList facts READ facts NOTIFY contentChanged FINAL)
    Q_PROPERTY(QString message READ message NOTIFY contentChanged FINAL) // Message: why nothing shows
    Q_PROPERTY(bool slow READ slow NOTIFY slowChanged FINAL) // Loading for a moment already

public:
    enum Kind { Loading, Picture, Text, Handler, Facts, Message };
    Q_ENUM(Kind)

    struct Item {
        QString path;
        QString name;
        bool isDir = false;
    };

    explicit FilePreview(QObject* parent = nullptr);
    ~FilePreview() override;

    // The app's part: the window (made the first time it is wanted, with
    // its frame ready), the launcher it goes beside, the settings.
    void setWindowFactory(std::function<QQuickWindow*()> factory) { m_makeWindow = std::move(factory); }
    void setLauncherWindow(QWindow* launcher);
    void setBuiltInOnly(bool on) { m_builtInOnly = on; } // 预览方式: 内置
    void setDark(bool dark);
    // Text files besides the usual ones (the content search's kinds).
    void setTextExtensions(QStringList extensions);

    bool isOpen() const { return m_open; }
    // A file shown now would go to QuickLook or Seer (预览方式 自动, one runs).
    bool showsElsewhere() const;
    // Shown in QuickLook or Seer, which the user closed there (its own
    // Space or Esc, a click): no longer open here either. Not every key
    // (Seer is asked through a message), only when it matters: Alt+P, Esc.
    void forgetClosedExternal();
    bool isShowing(const QString& path) const;
    void show(const Item& item);
    // Open: the current row's file now (none: a row that is not a file).
    void follow(const std::optional<Item>& item);
    Q_INVOKABLE void close();
    // The × in the header: closes, and says so (the launcher then does not
    // bring the preview back the next time it opens).
    Q_INVOKABLE void closeByUser();
    // The window in front belongs to the preview (ours, or the external
    // previewer's): the launcher is not to go for that.
    bool ownsForeground() const;
    bool foregroundIsOurs() const; // ...and it is a window of Win顺's own (handed back to the launcher)
    QQuickWindow* window() const { return m_window; }

    Kind kind() const { return m_kind; }
    QString name() const { return m_item.name; }
    QString path() const { return m_item.path; }
    QString iconUrl() const;
    QString summary() const { return m_summary; }
    QString imageUrl() const { return m_imageUrl; }
    QSize imageSize() const { return m_imageSize; }
    QStringListModel* lines() { return &m_lines; }
    int lineCount() const { return m_lineCount; }
    Q_INVOKABLE int lineNumberAt(int row) const { return row >= 0 && row < m_lineNumbers.size() ? m_lineNumbers[row] : 0; }
    QStringList facts() const { return m_facts; }
    QString message() const { return m_message; }
    bool slow() const { return m_slow; }

    Q_INVOKABLE void openFile(); // 打开 in the header: as Enter on the row
    // The content area, where a preview handler draws (PreviewWindow.qml).
    Q_INVOKABLE void setHandlerArea(QQuickItem* area);

    // "image://preview/<n>": the picture, or the thumbnail, of the file shown.
    QQuickImageProvider* imageProvider();

signals:
    void contentChanged();
    void slowChanged();
    void closedByUser();
    void openRequested(const QString& path); // 打开: the launcher opens it, as Enter would

private:
    struct Loaded; // what the worker found out
    class Images;

    void load(const Item& item);
    void apply(quint64 generation, std::shared_ptr<Loaded> loaded);
    void showWindow(QSize wanted); // logical
    QRect placeBeside(QSize wanted); // the window's place by the launcher (logical)
    void placeHandler();
    void startHandler(quint64 generation, const QUuid& handler);
    void handlerFailed(quint64 generation);
    bool screenRect(QQuickItem* item, int* left, int* top, int* right, int* bottom) const; // physical
    void showExternal(const Item& item);
    void setSlow(bool slow);

    std::function<QQuickWindow*()> m_makeWindow;
    QPointer<QQuickWindow> m_window;
    QPointer<QWindow> m_launcher;
    QPointer<QQuickItem> m_handlerArea;
    bool m_builtInOnly = false;
    bool m_dark = false;
    QStringList m_textExtensions;

    bool m_open = false;
    std::optional<extpreview::Engine> m_external; // open in QuickLook or Seer
    QElapsedTimer m_externalSince; // its window comes a moment after it is asked
    Item m_item;
    std::atomic<quint64> m_generation {0};
    ComWorker* m_worker = nullptr; // never deleted: a shell call may still be running at exit
    std::unique_ptr<PreviewHost> m_host;
    std::shared_ptr<Images> m_images;

    Kind m_kind = Loading;
    QString m_summary;
    QString m_imageUrl;
    QSize m_imageSize;
    QStringListModel m_lines;
    QVector<int> m_lineNumbers;
    int m_lineCount = 0;
    int m_textRows = 0;
    int m_side = -1; // placeBeside: right, left, below, or over the middle; -1 not chosen yet
    QRect m_sideFor; // the launcher's place when it was chosen
    bool m_overLauncher = false; // no room beside it: in front of it
    QStringList m_facts;
    QString m_message;
    bool m_slow = false;
    QTimer m_slowTimer;
    QTimer m_relayout; // the launcher moved or changed size: placed again
    QTimer m_externalCheck; // Seer asked: did it show anything? (GitHub #131)
    QString m_noHandlerFor; // a handler failed on it: shown as facts instead
};

} // namespace ws
