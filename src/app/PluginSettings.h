#pragma once

// The view-model of the settings window's 插件 page (the plugin design,
// §5.1–5.5): a card per plugin (name, state, switch, keywords, permissions,
// its declared settings), installing and removing plugins, developer mode
// with the log of what goes to and from them, and new plugins from
// templates. What the user sets goes into Settings ([Plugin.<id>]) through
// SettingsEditor, whose commit the app applies (App::applyPlugins);
// passwords go to the Credential Manager (plugin::Secrets), never into the
// INI. Without a host (the offscreen preview: loadPreview) it shows made-up
// plugins. The 插件库 tab's plugins to install are its `gallery`'s.

#include "Install.h"
#include "PluginGallery.h"

#include <QAbstractListModel>
#include <QObject>
#include <QPointer>
#include <QTimer>
#include <QVariantList>
#include <QVariantMap>
#include <QtQml/qqmlregistration.h>

#include <memory>
#include <optional>
#include <vector>

namespace ws {

class SettingsEditor;

namespace plugin {
class Host;
}

// The cards, one per plugin found.
class PluginListModel : public QAbstractListModel {
    Q_OBJECT
    QML_ANONYMOUS

public:
    struct Card {
        QString id;
        QString name;
        QString version;
        QString author;
        QString homepage;
        QString description;
        bool bundled = false;
        QString icon; // an image URL
        bool enabled = false;
        bool usable = true; // false: the manifest is wrong, or the program is missing (`error` says)
        QString status; // where it stands, said for the user
        QString tone; // ok | busy | idle | warn | error (ShareCard's)
        QString error;
        bool resident = false;
        QVariantList keywords; // [{command, title, keyword, defaults, conflict, user}]
        QVariantList permissions; // [{name, text, enforced, note}]
        QVariantList settings; // [{key, type, title, hint, value, options, optionIndex}]
        bool hasReadme = false;
        bool hasData = false;
        bool aiExpose = false; // the manifest offers its commands to the AI (ai.expose): the card shows the switch
        bool aiEnabled = true; // the switch: [Plugin.<id>] Ai=
        QString aiNote; // under the switch: the tool names, or why they are not on offer yet
        friend bool operator==(const Card&, const Card&) = default;
    };
    enum Role {
        IdRole = Qt::UserRole + 1,
        NameRole,
        VersionRole,
        AuthorRole,
        HomepageRole,
        DescriptionRole,
        BundledRole,
        IconRole,
        EnabledRole,
        UsableRole,
        StatusRole,
        ToneRole,
        ErrorRole,
        ResidentRole,
        KeywordsRole,
        PermissionsRole,
        SettingsRole,
        HasReadmeRole,
        HasDataRole,
        AiExposeRole,
        AiEnabledRole,
        AiNoteRole,
    };

    using QAbstractListModel::QAbstractListModel;
    int rowCount(const QModelIndex& parent = QModelIndex()) const override;
    QVariant data(const QModelIndex& index, int role) const override;
    QHash<int, QByteArray> roleNames() const override;

    // The cards as they are now: rows of the same ids are changed in
    // place (the delegates stay, with what they have unfolded, and only
    // the roles that differ are announced: a status line read every two
    // seconds must not rebuild the boxes being typed in), others reset
    // the list. True when the rows themselves changed.
    bool setCards(std::vector<Card> cards);
    const Card* card(const QString& id) const;

private:
    std::vector<Card> m_cards;
};

// Developer mode's log: the last 500 lines to and from the plugins.
class PluginLogModel : public QAbstractListModel {
    Q_OBJECT
    QML_ANONYMOUS

public:
    struct Line {
        QString time; // "HH:mm:ss.zzz"
        QString plugin;
        QString direction; // → ← stderr ·
        QString summary;
        QString detail;
        int elapsedMs = -1;
    };
    enum Role { TimeRole = Qt::UserRole + 1, PluginRole, DirectionRole, SummaryRole, DetailRole, ElapsedRole };
    static constexpr int kMaxLines = 500;

    using QAbstractListModel::QAbstractListModel;
    int rowCount(const QModelIndex& parent = QModelIndex()) const override;
    QVariant data(const QModelIndex& index, int role) const override;
    QHash<int, QByteArray> roleNames() const override;

    void append(Line line);
    void clear();
    void setFilter(const QString& pluginId); // empty: every plugin
    QString filter() const { return m_filter; }
    QStringList plugins() const; // the ids seen

private:
    bool shown(const Line& line) const { return m_filter.isEmpty() || line.plugin == m_filter; }
    void rebuild();

    std::vector<Line> m_all; // newest last
    std::vector<int> m_shown; // indices into m_all
    QString m_filter;
};

class PluginSettings : public QObject {
    Q_OBJECT
    QML_ELEMENT
    QML_UNCREATABLE("Provided by SettingsEditor")
    Q_PROPERTY(ws::PluginListModel* list READ list CONSTANT FINAL)
    Q_PROPERTY(int count READ count NOTIFY listChanged FINAL)
    Q_PROPERTY(ws::PluginLogModel* log READ log CONSTANT FINAL)
    Q_PROPERTY(QVariantList logPlugins READ logPlugins NOTIFY logPluginsChanged FINAL) // [{id, name}] for the filter
    Q_PROPERTY(QString logFilter READ logFilter WRITE setLogFilter NOTIFY logFilterChanged FINAL)
    Q_PROPERTY(bool developerMode READ developerMode WRITE setDeveloperMode NOTIFY developerModeChanged FINAL)
    // Why plugins may not see the network places the user signed in to in
    // File Explorer (Runner::TokenSource::Restricted); empty when they do.
    Q_PROPERTY(QString restrictedNote READ restrictedNote NOTIFY stateChanged FINAL)
    Q_PROPERTY(QString message READ message NOTIFY messageChanged FINAL) // how the last install or removal went
    Q_PROPERTY(QString userDir READ userDir NOTIFY stateChanged FINAL)
    Q_PROPERTY(bool active READ active WRITE setActive NOTIFY activeChanged FINAL) // the page is on show: memory figures are read
    // A card's hotkey recorder has the keyboard (SettingsWindow suspends the global hotkeys).
    Q_PROPERTY(bool recordingHotkey READ recordingHotkey WRITE setRecordingHotkey NOTIFY recordingHotkeyChanged FINAL)
    Q_PROPERTY(QString templateAuthor READ templateAuthor CONSTANT FINAL) // "<author>." of a new plugin's id
    // The made-up plugins of the offscreen preview (loadPreview): no frames, so no animations.
    Q_PROPERTY(bool preview READ preview NOTIFY listChanged FINAL)
    Q_PROPERTY(ws::PluginGallery* gallery READ gallery CONSTANT FINAL) // the 插件库 tab

public:
    explicit PluginSettings(SettingsEditor* editor, QObject* parent = nullptr);
    ~PluginSettings() override;

    void setHost(plugin::Host* host); // the app's; null for none
    void setLocale(const QString& locale);
    // Without a host: five made-up plugins in every state, for the
    // offscreen preview of the page (main.cpp --preview-plugins).
    void loadPreview();

    PluginListModel* list() { return &m_list; }
    int count() const { return m_list.rowCount(); }
    PluginLogModel* log() { return &m_log; }
    QVariantList logPlugins() const;
    QString logFilter() const { return m_log.filter(); }
    void setLogFilter(const QString& id);
    bool developerMode() const;
    void setDeveloperMode(bool on);
    QString restrictedNote() const { return m_restrictedNote; }
    QString message() const { return m_message; }
    QString userDir() const { return m_userDir; }
    bool active() const { return m_active; }
    bool preview() const { return m_preview != nullptr; }
    void setActive(bool on);
    bool recordingHotkey() const { return m_recordingHotkey; }
    void setRecordingHotkey(bool on);
    QString templateAuthor() const;
    PluginGallery* gallery() { return &m_gallery; }
    // What a permission lets a plugin do, as the cards and the dialog say it.
    static QString permissionText(plugin::Permission p);
    // install(), for a package downloaded from the 插件库: it must be
    // `expectedId`, and is called `name` in what is said of it.
    void installFrom(const QString& path, const QString& expectedId, const QString& name);

    // The switch: turning on asks first (confirmationNeeded) when the plugin
    // was never confirmed or asks for more permissions than it did.
    Q_INVOKABLE void setEnabled(const QString& id, bool on);
    Q_INVOKABLE void confirmEnable(const QString& id); // the dialog's yes
    Q_INVOKABLE void declineEnable(const QString& id); // its no: one waiting for the OK goes off
    Q_INVOKABLE void setAiEnabled(const QString& id, bool on); // the card's "给 AI 用" switch
    Q_INVOKABLE void setKeyword(const QString& id, const QString& command, const QString& keyword); // the user's; empty: none
    Q_INVOKABLE void resetKeyword(const QString& id, const QString& command); // back to the manifest's
    Q_INVOKABLE void setSettingValue(const QString& id, const QString& key, const QVariant& value);
    Q_INVOKABLE void pickFolderSetting(const QString& id, const QString& key);
    Q_INVOKABLE void pickFileSetting(const QString& id, const QString& key);
    // Installing: a folder or an archive chosen, or dropped on the page.
    Q_INVOKABLE void installFromFolder();
    Q_INVOKABLE void installFromArchive();
    Q_INVOKABLE void install(const QString& path); // installQuestion() when the id is installed already
    Q_INVOKABLE void confirmInstall();
    Q_INVOKABLE void cancelInstall();
    // Removing: uninstallQuestion() first (whether to take its data too).
    Q_INVOKABLE void uninstall(const QString& id);
    Q_INVOKABLE void confirmUninstall(const QString& id, bool withData);
    Q_INVOKABLE void reload(const QString& id);
    Q_INVOKABLE void openFolder(const QString& id);
    Q_INVOKABLE void openReadme(const QString& id);
    Q_INVOKABLE void openHomepage(const QString& id);
    Q_INVOKABLE void openLog();
    Q_INVOKABLE void openUserDir();
    Q_INVOKABLE void clearLog();
    Q_INVOKABLE void clearMessage();
    // A new plugin from a template: `kind` powershell | python |
    // python-resident | cpp | zero, `name` the part after "<author>." in its
    // id. Each is written on its SDK file (src/plugins-sdk, copied in). Why
    // it could not be made, or empty once it is (and its folder is opened).
    Q_INVOKABLE QString createFromTemplate(const QString& kind, const QString& name);
    Q_INVOKABLE void copyDevelopmentPrompt();
    static QStringList templateKinds()
    {
        return {QStringLiteral("powershell"), QStringLiteral("python"), QStringLiteral("python-resident"), QStringLiteral("cpp"), QStringLiteral("zero")};
    }
    // The wording of the permission dialog (the plugin design, §3.3).
    Q_INVOKABLE QString permissionsNote() const;

signals:
    void listChanged();
    void logPluginsChanged();
    void logFilterChanged();
    void developerModeChanged();
    void stateChanged();
    void messageChanged();
    void activeChanged();
    void recordingHotkeyChanged();
    // The dialogs: {id, name, version, author, homepage, source, permissions: [{name, text, enforced, note}], more: bool (asked again for new ones)}.
    void confirmationNeeded(const QVariantMap& info);
    // {id, name, installedVersion, newVersion, downgrade: bool}
    void installQuestion(const QVariantMap& info);
    // {id, name, hasData}
    void uninstallQuestion(const QVariantMap& info);

private:
    friend class PluginGallery; // reads the host, the language and the cards (their icons)
    struct Preview;
    void rebuild(); // the cards from the host and the settings
    PluginListModel::Card cardFor(const QString& id) const;
    void setMessage(const QString& message);
    void readMemory(); // the resident processes' working sets, into the status lines
    void finishInstall(plugin::install::Candidate candidate);
    QString pluginName(const QString& id) const;
    QVariantList permissionRows(const QString& id) const; // [{name, text, enforced, note}]
    void applyPluginSettings(const QString& id); // the host hears the values (secrets included)

    SettingsEditor* m_editor;
    QPointer<plugin::Host> m_host;
    QString m_locale = QStringLiteral("zh");
    QString m_userDir;
    QString m_dataRoot;
    QString m_logPath;
    QString m_restrictedNote;
    QString m_message;
    PluginListModel m_list;
    PluginLogModel m_log;
    QTimer m_memoryTimer;
    bool m_active = false;
    bool m_recordingHotkey = false;
    std::optional<plugin::install::Candidate> m_pendingInstall; // waiting for the user's answer
    std::unique_ptr<Preview> m_preview; // loadPreview(): no host
    PluginGallery m_gallery {this};
};

} // namespace ws
