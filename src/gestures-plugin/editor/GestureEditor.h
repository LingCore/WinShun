// What the settings window changes and shows: the rules, the options of the
// card, the programs. QML reads and writes it; every change that is kept
// goes out as `saveRequested` (EditorLink sends it to the plugin as a
// "save" line) and what the plugin says (applyState) comes back in without
// undoing what was typed since.
#pragma once

#include "RuleModels.h"

#include <QJsonObject>
#include <QObject>
#include <QPointer>
#include <QStringList>
#include <QTimer>
#include <QVariantList>
#include <QVariantMap>
#include <QWindow>
#include <QtQmlIntegration/qqmlintegration.h>



namespace ws::gestureseditor {

class GestureEditor : public QObject {
    Q_OBJECT
    QML_ELEMENT
    QML_UNCREATABLE("Made by the plugin")
    Q_PROPERTY(ws::gestureseditor::RulesModel* rules READ rules CONSTANT)
    Q_PROPERTY(bool chinese READ chinese NOTIFY chineseChanged)
    // Counts every change of the rules: a binding that reads it reads them again.
    Q_PROPERTY(int revision READ revision NOTIFY revisionChanged)
    Q_PROPERTY(bool trail READ trail WRITE setTrail NOTIFY trailChanged)
    Q_PROPERTY(bool fullScreen READ fullScreen WRITE setFullScreen NOTIFY fullScreenChanged)
    Q_PROPERTY(bool paused READ paused WRITE setPaused NOTIFY pausedChanged)
    Q_PROPERTY(QStringList excluded READ excluded NOTIFY excludedChanged)
    // The programs with rules of their own ({name, count}), those added here after them.
    Q_PROPERTY(QVariantList programs READ programs NOTIFY programsChanged)
    // The builtin actions by category ({title, icon, items: [{id, title, note}]}), for the picker.
    Q_PROPERTY(QVariantList categories READ categories NOTIFY chineseChanged)

public:
    explicit GestureEditor(QObject* parent = nullptr);

    RulesModel* rules() { return &m_rules; }
    bool chinese() const { return m_chinese; }
    int revision() const { return m_revision; }
    bool trail() const { return m_trail; }
    bool fullScreen() const { return m_fullScreen; }
    bool paused() const { return m_paused; }
    QStringList excluded() const { return m_excluded; }
    QVariantList programs() const { return m_programs; }
    QVariantList categories() const;

    void setTrail(bool on);
    void setFullScreen(bool on);
    void setPaused(bool on);
    void setChinese(bool chinese);
    void setWindow(QWindow* window) { m_window = window; }

    // What the plugin says: {rules, exclude, trail, fullScreen, paused}. `first`: the window was just
    // opened, everything is taken as it is; later what differs from what this window saved is
    // taken (an edit made here is not undone by the answer to an earlier one).
    void applyState(const QJsonObject& state, bool first);
    // For the preview and the self test: sample settings, without a plugin.
    void useSample();

    Q_INVOKABLE int addOrFind(const QString& on, int where, bool selection, const QString& app);
    Q_INVOKABLE void discardProvisional();
    Q_INVOKABLE void setAction(int row, const QString& kind, const QString& value, const QString& app);
    Q_INVOKABLE void setWhere(int row, int zone, const QString& app);
    Q_INVOKABLE QVariantList zoneChoices(int row) const; // [{text, glyph, zone}]: where it can be moved to
    Q_INVOKABLE void remove(int row, const QString& app);
    // A trigger that is one rule everywhere (a corner, an edge, the wheel), or a program's: {has, kind, title, icon, note, id}.
    Q_INVOKABLE QVariantMap slot(const QString& on, const QString& app) const;
    Q_INVOKABLE void setSlot(const QString& on, const QString& app, const QString& kind, const QString& value);
    Q_INVOKABLE void clearSlot(const QString& on, const QString& app);
    Q_INVOKABLE QString addProgram(const QString& name); // its name as kept ("" when it is none)
    Q_INVOKABLE void removeProgram(const QString& app);
    Q_INVOKABLE QString normalizeProgram(const QString& text) const; // "C:\x\Foo.EXE" -> "foo.exe", "" when none
    Q_INVOKABLE void addExcluded(const QString& program);
    Q_INVOKABLE void removeExcluded(const QString& program);
    Q_INVOKABLE void restoreDefaults();
    // The programs of the windows open now (file names, lower case), the window's own left out.
    Q_INVOKABLE QStringList visiblePrograms() const;
    Q_INVOKABLE QString browseFile(const QString& title);
    // A key pressed in the window, as the action's keys read it ("Ctrl+Shift+T"); "" when it cannot be one.
    Q_INVOKABLE QString keyText(int key, int modifiers, quint32 scanCode) const;
    Q_INVOKABLE bool keysValid(const QString& text) const;

signals:
    void chineseChanged();
    void revisionChanged();
    void trailChanged();
    void fullScreenChanged();
    void pausedChanged();
    void excludedChanged();
    void programsChanged();
    // A rule the pad drew: the one there was already (flash it) or a new one (ask for its action).
    void ruleFocused(int sourceRow, bool created);
    // Values to keep, as the plugin's settings name them: rules, exclude, trail, fullScreen.
    void saveRequested(const QJsonObject& values);
    void pauseRequested(bool paused);

private:
    void rulesEdited();
    void rebuildPrograms();
    void saveRules();
    void saveExcluded();
    int slotRow(const QString& on, const QString& app) const;
    static bool accept(QStringList& pending, QString& heard, const QString& incoming, const QString& current);

    RulesModel m_rules;
    bool m_chinese = true;
    int m_revision = 0;
    bool m_trail = true;
    bool m_fullScreen = false;
    bool m_paused = false;
    QStringList m_excluded;
    QStringList m_added; // programs added here that have no rule yet
    QVariantList m_programs;
    QTimer m_saveLater; // one "rules" line for the edits of a turn
    QPointer<QWindow> m_window;
    // What was saved and not yet heard back, per setting (the answers come in order).
    QStringList m_pendingRules;
    QStringList m_pendingExclude;
    QStringList m_pendingTrail;
    QStringList m_pendingFullScreen;
    QStringList m_pendingPaused;
    // What the plugin said last, per setting: an answer that still says that carries a change of
    // another setting (the answers are of everything), not a change of this one.
    QString m_heardRules;
    QString m_heardExclude;
    QString m_heardTrail;
    QString m_heardFullScreen;
    QString m_heardPaused;
};

} // namespace ws::gestureseditor
