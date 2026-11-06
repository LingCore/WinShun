#pragma once

#include "ContentDetails.h"
#include "ContentReport.h"
#include "SearchEngine.h"

#include <QAbstractListModel>
#include <QColor>
#include <QPointer>
#include <QTimer>
#include <QVariantList>
#include <QWindow>
#include <QtQml/qqmlregistration.h>
#include <atomic>
#include <memory>

class QQuickItem;
namespace ws {
class ComWorker;
class IndexService;
class PreviewHost;

class ContentLines final : public QAbstractListModel {
    Q_OBJECT
public:
    enum Role { Value = Qt::UserRole + 1, Html, Location, Heading, Hit, Current, SourceLine, Number };
    explicit ContentLines(QObject* parent = nullptr) : QAbstractListModel(parent) { }
    int rowCount(const QModelIndex& parent = {}) const override;
    QVariant data(const QModelIndex& index, int role) const override;
    QHash<int, QByteArray> roleNames() const override;
    void assign(std::shared_ptr<const contentdetails::Matches> matches, std::vector<int> rows,
        std::vector<int> visibleMatches, QColor color,
        std::shared_ptr<const std::vector<contentdetails::Match>> refinements = {});
    void setCurrent(int sourceLine);
    int indexOf(int sourceLine) const;
    int sourceLineAt(int row) const { return row >= 0 && row < std::ssize(m_rows) ? m_rows[row] : -1; }
    QString plainText() const;
    static QString location(const contentdetails::Document& doc, int line);
private:
    std::shared_ptr<const contentdetails::Matches> m_matches;
    std::vector<int> m_rows;
    std::vector<int> m_visibleMatches;
    std::shared_ptr<const std::vector<contentdetails::Match>> m_refinements;
    QColor m_color;
    int m_current = -1;
};

// The panel's optional details. The two workers own their caches/extractor;
// ordinary searches never acquire this data or wait for these tasks.
class ContentPane final : public QObject {
    Q_OBJECT
    QML_ELEMENT
    QML_UNCREATABLE("Provided by the launcher")
    Q_PROPERTY(bool opened READ opened WRITE setOpened NOTIFY preferencesChanged FINAL)
    Q_PROPERTY(int preferredWidth READ preferredWidth WRITE setPreferredWidth NOTIFY preferencesChanged FINAL)
    Q_PROPERTY(int tab READ tab WRITE setTab NOTIFY preferencesChanged FINAL)
    Q_PROPERTY(bool active READ active WRITE setActive NOTIFY activeChanged FINAL)
    Q_PROPERTY(bool busy READ busy NOTIFY changed FINAL)
    Q_PROPERTY(QString path READ path NOTIFY changed FINAL)
    Q_PROPERTY(QString title READ title NOTIFY changed FINAL)
    Q_PROPERTY(QString message READ message NOTIFY changed FINAL)
    Q_PROPERTY(QString matchStatus READ matchStatus NOTIFY changed FINAL)
    Q_PROPERTY(QVariantList terms READ terms NOTIFY changed FINAL)
    Q_PROPERTY(QVariantList sheets READ sheets NOTIFY changed FINAL)
    Q_PROPERTY(int sheet READ sheet WRITE setSheet NOTIFY changed FINAL)
    Q_PROPERTY(int context READ context WRITE setContext NOTIFY changed FINAL)
    Q_PROPERTY(QAbstractItemModel* hits READ hits CONSTANT FINAL)
    Q_PROPERTY(QAbstractItemModel* textLines READ textLines CONSTANT FINAL)
    Q_PROPERTY(QString summary READ summary NOTIFY summaryChanged FINAL)
    Q_PROPERTY(QVariantList groups READ groups NOTIFY summaryChanged FINAL)
    Q_PROPERTY(bool reporting READ reporting NOTIFY reportChanged FINAL)
    Q_PROPERTY(bool reportReady READ reportReady NOTIFY reportChanged FINAL)
    Q_PROPERTY(QString reportStatus READ reportStatus NOTIFY reportChanged FINAL)
    Q_PROPERTY(QString reportPreview READ reportPreview NOTIFY reportChanged FINAL)
    Q_PROPERTY(QVariantMap reportOptions READ reportOptions NOTIFY reportChanged FINAL)
    Q_PROPERTY(QString previewMessage READ previewMessage NOTIFY previewChanged FINAL)
    Q_PROPERTY(QColor highlightColor READ highlightColor WRITE setHighlightColor NOTIFY changed FINAL)
    Q_PROPERTY(QString filterText READ filterText NOTIFY changed FINAL)
    Q_PROPERTY(bool matchCase READ matchCase NOTIFY changed FINAL)
    Q_PROPERTY(bool wholeWord READ wholeWord NOTIFY changed FINAL)
    Q_PROPERTY(bool regex READ regex NOTIFY changed FINAL)
public:
    // What the details are of: a file's row, or a plugin's (GitHub #202).
    struct Subject {
        QString key; // the row: a file's path, a plugin row's (winshun-plugin:…); empty: none
        QString file; // what is read: the file's path, the file a plugin's row opens or names
        QString title; // the header; empty: the file's name
        // A plugin's details as text, in place of a file; `cut`: only the
        // start of what it gave (plugin::kMaxDetailsText).
        std::shared_ptr<const QString> text;
        bool cut = false;
        QString message; // nothing to read, and why (still asked for: `waiting`)
        bool waiting = false;
        friend bool operator==(const Subject&, const Subject&) = default;
    };
    explicit ContentPane(IndexService* index, QObject* parent = nullptr);
    ~ContentPane() override;
    void loadPreferences(const QString& file);
    void update(SearchEngine::Request request, const QString& words, const QString& key,
        SearchResults files, SearchResults selected, const Subject& current,
        int scanned, int total, bool complete, bool searching, const ContentLeftOut& leftOut, int pluginRows = 0);
    bool opened() const { return m_opened; }
    void setOpened(bool value);
    int preferredWidth() const { return m_width; }
    void setPreferredWidth(int value);
    int tab() const { return m_tab; }
    void setTab(int value);
    bool active() const { return m_active; }
    void setActive(bool value);
    bool busy() const { return m_busy; }
    QString path() const { return m_path; }
    QString title() const;
    QString message() const { return m_message; }
    QString matchStatus() const;
    QVariantList terms() const;
    QVariantList sheets() const;
    int sheet() const { return m_sheet; }
    void setSheet(int value);
    int context() const { return m_context; }
    void setContext(int value);
    QAbstractItemModel* hits() { return &m_hits; }
    QAbstractItemModel* textLines() { return &m_text; }
    QString summary() const { return m_summary; }
    QVariantList groups() const { auto out = m_countGroups; out.append(m_groups); return out; }
    bool reporting() const { return m_reporting; }
    bool reportReady() const { return bool(m_report); }
    QString reportStatus() const { return m_reportStatus; }
    QString reportPreview() const { return m_reportPreview; }
    std::shared_ptr<const contentreport::Report> reportSnapshot() const { return m_report; }
    QVariantMap reportOptions() const;
    QString previewMessage() const { return m_previewMessage; }
    // Shows a file's matches, text or preview now (a plugin's details are asked for then).
    bool showsDetails() const { return m_active && m_opened && m_tab >= 1 && m_tab <= 3; }
    QColor highlightColor() const { return m_color; }
    QString filterText() const { return m_filter.text; }
    bool matchCase() const { return m_filter.matchCase; }
    bool wholeWord() const { return m_filter.wholeWord; }
    bool regex() const { return m_filter.regex; }
    void setHighlightColor(const QColor& color);
    Q_INVOKABLE void setFilter(const QString& text, bool matchCase, bool wholeWord, bool regex);
    Q_INVOKABLE void toggleTerm(int index);
    Q_INVOKABLE void nextMatch(int delta);
    Q_INVOKABLE void activateLine(int row, bool textView = false);
    Q_INVOKABLE void selectLine(int row, bool textView = false);
    Q_INVOKABLE void openCurrent();
    Q_INVOKABLE void refresh();
    Q_INVOKABLE void copyText(bool matchesOnly);
    Q_INVOKABLE void prepareReport(bool selectedOnly, bool allFiles, bool includeFiles, bool includeMatches, bool context);
    Q_INVOKABLE void setReportOptions(int scope, bool includeFiles, bool includeMatches, bool context);
    Q_INVOKABLE void cancelReport();
    Q_INVOKABLE void copyReport();
    Q_INVOKABLE void saveReport(int format);
    Q_INVOKABLE void printReport();
    // Called only by the preview tab, with the actual clipped viewport.
    Q_INVOKABLE void setPreviewArea(QQuickItem* area, bool dark);
    Q_INVOKABLE void movePreview();
    void closePreview();
    bool ownsForeground() const;
    std::optional<QRect> previewRect() const;
    bool modalOperation() const { return m_dialogOpen; }
    void setWindow(QWindow* window) { m_window = window; }
    void setContentIndexEnabled(bool enabled) { m_indexEnabled = enabled; }
signals:
    void preferencesChanged();
    void activeChanged();
    void changed();
    void summaryChanged();
    void reportChanged();
    void previewChanged();
    void positionRequested(int hitRow, int textRow);
    // `key`: the Subject's, whose row it is (a plugin's opens as its permissions allow).
    void openRequested(const QString& key, const QString& path, int line);
private:
    struct Worker;
    void schedule();
    void load();
    void showRows();
    void savePreferences();
    void buildReport(SearchResults files, bool full, bool complete, bool includeFiles, bool includeMatches, bool context);
    void startPreview();
    IndexService* m_index;
    QString m_stateFile;
    bool m_opened = true;
    bool m_active = false;
    int m_width = 420;
    int m_tab = 1;
    int m_sheet = -1;
    int m_context = 1;
    bool m_busy = false;
    QString m_path, m_message, m_key;
    Subject m_subject; // m_path: its file
    QStringList m_terms;
    std::vector<bool> m_enabled;
    contentdetails::Filter m_filter;
    ContentLines m_hits, m_text;
    std::shared_ptr<const contentdetails::Matches> m_matches;
    contentdetails::Rows m_rows;
    int m_current = -1;
    QColor m_color;
    QTimer m_debounce;
    SearchEngine::Request m_request;
    SearchResults m_files, m_selected;
    QString m_summary;
    QVariantList m_groups;
    QVariantList m_countGroups;
    ContentLeftOut m_auditLeftOut;
    QStringList m_auditTerms;
    bool m_snapshotComplete = false;
    bool m_indexEnabled = false;
    bool m_dialogOpen = false;
    QPointer<QWindow> m_window;
    bool m_reporting = false;
    QString m_reportStatus, m_reportPreview;
    QVariantMap m_reportOptions;
    std::shared_ptr<const contentreport::Report> m_report;
    std::unique_ptr<SearchEngine> m_audit;
    quint64 m_auditId = 0;
    SearchResults m_auditFiles;
    std::shared_ptr<Worker> m_detailState, m_reportState;
    std::unique_ptr<PreviewHost> m_preview;
    QPointer<QQuickItem> m_previewArea;
    bool m_dark = false;
    QString m_previewPath, m_previewMessage;
    quint64 m_previewTicket = 0;
    // Last: destroyed before the states and models tasks may use.
    std::unique_ptr<ComWorker> m_detailWorker, m_reportWorker;
};
} // namespace ws
