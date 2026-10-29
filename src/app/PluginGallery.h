#pragma once

// The 插件 page's 插件库 tab: the plugins the WinShun-plugins repository
// publishes (plugin::gallery), each with what it is here -- 安装, 更新到,
// 已安装, 随 Win顺 自带, 需要更新 Win顺 -- and installing one: the package
// downloaded (one at a time, with its progress, cancellable), checked
// against the list, then handed to the page's own install
// (PluginSettings::installFrom), which asks what it always asks and leaves
// the plugin off until it is turned on. The list is fetched only when the
// tab is opened (once in a while) or 刷新 is pressed; the last good copy is
// kept (plugin-index.json in the data folder) and shown meanwhile. With
// WINSHUN_PLUGIN_INDEX set (an address or a file), that list instead, and
// nothing kept.

#include "Gallery.h"

#include <QAbstractListModel>
#include <QDateTime>
#include <QElapsedTimer>
#include <QHash>
#include <QObject>
#include <QStringList>
#include <QVariantList>
#include <QtQml/qqmlregistration.h>

#include <atomic>
#include <memory>
#include <vector>

namespace ws {

class PluginSettings;

// The list's plugins, filtered by `shown` (the delegates stay while the
// filter changes).
class PluginGalleryModel : public QAbstractListModel {
    Q_OBJECT
    QML_ANONYMOUS

public:
    struct Row {
        QString id;
        QString name;
        QString description;
        QString author;
        QString version; // the list's
        QString icon; // an image URL: the installed copy's icon, else the plugins' glyph
        QStringList keywords;
        QVariantList permissions; // [{text, risky}]
        QString needs; // what has to be installed first ("需要 Python 3"); empty: nothing
        bool needsMissing = false; // ... and it is not on this computer
        QString stage; // install | update | installed | bundled | app (needs a newer Win顺)
        QString installedVersion; // the user's copy's
        QString minWinShun;
        bool busy = false; // being downloaded
        double progress = 0; // 0–1
        QString problem; // why the last install from here did not happen
        bool shown = true; // fits the filter
        QString words; // what the filter looks in: the id, the names, descriptions and keywords, lower-cased
        friend bool operator==(const Row&, const Row&) = default;
    };
    enum Role {
        IdRole = Qt::UserRole + 1,
        NameRole,
        DescriptionRole,
        AuthorRole,
        VersionRole,
        IconRole,
        KeywordsRole,
        PermissionsRole,
        NeedsRole,
        NeedsMissingRole,
        StageRole,
        InstalledVersionRole,
        MinWinShunRole,
        BusyRole,
        ProgressRole,
        ProblemRole,
        ShownRole,
    };

    using QAbstractListModel::QAbstractListModel;
    int rowCount(const QModelIndex& parent = QModelIndex()) const override;
    QVariant data(const QModelIndex& index, int role) const override;
    QHash<int, QByteArray> roleNames() const override;

    // Rows of the same ids are changed in place (only the roles that
    // differ announced); others reset the list.
    void setRows(std::vector<Row> rows);
    void setProgress(const QString& id, double progress); // a download's, often: that row alone
    const std::vector<Row>& rows() const { return m_rows; }

private:
    std::vector<Row> m_rows;
};

class PluginGallery : public QObject {
    Q_OBJECT
    QML_ELEMENT
    QML_UNCREATABLE("Provided by PluginSettings")
    Q_PROPERTY(ws::PluginGalleryModel* list READ list CONSTANT FINAL)
    Q_PROPERTY(int count READ count NOTIFY listChanged FINAL)
    Q_PROPERTY(int shownCount READ shownCount NOTIFY listChanged FINAL) // those that fit the filter
    Q_PROPERTY(QString filter READ filter WRITE setFilter NOTIFY filterChanged FINAL)
    // The tab is on show: the list is read from the copy kept, and fetched
    // when this run has not fetched it lately.
    Q_PROPERTY(bool active READ active WRITE setActive NOTIFY activeChanged FINAL)
    Q_PROPERTY(bool fetching READ fetching NOTIFY stateChanged FINAL)
    Q_PROPERTY(QString status READ status NOTIFY stateChanged FINAL) // how many, from when, or that it is being fetched
    Q_PROPERTY(QString problem READ problem NOTIFY stateChanged FINAL) // why the last fetch failed; empty when it did not
    // ... and it is that GitHub could not be reached: the page offers the proxy setting (GitHub #84).
    Q_PROPERTY(bool problemIsConnection READ problemIsConnection NOTIFY stateChanged FINAL)
    Q_PROPERTY(bool downloading READ downloading NOTIFY stateChanged FINAL) // a package is on its way (one at a time)

public:
    explicit PluginGallery(PluginSettings* settings);
    ~PluginGallery() override;

    PluginGalleryModel* list() { return &m_list; }
    int count() const { return static_cast<int>(m_list.rows().size()); }
    int shownCount() const;
    QString filter() const { return m_filter; }
    void setFilter(const QString& filter);
    bool active() const { return m_active; }
    void setActive(bool on);
    bool fetching() const { return m_fetching; }
    QString status() const;
    QString problem() const { return m_problem; }
    bool problemIsConnection() const { return m_problemIsConnection; }
    bool downloading() const { return !m_downloadId.isEmpty(); }

    Q_INVOKABLE void refresh(); // 刷新
    Q_INVOKABLE void install(const QString& id); // 安装, 更新到
    Q_INVOKABLE void cancel(const QString& id); // the download
    Q_INVOKABLE void openRepository(); // 看看怎么投稿

    // The rows again: the list, or what is installed (the catalog), or the
    // language changed.
    void rebuild();
    // Made-up plugins in every state, and no network (the offscreen previews).
    void loadPreview();

signals:
    void listChanged();
    void filterChanged();
    void activeChanged();
    void stateChanged();

private:
    struct Download;
    QString source() const; // the list's address or file
    bool keeping() const; // the list is the published one: its copy is kept
    QString cachePath() const;
    void readCache();
    void finishFetch(const QString& source, const plugin::gallery::Fetched& fetched);
    void finishDownload(const QString& id, const Download& download);
    void setRowProgress(const QString& id, double progress);
    QString networkProblem(const plugin::gallery::Fetched& fetched) const;
    static bool connectionFailed(const plugin::gallery::Fetched& fetched); // no answer: the network, the proxy or GitHub

    PluginSettings* m_settings;
    plugin::gallery::Index m_index;
    bool m_hasIndex = false;
    QDateTime m_listTime; // when the list shown was fetched (the kept copy's time until this run fetches it)
    QElapsedTimer m_fetchedThisRun; // invalid: not fetched in this run
    bool m_cacheRead = false;
    bool m_active = false;
    bool m_fetching = false;
    QString m_problem;
    bool m_problemIsConnection = false;
    QString m_filter;
    // The download: its plugin, what stops it, and its number (a result
    // from one cancelled meanwhile is let go of).
    QString m_downloadId;
    std::shared_ptr<std::atomic_bool> m_cancel;
    quint64 m_generation = 0;
    double m_progress = 0;
    QHash<QString, QString> m_problems; // by plugin: why its last install from here did not happen
    bool m_preview = false;
    std::vector<PluginGalleryModel::Row> m_previewRows; // loadPreview()'s
    PluginGalleryModel m_list;
};

} // namespace ws
